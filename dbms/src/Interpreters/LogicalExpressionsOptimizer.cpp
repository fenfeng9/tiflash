// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/typeid_cast.h>
#include <Interpreters/LogicalExpressionsOptimizer.h>
#include <Interpreters/Settings.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>

#include <deque>


namespace DB
{
namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
}


LogicalExpressionsOptimizer::OrWithExpression::OrWithExpression(
    ASTFunction * or_function_,
    const IAST::Hash & expression_,
    const std::string & alias_)
    : or_function(or_function_)
    , expression(expression_)
    , alias(alias_)
{}

bool LogicalExpressionsOptimizer::OrWithExpression::operator<(const OrWithExpression & rhs) const
{
    return std::tie(this->or_function, this->expression) < std::tie(rhs.or_function, rhs.expression);
}

LogicalExpressionsOptimizer::LogicalExpressionsOptimizer(ASTSelectQuery * select_query_, const Settings & settings_)
    : select_query(select_query_)
    , settings(settings_)
{}

void LogicalExpressionsOptimizer::perform()
{
    if (select_query == nullptr)
        return;
    if (visited_nodes.count(select_query))
        return;

    size_t position = 0;
    for (auto & column : select_query->select_expression_list->children)
    {
        bool inserted = column_to_position.emplace(column.get(), position).second;

        /// Do not run, if AST was already converted to DAG.
        /// TODO This is temporary solution. We must completely eliminate conversion of AST to DAG.
        /// (see ExpressionAnalyzer::normalizeTree)
        if (!inserted)
            return;

        ++position;
    }

    collectDisjunctiveEqualityChains();

    for (auto & chain : disjunctive_equality_chains_map)
    {
        if (!mayOptimizeDisjunctiveEqualityChain(chain))
            continue;
        addInExpression(chain);

        auto & equalities = chain.second;
        equalities.is_processed = true;
        ++processed_count;
    }

    if (processed_count > 0)
    {
        cleanupOrExpressions();
        fixBrokenOrExpressions();
        reorderColumns();
    }
}

void LogicalExpressionsOptimizer::reorderColumns()
{
    auto & columns = select_query->select_expression_list->children;
    size_t cur_position = 0;

    while (cur_position < columns.size())
    {
        size_t expected_position = column_to_position.at(columns[cur_position].get());
        if (cur_position != expected_position)
            std::swap(columns[cur_position], columns[expected_position]);
        else
            ++cur_position;
    }
}

void LogicalExpressionsOptimizer::collectDisjunctiveEqualityChains()
{
    if (visited_nodes.count(select_query))
        return;

    using Edge = std::pair<IAST *, IAST *>;
    std::deque<Edge> to_visit;

    to_visit.emplace_back(nullptr, select_query);
    while (!to_visit.empty())
    {
        auto edge = to_visit.back();
        auto * from_node = edge.first;
        auto * to_node = edge.second;

        to_visit.pop_back();

        bool found_chain = false;

        auto * function = typeid_cast<ASTFunction *>(to_node);
        if ((function != nullptr) && (function->name == "or") && (function->children.size() == 1))
        {
            auto * expression_list = typeid_cast<ASTExpressionList *>(&*(function->children[0]));
            if (expression_list != nullptr)
            {
                /// The chain of elements of the OR expression.
                for (auto & child : expression_list->children)
                {
                    auto * equals = typeid_cast<ASTFunction *>(&*child);
                    if ((equals != nullptr) && (equals->name == "equals") && (equals->children.size() == 1))
                    {
                        auto * equals_expression_list = typeid_cast<ASTExpressionList *>(&*(equals->children[0]));
                        if ((equals_expression_list != nullptr) && (equals_expression_list->children.size() == 2))
                        {
                            /// Equality expr = xN.
                            auto * literal = typeid_cast<ASTLiteral *>(&*(equals_expression_list->children[1]));
                            if (literal != nullptr)
                            {
                                auto expr_lhs = equals_expression_list->children[0]->getTreeHash();
                                OrWithExpression or_with_expression{function, expr_lhs, function->tryGetAlias()};
                                disjunctive_equality_chains_map[or_with_expression].functions.push_back(equals);
                                found_chain = true;
                            }
                        }
                    }
                }
            }
        }

        visited_nodes.insert(to_node);

        if (found_chain)
        {
            if (from_node != nullptr)
            {
                auto res = or_parent_map.insert(std::make_pair(function, ParentNodes{from_node}));
                if (!res.second)
                    throw Exception(
                        "LogicalExpressionsOptimizer: parent node information is corrupted",
                        ErrorCodes::LOGICAL_ERROR);
            }
        }
        else
        {
            for (auto & child : to_node->children)
            {
                if (typeid_cast<ASTSelectQuery *>(child.get()) == nullptr)
                {
                    if (!visited_nodes.count(child.get()))
                        to_visit.push_back(Edge(to_node, &*child));
                    else
                    {
                        /// If the node is an OR function, update the information about its parents.
                        auto it = or_parent_map.find(&*child);
                        if (it != or_parent_map.end())
                        {
                            auto & parent_nodes = it->second;
                            parent_nodes.push_back(to_node);
                        }
                    }
                }
            }
        }
    }

    for (auto & chain : disjunctive_equality_chains_map)
    {
        auto & equalities = chain.second;
        auto & equality_functions = equalities.functions;
        std::sort(equality_functions.begin(), equality_functions.end());
    }
}

