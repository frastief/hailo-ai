/**
 * Copyright (c) 2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file context_switch_actions.cpp
 * @brief Contains classes represents the context switch action (Actions found in the HEFs
 *        and action sent to the fw).
 **/

#include "context_switch_actions.hpp"
#include "context_switch_defs.h"
#include "context_switch/multi_context/resource_manager.hpp"

namespace hailort
{


static uint8_t pack_vdma_channel_id(const vdma::ChannelId &channel_id)
{
    return static_cast<uint8_t>(channel_id.channel_index |
        (channel_id.engine_index << CONTEXT_SWITCH_DEFS__PACKED_VDMA_CHANNEL_ID__ENGINE_INDEX_SHIFT));
}

static uint8_t pack_lcu_id(uint8_t cluster_index, uint8_t lcu_index)
{
    return static_cast<uint8_t>(lcu_index |
        (cluster_index << CONTEXT_SWITCH_DEFS__PACKED_LCU_ID_CLUSTER_INDEX_SHIFT));
}

ContextSwitchConfigAction::ContextSwitchConfigAction(Type type) :
    ContextSwitchConfigAction(type, CONTEXT_SWITCH_DEFS__ACTION_TYPE_COUNT)
{}

ContextSwitchConfigAction::ContextSwitchConfigAction(Type type, CONTEXT_SWITCH_DEFS__ACTION_TYPE_t action_list_type) :
    m_type(type),
    m_action_list_type(action_list_type)
{}

Expected<std::vector<Buffer>> ContextSwitchConfigAction::serialize(const ContextResources &context_resources,
    bool is_repeated /*=false*/) const
{
    CHECK_AS_EXPECTED(m_action_list_type < CONTEXT_SWITCH_DEFS__ACTION_TYPE_COUNT, HAILO_INTERNAL_FAILURE,
        "Action cannot be serialized");

    auto header = serialize_header(is_repeated);
    CHECK_EXPECTED(header);

    auto params = serialize_params(context_resources);
    CHECK_EXPECTED(params);

    auto serialized_action = Buffer::create(header->size() + params->size());
    CHECK_EXPECTED(serialized_action);

    std::copy(header->begin(), header->end(), serialized_action->data());
    std::copy(params->begin(), params->end(), serialized_action->data() + header->size());

    std::vector<Buffer> buffers;
    buffers.emplace_back(serialized_action.release());
    return buffers;
}

ContextSwitchConfigAction::Type ContextSwitchConfigAction::get_type() const
{
    return m_type;
}

CONTEXT_SWITCH_DEFS__ACTION_TYPE_t ContextSwitchConfigAction::get_action_list_type() const
{
    return m_action_list_type;
}

Expected<Buffer> ContextSwitchConfigAction::serialize_header(bool is_repeated) const
{
    CHECK_AS_EXPECTED(m_action_list_type != CONTEXT_SWITCH_DEFS__ACTION_TYPE_COUNT, HAILO_INTERNAL_FAILURE,
        "Action cannot be serialized");

    auto header = Buffer::create(sizeof(CONTROL_PROTOCOL__ACTION_HEADER_t), 0);
    CHECK_EXPECTED(header);
    header->as_type<CONTROL_PROTOCOL__ACTION_HEADER_t>().action_type = m_action_list_type;
    header->as_type<CONTROL_PROTOCOL__ACTION_HEADER_t>().is_repeated = is_repeated; // TODO: prettier
    return header.release();
}

Expected<ContextSwitchConfigActionPtr> NoneAction::create()
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) NoneAction());
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

NoneAction::NoneAction() :
    ContextSwitchConfigAction(Type::None)
{}

Expected<std::vector<Buffer>> NoneAction::serialize(const ContextResources &, bool) const
{
    // Do nothing
    return std::vector<Buffer>();
}

bool NoneAction::supports_repeated_block() const
{
    // None actions are ignored and aren't written to the FW's action list. Hence they can't be part of a repeated block.
    return false;
}

Expected<Buffer> NoneAction::serialize_params(const ContextResources &) const
{
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}

Expected<ContextSwitchConfigActionPtr> ActivateConfigChannelAction::create(uint8_t config_stream_index,
    const vdma::ChannelId &channel_id, const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateConfigChannelAction(config_stream_index,
        channel_id, host_buffer_info));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateConfigChannelAction::ActivateConfigChannelAction(uint8_t config_stream_index,
    const vdma::ChannelId &channel_id, const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info) :
    ContextSwitchConfigAction(Type::ActivateConfigChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_CFG_CHANNEL),
    m_config_stream_index(config_stream_index),
    m_channel_id(channel_id),
    m_host_buffer_info(host_buffer_info)
{}

bool ActivateConfigChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be in repeated block for easier debug.
    return false;
}

Expected<Buffer> ActivateConfigChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_cfg_channel_t params{};
    params.config_stream_index = m_config_stream_index;
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.host_buffer_info = m_host_buffer_info;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> DeactivateConfigChannelAction::create(uint8_t config_stream_index,
    const vdma::ChannelId &channel_id)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) DeactivateConfigChannelAction(config_stream_index,
        channel_id));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

DeactivateConfigChannelAction::DeactivateConfigChannelAction(uint8_t config_stream_index,
    const vdma::ChannelId &channel_id) :
    ContextSwitchConfigAction(Type::DeactivateConfigChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_DEACTIVATE_CFG_CHANNEL),
    m_config_stream_index(config_stream_index),
    m_channel_id(channel_id)
{}

bool DeactivateConfigChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be in repeated block for easier debug.
    return false;
}

