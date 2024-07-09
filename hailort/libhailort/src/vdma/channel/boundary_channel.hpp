/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file boundary_channel.hpp
 * @brief BoundaryChannel - vdma boundary channel
 **/

#ifndef _HAILO_VDMA_BOUNDARY_CHANNEL_HPP_
#define _HAILO_VDMA_BOUNDARY_CHANNEL_HPP_

#include "vdma/channel/channel_id.hpp"
#include "vdma/channel/transfer_launcher.hpp"
#include "vdma/memory/descriptor_list.hpp"
#include "stream_common/transfer_common.hpp"

#include "common/latency_meter.hpp"

#include "context_switch_defs.h"

#include <memory>


namespace hailort {
namespace vdma {

struct OngoingTransfer {
    TransferRequest request;
    uint16_t last_desc;
};

class BoundaryChannel;
using BoundaryChannelPtr = std::shared_ptr<BoundaryChannel>;
class BoundaryChannel final
{
public:
    using Direction = HailoRTDriver::DmaDirection;

    static Expected<BoundaryChannelPtr> create(HailoRTDriver &driver, vdma::ChannelId channel_id, Direction direction,
        vdma::DescriptorList &&desc_list, TransferLauncher &transfer_launcher, size_t ongoing_transfers,
        size_t pending_transfers = 0, const std::string &stream_name = "", LatencyMeterPtr latency_meter = nullptr);

    BoundaryChannel(HailoRTDriver &driver, vdma::ChannelId channel_id, Direction direction, DescriptorList &&desc_list,
        TransferLauncher &transfer_launcher, size_t ongoing_transfers_queue_size, size_t pending_transfers_queue_size,
        const std::string &stream_name, LatencyMeterPtr latency_meter, hailo_status &status);
    BoundaryChannel(const BoundaryChannel &other) = delete;
    BoundaryChannel &operator=(const BoundaryChannel &other) = delete;
    BoundaryChannel(BoundaryChannel &&other) = delete;
    BoundaryChannel &operator=(BoundaryChannel &&other) = delete;
    virtual ~BoundaryChannel() = default;

    /**
     * Activates the channel object, assume the vDMA channel registers are already in activated state.
     */
    hailo_status activate();

    /**
     * Deactivates the channel object, assume the vDMA channel registers are already in deactivated state.
     */
    void deactivate();

    // Calls all pending transfer callbacks (if they exist), marking them as canceled by passing
    // HAILO_STREAM_ABORT as a status to the callbacks.
    // Note: This function is to be called on a deactivated channel object. Calling on an active channel will lead to
    // unexpected results
    void cancel_pending_transfers();

    /**
     * Called when some transfer (or transfers) is completed.
     */
    hailo_status trigger_channel_completion(const ChannelIrqData &irq_data);

    hailo_status launch_transfer(TransferRequest &&transfer_request);

    // To avoid buffer bindings, one can call this function to statically bind a full buffer to the channel. The buffer
    // size should be exactly desc_page_size() * descs_count() of current descriptors list.
    hailo_status bind_buffer(MappedBufferPtr buffer);

    // TODO: rename BoundaryChannel::get_max_ongoing_transfers to BoundaryChannel::get_max_parallel_transfers (HRT-13513)
    size_t get_max_ongoing_transfers(size_t transfer_size) const;
    size_t get_max_aligned_transfers_in_desc_list(size_t transfer_size) const;

    CONTROL_PROTOCOL__host_buffer_info_t get_boundary_buffer_info(uint32_t transfer_size) const;

    vdma::ChannelId get_channel_id() const
    {
        return m_channel_id;
    }

    const std::string &stream_name() const
    {
        return m_stream_name;
    }

    DescriptorList &get_desc_list()
    {
        return m_desc_list;
    }

    bool should_measure_timestamp() const { return m_latency_meter != nullptr; }

private:
    hailo_status update_latency_meter();

    void on_request_complete(std::unique_lock<std::mutex> &lock, TransferRequest &request,
        hailo_status complete_status);
    hailo_status launch_transfer_impl(TransferRequest &&transfer_request);

    static bool is_desc_between(uint16_t begin, uint16_t end, uint16_t desc);
    hailo_status validate_bound_buffer(TransferRequest &transfer_request);

    const vdma::ChannelId m_channel_id;
    const Direction m_direction;
    HailoRTDriver &m_driver;
    TransferLauncher &m_transfer_launcher;
    DescriptorList m_desc_list; // Host side descriptor list
    const std::string m_stream_name;
    // Since all desc list sizes are a power of 2, we can use IsPow2Tag to optimize the circular buffer
    CircularBuffer<IsPow2Tag> m_descs;
    bool m_is_channel_activated;
    std::mutex m_channel_mutex;
    // * m_pending_transfers holds transfers that are waiting to be bound to the descriptor list.
    // * m_ongoing_transfers holds transfers that have been bound to the descriptor list and
    //   are waiting to be completed.
    // * Note that the capacity of the pending_transfers and ongoing_transfers circular
    //   buffers may not be a power of 2, hence the IsNotPow2Tag
    CircularArray<OngoingTransfer, IsNotPow2Tag> m_ongoing_transfers;
    CircularArray<TransferRequest, IsNotPow2Tag> m_pending_transfers;

    // About HW latency measurements:
    //  - For each ongoing transfer, we push some num-proc value to the pending_latency_measurements array. When this
    //    descriptor is processed, we can add a sample to the latency meter.
    //  - On H2D, the descriptor is the first descriptor on each transfer, so we start the measure after the first
    //    vdma descriptor is processed. We don't measure on launch_transfer since the hw may be busy processing
    //    requests. When the first descriptor is processed, we can be sure the hw has really started processing the
    //    frame.
    //  - On D2H, the descriptor is the last descriptor on each transfer, so we end the measure after the transfer is
    //    processed.
    //  - To get the timestamp, the read_timestamps ioctl is called. This ioctl returns pairs of num-processed and
    //    and their interrupt timestamp, then, using m_last_timestamp_num_processed, we can check if some
    //    pending_latency_measurement is done.
    //  - We don't use m_ongoing_transfers to store the latency measurements because we to finish an ongoing transfer
    //    we use hw num processed given by trigger_channel_completion, which may be different that the hw num processed
    //    returned from read_timestamps_ioctl (one is measured in the ioctl and the other is measured in the interrupt).
    LatencyMeterPtr m_latency_meter;
    CircularArray<uint16_t> m_pending_latency_measurements;
    uint16_t m_last_timestamp_num_processed;

    // When bind_buffer is called, we keep a reference to the buffer here. This is used to avoid buffer bindings.
    std::shared_ptr<MappedBuffer> m_bounded_buffer;
};

} /* namespace vdma */
} /* namespace hailort */

#endif  // _HAILO_VDMA_BOUNDARY_CHANNEL_HPP_