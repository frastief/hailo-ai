#include "utils/profiler/tracer_macros.hpp"
#include "vdma/vdma_config_core_op.hpp"
#include "network_group/network_group_internal.hpp"
#include "net_flow/pipeline/vstream_internal.hpp"
#include "device_common/control.hpp"

namespace hailort
{

Expected<VdmaConfigCoreOp> VdmaConfigCoreOp::create(ActiveCoreOpHolder &active_core_op_holder,
        const ConfigureNetworkParams &config_params,
        std::shared_ptr<ResourcesManager> resources_manager,
        std::shared_ptr<CacheManager> cache_manager,
        std::shared_ptr<CoreOpMetadata> metadata)
{
    auto status = HAILO_UNINITIALIZED;

    VdmaConfigCoreOp core_op(active_core_op_holder, config_params, std::move(resources_manager), cache_manager,
        metadata, status);
    CHECK_SUCCESS_AS_EXPECTED(status);

    return core_op;
}

Expected<std::shared_ptr<VdmaConfigCoreOp>> VdmaConfigCoreOp::create_shared(ActiveCoreOpHolder &active_core_op_holder,
        const ConfigureNetworkParams &config_params,
        std::shared_ptr<ResourcesManager> resources_manager,
        std::shared_ptr<CacheManager> cache_manager,
        std::shared_ptr<CoreOpMetadata> metadata)
{
    TRY(auto core_op, create(active_core_op_holder, config_params, resources_manager, cache_manager, metadata));
    auto core_op_ptr = make_shared_nothrow<VdmaConfigCoreOp>(std::move(core_op));
    CHECK_NOT_NULL_AS_EXPECTED(core_op_ptr, HAILO_OUT_OF_HOST_MEMORY);

    return core_op_ptr;
}

VdmaConfigCoreOp::VdmaConfigCoreOp(ActiveCoreOpHolder &active_core_op_holder, const ConfigureNetworkParams &config_params,
                                   std::shared_ptr<ResourcesManager> &&resources_manager, std::shared_ptr<CacheManager> cache_manager,
                                   std::shared_ptr<CoreOpMetadata> metadata, hailo_status &status) :
    CoreOp(config_params, metadata, active_core_op_holder, status),
    m_resources_manager(std::move(resources_manager)),
    m_cache_manager(cache_manager)
{}


hailo_status VdmaConfigCoreOp::cancel_pending_transfers()
{
    // Best effort
    auto status = HAILO_SUCCESS;
    auto deactivate_status = HAILO_UNINITIALIZED;
    for (const auto &name_pair : m_input_streams) {
        deactivate_status = name_pair.second->cancel_pending_transfers();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed to cancel pending transfers for input stream {}", name_pair.first);
            status = deactivate_status;
        }
    }
    for (const auto &name_pair : m_output_streams) {
        deactivate_status = name_pair.second->cancel_pending_transfers();
        if (HAILO_SUCCESS != deactivate_status) {
            LOGGER__ERROR("Failed to cancel pending transfers for output stream {}", name_pair.first);
            status = deactivate_status;
        }
    }

    return status;
}

hailo_status VdmaConfigCoreOp::activate_impl(uint16_t dynamic_batch_size)
{
    auto status = register_cache_update_callback();
    CHECK_SUCCESS(status, "Failed to register cache update callback");

    auto start_time = std::chrono::steady_clock::now();
    if (CONTROL_PROTOCOL__IGNORE_DYNAMIC_BATCH_SIZE != dynamic_batch_size) {
        CHECK(dynamic_batch_size <= get_smallest_configured_batch_size(get_config_params()),
            HAILO_INVALID_ARGUMENT, "Batch size given is {} although max is {}", dynamic_batch_size,
            get_smallest_configured_batch_size(get_config_params()));
    }

    status = m_resources_manager->enable_state_machine(dynamic_batch_size);
    CHECK_SUCCESS(status, "Failed to activate state-machine");

    CHECK_SUCCESS(activate_host_resources(), "Failed to activate host resources");

    //TODO: HRT-13019 - Unite with the calculation in core_op.cpp
    const auto elapsed_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_time).count();
    TRACE(ActivateCoreOpTrace, std::string(m_resources_manager->get_dev_id()), vdevice_core_op_handle(), elapsed_time_ms, dynamic_batch_size);

    return HAILO_SUCCESS;
}

hailo_status VdmaConfigCoreOp::deactivate_impl()
{
    auto start_time = std::chrono::steady_clock::now();

    auto status = deactivate_host_resources();
    CHECK_SUCCESS(status);

    status = m_resources_manager->reset_state_machine();
    CHECK_SUCCESS(status, "Failed to reset context switch state machine");

    // After the state machine has been reset the vdma channels are no longer active, so we
    // can cancel pending transfers, thus allowing vdma buffers linked to said transfers to be freed
    status = cancel_pending_transfers();
    CHECK_SUCCESS(status, "Failed to cancel pending transfers");

    //TODO: HRT-13019 - Unite with the calculation in core_op.cpp
    const auto elapsed_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_time).count();
    TRACE(DeactivateCoreOpTrace, std::string(m_resources_manager->get_dev_id()), vdevice_core_op_handle(), elapsed_time_ms);

    status = unregister_cache_update_callback();
    CHECK_SUCCESS(status, "Failed to unregister cache update callback");

    return HAILO_SUCCESS;
}

