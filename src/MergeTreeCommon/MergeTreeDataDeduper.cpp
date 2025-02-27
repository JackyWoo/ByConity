/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <IO/Operators.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/Context.h>
#include <MergeTreeCommon/MergeTreeDataDeduper.h>
#include <MergeTreeCommon/MergeTreeMetaBase.h>
#include <Storages/IndexFile/IndexFileMergeIterator.h>
#include <Storages/UniqueKeyIndex.h>
#include <Common/Coding.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

using IndexFileIteratorPtr = std::unique_ptr<IndexFile::Iterator>;
using IndexFileIterators = std::vector<IndexFileIteratorPtr>;

MergeTreeDataDeduper::MergeTreeDataDeduper(const MergeTreeMetaBase & data_, ContextPtr context_)
    : data(data_), context(context_), log(&Poco::Logger::get(data_.getLogName() + " (Deduper)"))
{
    if (data.merging_params.hasExplicitVersionColumn())
        version_mode = VersionMode::ExplicitVersion;
    else if (data.merging_params.partition_value_as_version)
        version_mode = VersionMode::PartitionValueAsVersion;
    else
        version_mode = VersionMode::NoVersion;
}

namespace
{

    bool DecodeRowid(Slice & input, UInt32 & rowid)
    {
        return GetVarint32(&input, &rowid);
    }

    bool DecodeVersion(Slice & input, UInt64 & version)
    {
        if (input.size() >= sizeof(UInt64))
        {
            version = DecodeFixed64(input.data());
            input.remove_prefix(sizeof(UInt64));
            return true;
        }
        return false;
    }

    struct DeleteInfo
    {
        /// If false, it means this row is insert operation with delete info. Otherwise, it means this row is just delete row.
        bool just_delete_row;
        UInt64 delete_version;
        DeleteInfo(bool just_delete_row_, UInt64 delete_version_) : just_delete_row(just_delete_row_), delete_version(delete_version_) { }
    };
    using DeleteInfoPtr = std::shared_ptr<DeleteInfo>;

    struct RowPos
    {
        RowPos() : child(0), rowid(0), version(0), delete_info(nullptr) { }
        // RowPos(UInt32 child_, UInt32 rowid_, UInt64 version_) : child(child_), rowid(rowid_), version(version_), delete_info(nullptr) { }

        UInt32 child; /// index of child iterator
        UInt32 rowid; /// row number
        UInt64 version;
        DeleteInfoPtr delete_info;
    };

    RowPos decodeCurrentRowPos(
        const IndexFile::IndexFileMergeIterator & iter,
        MergeTreeDataDeduper::VersionMode version_mode,
        const IMergeTreeDataPartsVector & child_parts, // only for exception msg
        const std::vector<UInt64> & child_implicit_versions,
        const ImmutableDeleteBitmapVector & delete_flag_bitmaps = {})
    {
        RowPos res;
        res.child = iter.child_index();
        Slice value = iter.value();
        if (!DecodeRowid(value, res.rowid))
            throw Exception("Can't decode row number from part " + child_parts[res.child]->name, ErrorCodes::CORRUPTED_DATA);
        res.version = child_implicit_versions[res.child];
        if (version_mode == MergeTreeDataDeduper::VersionMode::ExplicitVersion)
        {
            if (!DecodeVersion(value, res.version))
                throw Exception("Can't decode version from part " + child_parts[res.child]->name, ErrorCodes::CORRUPTED_DATA);
        }

        /// Check whether it's a delete row
        if (res.child < delete_flag_bitmaps.size() && delete_flag_bitmaps[res.child] && delete_flag_bitmaps[res.child]->contains(res.rowid))
            res.delete_info = std::make_shared<DeleteInfo>(/*just_delete_row*/ true, res.version);
        return res;
    }

    using DeleteCallback = std::function<void(const RowPos &)>;

    /// TODO(GDY) doc
    class ReplacingSortedKeysIterator
{
    public:
        ReplacingSortedKeysIterator(
            const IndexFile::Comparator * comparator_,
            const IMergeTreeDataPartsVector & parts_,
            std::vector<std::unique_ptr<IndexFile::Iterator>> child_iters_,
            DeleteCallback delete_cb_,
            MergeTreeDataDeduper::VersionMode version_mode_,
            const ImmutableDeleteBitmapVector & delete_flag_bitmaps_ = {})
            : comparator(comparator_)
            , parts(parts_)
            , iter(comparator, std::move(child_iters_))
            , delete_cb(delete_cb_)
            , version_mode(version_mode_)
            , part_implicit_versions(parts.size(), 0)
            , delete_flag_bitmaps(delete_flag_bitmaps_)
        {
            if (version_mode == MergeTreeDataDeduper::VersionMode::PartitionValueAsVersion)
            {
                for (size_t i = 0; i < parts.size(); ++i)
                    part_implicit_versions[i] = parts[i]->getVersionFromPartition();
            }
        }

