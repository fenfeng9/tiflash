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

#include <Core/Block.h>


namespace DB
{
/** Column, that is just group of few another columns.
  *
  * For constant Tuples, see ColumnConst.
  * Mixed constant/non-constant columns is prohibited in tuple
  *  for implementation simplicity.
  */
class ColumnTuple final : public COWPtrHelper<IColumn, ColumnTuple>
{
private:
    friend class COWPtrHelper<IColumn, ColumnTuple>;

    Columns columns;

    template <bool positive>
    struct Less;

    explicit ColumnTuple(MutableColumns && columns);
    ColumnTuple(const ColumnTuple &) = default;

public:
    /** Create immutable column using immutable arguments. This arguments may be shared with other columns.
      * Use IColumn::mutate in order to make mutable column and mutate shared nested columns.
      */
    using Base = COWPtrHelper<IColumn, ColumnTuple>;
    static Ptr create(const Columns & columns);

    template <typename Arg, typename = typename std::enable_if<std::is_rvalue_reference<Arg &&>::value>::type>
    static MutablePtr create(Arg && arg)
    {
        return Base::create(std::forward<Arg>(arg));
    }

    std::string getName() const override;
    const char * getFamilyName() const override { return "Tuple"; }

    MutableColumnPtr cloneEmpty() const override;

    size_t size() const override { return columns.at(0)->size(); }

    Field operator[](size_t n) const override;
    void get(size_t n, Field & res) const override;

    StringRef getDataAt(size_t n) const override;
    void insertData(const char * pos, size_t length) override;
    void insert(const Field & x) override;
    void insertFrom(const IColumn & src_, size_t n) override;

    void insertManyFrom(const IColumn & src_, size_t n, size_t length) override
    {
        for (size_t i = 0; i < length; ++i)
            insertFrom(src_, n);
    }

    void insertSelectiveRangeFrom(const IColumn & src_, const Offsets & selective_offsets, size_t start, size_t length)
        override
    {
        RUNTIME_CHECK(selective_offsets.size() >= start + length);
        for (size_t i = start; i < start + length; ++i)
            insertFrom(src_, selective_offsets[i]);
    }

    void insertDefault() override;

    void insertManyDefaults(size_t length) override
    {
        for (size_t i = 0; i < length; ++i)
            insertDefault();
    }

    void popBack(size_t n) override;
    StringRef serializeValueIntoArena(
        size_t n,
        Arena & arena,
        char const *& begin,
        const TiDB::TiDBCollatorPtr &,
        String &) const override;
    const char * deserializeAndInsertFromArena(const char * pos, const TiDB::TiDBCollatorPtr &) override;

    size_t serializeByteSize() const override
    {
        size_t res = 0;
        for (const auto & column : columns)
            res += column->serializeByteSize();
        return res;
    }

    void countSerializeByteSize(PaddedPODArray<size_t> & byte_size) const override
    {
        for (const auto & column : columns)
            column->countSerializeByteSize(byte_size);
    }
    void countSerializeByteSizeForCmp(
        PaddedPODArray<size_t> & byte_size,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator) const override
    {
        for (const auto & column : columns)
            column->countSerializeByteSizeForCmp(byte_size, nullmap, collator);
    }

    void countSerializeByteSizeForColumnArray(
        PaddedPODArray<size_t> & byte_size,
        const IColumn::Offsets & array_offsets) const override
    {
        for (const auto & column : columns)
            column->countSerializeByteSizeForColumnArray(byte_size, array_offsets);
    }
    void countSerializeByteSizeForCmpColumnArray(
        PaddedPODArray<size_t> & byte_size,
        const IColumn::Offsets & array_offsets,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator) const override
    {
        for (const auto & column : columns)
            column->countSerializeByteSizeForCmpColumnArray(byte_size, array_offsets, nullmap, collator);
    }

    void serializeToPos(PaddedPODArray<char *> & pos, size_t start, size_t length, bool has_null) const override
    {
        for (const auto & column : columns)
            column->serializeToPos(pos, start, length, has_null);
    }
    void serializeToPosForCmp(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const NullMap * nullmap,
        const TiDB::TiDBCollatorPtr & collator,
        String * sort_key_container) const override
    {
        for (const auto & column : columns)
            column->serializeToPosForCmp(pos, start, length, has_null, nullmap, collator, sort_key_container);
    }