Expected<Buffer> DeactivateConfigChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__deactivate_cfg_channel_t params{};
    params.config_stream_index = m_config_stream_index;
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> WriteDataCcwAction::create(
    Buffer &&data, uint8_t config_stream_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WriteDataCcwAction(
        std::move(data), config_stream_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WriteDataCcwAction::WriteDataCcwAction(Buffer &&data, uint8_t config_stream_index) :
    ContextSwitchConfigAction(Type::WriteDataCcw),
    m_data(std::move(data)),
    m_config_stream_index(config_stream_index)
{}

Expected<std::vector<Buffer>> WriteDataCcwAction::serialize(const ContextResources &, bool) const
{
    // WriteDataCcwActions aren't written to the FW's action list. Hence the execute will do nothing.
    return std::vector<Buffer>();
}

bool WriteDataCcwAction::supports_repeated_block() const
{
    // WriteDataCcwActions aren't written to the FW's action list. Hence they can't be part of a repeated block.
    return false;
}

Expected<Buffer> WriteDataCcwAction::serialize_params(const ContextResources &) const
{
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}

Expected<ContextSwitchConfigActionPtr> AddCcwBurstAction::create(uint8_t config_stream_index, uint16_t ccw_bursts)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) AddCcwBurstAction(config_stream_index, ccw_bursts));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

AddCcwBurstAction::AddCcwBurstAction(uint8_t config_stream_index, uint16_t ccw_bursts) :
    ContextSwitchConfigAction(Type::AddCcwBurst, CONTEXT_SWITCH_DEFS__ACTION_TYPE_FETCH_CCW_BURSTS),
    m_config_stream_index(config_stream_index),
    m_ccw_bursts(ccw_bursts)
{}

Expected<Buffer> AddCcwBurstAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__fetch_ccw_bursts_action_data_t params{};
    params.ccw_bursts = m_ccw_bursts;
    params.config_stream_index = m_config_stream_index;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

bool AddCcwBurstAction::supports_repeated_block() const
{
    return false;
}

Expected<ContextSwitchConfigActionPtr> FetchCfgChannelDescriptorsAction::create(const vdma::ChannelId &channel_id,
    uint16_t desc_count)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) FetchCfgChannelDescriptorsAction(channel_id,
        desc_count));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

FetchCfgChannelDescriptorsAction::FetchCfgChannelDescriptorsAction(const vdma::ChannelId &channel_id, uint16_t desc_count) :
    ContextSwitchConfigAction(Type::FetchCfgChannelDescriptors, CONTEXT_SWITCH_DEFS__ACTION_TYPE_FETCH_CFG_CHANNEL_DESCRIPTORS),
    m_channel_id(channel_id),
    m_desc_count(desc_count)
{}

bool FetchCfgChannelDescriptorsAction::supports_repeated_block() const
{
    return true;
}

Expected<Buffer> FetchCfgChannelDescriptorsAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__fetch_cfg_channel_descriptors_action_data_t params{};
    params.descriptors_count = m_desc_count;
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> StartBurstCreditsTaskAction::create()
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) StartBurstCreditsTaskAction());
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

StartBurstCreditsTaskAction::StartBurstCreditsTaskAction() :
    ContextSwitchConfigAction(Type::StartBurstCreditsTask, CONTEXT_SWITCH_DEFS__ACTION_TYPE_BURST_CREDITS_TASK_START)
{}

bool StartBurstCreditsTaskAction::supports_repeated_block() const
{
    // We don't support repeated blocks for this action, since only one is added per group of consecutive
    // TriggerNewDataFromDataInput actions.
    return false;
}

Expected<Buffer> StartBurstCreditsTaskAction::serialize_params(const ContextResources &) const
{
    return Buffer::create(0);
}

Expected<ContextSwitchConfigActionPtr> WaitForNetworkGroupChangeAction::create()
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitForNetworkGroupChangeAction());
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitForNetworkGroupChangeAction::WaitForNetworkGroupChangeAction() :
    ContextSwitchConfigAction(Type::WaitForNetworkGroupChange,
    CONTEXT_SWITCH_DEFS__ACTION_TYPE_APPLICATION_CHANGE_INTERRUPT)
{}

bool WaitForNetworkGroupChangeAction::supports_repeated_block() const
{
    // Only one network group change action exists.
    return false;
}

Expected<Buffer> WaitForNetworkGroupChangeAction::serialize_params(const ContextResources &) const
{
    return Buffer::create(0);
}


Expected<ContextSwitchConfigActionPtr> RepeatedAction::create(
    std::vector<ContextSwitchConfigActionPtr> &&actions)
{
    CHECK_AS_EXPECTED(!actions.empty(), HAILO_INVALID_HEF, "Invalid sub-action count (must be greater than zero)");
    CHECK_AS_EXPECTED(IS_FIT_IN_UINT8(actions.size()), HAILO_INTERNAL_FAILURE,
        "Too many repeated actions {}", actions.size());
    CHECK_AS_EXPECTED(actions[0]->supports_repeated_block(), HAILO_INVALID_HEF,
        "Invalid repeated sub-action type (Action does not support repeated)");
    CHECK_AS_EXPECTED(actions[0]->get_action_list_type() != CONTEXT_SWITCH_DEFS__ACTION_TYPE_COUNT, HAILO_INVALID_HEF,
        "Invalid repeated sub-action type (can't have sub-action with type CONTEXT_SWITCH_DEFS__ACTION_TYPE_COUNT)");

    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) RepeatedAction(std::move(actions)));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