        bool Valid() const { return valid; }

        IndexFile::Status status() const { return iter.status(); }

        void SeekToFirst()
        {
            iter.SeekToFirst();
            MoveToNextKey();
        }

        /// REQUIRES: Valid()
        void Next()
        {
            assert(Valid());
            MoveToNextKey();
        }

        /// REQUIRES: Valid()
        const String & CurrentKey() const
        {
            assert(Valid());
            return cur_key;
        }

        const RowPos & CurrentRowPos() const
        {
            assert(Valid());
            return cur_row;
        }

        bool IsCurrentLowPriority() const
        {
            return parts[cur_row.child]->low_priority;
        }

    private:
        /// Find final record pos of all rows with the same key
        size_t FindFinalPos(std::vector<RowPos> & rows_with_key)
        {
            if (rows_with_key.size() == 1)
                return 0;
            size_t max_pos = 0;
            if (version_mode == MergeTreeDataDeduper::VersionMode::NoVersion)
                max_pos = rows_with_key.size() - 1;
            else
            {
            /*************************************************************************************************************************
             * When there has version and delete_flag, need to handle the following cases between multiple invisible parts:
             * 1. force delete: when version is set to zero, it means that force delete ignoring version
             *    e.g.(in order)  unique key      version      value       _delete_flag_
             *    visible row:       key1            5            a              0
             *    invisible row1:    key1            0            b              1
             *    invisible row2:    key1            3            c              0
             *    In this case, the correct result is to keep invisible row2. Thus need to return invisible row2 with delete info to force delete visible row.
             * 2. first delete, and then insert a row with smaller version than before
             *    e.g.(in order)  unique key      version      value       _delete_flag_
             *    visible row:       key1            3(5)         a              0
             *    invisible row1:    key1            4            b              1
             *    invisible row2:    key1            3            c              0
             *    In this case, the correct result depends on version of visible row:
             *    a. if version of visible row is 3, need to keep invisible row2.
             *    b. if version of visible row is 5, need to keep visible row.
             *    Thus need to return invisible row2 with delete info.
             * In the above two cases, both need to return row info with delete info.
             *
             * TODO: handle the above two cases in same block in writing process
             ************************************************************************************************************************/
                size_t insert_pos = rows_with_key.size(), delete_pos = rows_with_key.size();
                for (size_t i = 0; i < rows_with_key.size(); ++i)
                {
                    const auto & row_info = rows_with_key[i];
                    if (row_info.delete_info)
                    {
                        if (!row_info.delete_info->delete_version) /// force delete
                        {
                            delete_pos = i;
                            insert_pos = rows_with_key.size();
                        }
                        else
                        {
                            if (insert_pos < rows_with_key.size()
                                && row_info.delete_info->delete_version < rows_with_key[insert_pos].version)
                                continue;
                            insert_pos = rows_with_key.size();
                            if (delete_pos < rows_with_key.size()
                                && (!rows_with_key[delete_pos].delete_info->delete_version
                                    || rows_with_key[delete_pos].delete_info->delete_version >= row_info.delete_info->delete_version))
                                continue;
                            delete_pos = i;
                        }
                    }
                    else
                    {
                        if (insert_pos < rows_with_key.size() && row_info.version < rows_with_key[insert_pos].version)
                            continue;
                        insert_pos = i;
                        if (delete_pos < rows_with_key.size()
                            && (!rows_with_key[delete_pos].delete_info->delete_version
                                || rows_with_key[delete_pos].delete_info->delete_version > row_info.version))
                            continue;
                        delete_pos = rows_with_key.size();
                    }
                }
                assert(insert_pos < rows_with_key.size() || delete_pos < rows_with_key.size());
                if (insert_pos < rows_with_key.size())
                {
                    if (delete_pos < rows_with_key.size())
                        rows_with_key[insert_pos].delete_info = std::make_shared<DeleteInfo>(
                            /*just_delete_row*/ false, rows_with_key[delete_pos].delete_info->delete_version);
                    max_pos = insert_pos;
                }
                else
                    max_pos = delete_pos;
            }
            return max_pos;
        }

