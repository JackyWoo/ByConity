/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include <Columns/ColumnString.h>

#include <Columns/Collator.h>
#include <Columns/ColumnsCommon.h>
#include <Columns/ColumnCompressed.h>
#include <DataStreams/ColumnGathererStream.h>
#include <Common/Arena.h>
#include <Common/HashTable/Hash.h>
#include <Common/WeakHash.h>
#include <Common/assert_cast.h>
#include <Common/memcmpSmall.h>
#include <common/sort.h>
#include <common/unaligned.h>
#include <common/scope_guard.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int PARAMETER_OUT_OF_BOUND;
    extern const int SIZES_OF_COLUMNS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
}


ColumnString::ColumnString(const ColumnString & src)
    : COWHelper<IColumn, ColumnString>(src),
    offsets(src.offsets.begin(), src.offsets.end()),
    chars(src.chars.begin(), src.chars.end())
{
    if (!offsets.empty())
    {
        Offset last_offset = offsets.back();

        /// This will also prevent possible overflow in offset.
        if (chars.size() != last_offset)
            throw Exception("String offsets has data inconsistent with chars array", ErrorCodes::LOGICAL_ERROR);
    }
}


MutableColumnPtr ColumnString::cloneResized(size_t to_size) const
{
    auto res = ColumnString::create();

    if (to_size == 0)
        return res;

    size_t from_size = size();

    if (to_size <= from_size)
    {
        /// Just cut column.

        res->offsets.assign(offsets.begin(), offsets.begin() + to_size);
        res->chars.assign(chars.begin(), chars.begin() + offsets[to_size - 1]);
    }
    else
    {
        /// Copy column and append empty strings for extra elements.

        Offset offset = 0;
        if (from_size > 0)
        {
            res->offsets.assign(offsets.begin(), offsets.end());
            res->chars.assign(chars.begin(), chars.end());
            offset = offsets.back();
        }

        /// Empty strings are just zero terminating bytes.

        res->chars.resize_fill(res->chars.size() + to_size - from_size);

        res->offsets.resize(to_size);
        for (size_t i = from_size; i < to_size; ++i)
        {
            ++offset;
            res->offsets[i] = offset;
        }
    }

    return res;
}

void ColumnString::updateWeakHash32(WeakHash32 & hash) const
{
    auto s = offsets.size();

    if (hash.getData().size() != s)
        throw Exception("Size of WeakHash32 does not match size of column: column size is " + std::to_string(s) +
                        ", hash size is " + std::to_string(hash.getData().size()), ErrorCodes::LOGICAL_ERROR);

    const UInt8 * pos = chars.data();
    UInt32 * hash_data = hash.getData().data();
    Offset prev_offset = 0;

    for (const auto & offset : offsets)
    {
        auto str_size = offset - prev_offset;
        /// Skip last zero byte.
        *hash_data = ::updateWeakHash32(pos, str_size - 1, *hash_data);

        pos += str_size;
        prev_offset = offset;
        ++hash_data;
    }
}


void ColumnString::insertRangeFrom(const IColumn & src, size_t start, size_t length)
{
    if (length == 0)
        return;

    const ColumnString & src_concrete = assert_cast<const ColumnString &>(src);

    if (start + length > src_concrete.offsets.size())
        throw Exception("Parameter out of bound in IColumnString::insertRangeFrom method.",
            ErrorCodes::PARAMETER_OUT_OF_BOUND);

    size_t nested_offset = src_concrete.offsetAt(start);
    size_t nested_length = src_concrete.offsets[start + length - 1] - nested_offset;

    size_t old_chars_size = chars.size();
    chars.resize(old_chars_size + nested_length);
    memcpy(&chars[old_chars_size], &src_concrete.chars[nested_offset], nested_length);

    if (start == 0 && offsets.empty())
    {
        offsets.assign(src_concrete.offsets.begin(), src_concrete.offsets.begin() + length);
    }
    else
    {
        size_t old_size = offsets.size();
        size_t prev_max_offset = offsets.back();    /// -1th index is Ok, see PaddedPODArray
        offsets.resize(old_size + length);

        for (size_t i = 0; i < length; ++i)
            offsets[old_size + i] = src_concrete.offsets[start + i] - nested_offset + prev_max_offset;
    }
}