RepeatedAction::RepeatedAction(std::vector<ContextSwitchConfigActionPtr> &&actions) :
    ContextSwitchConfigAction(Type::AddRepeated, CONTEXT_SWITCH_DEFS__ACTION_TYPE_REPEATED_ACTION),
    m_actions(std::move(actions)),
    m_sub_action_type(m_actions[0]->get_action_list_type())
{}

bool RepeatedAction::supports_repeated_block() const
{
    // RepeatedActions can't be part of a repeated block themselves
    return false;
}

Expected<Buffer> RepeatedAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__repeated_action_header_t params{};
    params.sub_action_type = m_sub_action_type;
    params.last_executed = 0;
    params.count = static_cast<uint8_t>(m_actions.size());
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<std::vector<Buffer>> RepeatedAction::serialize(const ContextResources &context_resources,
    bool is_repeated/*=false*/) const
{
    CHECK_AS_EXPECTED(!is_repeated, HAILO_INTERNAL_FAILURE, "Cant use recursive repeated");
    std::vector<Buffer> buffers;
    buffers.reserve(m_actions.size() + 1); // Contains the repeated header and all of the actions

    auto repeated_header = ContextSwitchConfigAction::serialize(context_resources);
    CHECK_EXPECTED(repeated_header);
    CHECK_AS_EXPECTED(repeated_header->size() == 1, HAILO_INTERNAL_FAILURE, "Repeated action header should contain one buffer");
    buffers.emplace_back(std::move(repeated_header->at(0)));

    for (const auto &action : m_actions) {
        assert(action->get_action_list_type() == m_sub_action_type);
        const bool REPEATED = true;
        auto action_buffers = action->serialize(context_resources, REPEATED);
        CHECK_EXPECTED(action_buffers);
        CHECK_AS_EXPECTED(action_buffers->size() == 1, HAILO_INTERNAL_FAILURE, "Sub action should contain one buffer");
        buffers.emplace_back(std::move(action_buffers->at(0)));
    }

    return buffers;
}

Expected<ContextSwitchConfigActionPtr> DisableLcuAction::create(uint8_t cluster_index, uint8_t lcu_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) DisableLcuAction(cluster_index, lcu_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

DisableLcuAction::DisableLcuAction(uint8_t cluster_index, uint8_t lcu_index) :
    ContextSwitchConfigAction(Type::DisableLcu, CONTEXT_SWITCH_DEFS__ACTION_TYPE_DISABLE_LCU),
    m_cluster_index(cluster_index),
    m_lcu_index(lcu_index)
{}

bool DisableLcuAction::supports_repeated_block() const
{
    return true;
}

Expected<Buffer> DisableLcuAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__disable_lcu_action_data_t params{};
    params.packed_lcu_id = pack_lcu_id(m_cluster_index, m_lcu_index);
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> WaitForLcuAction::create(uint8_t cluster_index, uint8_t lcu_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitForLcuAction(cluster_index, lcu_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitForLcuAction::WaitForLcuAction(uint8_t cluster_index, uint8_t lcu_index) :
    ContextSwitchConfigAction(Type::WaitForLcu, CONTEXT_SWITCH_DEFS__ACTION_TYPE_LCU_INTERRUPT),
    m_cluster_index(cluster_index),
    m_lcu_index(lcu_index)
{}

bool WaitForLcuAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitForLcuAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__lcu_interrupt_data_t params{};
    params.packed_lcu_id = pack_lcu_id(m_cluster_index, m_lcu_index);
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> EnableLcuAction::create(uint8_t cluster_index, uint8_t lcu_index,
    uint8_t network_index, uint16_t kernel_done_address, uint32_t kernel_done_count)
{
    const auto is_default = (CONTEXT_SWITCH_DEFS__ENABLE_LCU_DEFAULT_KERNEL_ADDRESS == kernel_done_address) &&
        (CONTEXT_SWITCH_DEFS__ENABLE_LCU_DEFAULT_KERNEL_COUNT == kernel_done_count);
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) EnableLcuAction(cluster_index, lcu_index,
        network_index, kernel_done_address, kernel_done_count, is_default));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

CONTEXT_SWITCH_DEFS__ACTION_TYPE_t EnableLcuAction::get_enable_lcu_action_type(bool is_default)
{
    return is_default ? CONTEXT_SWITCH_DEFS__ACTION_TYPE_ENABLE_LCU_DEFAULT :
        CONTEXT_SWITCH_DEFS__ACTION_TYPE_ENABLE_LCU_NON_DEFAULT;
}

ContextSwitchConfigAction::Type EnableLcuAction::get_enable_lcu_type(bool is_default)
{
    return is_default ? Type::EnableLcuDefault : Type::EnableLcuNonDefault;
}

EnableLcuAction::EnableLcuAction(uint8_t cluster_index, uint8_t lcu_index,
    uint8_t network_index, uint16_t kernel_done_address, uint32_t kernel_done_count, bool is_default) :
    ContextSwitchConfigAction(get_enable_lcu_type(is_default), get_enable_lcu_action_type(is_default)),
    m_cluster_index(cluster_index),
    m_lcu_index(lcu_index),
    m_network_index(network_index),
    m_kernel_done_address(kernel_done_address),
    m_kernel_done_count(kernel_done_count),
    m_is_default(is_default)
{}

Expected<Buffer> EnableLcuAction::serialize_params(const ContextResources &) const
{
    if (m_is_default) {
        CONTEXT_SWITCH_DEFS__enable_lcu_action_default_data_t params{};
        params.packed_lcu_id = pack_lcu_id(m_cluster_index, m_lcu_index);
        params.network_index = m_network_index;
        return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
    }
    else {
        CONTEXT_SWITCH_DEFS__enable_lcu_action_non_default_data_t params{};
        params.packed_lcu_id = pack_lcu_id(m_cluster_index, m_lcu_index);
        params.kernel_done_address = m_kernel_done_address;
        params.kernel_done_count = m_kernel_done_count;
        params.network_index = m_network_index;
        return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
    }
}

bool EnableLcuAction::supports_repeated_block() const
{
    return true;
}

Expected<ContextSwitchConfigActionPtr> EnableSequencerAction::create(uint8_t cluster_index,
    uint8_t initial_l3_cut, uint16_t initial_l3_offset, uint32_t active_apu, uint32_t active_ia,
        uint64_t active_sc, uint64_t active_l2, uint64_t l2_offset_0, uint64_t l2_offset_1)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) EnableSequencerAction(cluster_index, initial_l3_cut,
        initial_l3_offset, active_apu, active_ia, active_sc, active_l2, l2_offset_0, l2_offset_1));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