        void MoveToNextKey()
        {
            valid = iter.Valid();
            if (valid)
            {
                auto slice = iter.key();
                cur_key.assign(slice.data(), slice.size());

                /// positions of all rows having `cur_key` with high priority
                std::vector<RowPos> rows_with_key_high_p;
                /// positions of all rows having `cur_key` with low priority
                std::vector<RowPos> rows_with_key_low_p;

                auto fill_rows = [&](RowPos && row) {
                    if (parts[row.child]->low_priority)
                        rows_with_key_low_p.push_back(std::move(row));
                    else
                        rows_with_key_high_p.push_back(std::move(row));
                };
                fill_rows(decodeCurrentRowPos(iter, version_mode, parts, part_implicit_versions, delete_flag_bitmaps));

                /// record pos of all rows with the same key
                do
                {
                    iter.Next();
                    if (!iter.Valid() || comparator->Compare(cur_key, iter.key()) != 0)
                        break;

                    fill_rows(decodeCurrentRowPos(iter, version_mode, parts, part_implicit_versions, delete_flag_bitmaps));
                } while (1);

                auto delete_rows = [&](std::vector<RowPos> & rows_with_key, size_t target_pos) {
                    /// mark deleted rows with lower version
                    for (size_t i = 0; i < rows_with_key.size(); ++i)
                    {
                        if (i != target_pos && !rows_with_key[i].delete_info)
                            delete_cb(rows_with_key[i]);
                    }
                };

                if (rows_with_key_high_p.empty())
                {
                    size_t max_pos = FindFinalPos(rows_with_key_low_p);
                    /// Set the row with maximum version as the current row
                    cur_row = rows_with_key_low_p[max_pos];
                    delete_rows(rows_with_key_low_p, max_pos);
                }
                else
                {
                    size_t max_pos = FindFinalPos(rows_with_key_high_p);
                    /// Set the row with maximum version as the current row
                    cur_row = rows_with_key_high_p[max_pos];
                    delete_rows(rows_with_key_high_p, max_pos);
                    delete_rows(rows_with_key_low_p, rows_with_key_low_p.size()); /// Remove all rows with low priority
                }
            }
        }

        const IndexFile::Comparator * comparator;
        const IMergeTreeDataPartsVector & parts;
        IndexFile::IndexFileMergeIterator iter;
        DeleteCallback delete_cb;
        MergeTreeDataDeduper::VersionMode version_mode;
        std::vector<UInt64> part_implicit_versions;
        const ImmutableDeleteBitmapVector & delete_flag_bitmaps;

        bool valid = false;
        String cur_key; /// cache the data of current key
        RowPos cur_row; /// cache the maximum version of the decoded current row
    };

    using DeleteBitmapGetter = std::function<ImmutableDeleteBitmapPtr(const IMergeTreeDataPartPtr &)>;

    IndexFileIterators openUniqueKeyIndexIterators(
        const IMergeTreeDataPartsVector & parts,
        std::vector<UniqueKeyIndexPtr> & index_holders,
        bool fill_cache,
        DeleteBitmapGetter delete_bitmap_getter)
    {
        index_holders.clear();
        index_holders.reserve(parts.size());
        for (auto & part : parts)
            index_holders.push_back(part->getUniqueKeyIndex());

        IndexFileIterators iters;
        iters.reserve(parts.size());
        for (size_t i = 0; i < parts.size(); ++i)
        {
            IndexFile::ReadOptions opts;
            opts.fill_cache = fill_cache;
            {
                ImmutableDeleteBitmapPtr delete_bitmap = delete_bitmap_getter(parts[i]);
                if (delete_bitmap && !delete_bitmap->isEmpty())
                {
                    opts.select_predicate = [bitmap = std::move(delete_bitmap)](const Slice &, const Slice & val) {
                        UInt32 rowid;
                        Slice input = val;
                        /// TODO: handle corrupt data in a better way.
                        /// E.g., make select_predicate return Status in order to propogate the error to the client
                        bool deleted = DecodeRowid(input, rowid) && bitmap->contains(rowid);
                        return !deleted;
                    };
                }
            }
            iters.push_back(index_holders[i]->new_iterator(opts));
        }
        return iters;
    }

    void addRowIdToBitmap(DeleteBitmapPtr & bitmap, UInt32 rowid)
    {
        if (bitmap)
        {
            bitmap->add(rowid);
        }
        else
        {
            bitmap = std::make_shared<Roaring>();
            bitmap->add(rowid);
        }
    }