void ColumnString::insertRangeSelective(const IColumn & src, const Selector & selector, size_t selector_start, size_t length)
{
    const ColumnString & src_concrete = static_cast<const ColumnString &>(src);
    const Offsets & src_offsets = src_concrete.getOffsets();
    auto * src_data_start = src_concrete.chars.data();

    Offsets & cur_offsets = getOffsets();

    if (length == 0)
        return;

    size_t old_offset_size = cur_offsets.size();
    cur_offsets.resize(old_offset_size + length);

    size_t old_chars_size = chars.size();
    size_t new_chars_size = old_chars_size;
    for (size_t i = 0; i < length; ++i)
    {
        new_chars_size += src_concrete.sizeAt(selector[selector_start + i]);
    }
    chars.resize(new_chars_size);

    size_t cur_offset = cur_offsets[old_offset_size - 1];

    auto * cur_chars_start = chars.data(); // realloc memory is not allowed in the following
    for (size_t i = 0; i < length; ++i)
    {
        size_t src_pos = selector[selector_start + i];
        size_t offset = src_offsets[src_pos - 1];
        const size_t size_to_append = src_offsets[src_pos] - offset; /// -1th index is Ok, see PaddedPODArray.

        memcpySmallAllowReadWriteOverflow15(cur_chars_start + cur_offset, src_data_start + offset, size_to_append);
        cur_offset += size_to_append;
        cur_offsets[old_offset_size + i] = cur_offset;
    }
}

ColumnPtr ColumnString::filter(const Filter & filt, ssize_t result_size_hint) const
{
    if (offsets.empty())
        return ColumnString::create();

    auto res = ColumnString::create();

    Chars & res_chars = res->chars;
    Offsets & res_offsets = res->offsets;

    filterArraysImpl<UInt8>(chars, offsets, res_chars, res_offsets, filt, result_size_hint);
    return res;
}


