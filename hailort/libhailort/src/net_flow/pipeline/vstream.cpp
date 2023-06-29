/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file vstream.cpp
 * @brief Implementation of the virtual stream
 **/

#include "common/utils.hpp"
#include "hailo/vstream.hpp"
#include "hailo/hailort_defaults.hpp"
#include "hailo/hailort_common.hpp"

#include "common/runtime_statistics_internal.hpp"

#include "net_flow/pipeline/vstream_internal.hpp"

#ifdef HAILO_SUPPORT_MULTI_PROCESS
#include "rpc/rpc_definitions.hpp"
#include "service/rpc_client_utils.hpp"
#endif // HAILO_SUPPORT_MULTI_PROCESS

#include <unordered_set>


namespace hailort
{

static std::map<std::string, AccumulatorPtr> get_pipeline_accumulators_by_type(
    const std::vector<std::shared_ptr<PipelineElement>> &pipeline, AccumulatorType accumulator_type);

static std::map<std::string, std::vector<AccumulatorPtr>> get_pipeline_queue_size_accumulators(
    const std::vector<std::shared_ptr<PipelineElement>> &pipeline);

Expected<std::shared_ptr<PreInferElement>> PreInferElement::create(const hailo_3d_image_shape_t &src_image_shape, const hailo_format_t &src_format,
    const hailo_3d_image_shape_t &dst_image_shape, const hailo_format_t &dst_format, const hailo_quant_info_t &dst_quant_info,
    const std::string &name, std::chrono::milliseconds timeout, size_t buffer_pool_size, hailo_pipeline_elem_stats_flags_t elem_flags,
    hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    auto transform_context = InputTransformContext::create(src_image_shape, src_format, dst_image_shape, dst_format,
        dst_quant_info);
    CHECK_EXPECTED(transform_context, "Failed Creating InputTransformContext");

    auto buffer_pool = BufferPool::create(transform_context.value()->get_dst_frame_size(), buffer_pool_size, shutdown_event, elem_flags,
        vstream_flags);
    CHECK_EXPECTED(buffer_pool, "Failed creating BufferPool for {}", name);

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto pre_infer_elem_ptr = make_shared_nothrow<PreInferElement>(transform_context.release(),
        buffer_pool.release(), name, timeout, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != pre_infer_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", pre_infer_elem_ptr->name());

    return pre_infer_elem_ptr;
}

Expected<std::shared_ptr<PreInferElement>> PreInferElement::create(const hailo_3d_image_shape_t &src_image_shape, const hailo_format_t &src_format,
        const hailo_3d_image_shape_t &dst_image_shape, const hailo_format_t &dst_format, const hailo_quant_info_t &dst_quant_info, const std::string &name,
        const hailo_vstream_params_t &vstream_params, EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    return PreInferElement::create(src_image_shape, src_format, dst_image_shape, dst_format, dst_quant_info, name,
        std::chrono::milliseconds(vstream_params.timeout_ms), vstream_params.queue_size, vstream_params.pipeline_elements_stats_flags,
        vstream_params.vstream_stats_flags, shutdown_event, pipeline_status);
}

PreInferElement::PreInferElement(std::unique_ptr<InputTransformContext> &&transform_context, BufferPoolPtr buffer_pool,
                                const std::string &name, std::chrono::milliseconds timeout, DurationCollector &&duration_collector,
                                std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    FilterElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_transform_context(std::move(transform_context)),
    m_pool(buffer_pool),
    m_timeout(timeout)
{}

Expected<PipelineBuffer> PreInferElement::run_pull(PipelineBuffer &&/*optional*/, const PipelinePad &/*source*/)
{
    LOGGER__ERROR("PreInferElement does not support run_pull operation");
    return make_unexpected(HAILO_INVALID_OPERATION);
}

std::vector<AccumulatorPtr> PreInferElement::get_queue_size_accumulators()
{
    if (nullptr == m_pool->get_queue_size_accumulator()) {
        return std::vector<AccumulatorPtr>();
    }
    return {m_pool->get_queue_size_accumulator()};
}

PipelinePad &PreInferElement::next_pad()
{
    // Note: The next elem to be run is downstream from this elem (i.e. buffers are pushed)
    return *m_sources[0].next();
}

std::string PreInferElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | " << m_transform_context->description() << ")";
    return element_description.str();
}

Expected<PipelineBuffer> PreInferElement::action(PipelineBuffer &&input, PipelineBuffer &&optional)
{
    if (PipelineBuffer::Type::FLUSH == input.get_type()) {
        return std::move(input);
    }

    auto transformed_buffer = m_pool->get_available_buffer(std::move(optional), m_timeout);
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == transformed_buffer.status()) {
        return make_unexpected(transformed_buffer.status());
    }
    CHECK_AS_EXPECTED(HAILO_TIMEOUT != transformed_buffer.status(), HAILO_TIMEOUT,
        "{} (H2D) failed with status={} (timeout={}ms)", name(), HAILO_TIMEOUT, m_timeout.count());
    CHECK_EXPECTED(transformed_buffer);

    auto dst = transformed_buffer->as_view();
    m_duration_collector.start_measurement();
    const auto status = m_transform_context->transform(input.as_view(), dst);
    m_duration_collector.complete_measurement();
    CHECK_SUCCESS_AS_EXPECTED(status);

    // Note: The latency to be measured starts as the input buffer is sent to the InputVStream (via write())
    transformed_buffer->set_metadata(input.get_metadata());

    return transformed_buffer.release();
}

Expected<std::shared_ptr<PostInferElement>> PostInferElement::create(const hailo_3d_image_shape_t &src_image_shape,
    const hailo_format_t &src_format, const hailo_3d_image_shape_t &dst_image_shape, const hailo_format_t &dst_format,
    const hailo_quant_info_t &dst_quant_info, const hailo_nms_info_t &nms_info, const std::string &name,
    hailo_pipeline_elem_stats_flags_t elem_flags, std::shared_ptr<std::atomic<hailo_status>> pipeline_status,
    std::chrono::milliseconds timeout, hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event,
    size_t buffer_pool_size)
{
    auto frame_size = (dst_format.order == HAILO_FORMAT_ORDER_HAILO_NMS) ? HailoRTCommon::get_nms_host_frame_size(nms_info, dst_format) : HailoRTCommon::get_frame_size(dst_image_shape, dst_format);
    auto buffer_pool_expected = BufferPool::create(frame_size, buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
    CHECK_EXPECTED(buffer_pool_expected, "Failed creating BufferPool for {}", name);

    auto transform_context = OutputTransformContext::create(src_image_shape, src_format, dst_image_shape, dst_format,
        dst_quant_info, nms_info);
    CHECK_EXPECTED(transform_context, "Failed Creating OutputTransformContext");

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto post_infer_elem_ptr = make_shared_nothrow<PostInferElement>(transform_context.release(),
        name, duration_collector.release(), std::move(pipeline_status), buffer_pool_expected.release(), timeout);
    CHECK_AS_EXPECTED(nullptr != post_infer_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", post_infer_elem_ptr->name());

    return post_infer_elem_ptr;
}

Expected<std::shared_ptr<PostInferElement>> PostInferElement::create(const hailo_3d_image_shape_t &src_image_shape, const hailo_format_t &src_format,
        const hailo_3d_image_shape_t &dst_image_shape, const hailo_format_t &dst_format, const hailo_quant_info_t &dst_quant_info, const hailo_nms_info_t &nms_info,
        const std::string &name, const hailo_vstream_params_t &vstream_params, std::shared_ptr<std::atomic<hailo_status>> pipeline_status,
        EventPtr shutdown_event)
{
    return PostInferElement::create(src_image_shape, src_format, dst_image_shape, dst_format, dst_quant_info, nms_info,
        name, vstream_params.pipeline_elements_stats_flags, pipeline_status, std::chrono::milliseconds(vstream_params.timeout_ms),
        vstream_params.vstream_stats_flags, shutdown_event, vstream_params.queue_size);
}

PostInferElement::PostInferElement(std::unique_ptr<OutputTransformContext> &&transform_context, const std::string &name,
                                   DurationCollector &&duration_collector,
                                   std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status,
                                   BufferPoolPtr buffer_pool, std::chrono::milliseconds timeout) :
    FilterElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_transform_context(std::move(transform_context)),
    m_pool(buffer_pool),
    m_timeout(timeout)
{}

hailo_status PostInferElement::run_push(PipelineBuffer &&/*buffer*/)
{
    LOGGER__ERROR("PostInferElement does not support run_push operation");
    return HAILO_INVALID_OPERATION;
}

PipelinePad &PostInferElement::next_pad()
{
    // Note: The next elem to be run is upstream from this elem (i.e. buffers are pulled)
    return *m_sinks[0].prev();
}

std::string PostInferElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | " << m_transform_context->description() << ")";
    return element_description.str();
}

Expected<PipelineBuffer> PostInferElement::action(PipelineBuffer &&input, PipelineBuffer &&optional)
{
    auto buffer = m_pool->get_available_buffer(std::move(optional), m_timeout);
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == buffer.status()) {
        return make_unexpected(buffer.status());
    }
    CHECK_EXPECTED(buffer, "{} (D2H) failed with status={}", name(), buffer.status());

    // Note: The latency to be measured starts as the buffer is read from the HW (it's 'input' in this case)
    buffer->set_metadata(input.get_metadata());

    auto dst = buffer->as_view();
    m_duration_collector.start_measurement();
    const auto status = m_transform_context->transform(input.as_view(), dst);
    m_duration_collector.complete_measurement();
    CHECK_SUCCESS_AS_EXPECTED(status);

    return buffer.release();
}

std::vector<AccumulatorPtr> PostInferElement::get_queue_size_accumulators()
{
    if (nullptr == m_pool->get_queue_size_accumulator()) {
        return std::vector<AccumulatorPtr>();
    }
    return {m_pool->get_queue_size_accumulator()};
}

static hailo_nms_info_t fuse_nms_info(const std::vector<hailo_nms_info_t> &nms_infos)
{
    hailo_nms_info_t fused_info = nms_infos[0];
    fused_info.is_defused = false;
    fused_info.number_of_classes = 0;
    for (const auto &nms_info : nms_infos) {
        fused_info.number_of_classes += nms_info.number_of_classes;
    }

    return fused_info;
}