    void dedupKeysWithParts(
        ReplacingSortedKeysIterator & keys,
        const IMergeTreeDataPartsVector & parts,
        DeleteBitmapVector & delta_bitmaps,
        MergeTreeDataDeduper::VersionMode version_mode)
    {
        const IndexFile::Comparator * comparator = IndexFile::BytewiseComparator();

        std::vector<UniqueKeyIndexPtr> key_indices;
        DeleteBitmapGetter delete_bitmap_getter = [](const IMergeTreeDataPartPtr & part) { return part->getDeleteBitmap(); };
        IndexFileIterators base_input_iters = openUniqueKeyIndexIterators(parts, key_indices, /*fill_cache*/ true, delete_bitmap_getter);

        std::vector<UInt64> base_implicit_versions(parts.size(), 0);
        if (version_mode == MergeTreeDataDeduper::VersionMode::PartitionValueAsVersion)
        {
            for (size_t i = 0; i < parts.size(); ++i)
                base_implicit_versions[i] = parts[i]->getVersionFromPartition();
        }

        IndexFile::IndexFileMergeIterator base_iter(comparator, std::move(base_input_iters));
        keys.SeekToFirst();
        if (keys.Valid())
            base_iter.Seek(keys.CurrentKey());

        while (keys.Valid())
        {
            if (!keys.status().ok())
                throw Exception("Deduper new parts iterator has error " + keys.status().ToString(), ErrorCodes::INCORRECT_DATA);

            if (!base_iter.status().ok())
                throw Exception("Deduper visible parts iterator has error " + base_iter.status().ToString(), ErrorCodes::INCORRECT_DATA);

            if (!base_iter.Valid())
            {
                keys.Next();
                /// needs to read `keys` iter to the end in order to remove duplicate keys among new parts
                continue;
            }
            bool exact_match = false;
            int cmp = comparator->Compare(keys.CurrentKey(), base_iter.key());
            if (cmp < 0)
            {
                keys.Next();
                continue;
            }
            else if (cmp > 0)
            {
                base_iter.NextUntil(keys.CurrentKey(), exact_match);
            }
            else
            {
                exact_match = true;
            }

            while (exact_match)
            {
                RowPos lhs = decodeCurrentRowPos(base_iter, version_mode, parts, base_implicit_versions);
                const RowPos & rhs = keys.CurrentRowPos();
                if (keys.IsCurrentLowPriority())
                    addRowIdToBitmap(delta_bitmaps[rhs.child + parts.size()], rhs.rowid);
                else
                {
                    if (rhs.delete_info)
                    {
                        if (!rhs.delete_info->delete_version || rhs.delete_info->delete_version >= lhs.version)
                            addRowIdToBitmap(delta_bitmaps[lhs.child], lhs.rowid);
                        else if (!rhs.delete_info->just_delete_row)
                        {
                            if (lhs.version <= rhs.version)
                                addRowIdToBitmap(delta_bitmaps[lhs.child], lhs.rowid);
                            else
                                addRowIdToBitmap(delta_bitmaps[rhs.child + parts.size()], rhs.rowid);
                        }
                    }
                    else
                    {
                        if (lhs.version <= rhs.version)
                            addRowIdToBitmap(delta_bitmaps[lhs.child], lhs.rowid);
                        else
                            addRowIdToBitmap(delta_bitmaps[rhs.child + parts.size()], rhs.rowid);
                    }
                }

                exact_match = false;
                keys.Next();
                if (keys.Valid())
                    base_iter.NextUntil(keys.CurrentKey(), exact_match);
            }
        }
    }

} /// namespace

String MergeTreeDataDeduper::DedupTask::getDedupLevelInfo() const
{
    WriteBufferFromOwnString os;
    os << (partition_id.empty() ? "table" : "partition " + partition_id) << (bucket_valid ? ("(bucket " + toString(bucket_number) + ")") : "");
    return os.str();
}