EnableSequencerAction::EnableSequencerAction(uint8_t cluster_index, uint8_t initial_l3_cut, uint16_t initial_l3_offset,
    uint32_t active_apu, uint32_t active_ia, uint64_t active_sc, uint64_t active_l2, uint64_t l2_offset_0,
    uint64_t l2_offset_1) :
    ContextSwitchConfigAction(Type::TriggerSequencer, CONTEXT_SWITCH_DEFS__ACTION_TYPE_TRIGGER_SEQUENCER),
    m_cluster_index(cluster_index),
    m_initial_l3_cut(initial_l3_cut),
    m_initial_l3_offset(initial_l3_offset),
    m_active_apu(active_apu),
    m_active_ia(active_ia),
    m_active_sc(active_sc),
    m_active_l2(active_l2),
    m_l2_offset_0(l2_offset_0),
    m_l2_offset_1(l2_offset_1)
{}

bool EnableSequencerAction::supports_repeated_block() const
{
    return true;
}

Expected<Buffer> EnableSequencerAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__trigger_sequencer_action_data_t params{};
    params.cluster_index = m_cluster_index;
    params.sequencer_config.initial_l3_cut = m_initial_l3_cut;
    params.sequencer_config.initial_l3_offset = m_initial_l3_offset;
    params.sequencer_config.active_apu = m_active_apu;
    params.sequencer_config.active_ia = m_active_ia;
    params.sequencer_config.active_sc = m_active_sc;
    params.sequencer_config.active_l2 = m_active_l2;
    params.sequencer_config.l2_offset_0 = m_l2_offset_0;
    params.sequencer_config.l2_offset_1 = m_l2_offset_1;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> WaitForSequencerAction::create(uint8_t cluster_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitForSequencerAction(cluster_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitForSequencerAction::WaitForSequencerAction(uint8_t cluster_index) :
    ContextSwitchConfigAction(Type::WaitForSequencerDone, CONTEXT_SWITCH_DEFS__ACTION_TYPE_SEQUENCER_DONE_INTERRUPT),
    m_cluster_index(cluster_index)
{}

bool WaitForSequencerAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitForSequencerAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__sequencer_interrupt_data_t params{};
    params.sequencer_index = m_cluster_index;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> AllowInputDataflowAction::create(uint8_t stream_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) AllowInputDataflowAction(stream_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}


AllowInputDataflowAction::AllowInputDataflowAction(uint8_t stream_index) :
    ContextSwitchConfigAction(Type::TriggerNewDataFromDataInput,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_FETCH_DATA_FROM_VDMA_CHANNEL),
    m_stream_index(stream_index)
{}

bool AllowInputDataflowAction::supports_repeated_block() const
{
    // DDR threads are implemented on HailoRT so no FW action is required. Hence they can't be part of a repeated block.
    if (Type::TriggerNewDataFromDataInputDdr == m_type) {
        return false;
    }

    return true;
}

Expected<Buffer> AllowInputDataflowAction::serialize_params(const ContextResources &context_resources) const
{
    for (const auto &edge_layer : context_resources.get_boundary_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            CONTEXT_SWITCH_DEFS__fetch_data_action_data_t params{};
            params.packed_vdma_channel_id = pack_vdma_channel_id(edge_layer.channel_id);
            params.stream_index = m_stream_index;
            params.network_index = edge_layer.layer_info.network_index;
            params.credit_type = CONTEXT_SWITCH_DEFS__CREDIT_IN_BYTES;
            params.host_buffer_type = CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t(edge_layer.buffer_info.buffer_type);
            params.periph_bytes_per_buffer = edge_layer.layer_info.nn_stream_config.periph_bytes_per_buffer;
            params.frame_periph_size = edge_layer.layer_info.nn_stream_config.periph_bytes_per_buffer *
                edge_layer.layer_info.nn_stream_config.periph_buffers_per_frame;
            return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
        }
    }

    for (const auto &edge_layer : context_resources.get_inter_context_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            CONTEXT_SWITCH_DEFS__fetch_data_action_data_t params{};
            params.packed_vdma_channel_id = pack_vdma_channel_id(edge_layer.channel_id);
            params.stream_index = m_stream_index;
            params.network_index = edge_layer.layer_info.network_index;
            params.credit_type = CONTEXT_SWITCH_DEFS__CREDIT_IN_DESCRIPTORS;
            params.host_buffer_type = CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t(edge_layer.buffer_info.buffer_type);
            params.periph_bytes_per_buffer = edge_layer.layer_info.nn_stream_config.periph_bytes_per_buffer;
            params.frame_periph_size = ((edge_layer.buffer_info.bytes_in_pattern - 1) / (edge_layer.buffer_info.desc_page_size)) + 1;
            return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
        }
    }

    LOGGER__ERROR("Stream {} not found in edge layers", m_stream_index);
    return make_unexpected(HAILO_INTERNAL_FAILURE);
}