Expected<std::shared_ptr<NmsPostProcessMuxElement>> NmsPostProcessMuxElement::create(std::shared_ptr<net_flow::Op> nms_op,
    hailo_nms_info_t nms_info, const std::string &name, std::chrono::milliseconds timeout, size_t buffer_pool_size,
    hailo_pipeline_elem_stats_flags_t elem_flags, hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    assert(nms_op->outputs_metadata().size() == 1);
    auto buffer_pool = BufferPool::create(HailoRTCommon::get_nms_host_frame_size(nms_info, nms_op->outputs_metadata().begin()->second.format),
        buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
    CHECK_EXPECTED(buffer_pool, "Failed creating BufferPool");

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto nms_elem_ptr = make_shared_nothrow<NmsPostProcessMuxElement>(nms_op, buffer_pool.release(),
        name, timeout, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != nms_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", nms_elem_ptr->name());
    return nms_elem_ptr;
}

Expected<std::shared_ptr<NmsPostProcessMuxElement>> NmsPostProcessMuxElement::create(std::shared_ptr<net_flow::Op> nms_op,
        hailo_nms_info_t nms_info, const std::string &name, const hailo_vstream_params_t &vstream_params,
        EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    return NmsPostProcessMuxElement::create(nms_op, nms_info, name, std::chrono::milliseconds(vstream_params.timeout_ms),
        vstream_params.queue_size, vstream_params.pipeline_elements_stats_flags, vstream_params.vstream_stats_flags, shutdown_event,
        pipeline_status);
}

NmsPostProcessMuxElement::NmsPostProcessMuxElement(std::shared_ptr<net_flow::Op> nms_op, BufferPoolPtr &&pool,
                                                   const std::string &name, std::chrono::milliseconds timeout,
                                                   DurationCollector &&duration_collector,
                                                   std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    BaseMuxElement(nms_op->inputs_metadata().size(), name, timeout, std::move(duration_collector), std::move(pipeline_status)),
    m_nms_op(nms_op),
    m_pool(std::move(pool))
{}

std::vector<AccumulatorPtr> NmsPostProcessMuxElement::get_queue_size_accumulators()
{
    if (nullptr == m_pool->get_queue_size_accumulator()) {
        return std::vector<AccumulatorPtr>();
    }
    return {m_pool->get_queue_size_accumulator()};
}

Expected<PipelineBuffer> NmsPostProcessMuxElement::action(std::vector<PipelineBuffer> &&input_buffers, PipelineBuffer &&optional)
{
    std::map<std::string, MemoryView> inputs;
    std::map<std::string, MemoryView> outputs;
    for (size_t i = 0; i < input_buffers.size(); ++i) {
        inputs.insert({m_sinks_names[i], input_buffers[i].as_view()});
    }
    auto acquired_buffer = m_pool->get_available_buffer(std::move(optional), m_timeout);
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == acquired_buffer.status()) {
        return make_unexpected(acquired_buffer.status());
    }
    CHECK_EXPECTED(acquired_buffer);
    outputs.insert({"", acquired_buffer.value().as_view()}); // TODO: fill with correct name
    m_duration_collector.start_measurement();
    
    auto post_process_result = m_nms_op->execute(inputs, outputs);
    m_duration_collector.complete_measurement();
    CHECK_SUCCESS_AS_EXPECTED(post_process_result);
    return acquired_buffer;
}

Expected<std::shared_ptr<NmsMuxElement>> NmsMuxElement::create(const std::vector<hailo_nms_info_t> &nms_infos,
    const std::string &name, std::chrono::milliseconds timeout, size_t buffer_pool_size,
    hailo_pipeline_elem_stats_flags_t elem_flags, hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    const auto &fused_info = fuse_nms_info(nms_infos);
    auto buffer_pool = BufferPool::create(HailoRTCommon::get_nms_hw_frame_size(fused_info),
        buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
    CHECK_EXPECTED(buffer_pool, "Failed creating BufferPool");

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto nms_elem_ptr = make_shared_nothrow<NmsMuxElement>(nms_infos, fused_info, buffer_pool.release(),
        name, timeout, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != nms_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", nms_elem_ptr->name());

    return nms_elem_ptr;
}

Expected<std::shared_ptr<NmsMuxElement>> NmsMuxElement::create(const std::vector<hailo_nms_info_t> &nms_infos, const std::string &name,
        const hailo_vstream_params_t &vstream_params, EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    return NmsMuxElement::create(nms_infos, name, std::chrono::milliseconds(vstream_params.timeout_ms), vstream_params.queue_size,
        vstream_params.pipeline_elements_stats_flags, vstream_params.vstream_stats_flags, shutdown_event, pipeline_status);
}

NmsMuxElement::NmsMuxElement(const std::vector<hailo_nms_info_t> &nms_infos, const hailo_nms_info_t &fused_nms_info, BufferPoolPtr &&pool,
                             const std::string &name, std::chrono::milliseconds timeout, DurationCollector &&duration_collector,
                             std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    BaseMuxElement(nms_infos.size(), name, timeout, std::move(duration_collector), std::move(pipeline_status)),
    m_nms_infos(nms_infos),
    m_fused_nms_info(fused_nms_info),
    m_pool(std::move(pool))
{}

const hailo_nms_info_t &NmsMuxElement::get_fused_nms_info() const
{
    return m_fused_nms_info;
}

std::vector<AccumulatorPtr> NmsMuxElement::get_queue_size_accumulators()
{
    if (nullptr == m_pool->get_queue_size_accumulator()) {
        return std::vector<AccumulatorPtr>();
    }
    return {m_pool->get_queue_size_accumulator()};
}

Expected<PipelineBuffer> NmsMuxElement::action(std::vector<PipelineBuffer> &&inputs, PipelineBuffer &&optional)
{
    std::vector<MemoryView> input_views;

    input_views.reserve(inputs.size());
    for (auto &input_buf : inputs) {
        input_views.push_back(input_buf.as_view());
    }

    auto acquired_buffer = m_pool->get_available_buffer(std::move(optional), m_timeout);
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == acquired_buffer.status()) {
        return make_unexpected(acquired_buffer.status());
    }
    CHECK_AS_EXPECTED(HAILO_TIMEOUT != acquired_buffer.status(), HAILO_TIMEOUT,
        "{} failed with status={} (timeout={}ms)", name(), HAILO_TIMEOUT, m_timeout.count());
    CHECK_EXPECTED(acquired_buffer);

    m_duration_collector.start_measurement();
    const auto status = fuse_buffers(input_views, m_nms_infos, acquired_buffer.value().as_view());
    m_duration_collector.complete_measurement();
    CHECK_SUCCESS_AS_EXPECTED(status);

    return acquired_buffer.release();
}

Expected<std::shared_ptr<TransformDemuxElement>> TransformDemuxElement::create(std::shared_ptr<OutputDemuxer> demuxer,
    const std::string &name, std::chrono::milliseconds timeout, size_t buffer_pool_size, hailo_pipeline_elem_stats_flags_t elem_flags,
    hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    std::vector<BufferPoolPtr> pools;
    pools.reserve(demuxer->get_edges_stream_info().size());

    for (const auto& mux_edge : demuxer->get_edges_stream_info()) {
        auto buffer_pool = BufferPool::create(mux_edge.hw_frame_size, buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
        CHECK_EXPECTED(buffer_pool, "Failed creating BufferPool");
        pools.push_back(buffer_pool.release());
    }

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto demux_elem_ptr = make_shared_nothrow<TransformDemuxElement>(demuxer, std::move(pools), name, timeout,
        duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != demux_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    return demux_elem_ptr;
}

TransformDemuxElement::TransformDemuxElement(std::shared_ptr<OutputDemuxer> demuxer, std::vector<BufferPoolPtr> &&pools,
                                             const std::string &name, std::chrono::milliseconds timeout,
                                             DurationCollector &&duration_collector,
                                             std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    BaseDemuxElement(demuxer->get_edges_stream_info().size(), name, timeout, std::move(duration_collector),
                     std::move(pipeline_status)),
    m_demuxer(demuxer),
    m_pools(std::move(pools))
{}

std::vector<AccumulatorPtr> TransformDemuxElement::get_queue_size_accumulators()
{
    std::vector<AccumulatorPtr> result;
    for (const auto& pool : m_pools) {
        if (nullptr != pool->get_queue_size_accumulator()) {
            result.emplace_back(pool->get_queue_size_accumulator());
        }
    }
    return result;
}

Expected<std::vector<PipelineBuffer>> TransformDemuxElement::action(PipelineBuffer &&input)
{
    std::vector<PipelineBuffer> outputs;
    std::vector<MemoryView> raw_buffers;

    auto mux_edges = m_demuxer->get_edges_stream_info();
    outputs.reserve(mux_edges.size());
    raw_buffers.reserve(mux_edges.size());

    for (uint32_t i = 0; i < mux_edges.size(); i++) {
        auto acquired_buffer = m_pools[i]->acquire_buffer(m_timeout);
        if (HAILO_SHUTDOWN_EVENT_SIGNALED == acquired_buffer.status()) {
            return make_unexpected(acquired_buffer.status());
        }
        CHECK_EXPECTED(acquired_buffer, "Failed to acquire buffer");
        outputs.emplace_back(acquired_buffer.release());
        
        raw_buffers.push_back(outputs.back().as_view());
    }

    m_duration_collector.start_measurement();
    const auto status = m_demuxer->transform_demux(input.as_view(), raw_buffers);
    m_duration_collector.complete_measurement();
    CHECK_SUCCESS_AS_EXPECTED(status);

    return outputs;
}

Expected<std::shared_ptr<ArgmaxPostProcessElement>> ArgmaxPostProcessElement::create(std::shared_ptr<net_flow::Op> argmax_op,
    const std::string &name, hailo_pipeline_elem_stats_flags_t elem_flags,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);
    auto argmax_elem_ptr = make_shared_nothrow<ArgmaxPostProcessElement>(argmax_op,
        name, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != argmax_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);
    LOGGER__INFO("Created {}", argmax_elem_ptr->name());
    return argmax_elem_ptr;
}

ArgmaxPostProcessElement::ArgmaxPostProcessElement(std::shared_ptr<net_flow::Op> argmax_op, const std::string &name,
                                   DurationCollector &&duration_collector,
                                   std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    FilterElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_argmax_op(argmax_op)
{}

hailo_status ArgmaxPostProcessElement::run_push(PipelineBuffer &&/*buffer*/)
{
    LOGGER__ERROR("ArgmaxPostProcessElement does not support run_push operation");
    return HAILO_INVALID_OPERATION;
}

PipelinePad &ArgmaxPostProcessElement::next_pad()
{
    // Note: The next elem to be run is upstream from this elem (i.e. buffers are pulled)
    return *m_sinks[0].prev();
}

std::string ArgmaxPostProcessElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | " << m_argmax_op->get_op_description() << ")";
    return element_description.str();
}

Expected<PipelineBuffer> ArgmaxPostProcessElement::action(PipelineBuffer &&input, PipelineBuffer &&optional)
{
    std::map<std::string, MemoryView> inputs;
    std::map<std::string, MemoryView> outputs;
    auto &input_name = m_argmax_op->inputs_metadata().begin()->first;
    auto &output_name = m_argmax_op->outputs_metadata().begin()->first;
    inputs.insert({input_name, input.as_view()});
    outputs.insert({output_name, optional.as_view()});
    m_duration_collector.start_measurement();
    auto post_process_result = m_argmax_op->execute(inputs, outputs);
    CHECK_SUCCESS_AS_EXPECTED(post_process_result);
    m_duration_collector.complete_measurement();

    return std::move(optional);
}

Expected<std::shared_ptr<SoftmaxPostProcessElement>> SoftmaxPostProcessElement::create(std::shared_ptr<net_flow::Op> softmax_op,
    const std::string &name, hailo_pipeline_elem_stats_flags_t elem_flags,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);
    auto softmax_elem_ptr = make_shared_nothrow<SoftmaxPostProcessElement>(softmax_op,
        name, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != softmax_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);
    LOGGER__INFO("Created {}", softmax_elem_ptr->name());
    return softmax_elem_ptr;
}

SoftmaxPostProcessElement::SoftmaxPostProcessElement(std::shared_ptr<net_flow::Op> softmax_op, const std::string &name,
                                   DurationCollector &&duration_collector,
                                   std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status) :
    FilterElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_softmax_op(softmax_op)
{}

hailo_status SoftmaxPostProcessElement::run_push(PipelineBuffer &&/*buffer*/)
{
    LOGGER__ERROR("SoftmaxPostProcessElement does not support run_push operation");
    return HAILO_INVALID_OPERATION;
}

PipelinePad &SoftmaxPostProcessElement::next_pad()
{
    // Note: The next elem to be run is upstream from this elem (i.e. buffers are pulled)
    return *m_sinks[0].prev();
}

std::string SoftmaxPostProcessElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | " << m_softmax_op->get_op_description() << ")";
    return element_description.str();
}

Expected<PipelineBuffer> SoftmaxPostProcessElement::action(PipelineBuffer &&input, PipelineBuffer &&optional)
{
    std::map<std::string, MemoryView> inputs;
    std::map<std::string, MemoryView> outputs;
    auto &input_name = m_softmax_op->inputs_metadata().begin()->first;
    auto &output_name = m_softmax_op->outputs_metadata().begin()->first;
    inputs.insert({input_name, input.as_view()});
    outputs.insert({output_name, optional.as_view()});
    m_duration_collector.start_measurement();
    auto post_process_result = m_softmax_op->execute(inputs, outputs);
    CHECK_SUCCESS_AS_EXPECTED(post_process_result);
    m_duration_collector.complete_measurement();

    return std::move(optional);
}

BaseVStream::BaseVStream(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
                         std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
                         std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status,
                         EventPtr shutdown_event, AccumulatorPtr pipeline_latency_accumulator, EventPtr &&core_op_activated_event,
                         hailo_status &output_status) :
    m_vstream_info(vstream_info),
    m_vstream_params(vstream_params),
    m_measure_pipeline_latency((vstream_params.vstream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_LATENCY) != 0),
    m_entry_element(pipeline_entry),
    m_pipeline(std::move(pipeline)),
    m_is_activated(false),
    m_is_aborted(false),
    m_pipeline_status(std::move(pipeline_status)),
    m_shutdown_event(shutdown_event),
    m_core_op_activated_event(std::move(core_op_activated_event)),
    m_fps_accumulators(get_pipeline_accumulators_by_type(m_pipeline, AccumulatorType::FPS)),
    m_latency_accumulators(get_pipeline_accumulators_by_type(m_pipeline, AccumulatorType::LATENCY)),
    m_queue_size_accumulators(get_pipeline_queue_size_accumulators(m_pipeline)),
    m_pipeline_latency_accumulator(pipeline_latency_accumulator)
{
    output_status = start_vstream();
}

BaseVStream::BaseVStream(BaseVStream &&other) noexcept :
    m_vstream_info(std::move(other.m_vstream_info)),
    m_vstream_params(std::move(other.m_vstream_params)),
    m_measure_pipeline_latency(std::move(other.m_measure_pipeline_latency)),
    m_entry_element(std::move(other.m_entry_element)),
    m_pipeline(std::move(other.m_pipeline)),
    m_is_activated(std::exchange(other.m_is_activated, false)),
    m_is_aborted(std::exchange(other.m_is_aborted, false)),
    m_pipeline_status(std::move(other.m_pipeline_status)),
    m_shutdown_event(std::move(other.m_shutdown_event)),
    m_core_op_activated_event(std::move(other.m_core_op_activated_event)),
    m_fps_accumulators(std::move(other.m_fps_accumulators)),
    m_latency_accumulators(std::move(other.m_latency_accumulators)),
    m_queue_size_accumulators(std::move(other.m_queue_size_accumulators)),
    m_pipeline_latency_accumulator(std::move(other.m_pipeline_latency_accumulator))
{}

BaseVStream& BaseVStream::operator=(BaseVStream &&other) noexcept
{
    if (this != &other) {
        // operator= is used only for vstream creation BEFORE activation. otherwise we should deactivate vstream here
        assert(!m_is_activated);
        m_vstream_info = std::move(other.m_vstream_info);
        m_vstream_params = std::move(other.m_vstream_params);
        m_measure_pipeline_latency = std::move(other.m_measure_pipeline_latency);
        m_entry_element = std::move(other.m_entry_element);
        m_pipeline = std::move(other.m_pipeline);
        m_is_activated = std::exchange(other.m_is_activated, false);
        m_is_aborted = std::exchange(other.m_is_aborted, false);
        m_pipeline_status = std::move(other.m_pipeline_status);
        m_shutdown_event = std::move(other.m_shutdown_event);
        m_core_op_activated_event = std::move(other.m_core_op_activated_event);
        m_fps_accumulators = std::move(other.m_fps_accumulators);
        m_latency_accumulators = std::move(other.m_latency_accumulators);
        m_queue_size_accumulators = std::move(other.m_queue_size_accumulators);
        m_pipeline_latency_accumulator = std::move(other.m_pipeline_latency_accumulator);
    }
    return *this;
}

hailo_status BaseVStream::start_vstream()
{
    auto status = m_shutdown_event->reset();
    CHECK_SUCCESS(status);

    status = resume();
    CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
        "Failed to resume stream in {}", name());

    LOGGER__DEBUG("Activating {}...", name());
    status = m_entry_element->activate();
    CHECK_SUCCESS(status);

    m_is_activated = true;
    return HAILO_SUCCESS;
}

hailo_status BaseVStream::abort()
{
    auto status = m_entry_element->abort();
    CHECK_SUCCESS(status);
    m_is_aborted = true;

    return HAILO_SUCCESS;
}

hailo_status BaseVStream::resume()
{
    auto status = m_entry_element->clear_abort();
    CHECK_SUCCESS(status);
    m_is_aborted = false;

    if (m_is_activated) {
        status = m_entry_element->activate();
        CHECK_SUCCESS(status);
    }
    return HAILO_SUCCESS;
}

hailo_status BaseVStream::stop_vstream()
{
    hailo_status status = HAILO_SUCCESS;
    if (m_is_activated) {
        m_is_activated = false;
        status = m_entry_element->deactivate();
        if (HAILO_SUCCESS != status) {
            LOGGER__WARNING("Failed deactivate of vstream {} status {}", name(), status);
        }

        // If VStream was aborted, do not clear low-level stream abortion,
        // otherwise flush would be called on low-level stream d-tor when there is no receiver.
        auto should_clear_abort = (!m_is_aborted);
        status = m_entry_element->post_deactivate(should_clear_abort);
        if (HAILO_SUCCESS != status) {
            LOGGER__WARNING("Failed post deactivate of vstream {} status {}", name(), status);
        }
    }
    return status;
}

hailo_status BaseVStream::stop_and_clear()
{
    auto status = HAILO_SUCCESS;
    if (nullptr != m_core_op_activated_event) {
        status = m_core_op_activated_event->wait(std::chrono::milliseconds(0));
        CHECK(HAILO_TIMEOUT == status, HAILO_INVALID_OPERATION,
            "Trying to clear {} vstream before its network group is deactivated", name());
    }

    status = stop_vstream();
    CHECK_SUCCESS(status);

    status = m_entry_element->clear();
    CHECK_SUCCESS(status, "Failed clearing vstream {}", name());

    const auto curr_pipeline_status = m_pipeline_status->load();
    if (HAILO_SUCCESS != curr_pipeline_status) {
        LOGGER__TRACE("Overwritting current pipeline status {}", curr_pipeline_status);
        m_pipeline_status->store(HAILO_SUCCESS);
    }

    return status;
}

size_t BaseVStream::get_frame_size() const
{
    if (HAILO_FORMAT_ORDER_HAILO_NMS == m_vstream_info.format.order) {
        return HailoRTCommon::get_nms_host_frame_size(m_vstream_info.nms_shape, m_vstream_params.user_buffer_format);
    }
    return HailoRTCommon::get_frame_size(m_vstream_info.shape, m_vstream_params.user_buffer_format);
}

const hailo_vstream_info_t &BaseVStream::get_info() const
{
    return m_vstream_info;
}

const hailo_format_t &BaseVStream::get_user_buffer_format() const
{
    return m_vstream_params.user_buffer_format;
}

std::string BaseVStream::name() const
{
    return std::string(m_vstream_info.name);
}

std::string BaseVStream::network_name() const
{
    return std::string(m_vstream_info.network_name);
}

const std::map<std::string, AccumulatorPtr> &BaseVStream::get_fps_accumulators() const
{
    return m_fps_accumulators;
}

const std::map<std::string, AccumulatorPtr> &BaseVStream::get_latency_accumulators() const
{
    return m_latency_accumulators;
}

const std::map<std::string, std::vector<AccumulatorPtr>> &BaseVStream::get_queue_size_accumulators() const
{
    return m_queue_size_accumulators;
}

AccumulatorPtr BaseVStream::get_pipeline_latency_accumulator() const
{
    return m_pipeline_latency_accumulator;
}


const std::vector<std::shared_ptr<PipelineElement>> &BaseVStream::get_pipeline() const
{
    return m_pipeline;
}

Expected<InputVStream> InputVStream::create(const hailo_vstream_info_t &vstream_info,
        const hailo_vstream_params_t &vstream_params, std::shared_ptr<PipelineElement> pipeline_entry,
        std::shared_ptr<SinkElement> pipeline_exit, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
        std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event, EventPtr core_op_activated_event,
        AccumulatorPtr pipeline_latency_accumulator)
{
    auto vstream_internal = InputVStreamInternal::create(vstream_info, vstream_params, pipeline_entry, pipeline_exit,
        std::move(pipeline), std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator);
    CHECK_EXPECTED(vstream_internal);

    InputVStream vstream(vstream_internal.release());
    return vstream;
}

hailo_status InputVStream::write(const MemoryView &buffer)
{
    return m_vstream->write(std::move(buffer));
}

hailo_status InputVStream::flush()
{
    return m_vstream->flush();
}

hailo_status InputVStream::clear(std::vector<InputVStream> &vstreams)
{
    for (auto &vstream : vstreams) {
        auto status = vstream.stop_and_clear();
        CHECK_SUCCESS(status);
    }
    for (auto &vstream : vstreams) {
        auto status = vstream.start_vstream();
        CHECK_SUCCESS(status);
    }

    return HAILO_SUCCESS;
}

hailo_status InputVStream::clear(std::vector<std::reference_wrapper<InputVStream>> &vstreams)
{
    for (auto &vstream : vstreams) {
        auto status = vstream.get().stop_and_clear();
        CHECK_SUCCESS(status);
    }
    for (auto &vstream : vstreams) {
        auto status = vstream.get().start_vstream();
        CHECK_SUCCESS(status);
    }

    return HAILO_SUCCESS;
}

hailo_status InputVStream::abort()
{
    return m_vstream->abort();
}

hailo_status InputVStream::resume()
{
    return m_vstream->resume();
}

size_t InputVStream::get_frame_size() const
{
    return m_vstream->get_frame_size();
}

const hailo_vstream_info_t &InputVStream::get_info() const
{
    return m_vstream->get_info();
}

const hailo_format_t &InputVStream::get_user_buffer_format() const
{
    return m_vstream->get_user_buffer_format();
}

std::string InputVStream::name() const
{
    return m_vstream->name();
}

std::string InputVStream::network_name() const
{
    return m_vstream->network_name();
}

const std::map<std::string, AccumulatorPtr> &InputVStream::get_fps_accumulators() const
{
    return m_vstream->get_fps_accumulators();
}

const std::map<std::string, AccumulatorPtr> &InputVStream::get_latency_accumulators() const
{
    return m_vstream->get_latency_accumulators();
}

const std::map<std::string, std::vector<AccumulatorPtr>> &InputVStream::get_queue_size_accumulators() const
{
    return m_vstream->get_queue_size_accumulators();
}

AccumulatorPtr InputVStream::get_pipeline_latency_accumulator() const
{
    return m_vstream->get_pipeline_latency_accumulator();
}

const std::vector<std::shared_ptr<PipelineElement>> &InputVStream::get_pipeline() const
{
    return m_vstream->get_pipeline();
}

hailo_status InputVStream::start_vstream()
{
    return m_vstream->start_vstream();
}

hailo_status InputVStream::stop_vstream()
{
    return m_vstream->stop_vstream();
}

hailo_status InputVStream::stop_and_clear()
{
    return m_vstream->stop_and_clear();
}

std::string InputVStream::get_pipeline_description() const
{
    return m_vstream->get_pipeline_description();
}

hailo_status InputVStream::before_fork()
{
    return m_vstream->before_fork();
}

hailo_status InputVStream::after_fork_in_parent()
{
    return m_vstream->after_fork_in_parent();
}

hailo_status InputVStream::after_fork_in_child()
{
    return m_vstream->after_fork_in_child();
}

bool InputVStream::is_aborted()
{
    return m_vstream->is_aborted();
}

InputVStream::InputVStream(std::shared_ptr<InputVStreamInternal> vstream) : m_vstream(std::move(vstream)) {}

Expected<OutputVStream> OutputVStream::create(
        const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
        std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
        std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event,
        EventPtr core_op_activated_event, AccumulatorPtr pipeline_latency_accumulator)
{
    auto vstream_internal = OutputVStreamInternal::create(vstream_info, vstream_params, pipeline_entry,
        std::move(pipeline), std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator);
    CHECK_EXPECTED(vstream_internal);

    OutputVStream vstream(vstream_internal.release());
    return vstream;
}

hailo_status OutputVStream::read(MemoryView buffer)
{
    return m_vstream->read(std::move(buffer));
}

hailo_status OutputVStream::clear(std::vector<OutputVStream> &vstreams)
{
    for (auto &vstream : vstreams) {
        auto status = vstream.stop_and_clear();
        CHECK_SUCCESS(status);
    }
    for (auto &vstream : vstreams) {
        auto status = vstream.start_vstream();
        CHECK_SUCCESS(status);
    }

    return HAILO_SUCCESS;
}

hailo_status OutputVStream::abort()
{
    return m_vstream->abort();
}

hailo_status OutputVStream::resume()
{
    return m_vstream->resume();
}

hailo_status OutputVStream::clear(std::vector<std::reference_wrapper<OutputVStream>> &vstreams)
{
    for (auto &vstream : vstreams) {
        auto status = vstream.get().stop_and_clear();
        CHECK_SUCCESS(status);
    }
    for (auto &vstream : vstreams) {
        auto status = vstream.get().start_vstream();
        CHECK_SUCCESS(status);
    }

    return HAILO_SUCCESS;
}

size_t OutputVStream::get_frame_size() const
{
    return m_vstream->get_frame_size();
}

const hailo_vstream_info_t &OutputVStream::get_info() const
{
    return m_vstream->get_info();
}

const hailo_format_t &OutputVStream::get_user_buffer_format() const
{
    return m_vstream->get_user_buffer_format();
}

std::string OutputVStream::name() const
{
    return m_vstream->name();
}

std::string OutputVStream::network_name() const
{
    return m_vstream->network_name();
}

const std::map<std::string, AccumulatorPtr> &OutputVStream::get_fps_accumulators() const
{
    return m_vstream->get_fps_accumulators();
}

const std::map<std::string, AccumulatorPtr> &OutputVStream::get_latency_accumulators() const
{
    return m_vstream->get_latency_accumulators();
}

const std::map<std::string, std::vector<AccumulatorPtr>> &OutputVStream::get_queue_size_accumulators() const
{
    return m_vstream->get_queue_size_accumulators();
}

AccumulatorPtr OutputVStream::get_pipeline_latency_accumulator() const
{
    return m_vstream->get_pipeline_latency_accumulator();
}

const std::vector<std::shared_ptr<PipelineElement>> &OutputVStream::get_pipeline() const
{
    return m_vstream->get_pipeline();
}

hailo_status OutputVStream::start_vstream()
{
    return m_vstream->start_vstream();
}

hailo_status OutputVStream::stop_vstream()
{
    return m_vstream->stop_vstream();
}

hailo_status OutputVStream::stop_and_clear()
{
    return m_vstream->stop_and_clear();
}

std::string OutputVStream::get_pipeline_description() const
{
    return m_vstream->get_pipeline_description();
}

hailo_status OutputVStream::before_fork()
{
    return m_vstream->before_fork();
}

hailo_status OutputVStream::after_fork_in_parent()
{
    return m_vstream->after_fork_in_parent();
}

hailo_status OutputVStream::after_fork_in_child()
{
    return m_vstream->after_fork_in_child();
}

bool OutputVStream::is_aborted()
{
    return m_vstream->is_aborted();
}

OutputVStream::OutputVStream(std::shared_ptr<OutputVStreamInternal> vstream) : m_vstream(std::move(vstream)) {}

std::map<std::string, AccumulatorPtr> get_pipeline_accumulators_by_type(
    const std::vector<std::shared_ptr<PipelineElement>> &pipeline, AccumulatorType accumulator_type)
{
    std::map<std::string, AccumulatorPtr> result;
    for (const auto &elem : pipeline) {
        if (nullptr == elem) {
            continue;
        }

        AccumulatorPtr accumulator = nullptr;
        if (AccumulatorType::FPS == accumulator_type) {
            accumulator = elem->get_fps_accumulator();
        } else if (AccumulatorType::LATENCY == accumulator_type) {
            accumulator = elem->get_latency_accumulator();
        } else {
            continue;
        }

        if (nullptr != accumulator) {
            result.emplace(elem->name(), accumulator);
        }
    }

    return result;
}

std::map<std::string, std::vector<AccumulatorPtr>> get_pipeline_queue_size_accumulators(
    const std::vector<std::shared_ptr<PipelineElement>> &pipeline)
{
    std::map<std::string, std::vector<AccumulatorPtr>> result;
    for (const auto &elem : pipeline) {
        if (nullptr == elem) {
            continue;
        }

        const auto accumulators = elem->get_queue_size_accumulators();
        if (0 != accumulators.size()) {
            result.emplace(elem->name(), accumulators);
        }
    }

    return result;
}

Expected<std::shared_ptr<InputVStreamInternal>> InputVStreamInternal::create(const hailo_vstream_info_t &vstream_info,
    const hailo_vstream_params_t &vstream_params, std::shared_ptr<PipelineElement> pipeline_entry,
    std::shared_ptr<SinkElement> pipeline_exit, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
    std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event, EventPtr core_op_activated_event,
    AccumulatorPtr pipeline_latency_accumulator)
{
    auto vstream = InputVStreamImpl::create(vstream_info, vstream_params, pipeline_entry, pipeline_exit,
        std::move(pipeline), std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator);
    CHECK_EXPECTED(vstream);
    auto vstream_ptr = std::shared_ptr<InputVStreamInternal>(vstream.release());
    return vstream_ptr;
}

InputVStreamInternal::InputVStreamInternal(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
                         std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
                         std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status,
                         EventPtr shutdown_event, AccumulatorPtr pipeline_latency_accumulator, EventPtr &&core_op_activated_event,
                         hailo_status &output_status) :
    BaseVStream(vstream_info, vstream_params, pipeline_entry, std::move(pipeline), std::move(pipeline_status),
                shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), output_status){}