LocalDeleteBitmaps MergeTreeDataDeduper::dedupParts(
    TxnTimestamp txn_id,
    const IMergeTreeDataPartsVector & all_visible_parts,
    const IMergeTreeDataPartsVector & all_staged_parts,
    const IMergeTreeDataPartsVector & all_uncommitted_parts)
{
    if (all_staged_parts.empty() && all_uncommitted_parts.empty())
        return {};

    LocalDeleteBitmaps res;
    auto prepare_bitmaps_to_dump = [txn_id, this, &res, log = this->log](
                                       const IMergeTreeDataPartsVector & visible_parts,
                                       const IMergeTreeDataPartsVector & new_parts,
                                       const DeleteBitmapVector & bitmaps) -> size_t {
        size_t num_bitmaps = 0;
        size_t max_delete_bitmap_meta_depth = data.getSettings()->max_delete_bitmap_meta_depth;
        for (size_t i = 0; i < bitmaps.size(); ++i)
        {
            DeleteBitmapPtr bitmap = bitmaps[i];
            /// always dump base bitmap for new parts
            if (!bitmap && i >= visible_parts.size())
                bitmap = std::make_shared<Roaring>();

            if (!bitmap)
                continue;

            if (i < visible_parts.size())
            {
                LOG_DEBUG(
                    log,
                    "Preparing bitmap for visible part: {}, base bitmap cardinality: {}, delta bitmap cardinality: {}, txn_id: {}",
                    visible_parts[i]->name,
                    visible_parts[i]->getDeleteBitmap()->cardinality(),
                    bitmap->cardinality(),
                    txn_id.toUInt64());
                size_t bitmap_meta_depth = visible_parts[i]->getDeleteBitmapMetaDepth();
                res.emplace_back(LocalDeleteBitmap::createBaseOrDelta(
                    visible_parts[i]->info,
                    visible_parts[i]->getDeleteBitmap(),
                    bitmap,
                    txn_id.toUInt64(),
                    bitmap_meta_depth >= max_delete_bitmap_meta_depth));
            }
            else /// new part
            {
                auto base_bitmap = new_parts[i - visible_parts.size()]->getDeleteBitmap(/*allow_null*/ true);
                LOG_DEBUG(
                    log,
                    "Preparing bitmap for new part: {}, base bitmap cardinality: {}, delta bitmap cardinality: {}, txn_id: {}",
                    new_parts[i - visible_parts.size()]->name,
                    (base_bitmap ? base_bitmap->cardinality() : 0),
                    bitmap->cardinality(),
                    txn_id.toUInt64());
                if (base_bitmap)
                {
                    size_t bitmap_meta_depth = new_parts[i - visible_parts.size()]->getDeleteBitmapMetaDepth();
                    res.push_back(LocalDeleteBitmap::createBaseOrDelta(
                        new_parts[i - visible_parts.size()]->info,
                        base_bitmap,
                        bitmap,
                        txn_id.toUInt64(),
                        bitmap_meta_depth >= max_delete_bitmap_meta_depth));
                }
                else
                    res.push_back(LocalDeleteBitmap::createBase(new_parts[i - visible_parts.size()]->info, bitmap, txn_id.toUInt64()));
            }
            num_bitmaps++;
        }
        return num_bitmaps;
    };

    auto log_dedup_detail = [&](const DedupTask & task, const IMergeTreeDataPartsVector & visible_parts, const IMergeTreeDataPartsVector & new_parts) {
        WriteBufferFromOwnString msg;
        msg << "Start to dedup in txn_id: " << txn_id.toUInt64() << ", dedup level info: " << task.getDedupLevelInfo() << ",, visible_parts: [";
        for (size_t i = 0; i < visible_parts.size(); ++i)
        {
            if (i > 0)
                msg << ", ";
            msg << visible_parts[i]->name;
        }
        msg << "], new_parts: [";
        for (size_t i = 0; i < new_parts.size(); ++i)
        {
            if (i > 0)
                msg << ", ";
            msg << new_parts[i]->name;
        }
        msg << "].";
        LOG_DEBUG(log, msg.str());
    };

    Stopwatch watch;
    bool bucket_level_dedup = data.getSettings()->enable_bucket_level_unique_keys;
    DedupTasks dedup_tasks = convertIntoSubDedupTasks(all_visible_parts, all_staged_parts, all_uncommitted_parts, bucket_level_dedup);

    size_t dedup_pool_size = std::min(static_cast<size_t>(data.getSettings()->unique_table_dedup_threads), dedup_tasks.size());
    ThreadPool dedup_pool(dedup_pool_size);
    std::mutex mutex;
    for (size_t i = 0; i < dedup_tasks.size(); ++i)
    {
        if (bucket_level_dedup && !dedup_tasks[i].bucket_valid)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Enable bucket level unique keys, but dedup task is bucket invalid, it's a bug!");

        dedup_pool.scheduleOrThrowOnError([&, i]() {
            Stopwatch sub_task_watch;
            auto & visible_parts = dedup_tasks[i].visible_parts;
            auto & new_parts = dedup_tasks[i].new_parts;
            log_dedup_detail(dedup_tasks[i], visible_parts, new_parts);
            DeleteBitmapVector bitmaps = dedupImpl(visible_parts, new_parts);

            std::lock_guard lock(mutex);
            size_t num_bitmaps_to_dump = prepare_bitmaps_to_dump(visible_parts, new_parts, bitmaps);
            LOG_DEBUG(
                log,
                "Dedup {} in {} ms, visible parts={}, new parts={}, result bitmaps={}",
                dedup_tasks[i].getDedupLevelInfo(),
                sub_task_watch.elapsedMilliseconds(),
                visible_parts.size(),
                new_parts.size(),
                num_bitmaps_to_dump);
        });
    }
    dedup_pool.wait();

    LOG_DEBUG(
        log,
        "Dedup {} tasks in {} ms, thread pool={}, visible parts={}, staged parts={}, uncommitted_parts = {}, result bitmaps={}",
        dedup_tasks.size(),
        watch.elapsedMilliseconds(),
        dedup_pool_size,
        all_visible_parts.size(),
        all_staged_parts.size(),
        all_uncommitted_parts.size(),
        res.size());
    return res;
}

