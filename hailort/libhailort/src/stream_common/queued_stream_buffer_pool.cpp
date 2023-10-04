/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file queued_stream_buffer_pool.cpp
 **/

#include "queued_stream_buffer_pool.hpp"

namespace hailort
{

Expected<std::unique_ptr<QueuedStreamBufferPool>> QueuedStreamBufferPool::create(size_t max_queue_size, size_t buffer_size,
    BufferStorageParams buffer_params)
{
    std::vector<BufferPtr> storage;
    storage.reserve(max_queue_size);
    for (size_t i = 0; i < max_queue_size; i++) {
        auto buffer = Buffer::create_shared(buffer_size, 0, buffer_params);
        CHECK_EXPECTED(buffer);
        storage.emplace_back(buffer.release());
    }

    auto pool = make_unique_nothrow<QueuedStreamBufferPool>(std::move(storage));
    CHECK_NOT_NULL_AS_EXPECTED(pool, HAILO_OUT_OF_HOST_MEMORY);
    return pool;
}

QueuedStreamBufferPool::QueuedStreamBufferPool(std::vector<BufferPtr> &&storage) :
    m_storage(std::move(storage))
{
    for (auto buffer : m_storage) {
        m_queue.push(buffer);
    }
}

size_t QueuedStreamBufferPool::max_queue_size() const
{
    return m_storage.size();
}

Expected<TransferBuffer> QueuedStreamBufferPool::dequeue()
{
    CHECK_AS_EXPECTED(!m_queue.empty(), HAILO_INTERNAL_FAILURE, "QueuedStreamBufferPool is empty");

    auto buffer = m_queue.front();
    m_queue.pop();
    return TransferBuffer(buffer);
}

hailo_status QueuedStreamBufferPool::enqueue(TransferBuffer &&buffer_info)
{
    CHECK(buffer_info.offset() == 0, HAILO_INTERNAL_FAILURE, "Cant use offset on queued buffer pool");
    CHECK(buffer_info.size() == m_storage[0]->size(), HAILO_INTERNAL_FAILURE, "Invalid enqueue buffer size");
    CHECK(buffer_info.base_buffer()->data() == m_storage[m_next_enqueue_buffer_index]->data(), HAILO_INTERNAL_FAILURE,
        "Out of order enqueue for queued stream buffer pool");

    m_queue.push(buffer_info.base_buffer());
    m_next_enqueue_buffer_index = (m_next_enqueue_buffer_index + 1) % (m_storage.size());
    return HAILO_SUCCESS;
}

void QueuedStreamBufferPool::reset_pointers()
{
    // First, clear all queued buffers (data may be lost, as required from reset_pointers).
    while (!m_queue.empty()) {
        m_queue.pop();
    }

    // Now fill the buffers from the storage in the right order
    for (auto buffer : m_storage) {
        m_queue.push(buffer);
    }
    m_next_enqueue_buffer_index = 0;
}

} /* namespace hailort */
