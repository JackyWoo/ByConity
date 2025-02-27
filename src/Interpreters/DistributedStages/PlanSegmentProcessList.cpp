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

#include <Interpreters/CancellationCode.h>
#include <Interpreters/Context.h>
#include <Interpreters/DistributedStages/PlanSegmentProcessList.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/DistributedStages/AddressInfo.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace DB
{
namespace ErrorCodes
{
    extern const int TOO_MANY_SIMULTANEOUS_QUERIES;
    extern const int QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING;
    extern const int LOGICAL_ERROR;
    extern const int QUERY_WAS_CANCELLED;
}

PlanSegmentProcessList::EntryPtr
PlanSegmentProcessList::insert(const PlanSegment & plan_segment, ContextMutablePtr query_context, bool force)
{
    const String & initial_query_id = plan_segment.getQueryId();
    const String & segment_id_str = std::to_string(plan_segment.getPlanSegmentId());
    const String & coordinator_address = extractExchangeStatusHostPort(plan_segment.getCoordinatorAddress());
    bool need_wait_cancel = false;

    if (shouldEraseGroup())
    {
        tryEraseGroup();
    }

    auto initial_query_start_time_ms = query_context->getClientInfo().initial_query_start_time_microseconds;
    {
        Element segment_group{nullptr};
        bool found = initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
            segment_group = it.second;
        });
        if (found && segment_group->coordinator_address != coordinator_address
            && segment_group->initial_query_start_time_ms <= initial_query_start_time_ms)
        {
            tryEraseGroup();

            if (initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
                segment_group = it.second;
            }))
            {
                if (!force && !query_context->getSettingsRef().replace_running_query)
                    throw Exception(
                        "Distributed query with id = " + initial_query_id + " is already running.",
                        ErrorCodes::QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING);

                LOG_WARNING(
                    logger,
                    "Distributed query with id = {} will be replaced by other coordinator: {}",
                    initial_query_id,
                    plan_segment.getCoordinatorAddress().toString());

                need_wait_cancel = segment_group->tryCancel();
            }
        }
    }

    WriteBufferFromOwnString pipeline_buffer;
    QueryPlan::ExplainPlanOptions options;
    plan_segment.getQueryPlan().explainPlan(pipeline_buffer, options);
    String pipeline_string = pipeline_buffer.str();

    ProcessList::EntryPtr entry;
    auto context_process_list_entry = query_context->getProcessListEntry().lock();
    if (context_process_list_entry)
        entry = std::move(context_process_list_entry);
    else
        entry = query_context->getProcessList().insert("\n" + pipeline_string, nullptr, query_context, force);

    auto res = std::make_shared<PlanSegmentProcessListEntry>(*this, entry->getPtr(), initial_query_id, plan_segment.getPlanSegmentId());
    res->setCoordinatorAddress(plan_segment.getCoordinatorAddress());
    res->setCurrentAddress(plan_segment.getCurrentAddress());

    if (need_wait_cancel)
    {
        std::unique_lock lock(mutex);
        const auto replace_running_query_max_wait_ms
            = query_context->getSettingsRef().replace_running_query_max_wait_ms.totalMilliseconds();
        if (!replace_running_query_max_wait_ms
            || !remove_group.wait_for(lock, std::chrono::milliseconds(replace_running_query_max_wait_ms), [&] {
                    tryEraseGroup();
                    bool inited = false;
                    bool found = initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
                        if (it.second->coordinator_address == coordinator_address)
                            inited = true;
                    });
                    return !found || inited;
               }))
        {
            throw Exception(
                "Distributed query with id = " + initial_query_id + " is already running and can't be stopped",
                ErrorCodes::QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING);
        }
    }
 
    {
        Element segment_group{nullptr};
        bool found = initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
            segment_group = it.second;
        });
        if (found && segment_group->coordinator_address != coordinator_address
            && segment_group->initial_query_start_time_ms <= initial_query_start_time_ms)
        {
            throw Exception(
                "Distributed query with id = " + initial_query_id + " is already running and can't be stopped",
                ErrorCodes::QUERY_WITH_SAME_ID_IS_ALREADY_RUNNING);
        }

        if (!found)
        {
            initail_query_to_groups.try_emplace(initial_query_id, std::make_shared<PlanSegmentGroup>(coordinator_address, initial_query_start_time_ms));
        }

        found = initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
            segment_group = it.second;
        });
        if (found && !segment_group->emplace(plan_segment.getPlanSegmentId(), std::move(entry)))
        {
            throw Exception("Exsited segment_id: " + segment_id_str + " for query: " + initial_query_id, ErrorCodes::LOGICAL_ERROR);
        }
    }
    return res;
}