Expected<ContextSwitchConfigActionPtr> WaitForModuleConfigDoneAction::create(uint8_t module_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitForModuleConfigDoneAction(module_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitForModuleConfigDoneAction::WaitForModuleConfigDoneAction(uint8_t module_index) :
    ContextSwitchConfigAction(Type::WaitForModuleConfigDone, CONTEXT_SWITCH_DEFS__ACTION_TYPE_MODULE_CONFIG_DONE_INTERRUPT),
    m_module_index(module_index)
{}

bool WaitForModuleConfigDoneAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitForModuleConfigDoneAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__module_config_done_interrupt_data_t params{};
    params.module_index = m_module_index;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> DdrPairInfoAction::create(const vdma::ChannelId &h2d_channel_id,
    const vdma::ChannelId &d2h_channel_id, uint8_t network_index, uint32_t descriptors_per_frame, uint16_t descs_count)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) DdrPairInfoAction(
        h2d_channel_id, d2h_channel_id, network_index, descriptors_per_frame, descs_count));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

DdrPairInfoAction::DdrPairInfoAction(const vdma::ChannelId &h2d_channel_id, const vdma::ChannelId &d2h_channel_id,
    uint8_t network_index, uint32_t descriptors_per_frame, uint16_t descs_count) :
    ContextSwitchConfigAction(Type::DdrPairInfo, CONTEXT_SWITCH_DEFS__ACTION_TYPE_ADD_DDR_PAIR_INFO),
    m_h2d_channel_id(h2d_channel_id),
    m_d2h_channel_id(d2h_channel_id),
    m_network_index(network_index),
    m_descriptors_per_frame(descriptors_per_frame),
    m_descs_count(descs_count)
{}

bool DdrPairInfoAction::supports_repeated_block() const
{
    return true;
}

Expected<Buffer> DdrPairInfoAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__add_ddr_pair_info_action_data_t params{};
    params.h2d_packed_vdma_channel_id = pack_vdma_channel_id(m_h2d_channel_id);
    params.d2h_packed_vdma_channel_id = pack_vdma_channel_id(m_d2h_channel_id);
    params.network_index = m_network_index;
    params.descriptors_per_frame = m_descriptors_per_frame;
    params.programmed_descriptors_count = m_descs_count;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> StartDdrBufferingTaskAction::create()
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) StartDdrBufferingTaskAction());
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

StartDdrBufferingTaskAction::StartDdrBufferingTaskAction() :
    ContextSwitchConfigAction(Type::StartDdrBufferingTask, CONTEXT_SWITCH_DEFS__ACTION_TYPE_DDR_BUFFERING_START)
{}

bool StartDdrBufferingTaskAction::supports_repeated_block() const
{
    // There should only be one "start ddr buffering task action" per context,
    // so there's no need to support repeated blocks.
    return false;
}

Expected<Buffer> StartDdrBufferingTaskAction::serialize_params(const ContextResources &) const
{
    return Buffer::create(0);
}

Expected<ContextSwitchConfigActionPtr> ResetDdrBufferingTaskAction::create()
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ResetDdrBufferingTaskAction());
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ResetDdrBufferingTaskAction::ResetDdrBufferingTaskAction() :
    ContextSwitchConfigAction(Type::ResetDdrBufferingTask, CONTEXT_SWITCH_DEFS__ACTION_TYPE_DDR_BUFFERING_RESET)
{}

bool ResetDdrBufferingTaskAction::supports_repeated_block() const
{
    // There should only be one "reset ddr buffering task action" per context at most,
    // so there's no need to support repeated blocks.
    return false;
}

Expected<Buffer> ResetDdrBufferingTaskAction::serialize_params(const ContextResources &) const
{
    return Buffer::create(0);
}

Expected<ContextSwitchConfigActionPtr> ChangeVdmaToStreamMapping::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, bool is_dummy_stream)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ChangeVdmaToStreamMapping(channel_id, stream_index,
        is_dummy_stream));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ChangeVdmaToStreamMapping::ChangeVdmaToStreamMapping(const vdma::ChannelId &channel_id, uint8_t stream_index,
    bool is_dummy_stream) :
    ContextSwitchConfigAction(Type::ChangeVdmaToStreamMapping,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_CHANGE_VDMA_TO_STREAM_MAPPING),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_is_dummy_stream(is_dummy_stream)
{}

bool ChangeVdmaToStreamMapping::supports_repeated_block() const
{
    return true;
}

Expected<Buffer> ChangeVdmaToStreamMapping::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__change_vdma_to_stream_mapping_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.is_dummy_stream = m_is_dummy_stream;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> WaitOutputTransferDoneAction::create(uint8_t stream_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitOutputTransferDoneAction(stream_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitOutputTransferDoneAction::WaitOutputTransferDoneAction(uint8_t stream_index) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::WaitOutputTransferDone, CONTEXT_SWITCH_DEFS__ACTION_TYPE_OUTPUT_CHANNEL_TRANSFER_DONE_INTERRUPT),
    m_stream_index(stream_index)
{}

bool WaitOutputTransferDoneAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitOutputTransferDoneAction::serialize_params(const ContextResources &context_resources) const
{
    const auto channel_id = get_layer_channel_id(context_resources);
    CHECK_EXPECTED(channel_id);

    CONTEXT_SWITCH_DEFS__vdma_dataflow_interrupt_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(channel_id.value());
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<vdma::ChannelId> WaitOutputTransferDoneAction::get_layer_channel_id(const ContextResources &context_resources) const
{
    // TODO: HRT-8611 use one loop
    for (const auto &edge_layer : context_resources.get_boundary_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            return vdma::ChannelId(edge_layer.channel_id);
        }
    }

    for (const auto &edge_layer : context_resources.get_inter_context_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            return vdma::ChannelId(edge_layer.channel_id);
        }
    }

    LOGGER__ERROR("Stream {} not found in edge layers", m_stream_index);
    return make_unexpected(HAILO_INTERNAL_FAILURE);
}