namespace
{
inline ASTs & getFunctionOperands(ASTFunction * or_function)
{
    auto * expression_list = static_cast<ASTExpressionList *>(&*(or_function->children[0]));
    return expression_list->children;
}

} // namespace

bool LogicalExpressionsOptimizer::mayOptimizeDisjunctiveEqualityChain(const DisjunctiveEqualityChain & chain) const
{
    const auto & equalities = chain.second;
    const auto & equality_functions = equalities.functions;

    /// We eliminate too short chains.
    if (equality_functions.size() < settings.optimize_min_equality_disjunction_chain_length)
        return false;

    /// We check that the right-hand sides of all equalities have the same type.
    auto & first_operands = getFunctionOperands(equality_functions[0]);
    auto * first_literal = static_cast<ASTLiteral *>(&*first_operands[1]);
    for (size_t i = 1; i < equality_functions.size(); ++i)
    {
        auto & operands = getFunctionOperands(equality_functions[i]);
        auto * literal = static_cast<ASTLiteral *>(&*operands[1]);

        if (literal->value.getType() != first_literal->value.getType())
            return false;
    }
    return true;
}

void LogicalExpressionsOptimizer::addInExpression(const DisjunctiveEqualityChain & chain)
{
    const auto & or_with_expression = chain.first;
    const auto & equalities = chain.second;
    const auto & equality_functions = equalities.functions;

    /// 1. Create a new IN expression based on information from the OR-chain.

    /// Construct a list of literals `x1, ..., xN` from the string `expr = x1 OR ... OR expr = xN`
    ASTPtr value_list = std::make_shared<ASTExpressionList>();
    for (auto * const function : equality_functions)
    {
        const auto & operands = getFunctionOperands(function);
        value_list->children.push_back(operands[1]);
    }

    /// Sort the literals so that they are specified in the same order in the IN expression.
    /// Otherwise, they would be specified in the order of the ASTLiteral addresses, which is nondeterministic.
    std::sort(
        value_list->children.begin(),
        value_list->children.end(),
        [](const DB::ASTPtr & lhs, const DB::ASTPtr & rhs) {
            const auto * const val_lhs = static_cast<const ASTLiteral *>(&*lhs);
            const auto * const val_rhs = static_cast<const ASTLiteral *>(&*rhs);
            return val_lhs->value < val_rhs->value;
        });

    /// Get the expression `expr` from the chain `expr = x1 OR ... OR expr = xN`
    ASTPtr equals_expr_lhs;
    {
        auto * function = equality_functions[0];
        const auto & operands = getFunctionOperands(function);
        equals_expr_lhs = operands[0];
    }

    auto tuple_function = std::make_shared<ASTFunction>();
    tuple_function->name = "tuple";
    tuple_function->arguments = value_list;
    tuple_function->children.push_back(tuple_function->arguments);

    ASTPtr expression_list = std::make_shared<ASTExpressionList>();
    expression_list->children.push_back(equals_expr_lhs);
    expression_list->children.push_back(tuple_function);

    /// Construct the expression `expr IN (x1, ..., xN)`
    auto in_function = std::make_shared<ASTFunction>();
    in_function->name = "in";
    in_function->arguments = expression_list;
    in_function->children.push_back(in_function->arguments);
    in_function->setAlias(or_with_expression.alias);

    /// 2. Insert the new IN expression.

    auto & operands = getFunctionOperands(or_with_expression.or_function);
    operands.push_back(in_function);
}