MergeTreeDataDeduper::DedupTasks MergeTreeDataDeduper::convertIntoSubDedupTasks(
    const IMergeTreeDataPartsVector & all_visible_parts,
    const IMergeTreeDataPartsVector & all_staged_parts,
    const IMergeTreeDataPartsVector & all_uncommitted_parts,
    const bool & bucket_level_dedup)
{
    /// Mark whether we can split dedup tasks into bucket granule.
    bool table_level_valid_bucket = bucket_level_dedup ? true : data.getInMemoryMetadataPtr()->checkIfClusterByKeySameWithUniqueKey();
    auto table_definition_hash = data.getTableHashForClusterBy();
    auto settings = data.getSettings();
    /// Check whether all parts has same table_definition_hash.
    /// Otherwise, we can not split dedup task to bucket granule.
    auto checkBucketTable = [&bucket_level_dedup, &table_definition_hash](const IMergeTreeDataPartsVector & all_parts, bool & valid_bucket) {
        if (!valid_bucket || bucket_level_dedup)
            return;
        auto it = std::find_if(all_parts.begin(), all_parts.end(), [&](const auto & part) {
            return part->bucket_number == -1 || part->table_definition_hash != table_definition_hash;
        });
        if (it != all_parts.end())
            valid_bucket = false;
    };

    DedupTasks res;
    /// Prepare all new parts (staged + uncommitted) that need to be dedupped with visible parts.
    /// NOTE: the order of new parts is significant because it reflects the write order of the same key.
    IMergeTreeDataPartsVector all_new_parts = all_staged_parts;
    if (settings->partition_level_unique_keys)
    {
        /// New parts are first sorted by partition, and then within each partition sorted by part's written time
        std::sort(all_new_parts.begin(), all_new_parts.end(), [](auto & lhs, auto & rhs) {
            return std::forward_as_tuple(lhs->info.partition_id, lhs->commit_time)
                < std::forward_as_tuple(rhs->info.partition_id, rhs->commit_time);
        });
        if (!all_uncommitted_parts.empty())
        {
            all_new_parts.insert(all_new_parts.end(), all_uncommitted_parts.begin(), all_uncommitted_parts.end());
            std::stable_sort(all_new_parts.begin(), all_new_parts.end(), [](auto & lhs, auto & rhs) {
                return lhs->info.partition_id < rhs->info.partition_id;
            });
        }

        size_t i = 0, j = 0;
        while (j < all_new_parts.size())
        {
            String partition_id = all_new_parts[j]->info.partition_id;
            IMergeTreeDataPartsVector visible_parts;
            IMergeTreeDataPartsVector new_parts;
            while (i < all_visible_parts.size() && all_visible_parts[i]->info.partition_id == partition_id)
                visible_parts.push_back(all_visible_parts[i++]);
            while (j < all_new_parts.size() && all_new_parts[j]->info.partition_id == partition_id)
                new_parts.push_back(all_new_parts[j++]);

            bool partition_level_valid_bucket = table_level_valid_bucket;
            checkBucketTable(visible_parts, partition_level_valid_bucket);
            checkBucketTable(new_parts, partition_level_valid_bucket);
            if (!partition_level_valid_bucket)
                res.emplace_back(partition_id, /*valid_bucket=*/false, -1, visible_parts, new_parts);
            else
            {
                /// There is no need to use stable sort for visible parts because there has no duplicated key in these parts.
                std::sort(visible_parts.begin(), visible_parts.end(), [](auto & lhs, auto & rhs) {
                    return lhs->bucket_number < rhs->bucket_number;
                });
                /// We must use stable sort here, because we can not change the order when bucket number is the same which is represented as commit order.
                std::stable_sort(
                    new_parts.begin(), new_parts.end(), [](auto & lhs, auto & rhs) { return lhs->bucket_number < rhs->bucket_number; });
                size_t p = 0, q = 0;
                while (q < new_parts.size())
                {
                    Int64 bucket_number = new_parts[q]->bucket_number;
                    IMergeTreeDataPartsVector bucket_visible_parts;
                    IMergeTreeDataPartsVector bucket_new_parts;
                    /// Skip all parts which bucket number is smaller.
                    while (p < visible_parts.size() && visible_parts[p]->bucket_number < bucket_number)
                        p++;
                    while (p < visible_parts.size() && visible_parts[p]->bucket_number == bucket_number)
                        bucket_visible_parts.push_back(visible_parts[p++]);
                    while (q < new_parts.size() && new_parts[q]->bucket_number == bucket_number)
                        bucket_new_parts.push_back(new_parts[q++]);

                    res.emplace_back(partition_id, /*valid_bucket=*/true, bucket_number, bucket_visible_parts, bucket_new_parts);
                }
            }
        }
    }
    else
    {
        /// New parts are sorted by part's written time
        std::sort(all_new_parts.begin(), all_new_parts.end(), [](auto & lhs, auto & rhs) { return lhs->commit_time < rhs->commit_time; });
        all_new_parts.insert(all_new_parts.end(), all_uncommitted_parts.begin(), all_uncommitted_parts.end());
        checkBucketTable(all_visible_parts, table_level_valid_bucket);
        checkBucketTable(all_new_parts, table_level_valid_bucket);
        if (!table_level_valid_bucket)
            res.emplace_back(/*partition_id=*/"", /*valid_bucket=*/false, -1, all_visible_parts, all_new_parts);
        else
        {
            IMergeTreeDataPartsVector sorted_visible_parts = all_visible_parts;
            /// There is no need to use stable sort for visible parts because there has no duplicated key in these parts.
            std::sort(sorted_visible_parts.begin(), sorted_visible_parts.end(), [](auto & lhs, auto & rhs) {
                return lhs->bucket_number < rhs->bucket_number;
            });
            /// We must use stable sort here, because we can not change the order when bucket number is the same which is represented as commit order.
            std::stable_sort(
                all_new_parts.begin(), all_new_parts.end(), [](auto & lhs, auto & rhs) { return lhs->bucket_number < rhs->bucket_number; });

            size_t i = 0;
            size_t j = 0;
            while (j < all_new_parts.size())
            {
                Int64 bucket_number = all_new_parts[j]->bucket_number;
                IMergeTreeDataPartsVector visible_parts;
                IMergeTreeDataPartsVector new_parts;
                /// Skip all parts which bucket number is smaller.
                while (i < sorted_visible_parts.size() && sorted_visible_parts[i]->bucket_number < bucket_number)
                    i++;
                while (i < sorted_visible_parts.size() && sorted_visible_parts[i]->bucket_number == bucket_number)
                    visible_parts.push_back(sorted_visible_parts[i++]);
                while (j < all_new_parts.size() && all_new_parts[j]->bucket_number == bucket_number)
                    new_parts.push_back(all_new_parts[j++]);

                res.emplace_back(/*partition_id=*/"", /*valid_bucket=*/true, bucket_number, visible_parts, new_parts);
            }
        }
    }
    return res;
}