Expected<ContextSwitchConfigActionPtr> OpenBoundaryInputChannelAction::create(const vdma::ChannelId &channel_id,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) OpenBoundaryInputChannelAction(channel_id,
        host_buffer_info));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

OpenBoundaryInputChannelAction::OpenBoundaryInputChannelAction(const vdma::ChannelId &channel_id,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::OpenBoundaryInputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_OPEN_BOUNDARY_INPUT_CHANNEL),
    m_channel_id(channel_id),
    m_host_buffer_info(host_buffer_info)
{}

bool OpenBoundaryInputChannelAction::supports_repeated_block() const
{
    // Open boundary actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> OpenBoundaryInputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__open_boundary_input_channel_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.host_buffer_info = m_host_buffer_info;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> OpenBoundaryOutputChannelAction::create(const vdma::ChannelId &channel_id,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) OpenBoundaryOutputChannelAction(channel_id,
        host_buffer_info));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

OpenBoundaryOutputChannelAction::OpenBoundaryOutputChannelAction(const vdma::ChannelId &channel_id,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::OpenBoundaryOutputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_OPEN_BOUNDARY_OUTPUT_CHANNEL),
    m_channel_id(channel_id),
    m_host_buffer_info(host_buffer_info)
{}

bool OpenBoundaryOutputChannelAction::supports_repeated_block() const
{
    // Open boundary actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> OpenBoundaryOutputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__open_boundary_output_channel_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.host_buffer_info = m_host_buffer_info;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

// TODO HRT-8705: remove nn_stream_config struct (that this function won't be needed)
static CONTEXT_SWITCH_DEFS__stream_reg_info_t parse_nn_config(const CONTROL_PROTOCOL__nn_stream_config_t &nn_config)
{
    CONTEXT_SWITCH_DEFS__stream_reg_info_t reg_info{};
    reg_info.core_bytes_per_buffer = nn_config.core_bytes_per_buffer;
    reg_info.core_buffers_per_frame = nn_config.core_buffers_per_frame;
    reg_info.feature_padding_payload = nn_config.feature_padding_payload;
    reg_info.buffer_padding_payload = nn_config.buffer_padding_payload;
    reg_info.buffer_padding = nn_config.buffer_padding;
    reg_info.periph_bytes_per_buffer = nn_config.periph_bytes_per_buffer;
    reg_info.periph_buffers_per_frame = nn_config.periph_buffers_per_frame;
    return reg_info;
}

Expected<ContextSwitchConfigActionPtr> ActivateBoundaryInputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateBoundaryInputChannelAction(channel_id,
        stream_index, nn_stream_config, host_buffer_info, initial_credit_size));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateBoundaryInputChannelAction::ActivateBoundaryInputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateBoundaryInputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_BOUNDARY_INPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info),
    m_initial_credit_size(initial_credit_size)
{}

bool ActivateBoundaryInputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateBoundaryInputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_boundary_input_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    params.initial_credit_size = m_initial_credit_size;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ActivateBoundaryOutputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateBoundaryOutputChannelAction(channel_id,
        stream_index, nn_stream_config, host_buffer_info));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateBoundaryOutputChannelAction::ActivateBoundaryOutputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateBoundaryOutputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_BOUNDARY_OUTPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info)
{}

bool ActivateBoundaryOutputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateBoundaryOutputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_boundary_output_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ActivateInterContextInputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateInterContextInputChannelAction(channel_id,
        stream_index, nn_stream_config, host_buffer_info, initial_credit_size));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateInterContextInputChannelAction::ActivateInterContextInputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateInterContextInputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_INTER_CONTEXT_INPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info),
    m_initial_credit_size(initial_credit_size)
{}

bool ActivateInterContextInputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateInterContextInputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_inter_context_input_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    params.initial_credit_size = m_initial_credit_size;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ActivateInterContextOutputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, uint8_t network_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateInterContextOutputChannelAction(channel_id,
        stream_index, network_index, nn_stream_config, host_buffer_info));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateInterContextOutputChannelAction::ActivateInterContextOutputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, uint8_t network_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateInterContextOutputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_INTER_CONTEXT_OUTPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_network_index(network_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info)
{}

bool ActivateInterContextOutputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateInterContextOutputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_inter_context_output_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.network_index = m_network_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ActivateDdrInputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size,
    const vdma::ChannelId &connected_d2h_channel_id)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateDdrInputChannelAction(channel_id,
        stream_index, nn_stream_config, host_buffer_info, initial_credit_size, connected_d2h_channel_id));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateDdrInputChannelAction::ActivateDdrInputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t initial_credit_size,
    const vdma::ChannelId &connected_d2h_channel_id) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateDdrInputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_DDR_BUFFER_INPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info),
    m_initial_credit_size(initial_credit_size),
    m_connected_d2h_channel_id(connected_d2h_channel_id)
{}

