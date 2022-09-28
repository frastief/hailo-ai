/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file hcp_config_manager.cpp
 * @brief Manager of HEF parsing and network groups resources
 *
 *
 **/

// https://github.com/protocolbuffers/protobuf/tree/master/cmake#notes-on-compiler-warnings
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4244 4267 4127)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "hef.pb.h"
#if defined(_MSC_VER)
#pragma warning( pop ) 
#else
#pragma GCC diagnostic pop
#endif

#include "context_switch/single_context/hcp_config_manager.hpp"
#include "hailo/hailort.h"
#include "common/utils.hpp"
#include "hailo/device.hpp"
#include "hailo/hef.hpp"
#include "control.hpp"
#include "pcie_device.hpp"
#include "hlpcie.hpp"

#include <new>
#include <algorithm>

namespace hailort
{

Expected<ConfiguredNetworkGroupVector> HcpConfigManager::add_hef(Hef &hef,
    const NetworkGroupsParamsMap &configure_params)
{
    auto device_arch_exp = m_device.get_architecture();
    CHECK_EXPECTED(device_arch_exp);
    auto device_arch = device_arch_exp.release();

    auto partial_clusters_layout_bitmap_exp = Control::get_partial_clusters_layout_bitmap(m_device);
    CHECK_EXPECTED(partial_clusters_layout_bitmap_exp);
    auto partial_clusters_layout_bitmap = partial_clusters_layout_bitmap_exp.release();

    auto &hef_network_groups = hef.pimpl->network_groups();
    auto current_net_group_index = static_cast<uint8_t>(m_net_groups.size());
    ConfiguredNetworkGroupVector added_network_groups;
    added_network_groups.reserve(hef_network_groups.size());
    auto configure_params_copy = configure_params;
    auto hef_arch = hef.pimpl->get_device_arch();

    // Reset FW state_machine status
    static const auto REMOVE_NN_CONFIG_DURING_RESET = false;
    auto status = Control::reset_context_switch_state_machine(m_device, REMOVE_NN_CONFIG_DURING_RESET);
    CHECK_SUCCESS_AS_EXPECTED(status);

    const ProtoHEFNetworkGroup *network_group_ptr = nullptr;
    for (const auto &net_group : hef_network_groups) {
        CHECK_NOT_NULL_AS_EXPECTED(net_group, HAILO_INTERNAL_FAILURE);

        if (ProtoHEFHwArch::PROTO__HW_ARCH__HAILO8L == hef_arch) {
            // Hailo8 can work with Hailo8L configurations. in that case we choose one of the configurations
            for (auto &partial_network_group : net_group->partial_network_groups()) {
                if ((partial_clusters_layout_bitmap == partial_network_group.layout().partial_clusters_layout_bitmap()) ||
                    ((HAILO_ARCH_HAILO8 == device_arch))) {
                    network_group_ptr = &partial_network_group.network_group();
                    break;
                }
            }
            CHECK_AS_EXPECTED(nullptr != network_group_ptr, HAILO_INTERNAL_FAILURE, "There is no matching partial_clusters_layout_bitmap configuration in the given HEF");
        } else {
            network_group_ptr = net_group.get();
        }
        CHECK_NOT_NULL_AS_EXPECTED(network_group_ptr, HAILO_INTERNAL_FAILURE);

        /* Validate that all network_groups are single context */
        CHECK(1 == network_group_ptr->contexts_size(), make_unexpected(HAILO_INTERNAL_FAILURE),
            "Only single_context network_groups is supported!. Network group {} has {} contexts.",
            network_group_ptr->network_group_metadata().network_group_name(), network_group_ptr->contexts_size());
        CHECK_AS_EXPECTED(!(Hef::Impl::contains_ddr_layers(*network_group_ptr)), HAILO_INVALID_OPERATION,
            "DDR layers are only supported for PCIe device. Network group {} contains DDR layers.",
            network_group_ptr->network_group_metadata().network_group_name());
        status = Hef::Impl::validate_net_group_unique_layer_names(*network_group_ptr);
        CHECK_SUCCESS_AS_EXPECTED(status);

        /* Update preliminary_config and dynamic_contexts recepies */
        auto &proto_preliminary_config = network_group_ptr->preliminary_config();
        auto net_group_config = Hef::Impl::create_single_context_network_group_config(proto_preliminary_config);
        CHECK_EXPECTED(net_group_config);

        ConfigureNetworkParams config_params = {};
        if (contains(configure_params_copy, network_group_ptr->network_group_metadata().network_group_name())) {
            config_params = configure_params_copy.at(network_group_ptr->network_group_metadata().network_group_name());
            configure_params_copy.erase(network_group_ptr->network_group_metadata().network_group_name());
        } else {
            auto interface = m_device.get_default_streams_interface();
            CHECK_EXPECTED(interface);
            auto config_params_exp = hef.create_configure_params(interface.value(), network_group_ptr->network_group_metadata().network_group_name());
            CHECK_EXPECTED(config_params_exp);
            config_params = config_params_exp.release();
        }

        auto network_group_metadata = hef.pimpl->get_network_group_metadata(network_group_ptr->network_group_metadata().network_group_name());
        CHECK_EXPECTED(network_group_metadata);

        auto single_context_app = HcpConfigNetworkGroup(m_device, m_active_net_group_holder, net_group_config.release(),
            config_params, current_net_group_index, network_group_metadata.release(), status);
        CHECK_SUCCESS_AS_EXPECTED(status);

        auto net_group_shared_ptr = make_shared_nothrow<HcpConfigNetworkGroup>(std::move(single_context_app));
        CHECK_AS_EXPECTED(nullptr != net_group_shared_ptr, HAILO_OUT_OF_HOST_MEMORY);
        m_net_groups.emplace_back(net_group_shared_ptr);

        auto net_group_wrapper = ConfiguredNetworkGroupWrapper::create(net_group_shared_ptr);
        CHECK_EXPECTED(net_group_wrapper);

        auto net_group_wrapper_ptr = make_shared_nothrow<ConfiguredNetworkGroupWrapper>(net_group_wrapper.release());
        CHECK_AS_EXPECTED(nullptr != net_group_wrapper_ptr, HAILO_OUT_OF_HOST_MEMORY);

        m_net_group_wrappers.emplace_back(net_group_wrapper_ptr);
        added_network_groups.emplace_back(std::static_pointer_cast<ConfiguredNetworkGroup>(net_group_wrapper_ptr));

        current_net_group_index++;

        // TODO: move this func into HcpConfigNetworkGroup c'tor
        status = net_group_shared_ptr->create_streams_from_config_params(m_device);
        CHECK_SUCCESS_AS_EXPECTED(status);

        // Check that all boundary streams were created
        status = validate_boundary_streams_were_created(hef, network_group_ptr->network_group_metadata().network_group_name(), *net_group_shared_ptr);
        CHECK_SUCCESS_AS_EXPECTED(status);
    }
    std::string unmatched_keys = "";
    for (const auto &pair : configure_params_copy) {
        unmatched_keys.append(" ");
        unmatched_keys.append(pair.first);
    }
    CHECK_AS_EXPECTED(unmatched_keys.size() == 0, HAILO_INVALID_ARGUMENT,
        "Some network group names in the configuration are not found in the hef file:{}", unmatched_keys);

    return added_network_groups;
}

ConfigManagerType HcpConfigManager::get_manager_type()
{
    return ConfigManagerType::HcpConfigManager;
}

} /* namespace hailort */