LocalDeleteBitmaps MergeTreeDataDeduper::repairParts(TxnTimestamp txn_id, IMergeTreeDataPartsVector all_visible_parts)
{
    if (all_visible_parts.empty())
        return {};

    LocalDeleteBitmaps res;
    auto prepare_bitmaps_to_dump
        = [txn_id, this, &res](const IMergeTreeDataPartsVector & visible_parts, const DeleteBitmapVector & delta_bitmaps) -> size_t {
        size_t num_bitmaps = 0;
        size_t max_delete_bitmap_meta_depth = data.getSettings()->max_delete_bitmap_meta_depth;
        for (size_t i = 0; i < delta_bitmaps.size(); ++i)
        {
            if (!delta_bitmaps[i])
                continue;
            size_t bitmap_meta_depth = visible_parts[i]->getDeleteBitmapMetaDepth();
            res.push_back(LocalDeleteBitmap::createBaseOrDelta(
                visible_parts[i]->info,
                visible_parts[i]->getDeleteBitmap(/*allow_null*/ true),
                delta_bitmaps[i],
                txn_id.toUInt64(),
                bitmap_meta_depth >= max_delete_bitmap_meta_depth));
            num_bitmaps++;
        }
        return num_bitmaps;
    };

    if (data.getSettings()->partition_level_unique_keys)
    {
        Stopwatch overall_watch;
        std::sort(all_visible_parts.begin(), all_visible_parts.end(), [](auto & lhs, auto & rhs) {
            return std::forward_as_tuple(lhs->info.partition_id, lhs->commit_time)
                < std::forward_as_tuple(rhs->info.partition_id, rhs->commit_time);
        });

        size_t num_partitions = 0;
        for (size_t i = 0; i < all_visible_parts.size(); ++num_partitions)
        {
            Stopwatch watch;
            String partition_id = all_visible_parts[i]->info.partition_id;
            IMergeTreeDataPartsVector visible_parts;
            while (i < all_visible_parts.size() && all_visible_parts[i]->info.partition_id == partition_id)
                visible_parts.push_back(all_visible_parts[i++]);

            DeleteBitmapVector delta_bitmaps = repairImpl(visible_parts);
            size_t num_bitmaps_to_dump = prepare_bitmaps_to_dump(visible_parts, delta_bitmaps);
            LOG_INFO(
                log,
                "Repair partition {} in {} ms, visible parts={}, result bitmaps={}",
                partition_id,
                watch.elapsedMilliseconds(),
                visible_parts.size(),
                num_bitmaps_to_dump);
        }
        LOG_INFO(
            log,
            "Repair table in {} ms, partitions={}, result bitmaps={}",
            overall_watch.elapsedMilliseconds(),
            num_partitions,
            res.size());
    }
    else
    {
        Stopwatch watch;
        std::sort(
            all_visible_parts.begin(), all_visible_parts.end(), [](auto & lhs, auto & rhs) { return lhs->commit_time < rhs->commit_time; });

        DeleteBitmapVector delta_bitmaps = repairImpl(all_visible_parts);
        size_t num_bitmaps_to_dump = prepare_bitmaps_to_dump(all_visible_parts, delta_bitmaps);
        LOG_INFO(
            log,
            "Repair table in {} ms, visible parts={}, result bitmaps={}",
            watch.elapsedMilliseconds(),
            all_visible_parts.size(),
            num_bitmaps_to_dump);
    }
    return res;
}