Expected<std::shared_ptr<InputVStreamImpl>> InputVStreamImpl::create(const hailo_vstream_info_t &vstream_info,
    const hailo_vstream_params_t &vstream_params, std::shared_ptr<PipelineElement> pipeline_entry,
    std::shared_ptr<SinkElement> pipeline_exit, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
    std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event, EventPtr core_op_activated_event,
    AccumulatorPtr pipeline_latency_accumulator)
{
    hailo_status status = HAILO_UNINITIALIZED;

    if (nullptr != pipeline_latency_accumulator) {
        pipeline_exit->sink().set_push_complete_callback([pipeline_latency_accumulator](const PipelineBuffer::Metadata& metadata) {
                const auto duration_sec = std::chrono::duration_cast<std::chrono::duration<double>>(
                    std::chrono::steady_clock::now() - metadata.get_start_time()).count();
                pipeline_latency_accumulator->add_data_point(duration_sec);
            });
    }

    auto vstream_ptr = std::shared_ptr<InputVStreamImpl>(new InputVStreamImpl(vstream_info, vstream_params, std::move(pipeline_entry), std::move(pipeline),
        std::move(pipeline_status), shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), status));
    CHECK_SUCCESS_AS_EXPECTED(status, "Failed to create virtual stream");

    return vstream_ptr;
}

InputVStreamImpl::InputVStreamImpl(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
    std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
    std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event, AccumulatorPtr pipeline_latency_accumulator,
    EventPtr core_op_activated_event, hailo_status &output_status) :
    InputVStreamInternal(vstream_info, vstream_params, pipeline_entry, std::move(pipeline), std::move(pipeline_status),
        shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), output_status)
{
    if (HAILO_SUCCESS != output_status) {
        return;
    }
    LOGGER__INFO("Creating {}...", name());
}

InputVStreamImpl::~InputVStreamImpl()
{
    (void)stop_vstream();
}

hailo_status InputVStreamImpl::write(const MemoryView &buffer)
{
    if (nullptr != m_core_op_activated_event) {
        CHECK(m_is_activated, HAILO_VSTREAM_PIPELINE_NOT_ACTIVATED, "Failed to write buffer! Virtual stream {} is not activated!", name());
        auto status = m_core_op_activated_event->wait(std::chrono::milliseconds(0));
        CHECK(HAILO_TIMEOUT != status, HAILO_NETWORK_GROUP_NOT_ACTIVATED,
            "Trying to write to vstream {} before its network group is activated", name());
    }

    auto status = m_entry_element->run_push(PipelineBuffer(buffer, m_measure_pipeline_latency));
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == status) {
        LOGGER__INFO("Sending to VStream was shutdown!");
        status = m_pipeline_status->load();
    }
    if (HAILO_STREAM_ABORTED_BY_USER == status) {
        LOGGER__INFO("Sending to VStream was aborted!");
        return HAILO_STREAM_ABORTED_BY_USER;
    }
    return status;
}

hailo_status InputVStreamImpl::flush()
{
    auto status = m_entry_element->run_push(PipelineBuffer(PipelineBuffer::Type::FLUSH));
    CHECK_SUCCESS(status);

    status = m_entry_element->flush();
    CHECK_SUCCESS(status);

    return HAILO_SUCCESS;
}

#ifdef HAILO_SUPPORT_MULTI_PROCESS
Expected<std::shared_ptr<InputVStreamClient>> InputVStreamClient::create(uint32_t input_vstream_handle)
{
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxReceiveMessageSize(-1);
    auto channel = grpc::CreateCustomChannel(HAILORT_SERVICE_DEFAULT_ADDR, grpc::InsecureChannelCredentials(), ch_args);
    CHECK_AS_EXPECTED(channel != nullptr, HAILO_INTERNAL_FAILURE);

    auto client = make_unique_nothrow<HailoRtRpcClient>(channel);
    CHECK_AS_EXPECTED(client != nullptr, HAILO_OUT_OF_HOST_MEMORY);

    auto user_buffer_format = client->InputVStream_get_user_buffer_format(input_vstream_handle);
    CHECK_EXPECTED(user_buffer_format);

    auto vstream_info = client->InputVStream_get_info(input_vstream_handle);
    CHECK_EXPECTED(vstream_info);

    return std::shared_ptr<InputVStreamClient>(new InputVStreamClient(std::move(client), std::move(input_vstream_handle),
        user_buffer_format.release(), vstream_info.release()));
}

InputVStreamClient::InputVStreamClient(std::unique_ptr<HailoRtRpcClient> client, uint32_t input_vstream_handle, hailo_format_t &&user_buffer_format, 
    hailo_vstream_info_t &&info)
    : m_client(std::move(client)), m_handle(std::move(input_vstream_handle)), m_user_buffer_format(user_buffer_format), m_info(info) {}

InputVStreamClient::~InputVStreamClient()
{
    auto reply = m_client->InputVStream_release(m_handle, OsUtils::get_curr_pid());
    if (reply != HAILO_SUCCESS) {
        LOGGER__CRITICAL("InputVStream_release failed!");
    }
}

hailo_status InputVStreamClient::write(const MemoryView &buffer)
{
    return m_client->InputVStream_write(m_handle, buffer);
}

hailo_status InputVStreamClient::flush()
{
    return m_client->InputVStream_flush(m_handle);
}

hailo_status InputVStreamClient::abort()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto abort_client = expected_client.release();
    return abort_client->InputVStream_abort(m_handle);
}

hailo_status InputVStreamClient::resume()
{
    return m_client->InputVStream_resume(m_handle);
}

hailo_status InputVStreamClient::stop_and_clear()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto stop_and_clear_client = expected_client.release();

    return stop_and_clear_client->InputVStream_stop_and_clear(m_handle);
}

hailo_status InputVStreamClient::start_vstream()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto start_vstream_client = expected_client.release();

    return start_vstream_client->InputVStream_start_vstream(m_handle);
}

size_t InputVStreamClient::get_frame_size() const
{
    auto frame_size = m_client->InputVStream_get_frame_size(m_handle);
    if (!frame_size) {
        LOGGER__CRITICAL("InputVStream_get_frame_size failed with status={}", frame_size.status());
        return 0;
    }
    return frame_size.release();
}

const hailo_vstream_info_t &InputVStreamClient::get_info() const
{
    return m_info;
}