hailo_status VdmaConfigCoreOp::register_cache_update_callback()
{
    const auto cache_offset_env_var = get_env_variable(HAILORT_AUTO_UPDATE_CACHE_OFFSET_ENV_VAR);
    if (cache_offset_env_var.has_value() && has_caches()) {
        std::string policy;
        int32_t offset_delta = 0;
        TRY(const auto cache_write_size, get_cache_write_size());
        if (cache_offset_env_var.value() == HAILORT_AUTO_UPDATE_CACHE_OFFSET_ENV_VAR_DEFAULT) {
            offset_delta = cache_write_size;
            policy = "cache write size (default)";
        } else if (cache_offset_env_var.value() == HAILORT_AUTO_UPDATE_CACHE_OFFSET_ENV_VAR_DISABLED) {
            LOGGER__INFO("Skipping cache offset updates");
            return HAILO_SUCCESS;
        } else {
            offset_delta = std::stoi(cache_offset_env_var.value());
            policy = "environment variable";
            CHECK(offset_delta <= static_cast<int32_t>(cache_write_size), HAILO_INVALID_ARGUMENT, "Invalid cache offset delta");
        }

        auto &vdma_device = m_resources_manager->get_device();
        vdma_device.set_notification_callback([this, offset_delta](Device &, const hailo_notification_t &notification, void *) {
            if (HAILO_NOTIFICATION_ID_START_UPDATE_CACHE_OFFSET != notification.id) {
                LOGGER__ERROR("Notification id passed to callback is invalid");
                return;
            }

            const auto status = this->update_cache_offset(static_cast<int32_t>(offset_delta));
            if (HAILO_SUCCESS != status) {
                LOGGER__ERROR("Failed to update cache offset");
            }
        }, HAILO_NOTIFICATION_ID_START_UPDATE_CACHE_OFFSET, nullptr);

        LOGGER__INFO("Cache offsets will automatically be updated by {} [{}]", offset_delta, policy);
    }

    return HAILO_SUCCESS;
}

hailo_status VdmaConfigCoreOp::unregister_cache_update_callback()
{
    auto &vdma_device = m_resources_manager->get_device();
    auto status = vdma_device.remove_notification_callback(HAILO_NOTIFICATION_ID_START_UPDATE_CACHE_OFFSET);
    CHECK_SUCCESS(status);
    return HAILO_SUCCESS;
}

hailo_status VdmaConfigCoreOp::shutdown()
{
    hailo_status status = HAILO_SUCCESS; // Success oriented

    auto abort_status = abort_low_level_streams();
    if (HAILO_SUCCESS != abort_status) {
        LOGGER__ERROR("Failed abort low level streams {}", abort_status);
        status = abort_status;
    }

    // On VdmaConfigCoreOp, shutdown is the same as deactivate. In the future, we can release the resources inside
    // the resource manager and free space in the firmware SRAM
    auto deactivate_status = deactivate_impl();
    if (HAILO_SUCCESS != deactivate_status) {
        LOGGER__ERROR("Failed deactivate core op with status {}", deactivate_status);
        status = deactivate_status;
    }

    return status;
}

hailo_status VdmaConfigCoreOp::activate_host_resources()
{
    CHECK_SUCCESS(m_resources_manager->start_vdma_transfer_launcher(), "Failed to start vdma transfer launcher");
    CHECK_SUCCESS(m_resources_manager->start_vdma_interrupts_dispatcher(), "Failed to start vdma interrupts");
    CHECK_SUCCESS(activate_low_level_streams(), "Failed to activate low level streams");
    return HAILO_SUCCESS;
}

hailo_status VdmaConfigCoreOp::deactivate_host_resources()
{
    CHECK_SUCCESS(deactivate_low_level_streams(), "Failed to deactivate low level streams");
    CHECK_SUCCESS(m_resources_manager->stop_vdma_interrupts_dispatcher(), "Failed to stop vdma interrupts");
    CHECK_SUCCESS(m_resources_manager->stop_vdma_transfer_launcher(), "Failed to stop vdma transfers pending launch");
    return HAILO_SUCCESS;
}

Expected<hailo_stream_interface_t> VdmaConfigCoreOp::get_default_streams_interface()
{
    return m_resources_manager->get_default_streams_interface();
}

bool VdmaConfigCoreOp::is_scheduled() const
{
    // Scheduler allowed only when working with VDevice and scheduler enabled.
    return false;
}