bool PlanSegmentGroup::tryCancel()
{
    std::vector<std::shared_ptr<ProcessListEntry>> to_cancel;
    std::unique_lock lock(mutex);
    for (auto &it : segment_queries)
    {
        to_cancel.push_back(it.second);
    }
    lock.unlock();

    for (auto& entry : to_cancel)
    {
        entry->get().cancelQuery(true);
    }

    return to_cancel.size() > 0;
}

bool PlanSegmentProcessList::shouldEraseGroup()
{
    std::unique_lock lock(group_to_erase_mutex);
    return group_to_erase.size() > group_to_erase_threshold;
}

bool PlanSegmentProcessList::tryEraseGroup()
{
    std::unique_lock lock(group_to_erase_mutex);
    if (group_to_erase.size() > 0)
    {
        for (auto& initial_query_id : group_to_erase)
        {
            initail_query_to_groups.erase(initial_query_id);
        }
        group_to_erase.clear();
    }
    return true;
}

bool PlanSegmentProcessList::remove(std::string initial_query_id, size_t segment_id)
{
    Element segment_group{nullptr};
    initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
        segment_group = it.second;
    });
    if (segment_group.get() && !segment_group->empty())
    {
        segment_group->erase(segment_id);

        LOG_TRACE(
            logger,
            "Remove segment {} for distributed query {}@{} from PlanSegmentProcessList",
            segment_id,
            initial_query_id,
            segment_group->coordinator_address);

        if (segment_group->empty())
        {
            LOG_TRACE(logger, "Remove segment group for distributed query {}@{}", initial_query_id, segment_group->coordinator_address);
            std::unique_lock lock(group_to_erase_mutex);
            group_to_erase.insert(initial_query_id);
            lock.unlock();
            remove_group.notify_all();
        }
        return true;
    }

    // shouldn't reach here.
    LOG_ERROR(logger, "Logical error: Cannot found query: {} in PlanSegmentProcessList", initial_query_id);
    return false;
}

CancellationCode PlanSegmentProcessList::tryCancelPlanSegmentGroup(const String & initial_query_id, String coordinator_address)
{
    auto res = CancellationCode::CancelSent;
    bool found = false;
    Element segment_group{nullptr};

    initail_query_to_groups.if_contains(initial_query_id, [&](auto & it){
        segment_group = it.second;
    });
    if (segment_group.get())
    {
        if (coordinator_address.empty() || segment_group->coordinator_address == coordinator_address)
            found = segment_group->tryCancel();
        else
        {
            LOG_WARNING(
                logger,
                "Fail to cancel distributed query[{}@{}], coordinator_address doesn't match, seg coordinator address is {}",
                initial_query_id,
                coordinator_address,
                segment_group->coordinator_address
            );
            return CancellationCode::CancelCannotBeSent;
        }
    }

    if (!found)
    {
        res = CancellationCode::NotFound;
    }
    return res;
}

PlanSegmentProcessListEntry::PlanSegmentProcessListEntry(
    PlanSegmentProcessList & parent_, Element status_, String initial_query_id_, size_t segment_id_)
    : parent(parent_), status(status_), initial_query_id(std::move(initial_query_id_)), segment_id(segment_id_)
{
}

PlanSegmentProcessListEntry::~PlanSegmentProcessListEntry()
{
    parent.remove(initial_query_id, segment_id);
}
}