void LogicalExpressionsOptimizer::cleanupOrExpressions()
{
    /// Saves for each optimized OR-chain the iterator on the first element
    /// list of operands to be deleted.
    std::unordered_map<ASTFunction *, ASTs::iterator> garbage_map;

    /// Initialization.
    garbage_map.reserve(processed_count);
    for (const auto & chain : disjunctive_equality_chains_map)
    {
        if (!chain.second.is_processed)
            continue;

        const auto & or_with_expression = chain.first;
        auto & operands = getFunctionOperands(or_with_expression.or_function);
        garbage_map.emplace(or_with_expression.or_function, operands.end());
    }

    /// Collect garbage.
    for (const auto & chain : disjunctive_equality_chains_map)
    {
        const auto & equalities = chain.second;
        if (!equalities.is_processed)
            continue;

        const auto & or_with_expression = chain.first;
        auto & operands = getFunctionOperands(or_with_expression.or_function);
        const auto & equality_functions = equalities.functions;

        auto it = garbage_map.find(or_with_expression.or_function);
        if (it == garbage_map.end())
            throw Exception("LogicalExpressionsOptimizer: garbage map is corrupted", ErrorCodes::LOGICAL_ERROR);

        auto & first_erased = it->second;
        first_erased = std::remove_if(operands.begin(), first_erased, [&](const ASTPtr & operand) {
            return std::binary_search(equality_functions.begin(), equality_functions.end(), &*operand);
        });
    }

    /// Delete garbage.
    for (const auto & entry : garbage_map)
    {
        auto * function = entry.first;
        auto first_erased = entry.second;

        auto & operands = getFunctionOperands(function);
        operands.erase(first_erased, operands.end());
    }
}

void LogicalExpressionsOptimizer::fixBrokenOrExpressions()
{
    for (const auto & chain : disjunctive_equality_chains_map)
    {
        const auto & equalities = chain.second;
        if (!equalities.is_processed)
            continue;

        const auto & or_with_expression = chain.first;
        auto * or_function = or_with_expression.or_function;
        auto & operands = getFunctionOperands(or_with_expression.or_function);

        if (operands.size() == 1)
        {
            auto it = or_parent_map.find(or_function);
            if (it == or_parent_map.end())
                throw Exception(
                    "LogicalExpressionsOptimizer: parent node information is corrupted",
                    ErrorCodes::LOGICAL_ERROR);
            auto & parents = it->second;

            auto it2 = column_to_position.find(or_function);
            if (it2 != column_to_position.end())
            {
                size_t position = it2->second;
                bool inserted = column_to_position.emplace(operands[0].get(), position).second;
                if (!inserted)
                    throw Exception("LogicalExpressionsOptimizer: internal error", ErrorCodes::LOGICAL_ERROR);
                column_to_position.erase(it2);
            }

            for (auto & parent : parents)
            {
                parent->children.push_back(operands[0]);
                auto first_erased = std::remove_if(
                    parent->children.begin(),
                    parent->children.end(),
                    [or_function](const ASTPtr & ptr) { return ptr.get() == or_function; });

                parent->children.erase(first_erased, parent->children.end());
            }

            /// If the OR node was the root of the WHERE, or HAVING expression, then update this root.
            /// Due to the fact that we are dealing with a directed acyclic graph, we must check all cases.
            if (select_query->where_expression && (or_function == &*(select_query->where_expression)))
                select_query->where_expression = operands[0];
            if (select_query->having_expression && (or_function == &*(select_query->having_expression)))
                select_query->having_expression = operands[0];
        }
    }
}

} // namespace DB