ColumnPtr ColumnString::permute(const Permutation & perm, size_t limit) const
{
    size_t size = offsets.size();

    if (limit == 0)
        limit = size;
    else
        limit = std::min(size, limit);

    if (perm.size() < limit)
        throw Exception("Size of permutation is less than required.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

    if (limit == 0)
        return ColumnString::create();

    auto res = ColumnString::create();

    Chars & res_chars = res->chars;
    Offsets & res_offsets = res->offsets;

    if (limit == size)
        res_chars.resize(chars.size());
    else
    {
        size_t new_chars_size = 0;
        for (size_t i = 0; i < limit; ++i)
            new_chars_size += sizeAt(perm[i]);
        res_chars.resize(new_chars_size);
    }

    res_offsets.resize(limit);

    Offset current_new_offset = 0;

    for (size_t i = 0; i < limit; ++i)
    {
        size_t j = perm[i];
        size_t string_offset = offsets[j - 1];
        size_t string_size = offsets[j] - string_offset;

        memcpySmallAllowReadWriteOverflow15(&res_chars[current_new_offset], &chars[string_offset], string_size);

        current_new_offset += string_size;
        res_offsets[i] = current_new_offset;
    }

    return res;
}


StringRef ColumnString::serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const
{
    size_t string_size = sizeAt(n);
    size_t offset = offsetAt(n);

    StringRef res;
    res.size = sizeof(string_size) + string_size;
    char * pos = arena.allocContinue(res.size, begin);
    memcpy(pos, &string_size, sizeof(string_size));
    memcpy(pos + sizeof(string_size), &chars[offset], string_size);
    res.data = pos;

    return res;
}

const char * ColumnString::deserializeAndInsertFromArena(const char * pos)
{
    const size_t string_size = unalignedLoad<size_t>(pos);
    pos += sizeof(string_size);

    const size_t old_size = chars.size();
    const size_t new_size = old_size + string_size;
    chars.resize(new_size);
    memcpy(chars.data() + old_size, pos, string_size);

    offsets.push_back(new_size);
    return pos + string_size;
}

const char * ColumnString::skipSerializedInArena(const char * pos) const
{
    const size_t string_size = unalignedLoad<size_t>(pos);
    pos += sizeof(string_size);
    return pos + string_size;
}

ColumnPtr ColumnString::index(const IColumn & indexes, size_t limit) const
{
    return selectIndexImpl(*this, indexes, limit);
}

template <typename Type>
ColumnPtr ColumnString::indexImpl(const PaddedPODArray<Type> & indexes, size_t limit) const
{
    if (limit == 0)
        return ColumnString::create();

    auto res = ColumnString::create();

    Chars & res_chars = res->chars;
    Offsets & res_offsets = res->offsets;

    size_t new_chars_size = 0;
    for (size_t i = 0; i < limit; ++i)
        new_chars_size += sizeAt(indexes[i]);
    res_chars.resize(new_chars_size);

    res_offsets.resize(limit);

    Offset current_new_offset = 0;

    for (size_t i = 0; i < limit; ++i)
    {
        size_t j = indexes[i];
        size_t string_offset = offsets[j - 1];
        size_t string_size = offsets[j] - string_offset;

        memcpySmallAllowReadWriteOverflow15(&res_chars[current_new_offset], &chars[string_offset], string_size);

        current_new_offset += string_size;
        res_offsets[i] = current_new_offset;
    }

    return res;
}

void ColumnString::compareColumn(
    const IColumn & rhs, size_t rhs_row_num,
    PaddedPODArray<UInt64> * row_indexes, PaddedPODArray<Int8> & compare_results,
    int direction, int nan_direction_hint) const
{
    return doCompareColumn<ColumnString>(assert_cast<const ColumnString &>(rhs), rhs_row_num, row_indexes,
                                         compare_results, direction, nan_direction_hint);
}

bool ColumnString::hasEqualValues() const
{
    return hasEqualValuesImpl<ColumnString>();
}

struct ColumnString::ComparatorBase
{
    const ColumnString & parent;

    explicit ComparatorBase(const ColumnString & parent_)
        : parent(parent_)
    {
    }

    ALWAYS_INLINE int compare(size_t lhs, size_t rhs) const
    {
        int res = memcmpSmallAllowOverflow15(
            parent.chars.data() + parent.offsetAt(lhs), parent.sizeAt(lhs) - 1,
            parent.chars.data() + parent.offsetAt(rhs), parent.sizeAt(rhs) - 1);

        return res;
    }
};

struct ColumnString::ComparatorCollationBase
{
    const ColumnString & parent;
    const Collator * collator;

    explicit ComparatorCollationBase(const ColumnString & parent_, const Collator * collator_)
        : parent(parent_), collator(collator_)
    {
    }

    ALWAYS_INLINE int compare(size_t lhs, size_t rhs) const
    {
        int res = collator->compare(
            reinterpret_cast<const char *>(&parent.chars[parent.offsetAt(lhs)]), parent.sizeAt(lhs),
            reinterpret_cast<const char *>(&parent.chars[parent.offsetAt(rhs)]), parent.sizeAt(rhs));

        return res;
    }
};

void ColumnString::getPermutation(PermutationSortDirection direction, PermutationSortStability stability,
                                size_t limit, int /*nan_direction_hint*/, Permutation & res) const
{
    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
        getPermutationImpl(limit, res, ComparatorAscendingUnstable(*this), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
        getPermutationImpl(limit, res, ComparatorAscendingStable(*this), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
        getPermutationImpl(limit, res, ComparatorDescendingUnstable(*this), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Stable)
        getPermutationImpl(limit, res, ComparatorDescendingStable(*this), DefaultSort(), DefaultPartialSort());
}

void ColumnString::updatePermutation(PermutationSortDirection direction, PermutationSortStability stability,
                                size_t limit, int /*nan_direction_hint*/, Permutation & res, EqualRanges & equal_ranges) const
{
    auto comparator_equal = ComparatorEqual(*this);

    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
        updatePermutationImpl(limit, res, equal_ranges, ComparatorAscendingUnstable(*this), comparator_equal, DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
        updatePermutationImpl(limit, res, equal_ranges, ComparatorAscendingStable(*this), comparator_equal, DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
        updatePermutationImpl(limit, res, equal_ranges, ComparatorDescendingUnstable(*this), comparator_equal, DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Stable)
        updatePermutationImpl(limit, res, equal_ranges, ComparatorDescendingStable(*this), comparator_equal, DefaultSort(), DefaultPartialSort());
}

void ColumnString::getPermutationWithCollation(const Collator & collator, PermutationSortDirection direction, PermutationSortStability stability,
                                size_t limit, int /*nan_direction_hint*/, Permutation & res) const
{
    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
        getPermutationImpl(limit, res, ComparatorCollationAscendingUnstable(*this, &collator), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
        getPermutationImpl(limit, res, ComparatorCollationAscendingStable(*this, &collator), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
        getPermutationImpl(limit, res, ComparatorCollationDescendingUnstable(*this, &collator), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Stable)
        getPermutationImpl(limit, res, ComparatorCollationDescendingStable(*this, &collator), DefaultSort(), DefaultPartialSort());
}

void ColumnString::updatePermutationWithCollation(const Collator & collator, PermutationSortDirection direction, PermutationSortStability stability,
                                size_t limit, int /*nan_direction_hint*/, Permutation & res, EqualRanges & equal_ranges) const
{
    auto comparator_equal = ComparatorCollationEqual(*this, &collator);

    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
        updatePermutationImpl(
            limit,
            res,
            equal_ranges,
            ComparatorCollationAscendingUnstable(*this, &collator),
            comparator_equal,
            DefaultSort(),
            DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
        updatePermutationImpl(
            limit,
            res,
            equal_ranges,
            ComparatorCollationAscendingStable(*this, &collator),
            comparator_equal,
            DefaultSort(),
            DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
        updatePermutationImpl(
            limit,
            res,
            equal_ranges,
            ComparatorCollationDescendingUnstable(*this, &collator),
            comparator_equal,
            DefaultSort(),
            DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Stable)
        updatePermutationImpl(
            limit,
            res,
            equal_ranges,
            ComparatorCollationDescendingStable(*this, &collator),
            comparator_equal,
            DefaultSort(),
            DefaultPartialSort());
}

ColumnPtr ColumnString::replicate(const Offsets & replicate_offsets) const
{
    size_t col_size = size();
    if (col_size != replicate_offsets.size())
        throw Exception("Size of offsets doesn't match size of column.", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);

    auto res = ColumnString::create();

    if (0 == col_size)
        return res;

    Chars & res_chars = res->chars;
    Offsets & res_offsets = res->offsets;
    res_chars.reserve(chars.size() / col_size * replicate_offsets.back());
    res_offsets.reserve(replicate_offsets.back());

    Offset prev_replicate_offset = 0;
    Offset prev_string_offset = 0;
    Offset current_new_offset = 0;

    for (size_t i = 0; i < col_size; ++i)
    {
        size_t size_to_replicate = replicate_offsets[i] - prev_replicate_offset;
        size_t string_size = offsets[i] - prev_string_offset;

        for (size_t j = 0; j < size_to_replicate; ++j)
        {
            current_new_offset += string_size;
            res_offsets.push_back(current_new_offset);

            res_chars.resize(res_chars.size() + string_size);
            memcpySmallAllowReadWriteOverflow15(
                &res_chars[res_chars.size() - string_size], &chars[prev_string_offset], string_size);
        }

        prev_replicate_offset = replicate_offsets[i];
        prev_string_offset = offsets[i];
    }

    return res;
}


void ColumnString::gather(ColumnGathererStream & gatherer)
{
    gatherer.gather(*this);
}

void ColumnString::reserve(size_t n)
{
    offsets.reserve(n);
}


void ColumnString::getExtremes(Field & min, Field & max) const
{
    min = String();
    max = String();

    size_t col_size = size();

    if (col_size == 0)
        return;

    size_t min_idx = 0;
    size_t max_idx = 0;

    ComparatorBase cmp_op(*this);

    for (size_t i = 1; i < col_size; ++i)
    {
        if (cmp_op.compare(i, min_idx) < 0)
            min_idx = i;
        else if (cmp_op.compare(max_idx, i) < 0)
            max_idx = i;
    }

    get(min_idx, min);
    get(max_idx, max);
}

ColumnPtr ColumnString::compress() const
{
    size_t source_chars_size = chars.size();
    size_t source_offsets_size = offsets.size() * sizeof(Offset);

    /// Don't compress small blocks.
    if (source_chars_size < 4096) /// A wild guess.
        return ColumnCompressed::wrap(this->getPtr());

    auto chars_compressed = ColumnCompressed::compressBuffer(chars.data(), source_chars_size, false);

    /// Return original column if not compressible.
    if (!chars_compressed)
        return ColumnCompressed::wrap(this->getPtr());

    auto offsets_compressed = ColumnCompressed::compressBuffer(offsets.data(), source_offsets_size, true);

    return ColumnCompressed::create(offsets.size(), chars_compressed->size() + offsets_compressed->size(),
        [
            chars_compressed = std::move(chars_compressed),
            offsets_compressed = std::move(offsets_compressed),
            source_chars_size,
            source_offsets_elements = offsets.size()
        ]
        {
            auto res = ColumnString::create();

            res->getChars().resize(source_chars_size);
            res->getOffsets().resize(source_offsets_elements);

            ColumnCompressed::decompressBuffer(
                chars_compressed->data(), res->getChars().data(), chars_compressed->size(), source_chars_size);

            ColumnCompressed::decompressBuffer(
                offsets_compressed->data(), res->getOffsets().data(), offsets_compressed->size(), source_offsets_elements * sizeof(Offset));

            return res;
        });
}


int ColumnString::compareAtWithCollation(size_t n, size_t m, const IColumn & rhs_, int, const Collator & collator) const
{
    const ColumnString & rhs = assert_cast<const ColumnString &>(rhs_);

    return collator.compare(
        reinterpret_cast<const char *>(&chars[offsetAt(n)]), sizeAt(n),
        reinterpret_cast<const char *>(&rhs.chars[rhs.offsetAt(m)]), rhs.sizeAt(m));
}

void ColumnString::protect()
{
    getChars().protect();
    getOffsets().protect();
}

void ColumnString::validate() const
{
    if (!offsets.empty() && offsets.back() != chars.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ColumnString validation failed: size mismatch (internal logical error) {} != {}", offsets.back(), chars.size());
}

}