const hailo_format_t &InputVStreamClient::get_user_buffer_format() const
{
    return m_user_buffer_format;
}

std::string InputVStreamClient::name() const
{
    auto expected_name = m_client->InputVStream_name(m_handle);
    if (!expected_name) {
        LOGGER__CRITICAL("InputVStream_name failed with status={}", expected_name.status());
        return "";
    }
    return expected_name.release();
}

std::string InputVStreamClient::network_name() const
{
    auto expected_name = m_client->InputVStream_network_name(m_handle);
    if (!expected_name) {
        LOGGER__CRITICAL("InputVStream_name failed with status={}", expected_name.status());
        return "";
    }
    return expected_name.release();
}

const std::map<std::string, AccumulatorPtr> &InputVStreamClient::get_fps_accumulators() const
{
    LOGGER__ERROR("InputVStream::get_fps_accumulators function is not supported when using multi-process service");
    return m_fps_accumulators;
}
const std::map<std::string, AccumulatorPtr> &InputVStreamClient::get_latency_accumulators() const
{
    LOGGER__ERROR("InputVStream::get_latency_accumulators function is not supported when using multi-process service");
    return m_latency_accumulators;
}

const std::map<std::string, std::vector<AccumulatorPtr>> &InputVStreamClient::get_queue_size_accumulators() const
{
    LOGGER__ERROR("InputVStream::get_queue_size_accumulators function is not supported when using multi-process service");
    return m_queue_size_accumulators;
}
AccumulatorPtr InputVStreamClient::get_pipeline_latency_accumulator() const
{
    LOGGER__ERROR("InputVStream::get_pipeline_latency_accumulator function is not supported when using multi-process service");
    return m_pipeline_latency_accumulator;
}
const std::vector<std::shared_ptr<PipelineElement>> &InputVStreamClient::get_pipeline() const
{
    LOGGER__ERROR("InputVStream::get_pipeline function is not supported when using multi-process service");
    return m_pipeline;
}

hailo_status InputVStreamClient::create_client()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    m_client = expected_client.release();
    return HAILO_SUCCESS;
}

hailo_status InputVStreamClient::before_fork()
{
    m_client.reset();
    return HAILO_SUCCESS;
}

hailo_status InputVStreamClient::after_fork_in_parent()
{
    return create_client();
}

hailo_status InputVStreamClient::after_fork_in_child()
{
    auto status = create_client();
    CHECK_SUCCESS(status);
    auto expected_dup_handle = m_client->InputVStream_dup_handle(OsUtils::get_curr_pid(), m_handle);
    CHECK_EXPECTED_AS_STATUS(expected_dup_handle);
    m_handle = expected_dup_handle.value();
    return HAILO_SUCCESS;
}

bool InputVStreamClient::is_aborted()
{
    auto is_aborted_exp = m_client->InputVStream_is_aborted(m_handle);
    if (!is_aborted_exp) {
        LOGGER__CRITICAL("InputVStream_is_aborted failed with status={}", is_aborted_exp.status());
        return true;
    }
    return is_aborted_exp.release();
}

#endif // HAILO_SUPPORT_MULTI_PROCESS

std::string InputVStreamInternal::get_pipeline_description() const
{
    std::stringstream pipeline_str;
    pipeline_str << "Input pipeline '" << name() << "': ";
    for (const auto &element : m_pipeline) {
        pipeline_str << element->description() << " >> ";
    }
    pipeline_str << "HW";
    return pipeline_str.str();
}

Expected<std::shared_ptr<OutputVStreamInternal>> OutputVStreamInternal::create(
        const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
        std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
        std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event,
        EventPtr core_op_activated_event, AccumulatorPtr pipeline_latency_accumulator)
{
    auto vstream = OutputVStreamImpl::create(vstream_info, vstream_params, pipeline_entry,
        std::move(pipeline), std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator);
    CHECK_EXPECTED(vstream);
    auto vstream_ptr = std::shared_ptr<OutputVStreamInternal>(vstream.release());
    return vstream_ptr;
}

OutputVStreamInternal::OutputVStreamInternal(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
                                             std::shared_ptr<PipelineElement> pipeline_entry,
                                             std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
                                             std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event,
                                             AccumulatorPtr pipeline_latency_accumulator,
                                             EventPtr core_op_activated_event, hailo_status &output_status) :
    BaseVStream(vstream_info, vstream_params, pipeline_entry, std::move(pipeline), std::move(pipeline_status),
                shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), output_status){}

Expected<std::shared_ptr<OutputVStreamImpl>> OutputVStreamImpl::create(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
    std::shared_ptr<PipelineElement> pipeline_entry, std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
    std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event,
    EventPtr core_op_activated_event, AccumulatorPtr pipeline_latency_accumulator)
{
    hailo_status status = HAILO_UNINITIALIZED;

    CHECK_AS_EXPECTED(1 == pipeline_entry->sources().size(), HAILO_INVALID_ARGUMENT,
        "OutputVStream's entry element is expected to have one source");

    if (nullptr != pipeline_latency_accumulator) {
        pipeline_entry->sources()[0].set_pull_complete_callback([pipeline_latency_accumulator](const PipelineBuffer::Metadata& metadata) {
                const auto duration_sec = std::chrono::duration_cast<std::chrono::duration<double>>(
                    std::chrono::steady_clock::now() - metadata.get_start_time()).count();
                pipeline_latency_accumulator->add_data_point(duration_sec);
            });
    }

    auto vstream_ptr = std::shared_ptr<OutputVStreamImpl>(new OutputVStreamImpl(vstream_info, vstream_params, std::move(pipeline_entry), std::move(pipeline),
        std::move(pipeline_status), shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), status));
    CHECK_SUCCESS_AS_EXPECTED(status, "Failed to create virtual stream");

    return vstream_ptr;
}

std::string OutputVStreamInternal::get_pipeline_description() const
{
    std::stringstream pipeline_str;
    pipeline_str << "Output pipeline '" << name() << "': HW";
    for (const auto &element : m_pipeline) {
        pipeline_str << " >> " << element->description();
    }
    return pipeline_str.str();
}

OutputVStreamImpl::OutputVStreamImpl(const hailo_vstream_info_t &vstream_info, const hailo_vstream_params_t &vstream_params,
                                     std::shared_ptr<PipelineElement> pipeline_entry,
                                     std::vector<std::shared_ptr<PipelineElement>> &&pipeline,
                                     std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr shutdown_event,
                                     AccumulatorPtr pipeline_latency_accumulator,
                                     EventPtr core_op_activated_event, hailo_status &output_status) :
    OutputVStreamInternal(vstream_info, vstream_params, pipeline_entry, std::move(pipeline), std::move(pipeline_status),
                shutdown_event, pipeline_latency_accumulator, std::move(core_op_activated_event), output_status)
{
    if (HAILO_SUCCESS != output_status) {
        return;
    }

    for (auto &element : m_pipeline) {
        element->set_on_cant_pull_callback([this] () {
            if (m_cant_read_callback) {
                m_cant_read_callback();
            }
        });
        element->set_on_can_pull_callback([this] () {
            if (m_can_read_callback) {
                m_can_read_callback();
            }
        });
    }

    LOGGER__INFO("Creating {}...", name());
}

OutputVStreamImpl::~OutputVStreamImpl()
{
    (void)stop_vstream();
}

hailo_status OutputVStreamImpl::read(MemoryView buffer)
{
    if (nullptr != m_core_op_activated_event) {
        CHECK(m_is_activated, HAILO_VSTREAM_PIPELINE_NOT_ACTIVATED, "read() failed! Virtual stream {} is not activated!", name());
        auto status = m_core_op_activated_event->wait(std::chrono::milliseconds(0));
        if (HAILO_TIMEOUT == status) {
            LOGGER__INFO("Trying to read from vstream {} before its network_group is activated", name());
            return HAILO_NETWORK_GROUP_NOT_ACTIVATED;
        }
        CHECK_SUCCESS(status);
    }

    assert(1 == m_entry_element->sources().size());
    auto recv_buffer = m_entry_element->sources()[0].run_pull(PipelineBuffer(buffer, m_measure_pipeline_latency));
    auto status = recv_buffer.status();
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == status) {
        LOGGER__INFO("Receiving to VStream was shutdown!");
        status = m_pipeline_status->load();
    }
    if (HAILO_STREAM_ABORTED_BY_USER == status) {
        LOGGER__INFO("Receiving to VStream was aborted!");
        m_entry_element->wait_for_finish();
        return HAILO_STREAM_ABORTED_BY_USER;
    }
    return status;
}

#ifdef HAILO_SUPPORT_MULTI_PROCESS
Expected<std::shared_ptr<OutputVStreamClient>> OutputVStreamClient::create(uint32_t outputs_vstream_handle)
{
    grpc::ChannelArguments ch_args;
    ch_args.SetMaxReceiveMessageSize(-1);
    auto channel = grpc::CreateCustomChannel(HAILORT_SERVICE_DEFAULT_ADDR, grpc::InsecureChannelCredentials(), ch_args);
    CHECK_AS_EXPECTED(channel != nullptr, HAILO_INTERNAL_FAILURE);

    auto client = make_unique_nothrow<HailoRtRpcClient>(channel);
    CHECK_AS_EXPECTED(client != nullptr, HAILO_OUT_OF_HOST_MEMORY);

    auto user_buffer_format = client->OutputVStream_get_user_buffer_format(outputs_vstream_handle);
    CHECK_EXPECTED(user_buffer_format);

    auto info = client->OutputVStream_get_info(outputs_vstream_handle);
    CHECK_EXPECTED(info);

    return std::shared_ptr<OutputVStreamClient>(new OutputVStreamClient(std::move(client), std::move(outputs_vstream_handle),
        user_buffer_format.release(), info.release()));
}

OutputVStreamClient::OutputVStreamClient(std::unique_ptr<HailoRtRpcClient> client, uint32_t outputs_vstream_handle, hailo_format_t &&user_buffer_format,
    hailo_vstream_info_t &&info)
    : m_client(std::move(client)), m_handle(std::move(outputs_vstream_handle)), m_user_buffer_format(user_buffer_format), m_info(info) {}

OutputVStreamClient::~OutputVStreamClient()
{
    auto reply = m_client->OutputVStream_release(m_handle, OsUtils::get_curr_pid());
    if (reply != HAILO_SUCCESS) {
        LOGGER__CRITICAL("OutputVStream_release failed!");
    }
}

hailo_status OutputVStreamClient::read(MemoryView buffer)
{
    return m_client->OutputVStream_read(m_handle, buffer);
}

hailo_status OutputVStreamClient::abort()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto abort_client = expected_client.release();
    return abort_client->OutputVStream_abort(m_handle);
}

hailo_status OutputVStreamClient::resume()
{
    return m_client->OutputVStream_resume(m_handle);
}

hailo_status OutputVStreamClient::stop_and_clear()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto stop_and_clear_client = expected_client.release();

    return stop_and_clear_client->OutputVStream_stop_and_clear(m_handle);
}

hailo_status OutputVStreamClient::start_vstream()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    auto start_vstream_client = expected_client.release();

    return start_vstream_client->OutputVStream_start_vstream(m_handle);
}

size_t OutputVStreamClient::get_frame_size() const
{
    auto frame_size =  m_client->OutputVStream_get_frame_size(m_handle);
    if (!frame_size) {
        LOGGER__CRITICAL("OutputVStream_get_frame_size failed with status={}", frame_size.status());
        return 0;
    }
    return frame_size.release();
}

const hailo_vstream_info_t &OutputVStreamClient::get_info() const
{
    return m_info;
}

const hailo_format_t &OutputVStreamClient::get_user_buffer_format() const
{
    return m_user_buffer_format;
}

std::string OutputVStreamClient::name() const
{
    auto expected_name = m_client->OutputVStream_name(m_handle);
    if (!expected_name) {
        LOGGER__CRITICAL("OutputVStream_name failed with status={}", expected_name.status());
        return "";
    }
    return expected_name.release();
}

std::string OutputVStreamClient::network_name() const
{
    auto expected_name = m_client->OutputVStream_network_name(m_handle);
    if (!expected_name) {
        LOGGER__CRITICAL("OutputVStream_name failed with status={}", expected_name.status());
        return "";
    }
    return expected_name.release();
}

const std::map<std::string, AccumulatorPtr> &OutputVStreamClient::get_fps_accumulators() const
{
    LOGGER__ERROR("OutputVStream::get_fps_accumulators function is not supported when using multi-process service");
    return m_fps_accumulators;
}
const std::map<std::string, AccumulatorPtr> &OutputVStreamClient::get_latency_accumulators() const
{
    LOGGER__ERROR("OutputVStream::get_latency_accumulators functoin is not supported when using multi-process service");
    return m_latency_accumulators;
}

const std::map<std::string, std::vector<AccumulatorPtr>> &OutputVStreamClient::get_queue_size_accumulators() const
{
    LOGGER__ERROR("OutputVStream::get_queue_size_accumulators function is not supported when using multi-process service");
    return m_queue_size_accumulators;
}
AccumulatorPtr OutputVStreamClient::get_pipeline_latency_accumulator() const
{
    LOGGER__ERROR("OutputVStream::get_pipeline_latency_accumulator function is not supported when using multi-process service");
    return m_pipeline_latency_accumulator;
}
const std::vector<std::shared_ptr<PipelineElement>> &OutputVStreamClient::get_pipeline() const
{
    LOGGER__ERROR("OutputVStream::get_pipeline function is not supported when using multi-process service");
    return m_pipeline;
}

hailo_status OutputVStreamClient::create_client()
{
    auto expected_client = HailoRtRpcClientUtils::create_client();
    CHECK_EXPECTED_AS_STATUS(expected_client);
    m_client = expected_client.release();
    return HAILO_SUCCESS;
}

hailo_status OutputVStreamClient::before_fork()
{
    m_client.reset();
    return HAILO_SUCCESS;
}

hailo_status OutputVStreamClient::after_fork_in_parent()
{
    return create_client();
}

hailo_status OutputVStreamClient::after_fork_in_child()
{
    auto status = create_client();
    CHECK_SUCCESS(status);
    auto expected_dup_handle = m_client->OutputVStream_dup_handle(OsUtils::get_curr_pid(), m_handle);
    CHECK_EXPECTED_AS_STATUS(expected_dup_handle);
    m_handle = expected_dup_handle.value();
    return HAILO_SUCCESS;
}

bool OutputVStreamClient::is_aborted()
{
    auto is_aborted_exp = m_client->OutputVStream_is_aborted(m_handle);
    if (!is_aborted_exp) {
        LOGGER__CRITICAL("OutputVStream_is_aborted failed with status={}", is_aborted_exp.status());
        return true;
    }
    return is_aborted_exp.release();
}
#endif // HAILO_SUPPORT_MULTI_PROCESS

Expected<std::shared_ptr<HwReadElement>> HwReadElement::create(std::shared_ptr<OutputStream> stream, const std::string &name, std::chrono::milliseconds timeout,
    size_t buffer_pool_size, hailo_pipeline_elem_stats_flags_t elem_flags, hailo_vstream_stats_flags_t vstream_flags, EventPtr shutdown_event,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status, std::unique_ptr<OutputTransformContext> transform_context)
{
    auto buffer_pool = BufferPool::create(stream->get_frame_size(), buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
    CHECK_EXPECTED(buffer_pool, "Failed creating BufferPool for {}", name);

    BufferPoolPtr transform_pool = nullptr;
    if (transform_context) {
        auto expected_transform_pool = BufferPool::create(transform_context->get_dst_frame_size(), buffer_pool_size, shutdown_event, elem_flags, vstream_flags);
        CHECK_EXPECTED(expected_transform_pool, "Failed creating BufferPool for {}", name);        
        transform_pool = expected_transform_pool.release();
    }

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto hw_read_elem_ptr = make_shared_nothrow<HwReadElement>(stream, buffer_pool.release(), name, timeout,
        duration_collector.release(), shutdown_event, std::move(pipeline_status), transform_pool, std::move(transform_context));
    CHECK_AS_EXPECTED(nullptr != hw_read_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", hw_read_elem_ptr->name());

    return hw_read_elem_ptr;
}

HwReadElement::HwReadElement(std::shared_ptr<OutputStream> stream, BufferPoolPtr buffer_pool, const std::string &name,
                             std::chrono::milliseconds timeout, DurationCollector &&duration_collector,
                             EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status,
                             BufferPoolPtr transform_pool, std::unique_ptr<OutputTransformContext> transform_context) :
    SourceElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_stream(stream),
    m_pool(buffer_pool),
    m_transform_pool(transform_pool),
    m_timeout(timeout),
    m_shutdown_event(shutdown_event),
    m_activation_wait_or_shutdown(stream->get_core_op_activated_event(), shutdown_event),
    m_transform_context(std::move(transform_context))
{}

uint32_t HwReadElement::get_invalid_frames_count()
{
    return m_stream->get_invalid_frames_count();
}

std::string HwReadElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | hw_frame_size: " << m_stream->get_info().hw_frame_size << ")";   

    return element_description.str();
}

