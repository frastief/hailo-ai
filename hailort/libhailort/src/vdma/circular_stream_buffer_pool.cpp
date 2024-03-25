/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file circular_stream_buffer_pool.cpp
 **/

#include "circular_stream_buffer_pool.hpp"
#include "vdma/memory/descriptor_list.hpp"
#include "utils/buffer_storage.hpp"

#include "utils.h"

namespace hailort
{

Expected<std::unique_ptr<CircularStreamBufferPool>> CircularStreamBufferPool::create(VdmaDevice &device,
    hailo_dma_buffer_direction_t direction, size_t desc_page_size, size_t descs_count, size_t transfer_size)
{
    // TODO: HRT-11220 calculate desc_count/desc_page_size base on transfer_size and queue_size
    CHECK(is_powerof2(descs_count), HAILO_INTERNAL_FAILURE, "descs_count {} must be power of 2", descs_count);
    CHECK(is_powerof2(desc_page_size), HAILO_INTERNAL_FAILURE, "desc_page_size {} must be power of 2",
        desc_page_size);

    const auto buffer_size = desc_page_size * descs_count;
    CHECK(transfer_size < buffer_size, HAILO_INTERNAL_FAILURE, "Transfer size {} must be smaller than buffer size {}",
        transfer_size, buffer_size);

    TRY(auto base_buffer, allocate_buffer(device, buffer_size));
    TRY(auto mapping, DmaMappedBuffer::create(device, base_buffer.data(), base_buffer.size(), direction));

    auto circular_buffer_pool = make_unique_nothrow<CircularStreamBufferPool>(desc_page_size, descs_count,
        transfer_size, std::move(base_buffer), std::move(mapping));
    CHECK_NOT_NULL(circular_buffer_pool, HAILO_OUT_OF_HOST_MEMORY);

    return circular_buffer_pool;
}

CircularStreamBufferPool::CircularStreamBufferPool(size_t desc_page_size, size_t descs_count, size_t transfer_size,
    Buffer &&base_buffer, DmaMappedBuffer &&mappings) :
        m_desc_page_size(desc_page_size),
        m_transfer_size(transfer_size),
        m_base_buffer(std::move(base_buffer)),
        m_mappings(std::move(mappings)),
        m_next_enqueue_desc_offset(0)
{
    assert(is_powerof2(descs_count) && (descs_count > 0));
    assert(m_base_buffer.size() == (m_desc_page_size * descs_count));
    CB_INIT(m_queue, descs_count);
    m_queue.head = static_cast<int>(descs_count - 1);
}

size_t CircularStreamBufferPool::max_queue_size() const
{
    return (m_queue.size - 1) / DIV_ROUND_UP(m_transfer_size, m_desc_page_size);
}

size_t CircularStreamBufferPool::buffers_ready_to_dequeue() const
{
    const size_t descs_available = CB_PROG(m_queue, CB_HEAD(m_queue), CB_TAIL(m_queue));
    return descs_available / descs_in_transfer();
}

Expected<TransferBuffer> CircularStreamBufferPool::dequeue()
{
    CHECK_AS_EXPECTED(buffers_ready_to_dequeue() > 0, HAILO_INTERNAL_FAILURE, "CircularStreamBufferPool is empty");

    const size_t offset_in_buffer = CB_TAIL(m_queue) * m_desc_page_size;
    CB_DEQUEUE(m_queue, descs_in_transfer());
    return TransferBuffer {
        MemoryView(m_base_buffer),
        m_transfer_size,
        offset_in_buffer
    };
}

hailo_status CircularStreamBufferPool::enqueue(TransferBuffer &&buffer_info)
{
    const size_t descs_required = descs_in_transfer();
    const size_t descs_available = CB_AVAIL(m_queue, CB_HEAD(m_queue), CB_TAIL(m_queue));
    CHECK(descs_available >= descs_required, HAILO_INTERNAL_FAILURE, "Can enqueue without previous dequeue");
    CHECK(buffer_info.base_buffer().data() == m_base_buffer.data(), HAILO_INTERNAL_FAILURE, "Got the wrong buffer");
    CHECK(buffer_info.size() == m_transfer_size, HAILO_INTERNAL_FAILURE, "Got invalid buffer size {}, expected {}",
        buffer_info.size(), m_transfer_size);

    const size_t expected_offset = m_next_enqueue_desc_offset * m_desc_page_size;
    CHECK(buffer_info.offset() == expected_offset, HAILO_INTERNAL_FAILURE,
        "Out of order enqueue is not supported in CircularStreamBufferPool. Got offset {}, expected {}",
        buffer_info.offset(), expected_offset);

    CB_ENQUEUE(m_queue, descs_required);
    m_next_enqueue_desc_offset = (m_next_enqueue_desc_offset + descs_required) & m_queue.size_mask;
    return HAILO_SUCCESS;
}

void CircularStreamBufferPool::reset_pointers()
{
    CB_RESET(m_queue);
    m_queue.head = static_cast<int>(m_queue.size - 1);
    m_next_enqueue_desc_offset = 0;
}

Expected<Buffer> CircularStreamBufferPool::allocate_buffer(VdmaDevice &device, size_t size)
{
    TRY(auto dma_able_buffer, vdma::DmaAbleBuffer::create_by_allocation(size, device.get_driver()));

    auto dma_storage = make_shared_nothrow<DmaStorage>(std::move(dma_able_buffer));
    CHECK_NOT_NULL_AS_EXPECTED(dma_storage, HAILO_OUT_OF_HOST_MEMORY);

    return Buffer::create(dma_storage);
}

size_t CircularStreamBufferPool::descs_in_transfer() const
{
    assert(IS_FIT_IN_UINT16(m_desc_page_size));
    return vdma::DescriptorList::descriptors_in_buffer(m_transfer_size, static_cast<uint16_t>(m_desc_page_size));
}

} /* namespace hailort */