bool ActivateDdrInputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateDdrInputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_ddr_buffer_input_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    params.initial_credit_size = m_initial_credit_size;
    params.connected_d2h_packed_vdma_channel_id = pack_vdma_channel_id(m_connected_d2h_channel_id);
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ActivateDdrOutputChannelAction::create(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t buffered_rows_count)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ActivateDdrOutputChannelAction(channel_id,
        stream_index, nn_stream_config, host_buffer_info, buffered_rows_count));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ActivateDdrOutputChannelAction::ActivateDdrOutputChannelAction(const vdma::ChannelId &channel_id,
    uint8_t stream_index, const CONTROL_PROTOCOL__nn_stream_config_t &nn_stream_config,
    const CONTROL_PROTOCOL__host_buffer_info_t &host_buffer_info, uint32_t buffered_rows_count) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ActivateDdrOutputChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_ACTIVATE_DDR_BUFFER_OUTPUT),
    m_channel_id(channel_id),
    m_stream_index(stream_index),
    m_nn_stream_config(nn_stream_config),
    m_host_buffer_info(host_buffer_info),
    m_buffered_rows_count(buffered_rows_count)
{}

bool ActivateDdrOutputChannelAction::supports_repeated_block() const
{
    // Activate actions shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ActivateDdrOutputChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__activate_ddr_buffer_output_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.stream_index = m_stream_index;
    params.stream_reg_info = parse_nn_config(m_nn_stream_config);
    params.host_buffer_info = m_host_buffer_info;
    params.buffered_rows_count = m_buffered_rows_count;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> ValidateChannelAction::create(const vdma::ChannelId &channel_id,
    hailo_stream_direction_t stream_direction, bool is_inter_context,
    CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t host_buffer_type, uint32_t initial_credit_size)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) ValidateChannelAction(channel_id, stream_direction,
        is_inter_context, host_buffer_type, initial_credit_size));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

ValidateChannelAction::ValidateChannelAction(const vdma::ChannelId &channel_id,
    hailo_stream_direction_t stream_direction, bool is_inter_context,
    CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t host_buffer_type, uint32_t initial_credit_size) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::ValidateChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_VALIDATE_VDMA_CHANNEL),
    m_channel_id(channel_id),
    m_stream_direction(stream_direction),
    m_is_inter_context(is_inter_context),
    m_host_buffer_type(host_buffer_type),
    m_initial_credit_size(initial_credit_size)
{}

bool ValidateChannelAction::supports_repeated_block() const
{
    // Validate action shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> ValidateChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__validate_vdma_channel_action_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.edge_layer_direction = m_stream_direction == HAILO_H2D_STREAM ?
        static_cast<uint8_t>(CONTEXT_SWITCH_DEFS__EDGE_LAYER_DIRECTION_HOST_TO_DEVICE) :
        static_cast<uint8_t>(CONTEXT_SWITCH_DEFS__EDGE_LAYER_DIRECTION_DEVICE_TO_HOST);
    params.is_inter_context = m_is_inter_context;
    params.host_buffer_type = static_cast<uint8_t>(m_host_buffer_type);
    params.initial_credit_size = m_initial_credit_size;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> DeactivateChannelAction::create(const vdma::ChannelId &channel_id,
    hailo_stream_direction_t stream_direction, bool is_inter_context,
    CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t host_buffer_type, uint32_t initial_credit_size)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) DeactivateChannelAction(channel_id, stream_direction,
        is_inter_context, host_buffer_type, initial_credit_size));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

DeactivateChannelAction::DeactivateChannelAction(const vdma::ChannelId &channel_id,
    hailo_stream_direction_t stream_direction, bool is_inter_context,
    CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t host_buffer_type, uint32_t initial_credit_size) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::DeactivateChannel,
                              CONTEXT_SWITCH_DEFS__ACTION_TYPE_DEACTIVATE_VDMA_CHANNEL),
    m_channel_id(channel_id),
    m_stream_direction(stream_direction),
    m_is_inter_context(is_inter_context),
    m_host_buffer_type(host_buffer_type),
    m_initial_credit_size(initial_credit_size)
{}

bool DeactivateChannelAction::supports_repeated_block() const
{
    // Deactivate action shouldn't be repeated (for easier debugging).
    return false;
}