hailo_status HwReadElement::execute_post_deactivate(bool should_clear_abort)
{
    if (should_clear_abort) {
        auto status = m_stream->clear_abort();
        CHECK(((HAILO_SUCCESS == status) || (HAILO_STREAM_NOT_ACTIVATED == status)), status,
            "Failed to clear abort stream in {}", name());
    }
    return HAILO_SUCCESS;
}

hailo_status HwReadElement::execute_clear()
{
    return HAILO_SUCCESS;
}

hailo_status HwReadElement::execute_flush()
{
    return HAILO_INVALID_OPERATION;
}

hailo_status HwReadElement::execute_abort()
{
    auto status = m_stream->abort();
    CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
        "Failed to execute abort stream in {}", name());
    return HAILO_SUCCESS;
}

hailo_status HwReadElement::execute_clear_abort()
{
    auto status = m_stream->clear_abort();
    CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
        "Failed to execute clear_abort stream in {}", name());
    return HAILO_SUCCESS;
}

hailo_status HwReadElement::execute_wait_for_finish()
{
    return HAILO_SUCCESS;
}

std::vector<AccumulatorPtr> HwReadElement::get_queue_size_accumulators()
{
    if (nullptr == m_pool->get_queue_size_accumulator()) {
        return std::vector<AccumulatorPtr>();
    }
    return {m_pool->get_queue_size_accumulator()};
}

hailo_status HwReadElement::run_push(PipelineBuffer &&/*buffer*/)
{
    return HAILO_INVALID_OPERATION;
}

Expected<PipelineBuffer> HwReadElement::run_pull(PipelineBuffer &&optional, const PipelinePad &/*source*/)
{
    auto buffer = m_pool->get_available_buffer(std::move(optional), m_timeout);
    if (HAILO_SHUTDOWN_EVENT_SIGNALED == buffer.status()) {
        return make_unexpected(buffer.status());
    }
    CHECK_EXPECTED(buffer, "{} (D2H) failed with status={}", name(), buffer.status());

    while (true) {
        if (!m_stream->is_scheduled()) {
            auto status = m_activation_wait_or_shutdown.wait(m_timeout);
            if (HAILO_SHUTDOWN_EVENT_SIGNALED == status) {
                return make_unexpected(HAILO_SHUTDOWN_EVENT_SIGNALED);
            }
            if (HAILO_TIMEOUT == status) {
                return make_unexpected(HAILO_NETWORK_GROUP_NOT_ACTIVATED);
            }
            CHECK_SUCCESS_AS_EXPECTED(status);
        } else {
            auto status = m_activation_wait_or_shutdown.wait(std::chrono::milliseconds(0));
            if (HAILO_SHUTDOWN_EVENT_SIGNALED == status) {
                return make_unexpected(HAILO_SHUTDOWN_EVENT_SIGNALED);
            }
        }

        MemoryView buffer_view(buffer.value().as_view());
        m_duration_collector.start_measurement();
        auto status = m_stream->read(buffer_view);
        if (HAILO_INVALID_FRAME == status) {
            m_stream->increase_invalid_frames_count(1);
            status = HAILO_SUCCESS;
        }
        if (HAILO_STREAM_NOT_ACTIVATED == status) {
            // Try again
            continue;
        }
        if (HAILO_STREAM_ABORTED_BY_USER == status) {
            LOGGER__INFO("Reading from stream was aborted!");
            return make_unexpected(HAILO_STREAM_ABORTED_BY_USER);
        }
        CHECK_SUCCESS_AS_EXPECTED(status, "{} (D2H) failed with status={}", name(), status);
        m_duration_collector.complete_measurement();

        // TODO: This is for rare cases where a transormation is needed before another pipeline element
        // Should be handled by the computational graph, and not here.
        if (m_transform_context) {
            auto transform_buffer = m_transform_pool->get_available_buffer(PipelineBuffer(), m_timeout);
            CHECK_EXPECTED(buffer);
            status = m_transform_context->transform(buffer_view, transform_buffer.value().as_view());
            CHECK_SUCCESS_AS_EXPECTED(status);
            return transform_buffer.release();
        }

        return buffer.release();
    }
}

hailo_status HwReadElement::execute_activate()
{
    return HAILO_SUCCESS;
}

hailo_status HwReadElement::execute_deactivate()
{
    auto signal_shutdown_status = m_shutdown_event->signal();
    if (HAILO_SUCCESS != signal_shutdown_status) {
        LOGGER__ERROR("Signaling {} shutdown event failed with {}", name(), signal_shutdown_status);
    }

    auto abort_status = m_stream->abort();
    if ((HAILO_SUCCESS != abort_status) && (HAILO_STREAM_NOT_ACTIVATED != abort_status)) {
        LOGGER__ERROR("Abort {} failed with {}", name(), abort_status);
        return abort_status;
    }

    return signal_shutdown_status;
}

