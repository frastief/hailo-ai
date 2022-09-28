/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file vdma_config_network_group.hpp
 * @brief Represent network_group from HEF file that can be activated 
 *
 * This network_group can be used for both single or multi context network_groups but for PCIE only
  **/

#ifndef _HAILO_CONTEXT_SWITCH_VDMA_CONFIG_NETWORK_GROUP_HPP_
#define _HAILO_CONTEXT_SWITCH_VDMA_CONFIG_NETWORK_GROUP_HPP_

#include "hailo/hailort.h"
#include "vdma_channel.hpp"
#include "common/utils.hpp"
#include "context_switch/multi_context/vdma_config_activated_network_group.hpp"
#include "control_protocol.h"
#include "context_switch/active_network_group_holder.hpp"
#include "hailort_defaults.hpp"
#include "context_switch/network_group_internal.hpp"
#include "context_switch/multi_context/resource_manager.hpp"
#include "network_group_scheduler.hpp"
#include "context_switch/pipeline_multiplexer.hpp"

#include <cstdint>
#include <assert.h>
#include <map>
#include <set>

namespace hailort
{

#define MAX_CONTEXTS_COUNT (CONTROL_PROTOCOL__MAX_TOTAL_CONTEXTS)

class VdmaConfigNetworkGroup : public ConfiguredNetworkGroupBase
{
public:
    static Expected<VdmaConfigNetworkGroup> create(VdmaConfigActiveAppHolder &active_net_group_holder,
        const ConfigureNetworkParams &config_params, 
        std::vector<std::shared_ptr<ResourcesManager>> resources_managers, const std::string &hef_hash,
        std::shared_ptr<NetworkGroupMetadata> network_group_metadata, NetworkGroupSchedulerWeakPtr network_group_scheduler);

    std::vector<std::shared_ptr<ResourcesManager>> &get_resources_managers()
    {
        return m_resources_managers;
    }

    hailo_status create_vdevice_streams_from_config_params(std::shared_ptr<PipelineMultiplexer> multiplexer,
        scheduler_ng_handle_t scheduler_handle);
    hailo_status create_input_vdevice_stream_from_config_params(
        const hailo_stream_parameters_t &stream_params, const std::string &stream_name,
        std::shared_ptr<PipelineMultiplexer> multiplexer, scheduler_ng_handle_t scheduler_handle);
    hailo_status create_output_vdevice_stream_from_config_params(
        const hailo_stream_parameters_t &stream_params, const std::string &stream_name,
        std::shared_ptr<PipelineMultiplexer> multiplexer, scheduler_ng_handle_t scheduler_handle);

    hailo_status create_vdevice_streams_from_duplicate(std::shared_ptr<VdmaConfigNetworkGroup> other);

    virtual Expected<std::unique_ptr<ActivatedNetworkGroup>> activate_impl(
        const hailo_activate_network_group_params_t &network_group_params, uint16_t dynamic_batch_size) override;

    virtual Expected<hailo_stream_interface_t> get_default_streams_interface() override;

    virtual Expected<std::shared_ptr<LatencyMetersMap>> get_latency_meters() override;
    virtual Expected<std::shared_ptr<VdmaChannel>> get_boundary_vdma_channel_by_stream_name(
        const std::string &stream_name) override;

    void set_network_group_handle(scheduler_ng_handle_t handle);
    scheduler_ng_handle_t network_group_handle() const;
    virtual hailo_status set_scheduler_timeout(const std::chrono::milliseconds &timeout, const std::string &network_name) override;
    virtual hailo_status set_scheduler_threshold(uint32_t threshold, const std::string &network_name) override;

    virtual ~VdmaConfigNetworkGroup() = default;
    VdmaConfigNetworkGroup(const VdmaConfigNetworkGroup &other) = delete;
    VdmaConfigNetworkGroup &operator=(const VdmaConfigNetworkGroup &other) = delete;
    VdmaConfigNetworkGroup &operator=(VdmaConfigNetworkGroup &&other) = delete;
    VdmaConfigNetworkGroup(VdmaConfigNetworkGroup &&other) noexcept : ConfiguredNetworkGroupBase(std::move(other)),
        m_active_net_group_holder(other.m_active_net_group_holder),
        m_resources_managers(std::move(other.m_resources_managers)), m_network_group_scheduler(std::move(other.m_network_group_scheduler)),
        m_scheduler_handle(std::move(other.m_scheduler_handle)), m_multiplexer_handle(std::move(other.m_multiplexer_handle)),
        m_multiplexer(std::move(other.m_multiplexer)), m_hef_hash(std::move(other.m_hef_hash))
        {}

    bool equals(const Hef &hef, const std::string &network_group_name) {
        return (network_group_name == name()) && (hef.hash() == m_hef_hash);
    }

    uint32_t multiplexer_duplicates_count()
    {
        assert(m_multiplexer->instances_count() > 0);
        return static_cast<uint32_t>(m_multiplexer->instances_count() - 1);
    }

    virtual Expected<std::vector<OutputVStream>> create_output_vstreams(const std::map<std::string, hailo_vstream_params_t> &outputs_params) override;

private:
    VdmaConfigNetworkGroup(VdmaConfigActiveAppHolder &active_net_group_holder,
        const ConfigureNetworkParams &config_params, 
        std::vector<std::shared_ptr<ResourcesManager>> &&resources_managers, const std::string &hef_hash,
        const NetworkGroupMetadata &network_group_metadata, NetworkGroupSchedulerWeakPtr network_group_scheduler, hailo_status &status);

    VdmaConfigActiveAppHolder &m_active_net_group_holder;
    std::vector<std::shared_ptr<ResourcesManager>> m_resources_managers;
    NetworkGroupSchedulerWeakPtr m_network_group_scheduler;
    scheduler_ng_handle_t m_scheduler_handle;
    multiplexer_ng_handle_t m_multiplexer_handle;
    std::shared_ptr<PipelineMultiplexer> m_multiplexer;
    std::string m_hef_hash;
};

} /* namespace hailort */

#endif /* _HAILO_CONTEXT_SWITCH_VDMA_CONFIG_NETWORK_GROUP_HPP_ */