DeleteBitmapVector
MergeTreeDataDeduper::dedupImpl(const IMergeTreeDataPartsVector & visible_parts, const IMergeTreeDataPartsVector & new_parts)
{
    if (new_parts.empty())
        throw Exception("Delta part is empty when MergeTreeDataDeduper handles dedup task.", ErrorCodes::LOGICAL_ERROR);

    std::vector<UniqueKeyIndexPtr> new_part_indices;
    DeleteBitmapGetter delete_bitmap_getter = [](const IMergeTreeDataPartPtr & part) -> ImmutableDeleteBitmapPtr {
        if (!part->delete_flag)
            return part->getDeleteBitmap(/*allow_null*/ true);
        return nullptr;
    };
    IndexFileIterators input_iters
        = openUniqueKeyIndexIterators(new_parts, new_part_indices, /*fill_cache*/ true, delete_bitmap_getter);

    DeleteBitmapVector res(visible_parts.size() + new_parts.size());
    DeleteCallback cb = [start = visible_parts.size(), &res](const RowPos & pos) { addRowIdToBitmap(res[start + pos.child], pos.rowid); };

    ImmutableDeleteBitmapVector delete_flag_bitmaps(new_parts.size());
    for (size_t i = 0; i < new_parts.size(); ++i)
    {
        if (new_parts[i]->delete_flag)
            delete_flag_bitmaps[i] = new_parts[i]->getDeleteBitmap(/*allow_null*/ true);
    }
    ReplacingSortedKeysIterator keys_iter(
        IndexFile::BytewiseComparator(), new_parts, std::move(input_iters), cb, version_mode, delete_flag_bitmaps);

    dedupKeysWithParts(keys_iter, visible_parts, res, version_mode);
    return res;
}

DeleteBitmapVector MergeTreeDataDeduper::repairImpl(const IMergeTreeDataPartsVector & parts)
{
    if (parts.empty())
        return {};

    std::vector<UniqueKeyIndexPtr> key_indices;
    DeleteBitmapGetter delete_bitmap_getter = [](const IMergeTreeDataPartPtr & part) { return part->getDeleteBitmap(/*allow_null*/ true); };
    IndexFileIterators input_iters = openUniqueKeyIndexIterators(parts, key_indices, /*fill_cache*/ false, delete_bitmap_getter);

    DeleteBitmapVector res(parts.size());
    DeleteCallback cb = [&res](const RowPos & pos) { addRowIdToBitmap(res[pos.child], pos.rowid); };
    ReplacingSortedKeysIterator keys_iter(IndexFile::BytewiseComparator(), parts, std::move(input_iters), cb, version_mode);
    keys_iter.SeekToFirst();
    while (keys_iter.Valid())
        // delete_flag_bitmaps is default initialized
        // coverity[use_invalid_in_call]
        keys_iter.Next();
    return res;
}

}