Expected<std::shared_ptr<HwWriteElement>> HwWriteElement::create(std::shared_ptr<InputStream> stream, const std::string &name,
    hailo_pipeline_elem_stats_flags_t elem_flags, std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{

    auto duration_collector = DurationCollector::create(elem_flags);
    CHECK_EXPECTED(duration_collector);

    auto got_flush_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != got_flush_event, HAILO_OUT_OF_HOST_MEMORY);

    auto hw_write_elem_ptr = make_shared_nothrow<HwWriteElement>(stream, name,
        duration_collector.release(), std::move(pipeline_status), got_flush_event);
    CHECK_AS_EXPECTED(nullptr != hw_write_elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", hw_write_elem_ptr->name());

    return hw_write_elem_ptr;
}

HwWriteElement::HwWriteElement(std::shared_ptr<InputStream> stream, const std::string &name, DurationCollector &&duration_collector,
                               std::shared_ptr<std::atomic<hailo_status>> &&pipeline_status, EventPtr got_flush_event) :
    SinkElement(name, std::move(duration_collector), std::move(pipeline_status)),
    m_stream(stream), m_got_flush_event(got_flush_event)
{}

Expected<PipelineBuffer> HwWriteElement::run_pull(PipelineBuffer &&/*optional*/, const PipelinePad &/*source*/)
{
    return make_unexpected(HAILO_INVALID_OPERATION);
}

hailo_status HwWriteElement::run_push(PipelineBuffer &&buffer)
{
    if (PipelineBuffer::Type::FLUSH == buffer.get_type()) {
        hailo_status flush_status = m_stream->flush();
        if (HAILO_STREAM_ABORTED_BY_USER == flush_status) {
            LOGGER__INFO("Failed flushing input stream {} because stream was aborted", m_stream->to_string());
        } else if (HAILO_SUCCESS != flush_status) {
            LOGGER__ERROR("flush has failed in {} with status {}", name(), flush_status);
        }
        hailo_status status = m_got_flush_event->signal();
        CHECK_SUCCESS(status);
        return HAILO_SUCCESS;
    }

    m_duration_collector.start_measurement();
    const auto status = m_stream->write(MemoryView(buffer.data(), buffer.size()));
    m_duration_collector.complete_measurement();

    if (HAILO_STREAM_ABORTED_BY_USER == status) {
        LOGGER__INFO("Failed to send on input stream {} because stream was aborted", m_stream->to_string());
        return HAILO_STREAM_ABORTED_BY_USER;
    }
    CHECK_SUCCESS(status, "{} (H2D) failed with status={}", name(), status);

    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_activate()
{
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_deactivate()
{
    // The flush operation will block until all buffers currently in the pipeline will be processed.
    // We assume that no buffers are sent after the call for deactivate.
    hailo_status flush_status = m_stream->flush();
    if (HAILO_STREAM_ABORTED_BY_USER == flush_status) {
        LOGGER__INFO("Failed flushing input stream {} because stream was aborted", m_stream->to_string());
        return HAILO_SUCCESS;
    } else if (HAILO_STREAM_NOT_ACTIVATED == flush_status) {
        LOGGER__INFO("Failed flushing input stream {} because stream is not activated", m_stream->to_string());
        return HAILO_SUCCESS;
    } else if (HAILO_SUCCESS != flush_status) {
        LOGGER__ERROR("flush has failed in {} with status {}", name(), flush_status);
    }

    auto abort_status = m_stream->abort();
    CHECK(((abort_status == HAILO_SUCCESS) || (abort_status == HAILO_STREAM_NOT_ACTIVATED)), abort_status,
        "Failed to abort stream in {}", name());
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_post_deactivate(bool should_clear_abort)
{
    if (should_clear_abort) {
        auto status = m_stream->clear_abort();
        CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
            "Failed to clear abort stream in {}", name());
    }
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_clear()
{
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_flush()
{
    hailo_status status = m_got_flush_event->wait(m_stream->get_timeout());
    CHECK_SUCCESS(status);

    status = m_got_flush_event->reset();
    CHECK_SUCCESS(status);

    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_abort()
{
    auto status = m_stream->abort();
    CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
        "Failed to execute abort stream in {}", name());
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_clear_abort()
{
    auto status = m_stream->clear_abort();
    CHECK(((status == HAILO_SUCCESS) || (status == HAILO_STREAM_NOT_ACTIVATED)), status,
        "Failed to execute clear_abort stream in {}", name());
    return HAILO_SUCCESS;
}

hailo_status HwWriteElement::execute_wait_for_finish()
{
    return HAILO_SUCCESS;
}

std::string HwWriteElement::description() const
{
    std::stringstream element_description;
    element_description << "(" << this->name() << " | hw_frame_size: " << m_stream->get_info().hw_frame_size << ")";   

    return element_description.str();
}

Expected<std::shared_ptr<CopyBufferElement>> CopyBufferElement::create(const std::string &name,
    std::shared_ptr<std::atomic<hailo_status>> pipeline_status)
{
    auto duration_collector = DurationCollector::create(HAILO_PIPELINE_ELEM_STATS_NONE);
    CHECK_EXPECTED(duration_collector);
    auto elem_ptr = make_shared_nothrow<CopyBufferElement>(name, duration_collector.release(), std::move(pipeline_status));
    CHECK_AS_EXPECTED(nullptr != elem_ptr, HAILO_OUT_OF_HOST_MEMORY);

    LOGGER__INFO("Created {}", elem_ptr->name());

    return elem_ptr;
}

CopyBufferElement::CopyBufferElement(const std::string &name, DurationCollector &&duration_collector, 
                                     std::shared_ptr<std::atomic<hailo_status>> pipeline_status) :
    FilterElement(name, std::move(duration_collector), std::move(pipeline_status))
{}

PipelinePad &CopyBufferElement::next_pad()
{
    // Note: The next elem to be run is downstream from this elem (i.e. buffers are pushed)
    return *m_sinks[0].prev();
}

Expected<PipelineBuffer> CopyBufferElement::action(PipelineBuffer &&input, PipelineBuffer &&optional)
{
    CHECK_AS_EXPECTED(optional, HAILO_INVALID_ARGUMENT, "Optional buffer must be passed to CopyBufferElement!");

    CHECK_AS_EXPECTED(optional.size() == input.size(), HAILO_INVALID_ARGUMENT, "Optional buffer size does not equal to the input buffer size!");
    memcpy(optional.data(), input.data(), optional.size());

    return std::move(optional);
}

Expected<std::pair<std::vector<InputVStream>, std::vector<OutputVStream>>> VStreamsBuilder::create_vstreams(
    ConfiguredNetworkGroup &net_group, bool quantized, hailo_format_type_t format_type,
    const std::string &network_name)
{
    const auto params = HailoRTDefaults::get_vstreams_params(quantized, format_type);
    return create_vstreams(net_group, params, network_name);
}

Expected<std::pair<std::vector<InputVStream>, std::vector<OutputVStream>>> VStreamsBuilder::create_vstreams(
    ConfiguredNetworkGroup &net_group, const hailo_vstream_params_t &vstreams_params,
    const std::string &network_name)
{
    std::map<std::string, hailo_vstream_params_t> vstreams_params_by_input_stream_name;
    auto input_vstream_params = net_group.make_input_vstream_params(true, HAILO_FORMAT_TYPE_AUTO, 
        HAILO_DEFAULT_VSTREAM_TIMEOUT_MS, HAILO_DEFAULT_VSTREAM_QUEUE_SIZE, network_name);
    CHECK_EXPECTED(input_vstream_params);

    for (auto params_pair : input_vstream_params.release()) {
        vstreams_params_by_input_stream_name.emplace(std::make_pair(params_pair.first, vstreams_params));
    }

    auto expected_all_inputs = create_input_vstreams(net_group, vstreams_params_by_input_stream_name);
    CHECK_EXPECTED(expected_all_inputs);

    std::map<std::string, hailo_vstream_params_t> vstreams_params_by_output_stream_name;
    auto output_vstream_params = net_group.make_output_vstream_params(true, HAILO_FORMAT_TYPE_AUTO, 
        HAILO_DEFAULT_VSTREAM_TIMEOUT_MS, HAILO_DEFAULT_VSTREAM_QUEUE_SIZE, network_name);
    CHECK_EXPECTED(output_vstream_params);

    for (auto params_pair : output_vstream_params.release()) {
        vstreams_params_by_output_stream_name.emplace(std::make_pair(params_pair.first, vstreams_params));
    }

    auto expected_all_outputs = create_output_vstreams(net_group, vstreams_params_by_output_stream_name);
    CHECK_EXPECTED(expected_all_outputs);

    return std::pair<std::vector<InputVStream>, std::vector<OutputVStream>>(
            expected_all_inputs.release(), expected_all_outputs.release());
}

static hailo_vstream_params_t expand_vstream_params_autos(const hailo_stream_info_t &stream_info,
    const hailo_vstream_params_t &vstream_params)
{
    if (HAILO_FORMAT_ORDER_HAILO_NMS == stream_info.format.order) {
        // TODO (HRT-11082): On NMS, return error if UINT16
        if (HAILO_FORMAT_TYPE_UINT16 == vstream_params.user_buffer_format.type) {
            LOGGER__WARNING("Passing 'HAILO_FORMAT_TYPE_UINT16' for NMS output is deprecated and will soon be unsupported. "\
                "One should use HAILO_FORMAT_TYPE_FLOAT32");
        }
    }
    auto local_vstream_params = vstream_params;
    local_vstream_params.user_buffer_format = HailoRTDefaults::expand_auto_format(vstream_params.user_buffer_format,
        stream_info.format);
    return local_vstream_params;
}

static hailo_vstream_params_t expand_vstream_params_autos_argmax(const hailo_vstream_params_t &vstream_params,
    hailo_format_t &op_input_format)
{
    auto local_vstream_params = vstream_params;
    if (local_vstream_params.user_buffer_format.type == HAILO_FORMAT_TYPE_AUTO) {
        local_vstream_params.user_buffer_format.type = op_input_format.type;
    }
    if (local_vstream_params.user_buffer_format.order == HAILO_FORMAT_ORDER_AUTO) {
        if (op_input_format.order == HAILO_FORMAT_ORDER_NHCW || op_input_format.order == HAILO_FORMAT_ORDER_NHWC) {
            local_vstream_params.user_buffer_format.order = HAILO_FORMAT_ORDER_NHW;
        }
        if (op_input_format.order == HAILO_FORMAT_ORDER_NC) {
            local_vstream_params.user_buffer_format.order = HAILO_FORMAT_ORDER_NC;
        }
    }
    return local_vstream_params;
}

static hailo_vstream_params_t expand_vstream_params_autos_softmax(const hailo_vstream_params_t &vstream_params,
    hailo_format_t &op_input_format)
{
    auto local_vstream_params = vstream_params;
    // Type should be float32, after de-quantization, and order NHWC or NC in softmax
    if (local_vstream_params.user_buffer_format.type == HAILO_FORMAT_TYPE_AUTO) {
        local_vstream_params.user_buffer_format.type = HAILO_FORMAT_TYPE_FLOAT32;
    }
    if (local_vstream_params.user_buffer_format.order == HAILO_FORMAT_ORDER_AUTO) {
        local_vstream_params.user_buffer_format.order = op_input_format.order;
    }
    return local_vstream_params;
}

Expected<std::vector<InputVStream>> VStreamsBuilder::create_input_vstreams(ConfiguredNetworkGroup &net_group,
    const std::map<std::string, hailo_vstream_params_t> &inputs_params)
{
    return net_group.create_input_vstreams(inputs_params);
}

Expected<std::vector<OutputVStream>> VStreamsBuilder::create_output_vstreams(ConfiguredNetworkGroup &net_group,
    const std::map<std::string, hailo_vstream_params_t> &outputs_params)
{
    return net_group.create_output_vstreams(outputs_params);
}

Expected<std::vector<InputVStream>> VStreamsBuilderUtils::create_inputs(std::shared_ptr<InputStream> input_stream, const hailo_vstream_info_t &vstream_info,
    const hailo_vstream_params_t &vstream_params)
{
    // TODO (HRT-4522): Support this measurement
    CHECK_AS_EXPECTED(!(vstream_params.vstream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_FPS), HAILO_NOT_IMPLEMENTED,
        "Pipeline FPS statistics measurement is not implemented");

    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<InputVStream> vstreams;

    EventPtr core_op_activated_event = nullptr;
    if (!input_stream->is_scheduled()) {
        core_op_activated_event = input_stream->get_core_op_activated_event();
    }

    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstream_params);
    CHECK_EXPECTED(pipeline_latency_accumulator);

    auto user_timeout = std::chrono::milliseconds(vstream_params.timeout_ms);

    auto hw_write_elem = HwWriteElement::create(input_stream,
        PipelineObject::create_element_name("HwWriteElement", input_stream->name(), input_stream->get_info().index),
        vstream_params.pipeline_elements_stats_flags, pipeline_status);
    CHECK_EXPECTED(hw_write_elem);
    elements.insert(elements.begin(), hw_write_elem.value());

    auto should_transform = InputTransformContext::is_transformation_required(input_stream->get_info().shape,
        vstream_params.user_buffer_format, input_stream->get_info().hw_shape, input_stream->get_info().format, 
        input_stream->get_info().quant_info);

    if (should_transform) {
        std::shared_ptr<SinkElement> elem_after_post_infer = hw_write_elem.value();
        auto queue_elem = PushQueueElement::create(
            PipelineObject::create_element_name("PushQueueElement", input_stream->get_info().name, input_stream->get_info().index),
            vstream_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED(queue_elem);
        elements.insert(elements.begin(), queue_elem.value());
        CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(queue_elem.value(), hw_write_elem.value()));

        auto pre_infer_elem = PreInferElement::create(input_stream->get_info().shape, vstream_params.user_buffer_format,
             input_stream->get_info().hw_shape, input_stream->get_info().format, input_stream->get_info().quant_info, 
             PipelineObject::create_element_name("PreInferElement", input_stream->get_info().name, input_stream->get_info().index),
             vstream_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED(pre_infer_elem);
        elements.insert(elements.begin(), pre_infer_elem.value());
        CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(pre_infer_elem.value(), queue_elem.value()));

        input_stream->set_timeout(user_timeout);
        auto vstream = InputVStream::create(vstream_info, vstream_params, pre_infer_elem.release(), hw_write_elem.release(), std::move(elements),
            std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
        CHECK_EXPECTED(vstream);
        vstreams.emplace_back(vstream.release());
    } else {
        input_stream->set_timeout(user_timeout);
        auto vstream = InputVStream::create(vstream_info, vstream_params, hw_write_elem.value(), hw_write_elem.value(), std::move(elements),
            std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
        CHECK_EXPECTED(vstream);
        vstreams.emplace_back(vstream.release());
    }

    for (const auto &vstream : vstreams) {
       LOGGER__INFO("{}", vstream.get_pipeline_description());
    }

    return vstreams;
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_outputs(std::shared_ptr<OutputStream> output_stream,
    NameToVStreamParamsMap &vstreams_params_map, const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos)
{
    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<OutputVStream> vstreams;

    EventPtr core_op_activated_event = nullptr;
    if (!output_stream->is_scheduled()) {
        core_op_activated_event = output_stream->get_core_op_activated_event();
    }

    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    assert(!vstreams_params_map.empty());

    // Note: In case of multiple values in vstreams_params_map (e.g. in the case of demux), we'll set the
    //       pipeline_elements_stats_flags for the hw_read_element as bitwise or of all the flags.
    hailo_pipeline_elem_stats_flags_t hw_read_element_stats_flags = HAILO_PIPELINE_ELEM_STATS_NONE;
    hailo_vstream_stats_flags_t hw_read_stream_stats_flags = HAILO_VSTREAM_STATS_NONE;
    size_t buffer_pool_size = 0;
    for (const auto &elem_name_params : vstreams_params_map) {
        hw_read_element_stats_flags |= elem_name_params.second.pipeline_elements_stats_flags;
        hw_read_stream_stats_flags |= elem_name_params.second.vstream_stats_flags;
        buffer_pool_size += elem_name_params.second.queue_size;
    }

    // TODO (HRT-4522): Support this measurement
    CHECK_AS_EXPECTED(!(hw_read_stream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_FPS), HAILO_NOT_IMPLEMENTED,
        "Pipeline FPS statistics measurement is not implemented");

    auto hw_read_element = add_hw_read_element(output_stream, pipeline_status, elements, "HwReadElement", shutdown_event,
        buffer_pool_size, hw_read_element_stats_flags, hw_read_stream_stats_flags);
    CHECK_EXPECTED(hw_read_element);

    if (output_stream->get_info().is_mux) {
        hailo_status status = add_demux(output_stream, vstreams_params_map, std::move(elements), vstreams, hw_read_element.value(),
            shutdown_event, pipeline_status, output_vstream_infos);
        CHECK_SUCCESS_AS_EXPECTED(status);
    } else {
        auto vstream_info = output_vstream_infos.find(output_stream->name());
        CHECK_AS_EXPECTED(vstream_info != output_vstream_infos.end(), HAILO_NOT_FOUND,
            "Failed to find vstream info of {}", output_stream->name());
        assert(1 == vstreams_params_map.size());
        auto vstream_params = expand_vstream_params_autos(output_stream->get_info(), vstreams_params_map.begin()->second);

        auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstream_params);
        CHECK_EXPECTED(pipeline_latency_accumulator);

        auto should_transform = OutputTransformContext::is_transformation_required(output_stream->get_info().hw_shape, 
            output_stream->get_info().format, output_stream->get_info().shape, 
            vstream_params.user_buffer_format, output_stream->get_info().quant_info);

        if (should_transform) {
            auto hw_read_queue_element = add_pull_queue_element(output_stream, pipeline_status, elements, "PullQueueElement_hw_read",
                shutdown_event, vstream_params);
            CHECK_EXPECTED(hw_read_queue_element);
            CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_element.value(), hw_read_queue_element.value()));

            auto post_infer_element = add_post_infer_element(output_stream, pipeline_status, elements,
                "PostInferElement", vstream_params, shutdown_event);
            CHECK_EXPECTED(post_infer_element);
            CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_queue_element.value(), post_infer_element.value()));
            auto user_buffer_queue_element = add_user_buffer_queue_element(output_stream, pipeline_status, elements,
                "UserBufferQueueElement", shutdown_event, vstream_params);
            CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(post_infer_element.value(), user_buffer_queue_element.value()));
            output_stream->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
            hw_read_queue_element->get()->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
            auto vstream = OutputVStream::create(vstream_info->second, vstream_params, user_buffer_queue_element.release(), std::move(elements),
                std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
            CHECK_EXPECTED(vstream);
            vstreams.emplace_back(vstream.release());
        } else {
            output_stream->set_timeout(std::chrono::milliseconds(vstream_params.timeout_ms));
            auto vstream = OutputVStream::create(vstream_info->second, vstream_params, hw_read_element.release(), std::move(elements),
                std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
            CHECK_EXPECTED(vstream);
            vstreams.emplace_back(vstream.release());
        }
    }

    for (const auto &vstream : vstreams) {
        LOGGER__INFO("{}", vstream.get_pipeline_description());
    }

    return vstreams;
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_output_post_process_softmax(std::shared_ptr<OutputStream> output_stream,
    const NameToVStreamParamsMap &vstreams_params_map, const hailo_vstream_info_t &output_vstream_info, const NetFlowElement &softmax_op)
{
    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<OutputVStream> vstreams;

    EventPtr core_op_activated_event = nullptr;
    if (!output_stream->is_scheduled()) {
        core_op_activated_event = output_stream->get_core_op_activated_event();
    }

    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    assert(!vstreams_params_map.empty());

    // Note: In case of multiple values in vstreams_params_map (e.g. in the case of demux), we'll set the
    //       pipeline_elements_stats_flags for the hw_read_element as bitwise or of all the flags.
    hailo_pipeline_elem_stats_flags_t hw_read_element_stats_flags = HAILO_PIPELINE_ELEM_STATS_NONE;
    hailo_vstream_stats_flags_t hw_read_stream_stats_flags = HAILO_VSTREAM_STATS_NONE;
    size_t buffer_pool_size = 0;
    for (const auto &elem_name_params : vstreams_params_map) {
        hw_read_element_stats_flags |= elem_name_params.second.pipeline_elements_stats_flags;
        hw_read_stream_stats_flags |= elem_name_params.second.vstream_stats_flags;
        buffer_pool_size += elem_name_params.second.queue_size;
    }

    // TODO (HRT-4522): Support this measurement
    CHECK_AS_EXPECTED(!(hw_read_stream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_FPS), HAILO_NOT_IMPLEMENTED,
        "Pipeline FPS statistics measurement is not implemented");

    assert(1 == vstreams_params_map.size());
    auto op_input_format = softmax_op.op->inputs_metadata().begin()->second.format;
    auto vstream_params = expand_vstream_params_autos_softmax(vstreams_params_map.begin()->second, op_input_format);
    if (HAILO_FORMAT_FLAGS_QUANTIZED & vstream_params.user_buffer_format.flags) {
        vstream_params.user_buffer_format.flags &= ~HAILO_FORMAT_FLAGS_QUANTIZED;
        LOGGER__WARNING("Note: The output_vstream {} format flag is marked as quantized, which is not supported with {}. "
            "flag has been automatically set to False.", softmax_op.output_vstream_info.name, softmax_op.op->get_name());
    }

    auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstream_params);
    CHECK_EXPECTED(pipeline_latency_accumulator);

    auto hw_read_element = add_hw_read_element(output_stream, pipeline_status, elements, "HwReadElement", shutdown_event,
        buffer_pool_size, hw_read_element_stats_flags, hw_read_stream_stats_flags);
    CHECK_EXPECTED(hw_read_element);

    auto hw_read_queue_element = add_pull_queue_element(output_stream, pipeline_status, elements, "PullQueueElement_hw_read",
        shutdown_event, vstream_params);
    CHECK_EXPECTED(hw_read_queue_element);
    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_element.value(), hw_read_queue_element.value()));

    auto post_infer_element = add_post_infer_element(output_stream, pipeline_status, elements,
        "PostInferElement", vstream_params, shutdown_event);
    CHECK_EXPECTED(post_infer_element);
    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_queue_element.value(), post_infer_element.value()));

    auto pre_softmax_queue_element = add_pull_queue_element(output_stream, pipeline_status, elements, "PullQueueElement_pre_softmax",
        shutdown_event, vstream_params);
    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(post_infer_element.value(), pre_softmax_queue_element.value()));

    auto softmax_element = add_softmax_element(output_stream, pipeline_status, elements, "SoftmaxPostProcessElement",
        vstream_params, softmax_op);
    CHECK_EXPECTED(softmax_element);
    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(pre_softmax_queue_element.value(), softmax_element.value()));
    auto user_buffer_queue_element = add_user_buffer_queue_element(output_stream, pipeline_status, elements,
        "UserBufferQueueElement", shutdown_event, vstream_params);
    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(softmax_element.value(), user_buffer_queue_element.value()));
    output_stream->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
    hw_read_queue_element->get()->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
    auto vstream = OutputVStream::create(output_vstream_info, vstream_params, user_buffer_queue_element.release(), std::move(elements),
        std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
    CHECK_EXPECTED(vstream);
    vstreams.emplace_back(vstream.release());

    for (const auto &curr_vstream : vstreams) {
        LOGGER__INFO("{}", curr_vstream.get_pipeline_description());
    }

    return vstreams;
}

InputVStream VStreamsBuilderUtils::create_input(std::shared_ptr<InputVStreamInternal> input_vstream)
{
    return InputVStream(std::move(input_vstream));
}

OutputVStream VStreamsBuilderUtils::create_output(std::shared_ptr<OutputVStreamInternal> output_vstream)
{
    return OutputVStream(std::move(output_vstream));
}

