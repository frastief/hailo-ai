/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file circular_stream_buffer_pool.hpp
 * @brief Single buffer used as a circular pool.
 **/

#ifndef _HAILO_CIRCULAR_STREAM_BUFFER_POOL_HPP_
#define _HAILO_CIRCULAR_STREAM_BUFFER_POOL_HPP_

#include "vdma/memory/mapped_buffer.hpp"
#include "common/circular_buffer.hpp"
#include "stream_common/stream_buffer_pool.hpp"
#include "vdma/vdma_device.hpp"
#include "hailo/dma_mapped_buffer.hpp"

#include <condition_variable>


namespace hailort
{

// A buffer pool taken from a single virtually continuous buffer.
// The buffer are dequeued in a circular way.
// This class can be used in multiple threads without any lock if there is only one consumer (calls dequeue and
// buffers_ready_to_dequeue)
// and one producer (calls enqueue).
class CircularStreamBufferPool final : public StreamBufferPool {
public:
    static Expected<std::unique_ptr<CircularStreamBufferPool>> create(VdmaDevice &device,
        hailo_dma_buffer_direction_t direction, size_t desc_page_size, size_t descs_count, size_t transfer_size);

    CircularStreamBufferPool(size_t desc_page_size, size_t descs_count, size_t transfer_size,
        Buffer &&base_buffer, DmaMappedBuffer &&mappings);

    virtual size_t max_queue_size() const override;
    size_t buffers_ready_to_dequeue() const;

    virtual Expected<TransferBuffer> dequeue() override;

    virtual hailo_status enqueue(TransferBuffer &&buffer_info) override;

    Buffer &get_base_buffer() { return m_base_buffer; }

    virtual void reset_pointers() override;

private:
    static Expected<Buffer> allocate_buffer(VdmaDevice &device, size_t size);

    size_t descs_in_transfer() const;

    // We always work in desc_page_size granularity to avoid the need for reprogram descriptors.
    const size_t m_desc_page_size;

    const size_t m_transfer_size;

    // m_mapped_buffer.size() must be CB_SIZE(m_queue) * m_desc_page_size
    Buffer m_base_buffer;
    DmaMappedBuffer m_mappings;

    // Head/tail based queue that manages the buffer pool.
    // The head and tail are in m_desc_page_size granularity.
    //
    // If CB_HEAD(m_queue) == CB_TAIL(m_queue) the pool is empty.
    // Otherwise, the buffers that can be in use starts from
    //   CB_TAIL(m_queue) * m_desc_page_size (inclusive)
    // until
    //   CB_HEAD(m_queue) * m_desc_page_size (exclusive)
    circbuf_t m_queue;

    // Used to validate that the buffers are enqueued in order.
    size_t m_next_enqueue_desc_offset;
};

} /* namespace hailort */

#endif /* _HAILO_CIRCULAR_STREAM_BUFFER_POOL_HPP_ */
