/**
 * Copyright (c) 2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file core_op_metadata.hpp
 * @brief Contains all relevant information about a core-op from the hef.
 **/

#ifndef _HAILO_CORE_OP_METADATA_HPP_
#define _HAILO_CORE_OP_METADATA_HPP_

#include "hef/layer_info.hpp"
#include "hef/context_switch_actions.hpp"


namespace hailort
{

constexpr const uint32_t PARTIAL_CLUSTERS_LAYOUT_IGNORE = static_cast<uint32_t>(-1);

struct SupportedFeatures {
    bool padded_ddr_buffers = false;
    bool multi_network_support = false;
    bool multi_context = false;
    bool preliminary_run_asap = false;
    bool hailo_net_flow = false;
    bool dual_direction_stream_index = false;
    bool nms_burst_mode = false;
    bool output_scale_by_feature = false;
    bool periph_calculation_in_hailort = false;
};

// For each config_stream_index we store vector of all ccw write length. The vector is used to build the config buffer.g
using ConfigBufferInfoMap = std::unordered_map<uint8_t, std::vector<uint32_t>>;


class ContextMetadata final {
public:
    ContextMetadata(std::vector<ContextSwitchConfigActionPtr> &&actions,
        ConfigBufferInfoMap&& config_buffers_info);

    const std::vector<ContextSwitchConfigActionPtr> &get_actions() const;
    std::vector<ContextSwitchConfigActionPtr> get_actions_of_type(
        const std::set<ContextSwitchConfigAction::Type> &action_types) const;

    const ConfigBufferInfoMap &config_buffers_info() const;

    void add_boundary_layer(const LayerInfo &layer_info);
    void add_inter_context_layer(const LayerInfo &layer_info);
    void add_ddr_layer(const LayerInfo &layer_info);

    const std::vector<LayerInfo> &get_boundary_input_layers() const;
    const std::vector<LayerInfo> &get_boundary_output_layers() const;
    const std::vector<LayerInfo> &get_inter_context_input_layers() const;
    const std::vector<LayerInfo> &get_inter_context_output_layers() const;
    const std::vector<LayerInfo> &get_ddr_input_layers() const;
    const std::vector<LayerInfo> &get_ddr_output_layers() const;

    Expected<size_t> get_layers_transfer_size(const std::vector<LayerInfo> &layer_infos) const;
    Expected<size_t> get_context_transfer_size() const;
private:
    std::vector<ContextSwitchConfigActionPtr> m_actions;
    ConfigBufferInfoMap m_config_buffers_info;

    std::vector<LayerInfo> m_boundary_input_layers;
    std::vector<LayerInfo> m_boundary_output_layers;
    std::vector<LayerInfo> m_inter_context_input_layers;
    std::vector<LayerInfo> m_inter_context_output_layers;
    std::vector<LayerInfo> m_ddr_input_layers;
    std::vector<LayerInfo> m_ddr_output_layers;
};

struct ConfigChannelInfo {
    uint8_t engine_index;
};

class CoreOpMetadata final {
public:
    CoreOpMetadata(const std::string &core_op_name,
        ContextMetadata &&preliminary_context,
        std::vector<ContextMetadata> &&dynamic_contexts,
        std::vector<ConfigChannelInfo> &&config_channels_info,
        SupportedFeatures &supported_features,
        std::vector<std::string> sorted_network_names);

    std::vector<LayerInfo> get_input_layer_infos() const;
    std::vector<LayerInfo> get_output_layer_infos() const;
    std::vector<LayerInfo> get_all_layer_infos() const;

    Expected<std::vector<LayerInfo>> get_input_layer_infos(const std::string &network_name) const;
    Expected<std::vector<LayerInfo>> get_output_layer_infos(const std::string &network_name) const;
    Expected<std::vector<LayerInfo>> get_all_layer_infos(const std::string &network_name) const;

    const ContextMetadata &preliminary_context() const;
    const std::vector<ContextMetadata> &dynamic_contexts() const;

    const std::vector<ConfigChannelInfo> &config_channels_info() const;

    // TODO: Move stream infos into NetworkGroupMetadata
    Expected<std::vector<hailo_stream_info_t>> get_input_stream_infos(const std::string &network_name = "") const;
    Expected<std::vector<hailo_stream_info_t>> get_output_stream_infos(const std::string &network_name = "") const;
    Expected<std::vector<hailo_stream_info_t>> get_all_stream_infos(const std::string &network_name = "") const;

    size_t get_contexts_count();

    const std::string &core_op_name() const
    {
        return m_core_op_name;
    }

    const SupportedFeatures &supported_features() const
    {
        return m_supported_features;
    }

    Expected<size_t> get_total_transfer_size();

    // TODO: Remove
    const std::vector<std::string> &get_network_names() const
    {
        return m_sorted_network_names;
    }

private:
    // TODO: Remove
    const std::string default_network_name() const
    {
        return HailoRTDefaults::get_network_name(m_core_op_name);
    }