hailo_status VdmaConfigCoreOp::set_scheduler_timeout(const std::chrono::milliseconds &/*timeout*/, const std::string &/*network_name*/)
{
    LOGGER__ERROR("Setting scheduler's timeout is only allowed when working with VDevice and scheduler enabled");
    return HAILO_INVALID_OPERATION;
}

hailo_status VdmaConfigCoreOp::set_scheduler_threshold(uint32_t /*threshold*/, const std::string &/*network_name*/)
{
    LOGGER__ERROR("Setting scheduler's threshold is only allowed when working with VDevice and scheduler enabled");
    return HAILO_INVALID_OPERATION;
}

hailo_status VdmaConfigCoreOp::set_scheduler_priority(uint8_t /*priority*/, const std::string &/*network_name*/)
{
    LOGGER__ERROR("Setting scheduler's priority is only allowed when working with VDevice and scheduler enabled");
    return HAILO_INVALID_OPERATION;
}

Expected<std::shared_ptr<LatencyMetersMap>> VdmaConfigCoreOp::get_latency_meters()
{
    auto latency_meters = m_resources_manager->get_latency_meters();
    auto res = make_shared_nothrow<LatencyMetersMap>(latency_meters);
    CHECK_NOT_NULL_AS_EXPECTED(res, HAILO_OUT_OF_HOST_MEMORY);

    return res;
}

Expected<vdma::BoundaryChannelPtr> VdmaConfigCoreOp::get_boundary_vdma_channel_by_stream_name(const std::string &stream_name)
{
    return m_resources_manager->get_boundary_vdma_channel_by_stream_name(stream_name);
}

Expected<HwInferResults> VdmaConfigCoreOp::run_hw_infer_estimator()
{
    return m_resources_manager->run_hw_only_infer();
}

Expected<Buffer> VdmaConfigCoreOp::get_intermediate_buffer(const IntermediateBufferKey &key)
{
    return m_resources_manager->read_intermediate_buffer(key);
}

Expected<Buffer> VdmaConfigCoreOp::get_cache_buffer(uint32_t cache_id)
{
    return m_resources_manager->read_cache_buffer(cache_id);
}

Expected<std::map<uint32_t, Buffer>> VdmaConfigCoreOp::get_cache_buffers()
{
    return m_resources_manager->read_cache_buffers();
}

bool VdmaConfigCoreOp::has_caches() const
{
    return m_resources_manager->get_cache_buffers().size() > 0;
}

Expected<uint32_t> VdmaConfigCoreOp::get_cache_read_size() const
{
    // Input to the core == cache read
    size_t input_size = 0;
    for (auto &cache_buffer : m_resources_manager->get_cache_buffers()) {
        const auto curr_input_size = cache_buffer.second.input_size();
        if (input_size == 0) {
            input_size = curr_input_size;
        } else {
            CHECK(input_size == curr_input_size, HAILO_INVALID_ARGUMENT, "Cache buffers have different input sizes");
        }
    }

    return static_cast<uint32_t>(input_size);
}

Expected<uint32_t> VdmaConfigCoreOp::get_cache_write_size() const
{
    // Output from the core == cache write
    size_t output_size = 0;
    for (auto &cache_buffer : m_resources_manager->get_cache_buffers()) {
        const auto curr_output_size = cache_buffer.second.output_size();
        if (output_size == 0) {
            output_size = curr_output_size;
        } else {
            CHECK(output_size == curr_output_size, HAILO_INVALID_ARGUMENT, "Cache buffers have different output sizes");
        }
    }

    return static_cast<uint32_t>(output_size);
}


hailo_status VdmaConfigCoreOp::init_cache(uint32_t read_offset, int32_t write_offset_delta)
{
    CHECK(has_caches(), HAILO_INVALID_OPERATION, "No caches in core-op");
    return m_cache_manager->init_caches(read_offset, write_offset_delta);
}

Expected<hailo_cache_info_t> VdmaConfigCoreOp::get_cache_info() const
{
    CHECK(has_caches(), HAILO_INVALID_OPERATION, "No caches in core-op");

    return hailo_cache_info_t{
        m_cache_manager->get_cache_size(),
        m_cache_manager->get_read_offset_bytes(),
        m_cache_manager->get_write_offset_bytes_delta()
    };
}

hailo_status VdmaConfigCoreOp::update_cache_offset(int32_t offset_delta_bytes)
{
    CHECK(has_caches(), HAILO_INVALID_OPERATION, "No caches in core-op");

    // TODO: figure out how to do this s.t. it'll work with the sched (HRT-14287)
    // auto status = wait_for_activation(std::chrono::milliseconds(0));
    // CHECK_SUCCESS(status, "Core op must be activated before updating cache offset");

    // Update the offsets in the cache manager
    auto status = m_cache_manager->update_cache_offset(offset_delta_bytes);
    CHECK_SUCCESS(status);

    // Signal to the fw that the cache offset has been updated
    status = Control::context_switch_signal_cache_updated(m_resources_manager->get_device());
    CHECK_SUCCESS(status);

    return HAILO_SUCCESS;
}

} /* namespace hailort */