    void serializeToPosForColumnArray(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const IColumn::Offsets & array_offsets) const override
    {
        for (const auto & column : columns)
            column->serializeToPosForColumnArray(pos, start, length, has_null, array_offsets);
    }
    void serializeToPosForCmpColumnArray(
        PaddedPODArray<char *> & pos,
        size_t start,
        size_t length,
        bool has_null,
        const NullMap * nullmap,
        const IColumn::Offsets & array_offsets,
        const TiDB::TiDBCollatorPtr & collator,
        String * sort_key_container) const override
    {
        for (const auto & column : columns)
            column->serializeToPosForCmpColumnArray(
                pos,
                start,
                length,
                has_null,
                nullmap,
                array_offsets,
                collator,
                sort_key_container);
    }

    void deserializeAndInsertFromPos(PaddedPODArray<char *> & pos, bool use_nt_align_buffer) override
    {
        for (auto & column : columns)
            column->assumeMutableRef().deserializeAndInsertFromPos(pos, use_nt_align_buffer);
    }

    void deserializeAndInsertFromPosForColumnArray(
        PaddedPODArray<char *> & pos,
        const IColumn::Offsets & array_offsets,
        bool use_nt_align_buffer) override
    {
        for (auto & column : columns)
            column->assumeMutableRef().deserializeAndInsertFromPosForColumnArray(
                pos,
                array_offsets,
                use_nt_align_buffer);
    }

    void flushNTAlignBuffer() override
    {
        for (auto & column : columns)
            column->assumeMutableRef().flushNTAlignBuffer();
    }

    void deserializeAndAdvancePos(PaddedPODArray<char *> & pos) const override
    {
        for (const auto & column : columns)
            column->deserializeAndAdvancePos(pos);
    }

    void deserializeAndAdvancePosForColumnArray(PaddedPODArray<char *> & pos, const IColumn::Offsets & array_offsets)
        const override
    {
        for (const auto & column : columns)
            column->deserializeAndAdvancePosForColumnArray(pos, array_offsets);
    }

    void updateHashWithValue(size_t n, SipHash & hash, const TiDB::TiDBCollatorPtr &, String &) const override;
    void updateHashWithValues(IColumn::HashValues & hash_values, const TiDB::TiDBCollatorPtr &, String &)
        const override;
    void updateWeakHash32(WeakHash32 & hash, const TiDB::TiDBCollatorPtr &, String &) const override;
    void updateWeakHash32(WeakHash32 & hash, const TiDB::TiDBCollatorPtr &, String &, const BlockSelective & selective)
        const override;
    void insertRangeFrom(const IColumn & src, size_t start, size_t length) override;
    ColumnPtr filter(const Filter & filt, ssize_t result_size_hint) const override;
    ColumnPtr permute(const Permutation & perm, size_t limit) const override;
    ColumnPtr replicateRange(size_t start_row, size_t end_row, const IColumn::Offsets & offsets) const override;

    MutableColumns scatter(ColumnIndex num_columns, const Selector & selector) const override;
    MutableColumns scatter(ColumnIndex num_columns, const Selector & selector, const BlockSelective & selective)
        const override;
    template <bool selective_block>
    MutableColumns scatterImplForColumnTuple(
        ColumnIndex num_columns,
        const Selector & selector,
        const BlockSelective & selective) const;

    void scatterTo(ScatterColumns & scatterColumns, const Selector & selector) const override;
    void scatterTo(ScatterColumns & scatterColumns, const Selector & selector, const BlockSelective & selective)
        const override;
    template <bool selective_block>
    void scatterToImplForColumnTuple(
        ScatterColumns & scatterColumns,
        const Selector & selector,
        const BlockSelective & selective) const;

    void gather(ColumnGathererStream & gatherer_stream) override;
    int compareAt(size_t n, size_t m, const IColumn & rhs, int nan_direction_hint) const override;
    void getExtremes(Field & min, Field & max) const override;
    void getPermutation(bool reverse, size_t limit, int nan_direction_hint, Permutation & res) const override;
    void reserve(size_t n) override;
    void reserveAlign(size_t n, size_t alignment) override;
    size_t byteSize() const override;
    size_t byteSize(size_t offset, size_t limit) const override;
    size_t allocatedBytes() const override;
    void forEachSubcolumn(ColumnCallback callback) override;

    size_t tupleSize() const { return columns.size(); }

    const IColumn & getColumn(size_t idx) const { return *columns[idx]; }
    IColumn & getColumn(size_t idx) { return columns[idx]->assumeMutableRef(); }

    const Columns & getColumns() const { return columns; }

    const ColumnPtr & getColumnPtr(size_t idx) const { return columns[idx]; }
};


} // namespace DB