static bool are_formats_equal(const hailo_format_t &format1, const hailo_format_t &format2) {
    return ((format1.order == format2.order) && (format1.flags == format2.flags) && (format1.type == format2.type));
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_output_vstreams_from_streams(const OutputStreamWithParamsVector &all_output_streams,
    OutputStreamPtrVector &output_streams, const hailo_vstream_params_t &vstream_params,
    const std::unordered_map<std::string, std::shared_ptr<NetFlowElement>> &post_process_ops,
    const std::unordered_map<stream_name_t, op_name_t> &op_inputs_to_op_name, const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos_map)
{
    auto first_stream_info = output_streams[0]->get_info();
    if ((HAILO_FORMAT_ORDER_HAILO_NMS == first_stream_info.format.order) &&
        (first_stream_info.nms_info.is_defused)) {
        // Case defuse NMS
        return create_output_nms(output_streams, vstream_params, output_vstream_infos_map);
    } else if (contains(op_inputs_to_op_name, static_cast<stream_name_t>(first_stream_info.name))) {
        // Case post-process on host
        auto &op_name = op_inputs_to_op_name.at(first_stream_info.name);
        auto &op = post_process_ops.at(op_name);
        switch (op.get()->op_type) {
            case HAILO_NET_FLOW_OP_TYPE_NMS:
            {
                assert(1 <= op->op->outputs_metadata().size());
                auto updated_outputs_metadata = op->op->outputs_metadata();
                updated_outputs_metadata.begin()->second.format = vstream_params.user_buffer_format;
                if (HAILO_FORMAT_ORDER_AUTO == updated_outputs_metadata.begin()->second.format.order) {
                    updated_outputs_metadata.begin()->second.format.order = HAILO_FORMAT_ORDER_HAILO_NMS;
                }
                if (HAILO_FORMAT_TYPE_AUTO == updated_outputs_metadata.begin()->second.format.type) {
                    updated_outputs_metadata.begin()->second.format.type = HAILO_FORMAT_TYPE_FLOAT32;
                }
                if (HAILO_FORMAT_FLAGS_QUANTIZED & updated_outputs_metadata.begin()->second.format.flags) {
                    updated_outputs_metadata.begin()->second.format.flags &= ~HAILO_FORMAT_FLAGS_QUANTIZED;
                    LOGGER__WARNING("Note: The output_vstream {} format flag is marked as quantized, which is not supported with {}. "
                        "flag has been automatically set to False.", op->output_vstream_info.name, op->op->get_name());
                }

                op->op->set_outputs_metadata(updated_outputs_metadata);
                CHECK_SUCCESS_AS_EXPECTED(op->op->validate_metadata());
                return create_output_post_process_nms(output_streams, vstream_params, output_vstream_infos_map, *op);
            }

            case HAILO_NET_FLOW_OP_TYPE_ARGMAX:
            {
                assert(output_streams.size() == 1);
                NameToVStreamParamsMap name_to_vstream_params_map;
                for (auto &output_stream : all_output_streams) {
                    if (output_stream.first->get_info().name == output_streams[0]->get_info().name) {
                        for (auto &vstream : output_stream.second) {
                            name_to_vstream_params_map.insert(vstream);
                        }
                    }
                }
                auto output_vstream_info = output_vstream_infos_map.at(op.get()->name);
                return create_output_post_process_argmax(output_streams[0], name_to_vstream_params_map, output_vstream_info, *op);
            }

             case HAILO_NET_FLOW_OP_TYPE_SOFTMAX:
            {
                assert(output_streams.size() == 1);
                NameToVStreamParamsMap name_to_vstream_params_map;
                for (auto &output_stream : all_output_streams) {
                    if (output_stream.first->get_info().name == output_streams[0]->get_info().name) {
                        for (auto &vstream : output_stream.second) {
                            name_to_vstream_params_map.insert(vstream);
                        }
                    }
                }
                auto output_vstream_info = output_vstream_infos_map.at(op.get()->name);
                return create_output_post_process_softmax(output_streams[0], name_to_vstream_params_map, output_vstream_info, *op);
             }

            default:
                LOGGER__ERROR("op type {} of op {} is not in any of the supported post process OP types", op.get()->op_type, op_name);
                return make_unexpected(HAILO_INVALID_OPERATION);
        }
    } else {
        // All other cases
        assert(output_streams.size() == 1);
        NameToVStreamParamsMap name_to_vstream_params_map;
        for (auto &output_stream : all_output_streams) {
            if (output_stream.first->get_info().name == output_streams[0]->get_info().name) {
                for (auto &vstream : output_stream.second) {
                    name_to_vstream_params_map.insert(vstream);
                }
            }
        }
        return create_outputs(output_streams[0], name_to_vstream_params_map, output_vstream_infos_map);
    }
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_output_nms(OutputStreamPtrVector &output_streams,
    hailo_vstream_params_t vstreams_params,
    const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos)
{
    for (const auto &out_stream : output_streams) {
        CHECK_AS_EXPECTED(are_formats_equal(output_streams[0]->get_info().format, out_stream->get_info().format),
            HAILO_INVALID_ARGUMENT, "All nms streams of the same virtual output must have the same format");
    }

    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<OutputVStream> vstreams;

    hailo_status status = add_nms_fuse(output_streams, vstreams_params, elements, vstreams, shutdown_event,
        pipeline_status, output_vstream_infos);
    CHECK_SUCCESS_AS_EXPECTED(status);

    for (const auto &vstream : vstreams) {
        LOGGER__INFO("{}", vstream.get_pipeline_description());
    }

    return vstreams;
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_output_post_process_nms(OutputStreamPtrVector &output_streams,
    hailo_vstream_params_t vstreams_params,
    const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos,
    const NetFlowElement &nms_op)
{
    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<OutputVStream> vstreams;

    hailo_status status = add_nms_post_process(output_streams, vstreams_params, elements, vstreams, shutdown_event,
        pipeline_status, output_vstream_infos, nms_op);
    CHECK_SUCCESS_AS_EXPECTED(status);

    for (const auto &vstream : vstreams) {
        LOGGER__INFO("{}", vstream.get_pipeline_description());
    }

    return vstreams;
}

Expected<std::shared_ptr<HwReadElement>> VStreamsBuilderUtils::add_hw_read_element(std::shared_ptr<OutputStream> &output_stream,
        std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
        const std::string &element_name, EventPtr &shutdown_event, size_t buffer_pool_size,
        const hailo_pipeline_elem_stats_flags_t &hw_read_element_stats_flags, const hailo_vstream_stats_flags_t &hw_read_stream_stats_flags)
{
    auto hw_read_elem = HwReadElement::create(output_stream,
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        HAILO_INFINITE_TIMEOUT, buffer_pool_size, hw_read_element_stats_flags, hw_read_stream_stats_flags, shutdown_event, pipeline_status);
    CHECK_EXPECTED(hw_read_elem);
    elements.push_back(hw_read_elem.value());
    return hw_read_elem;
}

Expected<std::shared_ptr<PullQueueElement>> VStreamsBuilderUtils::add_pull_queue_element(std::shared_ptr<OutputStream> &output_stream,
    std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
    const std::string &element_name, EventPtr &shutdown_event, const hailo_vstream_params_t &vstream_params)
{
    auto pull_queue_elem = PullQueueElement::create(
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        vstream_params, shutdown_event, pipeline_status);
    CHECK_EXPECTED(pull_queue_elem);
    elements.push_back(pull_queue_elem.value());
    return pull_queue_elem;
}

Expected<std::shared_ptr<ArgmaxPostProcessElement>> VStreamsBuilderUtils::add_argmax_element(std::shared_ptr<OutputStream> &output_stream,
    std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
    const std::string &element_name, hailo_vstream_params_t &vstream_params, const NetFlowElement &argmax_op)
{
    // Updating metadata according to user request. TODO: HRT-9737
    auto updated_outputs_metadata = argmax_op.op.get()->outputs_metadata();
    updated_outputs_metadata.begin()->second.format = vstream_params.user_buffer_format;
    argmax_op.op.get()->set_outputs_metadata(updated_outputs_metadata);
    CHECK_SUCCESS_AS_EXPECTED(argmax_op.op.get()->validate_metadata());
    // Updating metadata according to use request. TODO: HRT-9737 - End
    auto argmax_element = ArgmaxPostProcessElement::create(argmax_op.op,
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        vstream_params.pipeline_elements_stats_flags, pipeline_status);
    CHECK_EXPECTED(argmax_element);
    elements.push_back(argmax_element.value());
    return argmax_element;
}

Expected<std::shared_ptr<SoftmaxPostProcessElement>> VStreamsBuilderUtils::add_softmax_element(std::shared_ptr<OutputStream> &output_stream,
    std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
    const std::string &element_name, hailo_vstream_params_t &vstream_params, const NetFlowElement &softmax_op)
{
    // Updating metadata according to user request. TODO: HRT-9737
    // Currently softmax only supports inputs to be float32 and order NHWC or NC
    auto updated_inputs_metadata = softmax_op.op.get()->inputs_metadata();
    updated_inputs_metadata.begin()->second.format = vstream_params.user_buffer_format;
    softmax_op.op.get()->set_inputs_metadata(updated_inputs_metadata);

    auto updated_outputs_metadata = softmax_op.op.get()->outputs_metadata();
    updated_outputs_metadata.begin()->second.format = vstream_params.user_buffer_format;
    softmax_op.op.get()->set_outputs_metadata(updated_outputs_metadata);
    CHECK_SUCCESS_AS_EXPECTED(softmax_op.op.get()->validate_metadata());
    // Updating metadata according to use request. TODO: HRT-9737 - End
    auto softmax_element = SoftmaxPostProcessElement::create(softmax_op.op,
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        vstream_params.pipeline_elements_stats_flags, pipeline_status);
    CHECK_EXPECTED(softmax_element);
    elements.push_back(softmax_element.value());
    return softmax_element;
}

Expected<std::shared_ptr<UserBufferQueueElement>> VStreamsBuilderUtils::add_user_buffer_queue_element(std::shared_ptr<OutputStream> &output_stream,
    std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
    const std::string &element_name, EventPtr &shutdown_event, const hailo_vstream_params_t &vstream_params)
{
    auto post_argmax_queue_element = UserBufferQueueElement::create(
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        vstream_params, shutdown_event, pipeline_status);
    CHECK_EXPECTED(post_argmax_queue_element);
    elements.push_back(post_argmax_queue_element.value());
    return post_argmax_queue_element;
}

Expected<std::shared_ptr<PostInferElement>> VStreamsBuilderUtils::add_post_infer_element(std::shared_ptr<OutputStream> &output_stream,
    std::shared_ptr<std::atomic<hailo_status>> &pipeline_status, std::vector<std::shared_ptr<PipelineElement>> &elements,
    const std::string &element_name, const hailo_vstream_params_t &vstream_params, EventPtr shutdown_event)
{
    auto post_infer_element = PostInferElement::create(output_stream->get_info().hw_shape, output_stream->get_info().format,
        output_stream->get_info().shape, vstream_params.user_buffer_format, output_stream->get_info().quant_info, output_stream->get_info().nms_info,
        PipelineObject::create_element_name(element_name, output_stream->name(), output_stream->get_info().index),
        vstream_params, pipeline_status, shutdown_event);
    CHECK_EXPECTED(post_infer_element);
    elements.push_back(post_infer_element.value());
    return post_infer_element;
}

Expected<std::vector<OutputVStream>> VStreamsBuilderUtils::create_output_post_process_argmax(std::shared_ptr<OutputStream> output_stream,
    const NameToVStreamParamsMap &vstreams_params_map, const hailo_vstream_info_t &output_vstream_info, const NetFlowElement &argmax_op)
{
    std::vector<std::shared_ptr<PipelineElement>> elements;
    std::vector<OutputVStream> vstreams;

    EventPtr core_op_activated_event = nullptr;
    if (!output_stream->is_scheduled()) {
        core_op_activated_event = output_stream->get_core_op_activated_event();
    }

    auto shutdown_event = Event::create_shared(Event::State::not_signalled);
    CHECK_AS_EXPECTED(nullptr != shutdown_event, HAILO_OUT_OF_HOST_MEMORY);

    auto pipeline_status = make_shared_nothrow<std::atomic<hailo_status>>(HAILO_SUCCESS);
    CHECK_AS_EXPECTED(nullptr != pipeline_status, HAILO_OUT_OF_HOST_MEMORY);

    assert(!vstreams_params_map.empty());

    // Note: In case of multiple values in vstreams_params_map (e.g. in the case of demux), we'll set the
    //       pipeline_elements_stats_flags for the hw_read_element as bitwise or of all the flags.
    hailo_pipeline_elem_stats_flags_t hw_read_element_stats_flags = HAILO_PIPELINE_ELEM_STATS_NONE;
    hailo_vstream_stats_flags_t hw_read_stream_stats_flags = HAILO_VSTREAM_STATS_NONE;
    size_t buffer_pool_size = 0;
    for (const auto &elem_name_params : vstreams_params_map) {
        hw_read_element_stats_flags |= elem_name_params.second.pipeline_elements_stats_flags;
        hw_read_stream_stats_flags |= elem_name_params.second.vstream_stats_flags;
        buffer_pool_size += elem_name_params.second.queue_size;
    }

    // TODO (HRT-4522): Support this measurement
    CHECK_AS_EXPECTED(!(hw_read_stream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_FPS), HAILO_NOT_IMPLEMENTED,
        "Pipeline FPS statistics measurement is not implemented");

    auto hw_read_element = add_hw_read_element(output_stream, pipeline_status, elements, "HwReadElement", shutdown_event,
        buffer_pool_size, hw_read_element_stats_flags, hw_read_stream_stats_flags);
    CHECK_EXPECTED(hw_read_element);

    assert(1 == vstreams_params_map.size());
    auto op_input_format = argmax_op.op->inputs_metadata().begin()->second.format;
    auto vstream_params = expand_vstream_params_autos_argmax(vstreams_params_map.begin()->second, op_input_format);

    auto hw_read_queue_element = add_pull_queue_element(output_stream, pipeline_status, elements, "PullQueueElement_hw_read",
        shutdown_event, vstream_params);
    CHECK_EXPECTED(hw_read_queue_element);

    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_element.value(), hw_read_queue_element.value()));

    auto argmax_element = add_argmax_element(output_stream, pipeline_status, elements, "ArgmaxPostProcessElement",
        vstream_params, argmax_op);
    CHECK_EXPECTED(argmax_element);

    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(hw_read_queue_element.value(), argmax_element.value()));

    auto post_argmax_queue_element = add_user_buffer_queue_element(output_stream, pipeline_status, elements,
        "UserBufferQueueElement_post_argmax", shutdown_event, vstream_params);
    CHECK_EXPECTED(post_argmax_queue_element);

    CHECK_SUCCESS_AS_EXPECTED(PipelinePad::link_pads(argmax_element.value(), post_argmax_queue_element.value()));

    auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstream_params);
    CHECK_EXPECTED(pipeline_latency_accumulator);

    output_stream->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
    hw_read_queue_element->get()->set_timeout(std::chrono::milliseconds(HAILO_INFINITE));
    auto vstream = OutputVStream::create(output_vstream_info, vstream_params, post_argmax_queue_element.release(), std::move(elements),
        std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
    CHECK_EXPECTED(vstream);
    vstreams.emplace_back(vstream.release());

    for (const auto &current_vstream : vstreams) {
        LOGGER__INFO("{}", current_vstream.get_pipeline_description());
    }

    return vstreams;
}