Expected<Buffer> DeactivateChannelAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__deactivate_vdma_channel_action_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(m_channel_id);
    params.edge_layer_direction = m_stream_direction == HAILO_H2D_STREAM ? 
        static_cast<uint8_t>(CONTEXT_SWITCH_DEFS__EDGE_LAYER_DIRECTION_HOST_TO_DEVICE) : 
        static_cast<uint8_t>(CONTEXT_SWITCH_DEFS__EDGE_LAYER_DIRECTION_DEVICE_TO_HOST);
    params.is_inter_context = m_is_inter_context;
    params.host_buffer_type = static_cast<uint8_t>(m_host_buffer_type);
    params.initial_credit_size = m_initial_credit_size;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> WaitDmaIdleAction::create(uint8_t stream_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitDmaIdleAction(stream_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitDmaIdleAction::WaitDmaIdleAction(uint8_t stream_index) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::WaitDmaIdle, CONTEXT_SWITCH_DEFS__ACTION_TYPE_WAIT_FOR_DMA_IDLE_ACTION),
    m_stream_index(stream_index)
{}

bool WaitDmaIdleAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitDmaIdleAction::serialize_params(const ContextResources &context_resources) const
{
    auto channel_and_type = get_layer_channel_id_and_type(context_resources);
    CHECK_EXPECTED(channel_and_type);

    const auto channel_id = channel_and_type->first;
    assert(LayerType::INTER_CONTEXT == channel_and_type->second || LayerType::BOUNDARY == channel_and_type->second);
    const bool is_inter_context = (LayerType::INTER_CONTEXT == channel_and_type->second);

    CONTEXT_SWITCH_DEFS__wait_dma_idle_data_t params{};
    params.packed_vdma_channel_id = pack_vdma_channel_id(channel_id);
    params.is_inter_context = static_cast<uint8_t>(is_inter_context);
    params.stream_index = m_stream_index;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<std::pair<vdma::ChannelId, LayerType>> WaitDmaIdleAction::get_layer_channel_id_and_type(
    const ContextResources &context_resources) const
{
    // TODO: HRT-8611 use one loop
    for (const auto &edge_layer : context_resources.get_boundary_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            return std::make_pair(edge_layer.channel_id, LayerType::BOUNDARY);
        }
    }

    for (const auto &edge_layer : context_resources.get_inter_context_layers()) {
        if (m_stream_index == edge_layer.layer_info.stream_index) {
            return std::make_pair(edge_layer.channel_id, LayerType::INTER_CONTEXT);
        }
    }

    LOGGER__ERROR("Stream {} not found in edge layers (as boundary or inter context)", m_stream_index);
    return make_unexpected(HAILO_INTERNAL_FAILURE);
}

Expected<ContextSwitchConfigActionPtr> WaitNmsIdleAction::create(uint8_t aggregator_index,
    uint8_t pred_cluster_ob_index, uint8_t pred_cluster_ob_cluster_index, uint8_t pred_cluster_ob_interface,
    uint8_t succ_prepost_ob_index, uint8_t succ_prepost_ob_interface)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) WaitNmsIdleAction(aggregator_index,
        pred_cluster_ob_index, pred_cluster_ob_cluster_index, pred_cluster_ob_interface, succ_prepost_ob_index,
        succ_prepost_ob_interface));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

WaitNmsIdleAction::WaitNmsIdleAction(uint8_t aggregator_index, uint8_t pred_cluster_ob_index, uint8_t pred_cluster_ob_cluster_index,
    uint8_t pred_cluster_ob_interface, uint8_t succ_prepost_ob_index, uint8_t succ_prepost_ob_interface) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::WaitNmsIdle, CONTEXT_SWITCH_DEFS__ACTION_TYPE_WAIT_FOR_NMS),
    m_aggregator_index(aggregator_index),
    m_pred_cluster_ob_index(pred_cluster_ob_index),
    m_pred_cluster_ob_cluster_index(pred_cluster_ob_cluster_index),
    m_pred_cluster_ob_interface(pred_cluster_ob_interface),
    m_succ_prepost_ob_index(succ_prepost_ob_index),
    m_succ_prepost_ob_interface(succ_prepost_ob_interface)
{}

bool WaitNmsIdleAction::supports_repeated_block() const
{
    // Wait actions shouldn't be repeated (for easier debugging)
    return false;
}

Expected<Buffer> WaitNmsIdleAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__wait_nms_data_t params{};
    params.aggregator_index = m_aggregator_index;
    params.pred_cluster_ob_index = m_pred_cluster_ob_index;
    params.pred_cluster_ob_cluster_index = m_pred_cluster_ob_cluster_index;
    params.pred_cluster_ob_interface = m_pred_cluster_ob_interface;
    params.succ_prepost_ob_index = m_succ_prepost_ob_index;
    params.succ_prepost_ob_interface = m_succ_prepost_ob_interface;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

Expected<ContextSwitchConfigActionPtr> EnableNmsAction::create(uint8_t nms_unit_index, uint8_t network_index)
{
    auto result = ContextSwitchConfigActionPtr(new (std::nothrow) EnableNmsAction(nms_unit_index, network_index));
    CHECK_AS_EXPECTED((nullptr != result), HAILO_OUT_OF_HOST_MEMORY);
    return result;
}

EnableNmsAction::EnableNmsAction(uint8_t nms_unit_index, uint8_t network_index) :
    ContextSwitchConfigAction(ContextSwitchConfigAction::Type::EnableNms, CONTEXT_SWITCH_DEFS__ACTION_TYPE_ENABLE_NMS),
    m_nms_unit_index(nms_unit_index),
    m_network_index(network_index)
{}

Expected<Buffer> EnableNmsAction::serialize_params(const ContextResources &) const
{
    CONTEXT_SWITCH_DEFS__enable_nms_action_t params{};
    params.nms_unit_index = m_nms_unit_index;
    params.network_index = m_network_index;
    return Buffer::create(reinterpret_cast<uint8_t*>(&params), sizeof(params));
}

bool EnableNmsAction::supports_repeated_block() const
{
    return true;
}

ContextSwitchOperation::ContextSwitchOperation(std::vector<ContextSwitchConfigActionPtr> &&actions) :
    m_actions(std::move(actions))
{}

const std::vector<ContextSwitchConfigActionPtr> &ContextSwitchOperation::actions() const
{
    return m_actions;
}

std::vector<ContextSwitchConfigActionPtr> ContextSwitchOperation::get_actions_of_type(
    const std::set<ContextSwitchConfigAction::Type> &action_types) const
{
    std::vector<ContextSwitchConfigActionPtr> filtered_actions;
    for (const auto &action : m_actions) {
        if (action_types.find(action->get_type()) != action_types.end()) {
            filtered_actions.push_back(action);
        }
    }
    return filtered_actions;
}

} /* namespace hailort */
