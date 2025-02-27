#include <memory>
#include <DataStreams/AddingDefaultBlockOutputStream.h>
#include <DataStreams/CheckConstraintsBlockOutputStream.h>
#include <DataStreams/CountingBlockOutputStream.h>
#include <DataStreams/PushingToViewsBlockOutputStream.h>
#include <DataStreams/SquashingBlockOutputStream.h>
#include <Interpreters/Context.h>
#include <Processors/Sources/SinkToOutputStream.h>
#include <Processors/Transforms/ExpressionTransform.h>
#include <Processors/Transforms/TableWriteTransform.h>
#include <QueryPlan/ITransformingStep.h>
#include <QueryPlan/TableWriteStep.h>
#include <Transaction/CnchWorkerTransaction.h>
#include <Common/RpcClientPool.h>
#include <MergeTreeCommon/CnchTopologyMaster.h>

namespace DB
{

static ITransformingStep::Traits getTraits()
{
    return ITransformingStep::Traits{
        {.preserves_distinct_columns = true,
         .returns_single_stream = false,
         .preserves_number_of_streams = true,
         .preserves_sorting = true},
        {.preserves_number_of_rows = true}};
}

TableWriteStep::TableWriteStep(const DataStream & input_stream_, TargetPtr target_)
    : ITransformingStep(input_stream_, input_stream_.header, getTraits()), target(target_)
{
}

Block TableWriteStep::getHeader(const NamesAndTypes & input_columns)
{
    Block sample_block;
    for (const auto & input_column : input_columns)
        sample_block.insert(ColumnWithTypeAndName(input_column.type, input_column.name));
    return sample_block;
}

BlockOutputStreams TableWriteStep::createOutputStream(
    StoragePtr target_table,
    const BuildQueryPipelineSettings & settings,
    Block header,
    size_t max_threads,
    bool no_destination,
    bool no_squash)
{
    BlockOutputStreams out_streams;
    size_t out_streams_size = 1;
    auto query_settings = settings.context->getSettingsRef();
    if (target_table->supportsParallelInsert() && query_settings.max_insert_threads > 1)
        out_streams_size = std::min(size_t(query_settings.max_insert_threads), max_threads);

    for (size_t i = 0; i < out_streams_size; ++i)
    {
        /// We create a pipeline of several streams, into which we will write data.
        BlockOutputStreamPtr out;

        auto metadata_snapshot = target_table->getInMemoryMetadataPtr();
        /// NOTE: we explicitly ignore bound materialized views when inserting into Kafka Storage.
        ///       Otherwise we'll get duplicates when MV reads same rows again from Kafka.
        if (target_table->noPushingToViews() && !no_destination)
            out = target_table->write(nullptr, metadata_snapshot, settings.context);
        else
            out = std::make_shared<PushingToViewsBlockOutputStream>(
                target_table, metadata_snapshot, settings.context, nullptr, no_destination);

        /// Note that we wrap transforms one on top of another, so we write them in reverse of data processing order.

        /// Checking constraints. It must be done after calculation of all defaults, so we can check them on calculated columns.
        if (const auto & constraints = metadata_snapshot->getConstraints(); !constraints.empty())
            out = std::make_shared<CheckConstraintsBlockOutputStream>(
                target_table->getStorageID(), out, out->getHeader(), metadata_snapshot->getConstraints(), settings.context);

        bool null_as_default = query_settings.insert_null_as_default;

        /// Actually we don't know structure of input blocks from query/table,
        /// because some clients break insertion protocol (columns != header)
        out = std::make_shared<AddingDefaultBlockOutputStream>(
            out, header, metadata_snapshot->getColumns(), settings.context, null_as_default);

        /// It's important to squash blocks as early as possible (before other transforms),
        ///  because other transforms may work inefficient if block size is small.

        /// Do not squash blocks if it is a sync INSERT into Distributed, since it lead to double bufferization on client and server side.
        /// Client-side bufferization might cause excessive timeouts (especially in case of big blocks).
        if (!(query_settings.insert_distributed_sync && target_table->isRemote()) && !no_squash)
        {
            bool table_prefers_large_blocks = target_table->prefersLargeBlocks();

            out = std::make_shared<SquashingBlockOutputStream>(
                out,
                out->getHeader(),
                table_prefers_large_blocks ? query_settings.min_insert_block_size_rows : query_settings.max_block_size,
                table_prefers_large_blocks ? query_settings.min_insert_block_size_bytes : 0);
        }

        auto out_wrapper = std::make_shared<CountingBlockOutputStream>(out);
        out_streams.emplace_back(std::move(out_wrapper));
    }

    return out_streams;
}

void TableWriteStep::transformPipeline(QueryPipeline & pipeline, const BuildQueryPipelineSettings & settings)
{
    switch (target->getTargetType())
    {
        case TargetType::INSERT: {
            auto * insert_target = dynamic_cast<TableWriteStep::InsertTarget *>(target.get());
            auto target_storage = DatabaseCatalog::instance().getTable(insert_target->getStorageID(), settings.context);

            auto host_ports = settings.context->getCnchTopologyMaster()->getTargetServer(
                UUIDHelpers::UUIDToString(target_storage->getStorageUUID()), target_storage->getServerVwName(), true);
            auto server_client = host_ports.empty() ? settings.context->getCnchServerClientPool().get()
                                                    : settings.context->getCnchServerClientPool().get(host_ports);
            auto txn = std::make_shared<CnchWorkerTransaction>(settings.context->getGlobalContext(), server_client);
            const_cast<Context *>(settings.context.get())->setCurrentTransaction(txn);

            auto out_streams = createOutputStream(
                target_storage, settings, getHeader(insert_target->getColumns()), pipeline.getNumThreads(), false, false);

            if (out_streams.empty())
                throw Exception("No output stream when transfrom TableWriteStep", ErrorCodes::LOGICAL_ERROR);

            const auto & header = out_streams[0]->getHeader();
            auto actions_dag = ActionsDAG::makeConvertingActions(
                pipeline.getHeader().getColumnsWithTypeAndName(),
                header.getColumnsWithTypeAndName(),
                ActionsDAG::MatchColumnsMode::Position);
            auto actions = std::make_shared<ExpressionActions>(
                actions_dag, ExpressionActionsSettings::fromContext(settings.context, CompileExpressions::yes));

            pipeline.addSimpleTransform(
                [&](const Block & in_header) -> ProcessorPtr { return std::make_shared<ExpressionTransform>(in_header, actions); });

            pipeline.resize(out_streams.size());
            //LOG_DEBUG(&Poco::Logger::get("TableWriteStep"), fmt::format("pipeline size: {}, threads {}", pipeline.getNumStreams(), pipeline.getNumThreads()));

            auto stream = std::move(out_streams.back());
            out_streams.pop_back();

            //LOG_DEBUG(&Poco::Logger::get("TableWriteStep"), fmt::format("output header: {}", stream->getHeader().dumpStructure()));
            pipeline.addTransform(std::make_shared<TableWriteTransform>(
                std::move(stream), getHeader(insert_target->getColumns()), insert_target->getStorage(), settings.context));
            break;
        }
    }
}

void TableWriteStep::setInputStreams(const DataStreams & input_streams_)
{
    input_streams = input_streams_;
    output_stream = DataStream{.header = std::move((input_streams_[0].header))};
}

std::shared_ptr<IQueryPlanStep> TableWriteStep::copy(ContextPtr) const
{
    return std::make_shared<TableWriteStep>(input_streams[0], target);
}

void TableWriteStep::toProto(Protos::TableWriteStep & proto, bool) const
{
    ITransformingStep::serializeToProtoBase(*proto.mutable_query_plan_base());

    if (!target)
        throw Exception("Target cannot be nullptr", ErrorCodes::LOGICAL_ERROR);
    target->toProto(*proto.mutable_target());
}

std::shared_ptr<TableWriteStep> TableWriteStep::fromProto(const Protos::TableWriteStep & proto, ContextPtr context)
{
    auto [step_description, base_input_stream] = ITransformingStep::deserializeFromProtoBase(proto.query_plan_base());
    auto target = TableWriteStep::Target::fromProto(proto.target(), context);
    auto step = std::make_shared<TableWriteStep>(base_input_stream, target);
    step->setStepDescription(step_description);
    return step;
}

void TableWriteStep::allocate(const ContextPtr & context)
{
    auto storage_id = target->getStorage()->prepareTableWrite(context);
    if (auto * input_target = dynamic_cast<InsertTarget *>(target.get()))
        target = std::make_shared<InsertTarget>(input_target->getStorage(), storage_id, input_target->getColumns());
    else
        throw Exception("unknown TableWrite::Target", ErrorCodes::LOGICAL_ERROR);
}

void TableWriteStep::Target::toProto(Protos::TableWriteStep::Target & proto) const
{
    switch (getTargetType())
    {
        case TargetType::INSERT: {
            const auto * ptr = dynamic_cast<const TableWriteStep::InsertTarget *>(this);
            ptr->toProtoImpl(*proto.mutable_insert_target());
            return;
        }

        default:
            throw Exception(fmt::format("unknown TableWrite::Target {}", static_cast<int>(getTargetType())), ErrorCodes::LOGICAL_ERROR);
    }
}

TableWriteStep::TargetPtr TableWriteStep::Target::fromProto(const Protos::TableWriteStep::Target & proto, ContextPtr context)
{
    switch (proto.target_case())
    {
        case Protos::TableWriteStep::Target::TargetCase::kInsertTarget:
            return TableWriteStep::InsertTarget::createFromProtoImpl(proto.insert_target(), context);
        default:
            throw Exception(fmt::format("unknown TableWrite::Target {}", static_cast<int>(proto.target_case())), ErrorCodes::LOGICAL_ERROR);
    }
}

void TableWriteStep::InsertTarget::toProtoImpl(Protos::TableWriteStep::InsertTarget & proto) const
{
    storage_id.toProto(*proto.mutable_storage_id());
    for (const auto & element : columns)
        element.toProto(*proto.add_columns());
}

std::shared_ptr<TableWriteStep::InsertTarget>
TableWriteStep::InsertTarget::createFromProtoImpl(const Protos::TableWriteStep::InsertTarget & proto, ContextPtr context)
{
    auto storage_id = StorageID::fromProto(proto.storage_id(), context);
    auto storage = DatabaseCatalog::instance().getTable(storage_id, context);
    NamesAndTypes columns;
    for (const auto & proto_element : proto.columns())
    {
        NameAndTypePair element;
        element.fillFromProto(proto_element);
        columns.emplace_back(std::move(element));
    }
    auto step = std::make_shared<TableWriteStep::InsertTarget>(storage, storage_id, columns);

    return step;
}

String TableWriteStep::InsertTarget::toString() const
{
    return "Insert " + storage_id.getNameForLogs();
}

NameToNameMap TableWriteStep::InsertTarget::getTableColumnToInputColumnMap(const Names & input_columns) const
{
    NameToNameMap name_to_name_map;
    if (columns.size() != input_columns.size())
        throw Exception("Number of columns in insert target doesn't match number of columns in input", ErrorCodes::LOGICAL_ERROR);
    for (size_t i = 0; i < input_columns.size(); ++i)
        name_to_name_map.emplace(columns[i].name, input_columns[i]);
    return name_to_name_map;
}
}
