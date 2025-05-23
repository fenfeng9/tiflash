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

#pragma once

#include <Common/typeid_cast.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTNameTypePair.h>
#include <Parsers/CommonParsers.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/IParserBase.h>
#include <Poco/String.h>


namespace DB
{

/** A nested table. For example, Nested(UInt32 CounterID, FixedString(2) UserAgentMajor)
  */
class ParserNestedTable : public IParserBase
{
protected:
    const char * getName() const override { return "nested table"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


/** Parametric type or Storage. For example:
 *         FixedString(10) or
 *         Partitioned(Log, ChunkID) or
 *         Nested(UInt32 CounterID, FixedString(2) UserAgentMajor)
 * Result of parsing - ASTFunction with or without parameters.
 */
class ParserIdentifierWithParameters : public IParserBase
{
protected:
    const char * getName() const override { return "identifier with parameters"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


/** Data type or table engine, possibly with parameters. For example, UInt8 or see examples from ParserIdentifierWithParameters
  * Parse result is ASTFunction, with or without arguments.
  */
class ParserIdentifierWithOptionalParameters : public IParserBase
{
protected:
    const char * getName() const override { return "identifier with optional parameters"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

class ParserTypeInCastExpression : public IParserBase
{
protected:
    const char * getName() const override { return "type in cast expression"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


template <typename NameParser>
class IParserNameTypePair : public IParserBase
{
protected:
    const char * getName() const override { return "name and type pair"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

/** The name and type are separated by a space. For example, URL String. */
using ParserNameTypePair = IParserNameTypePair<ParserIdentifier>;
/** Name and type separated by a space. The name can contain a dot. For example, Hits.URL String. */
using ParserCompoundNameTypePair = IParserNameTypePair<ParserCompoundIdentifier>;

template <typename NameParser>
bool IParserNameTypePair<NameParser>::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    NameParser name_parser;
    ParserIdentifierWithOptionalParameters type_parser;

    ASTPtr name, type;
    if (name_parser.parse(pos, name, expected) && type_parser.parse(pos, type, expected))
    {
        auto name_type_pair = std::make_shared<ASTNameTypePair>();
        name_type_pair->name = typeid_cast<const ASTIdentifier &>(*name).name;
        name_type_pair->type = type;
        name_type_pair->children.push_back(type);
        node = name_type_pair;
        return true;
    }

    return false;
}

/** List of columns. */
class ParserNameTypePairList : public IParserBase
{
protected:
    const char * getName() const override { return "name and type pair list"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


template <typename NameParser>
class IParserColumnDeclaration : public IParserBase
{
protected:
    const char * getName() const override { return "column declaration"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

using ParserColumnDeclaration = IParserColumnDeclaration<ParserIdentifier>;
using ParserCompoundColumnDeclaration = IParserColumnDeclaration<ParserCompoundIdentifier>;

template <typename NameParser>
bool IParserColumnDeclaration<NameParser>::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    NameParser name_parser;
    ParserIdentifierWithOptionalParameters type_parser;
    ParserKeyword s_default{"DEFAULT"};
    ParserKeyword s_materialized{"MATERIALIZED"};
    ParserKeyword s_alias{"ALIAS"};
    ParserTernaryOperatorExpression expr_parser;

    /// mandatory column name
    ASTPtr name;
    if (!name_parser.parse(pos, name, expected))
        return false;

    /** column name should be followed by type name if it
      *    is not immediately followed by {DEFAULT, MATERIALIZED, ALIAS}
      */
    ASTPtr type;
    const auto fallback_pos = pos;
    if (!s_default.check(pos, expected) && !s_materialized.check(pos, expected) && !s_alias.check(pos, expected))
    {
        type_parser.parse(pos, type, expected);
    }
    else
        pos = fallback_pos;

    /// parse {DEFAULT, MATERIALIZED, ALIAS}
    String default_specifier;
    ASTPtr default_expression;
    Pos pos_before_specifier = pos;
    if (s_default.ignore(pos, expected) || s_materialized.ignore(pos, expected) || s_alias.ignore(pos, expected))
    {
        default_specifier = Poco::toUpper(std::string{pos_before_specifier->begin, pos_before_specifier->end});

        /// should be followed by an expression
        if (!expr_parser.parse(pos, default_expression, expected))
            return false;
    }
    else if (!type)
        return false; /// reject sole column name without type

    const auto column_declaration = std::make_shared<ASTColumnDeclaration>();
    node = column_declaration;
    column_declaration->name = typeid_cast<ASTIdentifier &>(*name).name;
    if (type)
    {
        column_declaration->type = type;
        column_declaration->children.push_back(std::move(type));
    }

    if (default_expression)
    {
        column_declaration->default_specifier = default_specifier;
        column_declaration->default_expression = default_expression;
        column_declaration->children.push_back(std::move(default_expression));
    }

    return true;
}

class ParserColumnDeclarationList : public IParserBase
{
protected:
    const char * getName() const override { return "column declaration list"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


/** ENGINE = name [PARTITION BY expr] [ORDER BY expr] [SAMPLE BY expr] [SETTINGS name = value, ...] */
class ParserStorage : public IParserBase
{
protected:
    const char * getName() const override { return "storage definition"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};


/** Query like this:
  * CREATE|ATTACH TABLE [IF NOT EXISTS] [db.]name
  * (
  *     name1 type1,
  *     name2 type2,
  *     ...
  * ) ENGINE = engine
  *
  * Or:
  * CREATE|ATTACH TABLE [IF NOT EXISTS] [db.]name AS [db2.]name2 [ENGINE = engine]
  *
  * Or:
  * CREATE|ATTACH DATABASE db [ENGINE = engine]
  */
class ParserCreateQuery : public IParserBase
{
protected:
    const char * getName() const override { return "CREATE TABLE or ATTACH TABLE query"; }
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

} // namespace DB