    ContextMetadata m_preliminary_context;
    std::vector<ContextMetadata> m_dynamic_contexts;
    std::vector<ConfigChannelInfo> m_config_channels_info;
    std::string m_core_op_name;
    SupportedFeatures m_supported_features;
    std::vector<std::string> m_sorted_network_names;
};

using CoreOpMetadataPtr = std::shared_ptr<CoreOpMetadata>;

class CoreOpMetadataPerArch final
{
public:
    CoreOpMetadataPerArch() = default;

    Expected<CoreOpMetadataPtr> get_metadata(uint32_t partial_clusters_layout_bitmap) const;
    void add_metadata(const CoreOpMetadataPtr &metadata, uint32_t partial_clusters_layout_bitmap);

private:
    std::map<uint32_t, CoreOpMetadataPtr> m_metadata_per_arch;
};

struct NetFlowElement;

class NetworkGroupMetadata final {
public:
    static Expected<NetworkGroupMetadata> create(const std::string &network_group_name,
        std::map<std::string, CoreOpMetadataPerArch> &&core_ops_metadata_per_arch,
        std::vector<std::string> &sorted_output_names,
        SupportedFeatures &supported_features,
        const std::vector<std::string> &sorted_network_names,
        std::vector<std::shared_ptr<hailort::NetFlowElement>> &net_flow_ops);

    NetworkGroupMetadata(const std::string &network_group_name,
        std::map<std::string, CoreOpMetadataPerArch> &&core_ops_metadata_per_arch,
        std::vector<std::string> &sorted_output_names,
        SupportedFeatures &supported_features,
        const std::vector<std::string> &sorted_network_names,
        std::vector<hailo_vstream_info_t> &input_vstreams_infos,
        std::vector<hailo_vstream_info_t> &output_vstreams_infos,
        std::vector<std::shared_ptr<hailort::NetFlowElement>> &net_flow_ops) :
            m_network_group_name(network_group_name),
            m_sorted_output_names(sorted_output_names),
            m_supported_features(supported_features),
            m_sorted_network_names(sorted_network_names),
            m_input_vstreams_infos(input_vstreams_infos),
            m_output_vstreams_infos(output_vstreams_infos),
            m_core_ops_metadata_per_arch(std::move(core_ops_metadata_per_arch)),
            m_net_flow_ops(net_flow_ops)
        {};

    Expected<std::vector<hailo_vstream_info_t>> get_input_vstream_infos(const std::string &network_name = "") const;
    Expected<std::vector<hailo_vstream_info_t>> get_output_vstream_infos(const std::string &network_name = "") const;
    Expected<std::vector<hailo_vstream_info_t>> get_all_vstream_infos(const std::string &network_name = "") const;

    Expected<std::vector<std::string>> get_vstream_names_from_stream_name(const std::string &stream_name);
    Expected<std::vector<std::string>> get_stream_names_from_vstream_name(const std::string &vstream_name);

    Expected<std::vector<hailo_network_info_t>> get_network_infos() const;

    const std::string &name() const
    {
        return m_network_group_name;
    }

    const std::string default_network_name() const
    {
        return HailoRTDefaults::get_network_name(m_network_group_name);
    }

    const std::vector<std::string> get_sorted_output_names() const
    {
        return m_sorted_output_names;
    }

    const SupportedFeatures &supported_features() const
    {
        return m_supported_features;
    }

    const std::vector<std::string> &get_network_names() const
    {
        return m_sorted_network_names;
    }

private:
    static Expected<std::vector<LayerInfo>> get_all_layer_infos(std::map<std::string, CoreOpMetadataPerArch> &core_ops_metadata_per_arch)
    /* This function is used for names getters (such as get_vstream_names_from_stream_name),
       so should be same across all clusters layouts */
    {
        CHECK_AS_EXPECTED(1 == core_ops_metadata_per_arch.size(), HAILO_INTERNAL_FAILURE);
        auto core_op_metadata = core_ops_metadata_per_arch.begin()->second.get_metadata(PARTIAL_CLUSTERS_LAYOUT_IGNORE);
        CHECK_EXPECTED(core_op_metadata);

        return core_op_metadata.value()->get_all_layer_infos();
    }

    std::string m_network_group_name;
    std::vector<std::string> m_sorted_output_names;
    SupportedFeatures m_supported_features;
    std::vector<std::string> m_sorted_network_names;

    std::vector<hailo_vstream_info_t> m_input_vstreams_infos;
    std::vector<hailo_vstream_info_t> m_output_vstreams_infos;

    std::map<std::string, CoreOpMetadataPerArch> m_core_ops_metadata_per_arch; // Key is core_op_name
    std::vector<std::shared_ptr<NetFlowElement>> m_net_flow_ops;

    friend class Hef;
    friend class ConfiguredNetworkGroupBase;
};

} /* namespace hailort */

#endif /* _HAILO_CORE_OP_METADATA_HPP_ */
