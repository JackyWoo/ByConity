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

#include <Storages/MergeTree/MergeTreeReverseSelectProcessor.h>
#include <Storages/MergeTree/MergeTreeBaseSelectProcessor.h>
#include <Storages/MergeTree/IMergeTreeReader.h>
#include <Interpreters/Context.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int MEMORY_LIMIT_EXCEEDED;
}

MergeTreeReverseSelectProcessor::MergeTreeReverseSelectProcessor(
    const MergeTreeMetaBase & storage_,
    const StorageMetadataPtr & metadata_snapshot_,
    const MergeTreeMetaBase::DataPartPtr & owned_data_part_,
    ImmutableDeleteBitmapPtr delete_bitmap_,
    UInt64 max_block_size_rows_,
    size_t preferred_block_size_bytes_,
    size_t preferred_max_column_in_block_size_bytes_,
    Names required_columns_,
    MarkRanges mark_ranges_,
    bool use_uncompressed_cache_,
    const SelectQueryInfo & query_info_,
    ExpressionActionsSettings actions_settings,
    bool check_columns_,
    const MergeTreeReaderSettings & reader_settings_,
    const Names & virt_column_names_,
    size_t part_index_in_query_,
    bool quiet)
    :
    MergeTreeBaseSelectProcessor{
        metadata_snapshot_->getSampleBlockForColumns(required_columns_, storage_.getVirtuals(), storage_.getStorageID()),
        storage_, metadata_snapshot_, query_info_, std::move(actions_settings), max_block_size_rows_,
        preferred_block_size_bytes_, preferred_max_column_in_block_size_bytes_,
        reader_settings_, use_uncompressed_cache_, virt_column_names_},
    required_columns{std::move(required_columns_)},
    data_part{owned_data_part_},
    delete_bitmap{std::move(delete_bitmap_)},
    all_mark_ranges(std::move(mark_ranges_)),
    part_index_in_query(part_index_in_query_),
    path(data_part->getFullRelativePath()),
    check_columns(check_columns_)
{
    /// Let's estimate total number of rows for progress bar.
    for (const auto & range : all_mark_ranges)
        total_marks_count += range.end - range.begin;

    size_t total_rows = data_part->index_granularity.getRowsCountInRanges(all_mark_ranges);

    if (!quiet)
        LOG_DEBUG(log, "Reading {} ranges in reverse order from part {}, approx. {} rows starting from {}",
            all_mark_ranges.size(), data_part->name, total_rows,
            data_part->index_granularity.getMarkStartingRow(all_mark_ranges.front().begin));

    addTotalRowsApprox(total_rows);
    ordered_names = header_without_virtual_columns.getNames();
}

bool MergeTreeReverseSelectProcessor::getNewTask()
try
{
    if ((chunks.empty() && all_mark_ranges.empty()) || total_marks_count == 0)
    {
        finish();
        return false;
    }

    /// We have some blocks to return in buffer.
    /// Return true to continue reading, but actually don't create a task.
    if (all_mark_ranges.empty())
        return true;

    task_columns = getReadTaskColumns(storage, metadata_snapshot, data_part, required_columns, prewhere_info, index_context, check_columns);

    /// will be used to distinguish between PREWHERE and WHERE columns when applying filter
    const auto & column_names = task_columns.columns.getNames();
    column_name_set = NameSet{column_names.begin(), column_names.end()};

    /// Read ranges from right to left.
    MarkRanges mark_ranges_for_task = { all_mark_ranges.back() };
    all_mark_ranges.pop_back();

    auto size_predictor = (preferred_block_size_bytes == 0)
        ? nullptr
        : std::make_unique<MergeTreeBlockSizePredictor>(data_part, ordered_names, metadata_snapshot->getSampleBlock());

    task = std::make_unique<MergeTreeReadTask>(
        data_part, delete_bitmap, mark_ranges_for_task, part_index_in_query, ordered_names, column_name_set,
        task_columns, prewhere_info && prewhere_info->remove_prewhere_column,
        task_columns.should_reorder, std::move(size_predictor));

    if (!reader)
        initializeReaders(mark_ranges_for_task);

    return true;
}
catch (...)
{
    /// Suspicion of the broken part. A part is added to the queue for verification.
    if (getCurrentExceptionCode() != ErrorCodes::MEMORY_LIMIT_EXCEEDED)
        storage.reportBrokenPart(data_part->name);
    throw;
}

Chunk MergeTreeReverseSelectProcessor::readFromPart()
{
    Chunk res;

    if (!chunks.empty())
    {
        res = std::move(chunks.back());
        chunks.pop_back();
        return res;
    }

    if (!task->range_reader.isInitialized())
        initializeRangeReaders(*task);

    while (!task->isFinished())
    {
        Chunk chunk = readFromPartImpl();
        chunks.push_back(std::move(chunk));
    }

    if (chunks.empty())
        return {};

    res = std::move(chunks.back());
    chunks.pop_back();

    return res;
}

void MergeTreeReverseSelectProcessor::finish()
{
    /** Close the files (before destroying the object).
    * When many sources are created, but simultaneously reading only a few of them,
    * buffers don't waste memory.
    */
    reader.reset();
    pre_reader.reset();
    data_part.reset();
    index_executor.reset();
    pre_index_executor.reset();
}

MergeTreeReverseSelectProcessor::~MergeTreeReverseSelectProcessor() = default;

}
