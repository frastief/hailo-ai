/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file transfer_common.hpp
 * @brief Common types/functions for async api
 **/

#ifndef _HAILO_TRANSFER_COMMON_HPP_
#define _HAILO_TRANSFER_COMMON_HPP_

#include "hailo/stream.hpp"
#include "hailo/buffer.hpp"

#include "vdma/driver/hailort_driver.hpp"
#include "vdma/memory/mapped_buffer.hpp"

namespace hailort
{

// Contains buffer that can be transferred. The buffer can be circular -
// It relies at [m_offset, m_base_buffer.size()) and [0, m_base_buffer.size() - m_size).
class TransferBuffer final {
public:

    TransferBuffer();

    TransferBuffer(MemoryView base_buffer);
    TransferBuffer(MemoryView base_buffer, size_t size, size_t offset);

    MemoryView base_buffer() { return m_base_buffer; }
    size_t offset() const { return m_offset; }
    size_t size() const { return m_size; }

    Expected<vdma::MappedBufferPtr> map_buffer(HailoRTDriver &driver, HailoRTDriver::DmaDirection direction);

    hailo_status copy_to(MemoryView buffer);
    hailo_status copy_from(const MemoryView buffer);

private:

    bool is_wrap_around() const;

    // Returns the continuous parts of the buffer.
    // There are 2 cases:
    //      1. If the buffer is_wrap_around(), both parts are valid, the first one starts at m_offset until
    //         m_base_buffer end
    //         The second part is the residue, starting from offset 0.
    //      2. If the buffer is not circular, the first part will contain the buffer, the second will point to nullptr.
    std::pair<MemoryView, MemoryView> get_continuous_parts();

    MemoryView m_base_buffer;
    size_t m_size;
    size_t m_offset;

    // Once map_buffer is called, a MappedBuffer object is stored here to make sure the buffer is mapped.
    vdma::MappedBufferPtr m_mappings;
};

// Internal function, wrapper to the user callbacks, accepts the callback status as an argument.
using TransferDoneCallback = std::function<void(hailo_status)>;

struct TransferRequest {
    std::vector<TransferBuffer> transfer_buffers;
    TransferDoneCallback callback;
    TransferRequest() = default;
    TransferRequest(TransferBuffer &&transfer_buffers_arg, const TransferDoneCallback &callback_arg):
        transfer_buffers(), callback(callback_arg)
    {
        transfer_buffers.emplace_back(std::move(transfer_buffers_arg));
    }
    TransferRequest(const TransferBuffer& transfer_buffers_arg, const TransferDoneCallback &callback_arg):
        transfer_buffers(), callback(callback_arg)
    {
        transfer_buffers.emplace_back(std::move(transfer_buffers_arg));
    }
    TransferRequest(std::vector<TransferBuffer> &&transfer_buffers_arg, const TransferDoneCallback &callback_arg) :
        transfer_buffers(std::move(transfer_buffers_arg)), callback(callback_arg)
    {}

    size_t get_total_transfer_size() const {
        size_t total_transfer_size = 0;
        for (size_t i = 0; i < transfer_buffers.size(); i++) {
            total_transfer_size += transfer_buffers[i].size();
        }
        return total_transfer_size;
    }
};

struct InferRequest {
    // Transfer for each stream
    std::unordered_map<std::string, TransferRequest> transfers;

    // Callback to be called when all transfer finishes
    TransferDoneCallback callback;
};

} /* namespace hailort */

#endif /* _HAILO_TRANSFER_COMMON_HPP_ */