hailo_status VStreamsBuilderUtils::add_demux(std::shared_ptr<OutputStream> output_stream, NameToVStreamParamsMap &vstreams_params_map,
    std::vector<std::shared_ptr<PipelineElement>> &&base_elements, std::vector<OutputVStream> &vstreams,
    std::shared_ptr<HwReadElement> hw_read_elem, EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status,
    const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos)
{
    auto expected_demuxer = OutputDemuxer::create(*output_stream);
    CHECK_EXPECTED_AS_STATUS(expected_demuxer);

    std::shared_ptr<OutputDemuxer> demuxer_ptr = expected_demuxer.release();
    CHECK(nullptr != demuxer_ptr, HAILO_OUT_OF_HOST_MEMORY);

    auto status = output_stream->set_timeout(HAILO_INFINITE_TIMEOUT);
    CHECK_SUCCESS(status);

    // Note: In case of multiple values in vstreams_params_map (e.g. in the case of demux), we'll set the
    //       pipeline_elements_stats_flags for the demux_elem as bitwise or of all the flags.
    hailo_pipeline_elem_stats_flags_t demux_elem_stats_flags = HAILO_PIPELINE_ELEM_STATS_NONE;
    hailo_vstream_stats_flags_t demux_vstream_stats_flags = HAILO_VSTREAM_STATS_NONE;
    size_t buffer_pool_size = 0;
    for (const auto &elem_name_params : vstreams_params_map) {
        demux_elem_stats_flags |= elem_name_params.second.pipeline_elements_stats_flags;
        demux_vstream_stats_flags |= elem_name_params.second.vstream_stats_flags;
        buffer_pool_size += elem_name_params.second.queue_size;
    }

    auto demux_elem = TransformDemuxElement::create(demuxer_ptr,
        PipelineObject::create_element_name("TransformDemuxElement", output_stream->name(), output_stream->get_info().index),
        std::chrono::milliseconds(HAILO_INFINITE), buffer_pool_size, demux_elem_stats_flags, demux_vstream_stats_flags, shutdown_event, pipeline_status);
    CHECK_EXPECTED_AS_STATUS(demux_elem);
    base_elements.push_back(demux_elem.value());
    CHECK_SUCCESS(PipelinePad::link_pads(hw_read_elem, demux_elem.value()));

    EventPtr core_op_activated_event = nullptr;
    if (!output_stream->is_scheduled()) {
        core_op_activated_event = output_stream->get_core_op_activated_event();
    }

    uint32_t i = 0;
    for (auto &edge_info : demuxer_ptr->get_edges_stream_info()) {
        auto name_params_pair = vstreams_params_map.find(edge_info.name);
        CHECK(name_params_pair != vstreams_params_map.end(), HAILO_NOT_FOUND,
            "Failed to find vstreams params of edge {}", edge_info.name);

        const auto vstream_info = output_vstream_infos.find(edge_info.name);
        CHECK(vstream_info != output_vstream_infos.end(), HAILO_NOT_FOUND,
            "Failed to find vstream info of {}", edge_info.name);

        const auto vstream_params = expand_vstream_params_autos(output_stream->get_info(), name_params_pair->second);

        // For each mux vstream, we create a copy of the previous elements
        auto current_vstream_elements = base_elements;

        // For muxed VStreams we use the same pipeline_status for all
        auto pipeline_status_copy = pipeline_status;
        auto demux_queue_elem = PullQueueElement::create(
            PipelineObject::create_element_name("PullQueueElement_demux", edge_info.name, edge_info.index),
            vstream_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(demux_queue_elem);
        current_vstream_elements.push_back(demux_queue_elem.value());
        CHECK_SUCCESS(PipelinePad::link_pads(demux_elem.value(), demux_queue_elem.value(), i, 0));

        demux_queue_elem.value()->set_timeout(HAILO_INFINITE_TIMEOUT);

        auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstream_params);
        CHECK_EXPECTED_AS_STATUS(pipeline_latency_accumulator);

        auto should_transform = OutputTransformContext::is_transformation_required(edge_info.hw_shape, 
            edge_info.format, edge_info.shape, vstream_params.user_buffer_format, edge_info.quant_info);

        if (should_transform) {
            auto post_infer_elem = PostInferElement::create(edge_info.hw_shape, edge_info.format, 
                edge_info.shape, vstream_params.user_buffer_format, edge_info.quant_info, edge_info.nms_info,
                PipelineObject::create_element_name("PostInferElement", edge_info.name, edge_info.index),
                vstream_params, pipeline_status, shutdown_event);
            CHECK_EXPECTED_AS_STATUS(post_infer_elem);
            current_vstream_elements.push_back(post_infer_elem.value());
            CHECK_SUCCESS(PipelinePad::link_pads(demux_queue_elem.value(), post_infer_elem.value()));

            auto post_infer_queue_elem = UserBufferQueueElement::create(
                PipelineObject::create_element_name("UserBufferQueueElement_post_infer", edge_info.name, edge_info.index),
                vstream_params, shutdown_event, pipeline_status);
            CHECK_EXPECTED_AS_STATUS(post_infer_queue_elem);
            current_vstream_elements.push_back(post_infer_queue_elem.value());
            CHECK_SUCCESS(PipelinePad::link_pads(post_infer_elem.value(), post_infer_queue_elem.value()));

            auto vstream = OutputVStream::create(vstream_info->second, vstream_params, post_infer_queue_elem.release(), std::move(current_vstream_elements),
                std::move(pipeline_status_copy), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
            CHECK_EXPECTED_AS_STATUS(vstream);
            vstreams.emplace_back(vstream.release());
        } else {
            // TODO: HRT-4179
            auto user_copy_elem = CopyBufferElement::create(
                PipelineObject::create_element_name("CopyBufferElement", edge_info.name, edge_info.index),
                pipeline_status);
            CHECK_EXPECTED_AS_STATUS(user_copy_elem);
            current_vstream_elements.push_back(user_copy_elem.value());
            CHECK_SUCCESS(PipelinePad::link_pads(demux_queue_elem.value(), user_copy_elem.value()));

            auto vstream = OutputVStream::create(vstream_info->second, vstream_params, user_copy_elem.release(), std::move(current_vstream_elements),
                std::move(pipeline_status_copy), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
            CHECK_EXPECTED_AS_STATUS(vstream);
            vstreams.emplace_back(vstream.release());
        }
        i++;
    }
    return HAILO_SUCCESS;
}

hailo_status VStreamsBuilderUtils::add_nms_fuse(OutputStreamPtrVector &output_streams, hailo_vstream_params_t &vstreams_params,
    std::vector<std::shared_ptr<PipelineElement>> &elements, std::vector<OutputVStream> &vstreams,
    EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status,
    const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos)
{
    std::vector<hailo_nms_info_t> nms_infos;
    nms_infos.reserve(output_streams.size());
    for (const auto &out_stream : output_streams) {
        CHECK(out_stream->get_info().nms_info.defuse_info.class_group_index <= output_streams.size(),
            HAILO_INVALID_ARGUMENT, "Not all defused nms outputs were grouped correctly!");
        nms_infos.emplace_back(out_stream->get_info().nms_info);
    }

    // To get the fused layer name and src stream format, we use the stream info of one of the defuses
    auto first_defused_stream_info = output_streams[0]->get_info();
    auto fused_layer_name = first_defused_stream_info.nms_info.defuse_info.original_name;
    auto src_stream_format = first_defused_stream_info.format;

    auto vstream_info = output_vstream_infos.find(fused_layer_name);
    CHECK(vstream_info != output_vstream_infos.end(), HAILO_NOT_FOUND,
        "Failed to find vstream info of {}. Could be due to use of old HEF. Try to re-compile network with newer Dataflow Compiler version", fused_layer_name);

    vstreams_params = expand_vstream_params_autos(first_defused_stream_info, vstreams_params);
    auto nms_elem = NmsMuxElement::create(nms_infos,
        PipelineObject::create_element_name("NmsMuxElement", fused_layer_name, 0),
        vstreams_params, shutdown_event, pipeline_status);
    CHECK_EXPECTED_AS_STATUS(nms_elem);
    auto fused_layer_nms_info = nms_elem.value()->get_fused_nms_info();

    for (uint32_t i = 0; i < output_streams.size(); ++i) {
        const auto &curr_stream_info = output_streams[i]->get_info();
        output_streams[i]->set_timeout(HAILO_INFINITE_TIMEOUT);

        auto hw_read_elem = HwReadElement::create(output_streams[i],
            PipelineObject::create_element_name("HwReadElement", curr_stream_info.name, curr_stream_info.index),
            HAILO_INFINITE_TIMEOUT, vstreams_params.queue_size, vstreams_params.pipeline_elements_stats_flags,
            vstreams_params.vstream_stats_flags, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(hw_read_elem);
        elements.push_back(hw_read_elem.value());

        auto nms_source_queue_elem = PullQueueElement::create(
            PipelineObject::create_element_name("PullQueueElement_nms_source", curr_stream_info.name, curr_stream_info.index),
            vstreams_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(nms_source_queue_elem);
        elements.push_back(nms_source_queue_elem.value());
        nms_source_queue_elem.value()->set_timeout(HAILO_INFINITE_TIMEOUT);
        CHECK_SUCCESS(PipelinePad::link_pads(hw_read_elem.value(), nms_source_queue_elem.value()));
        CHECK_SUCCESS(PipelinePad::link_pads(nms_source_queue_elem.value(), nms_elem.value(), 0, i));
    }
    elements.push_back(nms_elem.value());

    auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstreams_params);
    CHECK_EXPECTED_AS_STATUS(pipeline_latency_accumulator);

    auto should_transform = OutputTransformContext::is_transformation_required({}, src_stream_format, {},
        vstreams_params.user_buffer_format, vstream_info->second.quant_info);
    
    EventPtr core_op_activated_event = nullptr;
    if (!output_streams[0]->is_scheduled()) {
        core_op_activated_event = output_streams[0]->get_core_op_activated_event();
    }

    if (should_transform) {
        auto nms_queue_elem = PullQueueElement::create(
            PipelineObject::create_element_name("PullQueueElement_nms", fused_layer_name, 0),
            vstreams_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(nms_queue_elem);
        nms_queue_elem.value()->set_timeout(HAILO_INFINITE_TIMEOUT);
        elements.push_back(nms_queue_elem.value());
        CHECK_SUCCESS(PipelinePad::link_pads(nms_elem.value(), nms_queue_elem.value()));

        auto post_infer_elem = PostInferElement::create({}, src_stream_format,
            {}, vstreams_params.user_buffer_format, vstream_info->second.quant_info, fused_layer_nms_info,
            PipelineObject::create_element_name("PostInferElement", fused_layer_name, 0), vstreams_params, pipeline_status,
            shutdown_event);
        CHECK_EXPECTED_AS_STATUS(post_infer_elem);

        elements.push_back(post_infer_elem.value());
        CHECK_SUCCESS(PipelinePad::link_pads(nms_queue_elem.value(), post_infer_elem.value()));

        auto post_infer_queue_elem = UserBufferQueueElement::create(
            PipelineObject::create_element_name("UserBufferQueueElement_post_infer", fused_layer_name, 0),
            vstreams_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(post_infer_queue_elem);
        elements.push_back(post_infer_queue_elem.value());
        CHECK_SUCCESS(PipelinePad::link_pads(post_infer_elem.value(), post_infer_queue_elem.value()));

        auto vstream = OutputVStream::create(vstream_info->second, vstreams_params, post_infer_queue_elem.release(), std::move(elements),
            std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
        CHECK_EXPECTED_AS_STATUS(vstream);
        vstreams.emplace_back(vstream.release());
    } else {
        auto vstream = OutputVStream::create(vstream_info->second, vstreams_params, nms_elem.release(), std::move(elements),
            std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
        CHECK_EXPECTED_AS_STATUS(vstream);
        vstreams.emplace_back(vstream.release());
    }

    return HAILO_SUCCESS;
}

hailo_status VStreamsBuilderUtils::add_nms_post_process(OutputStreamPtrVector &output_streams, hailo_vstream_params_t &vstreams_params,
    std::vector<std::shared_ptr<PipelineElement>> &elements, std::vector<OutputVStream> &vstreams,
    EventPtr shutdown_event, std::shared_ptr<std::atomic<hailo_status>> pipeline_status,
    const std::map<std::string, hailo_vstream_info_t> &output_vstream_infos,
    const NetFlowElement &nms_op)
{
    auto first_stream_info = output_streams[0]->get_info();
    if (vstreams_params.user_buffer_format.type == HAILO_FORMAT_TYPE_AUTO) {
        vstreams_params.user_buffer_format.type = HAILO_FORMAT_TYPE_FLOAT32;
    }
    if (vstreams_params.user_buffer_format.order == HAILO_FORMAT_ORDER_AUTO) {
        vstreams_params.user_buffer_format.order = HAILO_FORMAT_ORDER_HAILO_NMS;
    }
    vstreams_params = expand_vstream_params_autos(first_stream_info, vstreams_params);
    CHECK(vstreams_params.user_buffer_format.type == HAILO_FORMAT_TYPE_FLOAT32, HAILO_INVALID_ARGUMENT,
        "NMS output format type must be HAILO_FORMAT_TYPE_FLOAT32");
    CHECK(vstreams_params.user_buffer_format.order == HAILO_FORMAT_ORDER_HAILO_NMS, HAILO_INVALID_ARGUMENT,
        "NMS output format order must be HAILO_FORMAT_ORDER_HAILO_NMS");

    std::map<std::string, net_flow::BufferMetaData> inputs_metadata;
    std::map<std::string, net_flow::BufferMetaData> outputs_metadata;
    for (uint32_t i = 0; i < output_streams.size(); ++i) {
        const auto &curr_stream_info = output_streams[i]->get_info();
        net_flow::BufferMetaData input_metadata = {
            curr_stream_info.shape,
            curr_stream_info.hw_shape,
            curr_stream_info.format,
            curr_stream_info.quant_info
        };
        inputs_metadata.insert({curr_stream_info.name, input_metadata});
    }

    const auto &output_pads = nms_op.op->outputs_metadata();
    assert(output_pads.size() == 1);
    auto vstream_info = output_vstream_infos.find(output_pads.begin()->first);
    CHECK(vstream_info != output_vstream_infos.end(), HAILO_NOT_FOUND,
        "Failed to find vstream info of {}", nms_op.name);
    net_flow::BufferMetaData output_metadata = {
        vstream_info->second.shape,
        vstream_info->second.shape,
        vstream_info->second.format,
        vstream_info->second.quant_info
    };
    outputs_metadata.insert({vstream_info->first, output_metadata});

    auto nms_elem = NmsPostProcessMuxElement::create(nms_op.op, nms_op.nms_info,
        PipelineObject::create_element_name("NmsPostProcessMuxElement", nms_op.name, 0),
        vstreams_params, shutdown_event, pipeline_status);
    CHECK_EXPECTED_AS_STATUS(nms_elem);

    hailo_format_t nms_src_format;
    nms_src_format.flags = HAILO_FORMAT_FLAGS_QUANTIZED;
    nms_src_format.order = HAILO_FORMAT_ORDER_NHCW;
    nms_src_format.type = first_stream_info.format.type;

    for (uint32_t i = 0; i < output_streams.size(); ++i) {
        const auto &curr_stream_info = output_streams[i]->get_info();
        output_streams[i]->set_timeout(HAILO_INFINITE_TIMEOUT);

        auto should_transform = OutputTransformContext::is_transformation_required(curr_stream_info.hw_shape, curr_stream_info.format,
            curr_stream_info.hw_shape, nms_src_format, vstream_info->second.quant_info);

        CHECK(!should_transform, HAILO_INVALID_ARGUMENT, "Unexpected transformation required for {}", curr_stream_info.name);

        auto hw_read_elem = HwReadElement::create(output_streams[i],
            PipelineObject::create_element_name("HwReadElement", curr_stream_info.name, curr_stream_info.index),
            HAILO_INFINITE_TIMEOUT, vstreams_params.queue_size, vstreams_params.pipeline_elements_stats_flags,
            vstreams_params.vstream_stats_flags, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(hw_read_elem);
        elements.push_back(hw_read_elem.value());

        auto nms_source_queue_elem = PullQueueElement::create(
            PipelineObject::create_element_name("PullQueueElement_nms_source", curr_stream_info.name, curr_stream_info.index),
            vstreams_params, shutdown_event, pipeline_status);
        CHECK_EXPECTED_AS_STATUS(nms_source_queue_elem);
        nms_source_queue_elem.value()->set_timeout(HAILO_INFINITE_TIMEOUT);
        elements.push_back(nms_source_queue_elem.value());
        CHECK_SUCCESS(PipelinePad::link_pads(hw_read_elem.value(), nms_source_queue_elem.value()));
        CHECK_SUCCESS(PipelinePad::link_pads(nms_source_queue_elem.value(), nms_elem.value(), 0, i));
        nms_elem.value()->add_sink_name(curr_stream_info.name);
    }
    elements.push_back(nms_elem.value());

    auto pipeline_latency_accumulator = create_pipeline_latency_accumulator(vstreams_params);
    CHECK_EXPECTED_AS_STATUS(pipeline_latency_accumulator);

    EventPtr core_op_activated_event = nullptr;
    if (!output_streams[0]->is_scheduled()) {
        core_op_activated_event = output_streams[0]->get_core_op_activated_event();
    }

    auto vstream = OutputVStream::create(vstream_info->second, vstreams_params, nms_elem.release(), std::move(elements),
        std::move(pipeline_status), shutdown_event, core_op_activated_event, pipeline_latency_accumulator.release());
    CHECK_EXPECTED_AS_STATUS(vstream);
    vstreams.emplace_back(vstream.release());

    return HAILO_SUCCESS;
}

Expected<AccumulatorPtr> VStreamsBuilderUtils::create_pipeline_latency_accumulator(const hailo_vstream_params_t &vstreams_params)
{
    AccumulatorPtr pipeline_latency_accumulator = nullptr;
    const auto measure_latency = ((vstreams_params.vstream_stats_flags & HAILO_VSTREAM_STATS_MEASURE_LATENCY) != 0);
    if (measure_latency) {
        pipeline_latency_accumulator = make_shared_nothrow<FullAccumulator<double>>("latency");
        CHECK_AS_EXPECTED(nullptr != pipeline_latency_accumulator, HAILO_OUT_OF_HOST_MEMORY);
    }

    return pipeline_latency_accumulator;
}

} /* namespace hailort */
