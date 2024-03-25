/**
 * Copyright (c) 2023 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file transfer_common.cpp
 **/

#include "transfer_common.hpp"
#include "vdma/memory/mapped_buffer.hpp"
#include "utils/buffer_storage.hpp"

namespace hailort
{


TransferBuffer::TransferBuffer() :
    m_base_buffer(MemoryView{}),
    m_size(0),
    m_offset(0)
{}

TransferBuffer::TransferBuffer(MemoryView base_buffer, size_t size, size_t offset) :
    m_base_buffer(base_buffer),
    m_size(size),
    m_offset(offset)
{
    assert(m_size <= base_buffer.size());
    assert(m_offset < base_buffer.size());
}

TransferBuffer::TransferBuffer(MemoryView base_buffer)
    : TransferBuffer(base_buffer, base_buffer.size(), 0)
{}

Expected<vdma::MappedBufferPtr> TransferBuffer::map_buffer(HailoRTDriver &driver, HailoRTDriver::DmaDirection direction)
{
    CHECK_AS_EXPECTED(!m_mappings, HAILO_INTERNAL_FAILURE, "Buffer is already mapped");

    vdma::DmaAbleBufferPtr dma_able_buffer;
    const auto storage_key = std::make_pair(m_base_buffer.data(), m_base_buffer.size());
    if (auto storage = BufferStorageResourceManager::get_resource(storage_key)) {
        auto dma_able_buffer_exp = storage->get()->get_dma_able_buffer();
        CHECK_EXPECTED(dma_able_buffer_exp);
        dma_able_buffer = dma_able_buffer_exp.release();
    } else {
        auto dma_able_buffer_exp = vdma::DmaAbleBuffer::create_from_user_address(m_base_buffer.data(), m_base_buffer.size());
        CHECK_EXPECTED(dma_able_buffer_exp);
        dma_able_buffer = dma_able_buffer_exp.release();
    }

    auto mapped_buffer = vdma::MappedBuffer::create_shared(std::move(dma_able_buffer), driver, direction);
    CHECK_EXPECTED(mapped_buffer);

    m_mappings = mapped_buffer.value();
    return mapped_buffer;
}

hailo_status TransferBuffer::copy_to(MemoryView buffer)
{
    CHECK(buffer.size() == m_size, HAILO_INTERNAL_FAILURE, "buffer size {} must be {}", buffer.size(), m_size);

    auto continuous_parts = get_continuous_parts();
    memcpy(buffer.data(), continuous_parts.first.data(), continuous_parts.first.size());
    if (!continuous_parts.second.empty()) {
        const size_t dest_offset = continuous_parts.first.size();
        memcpy(buffer.data() + dest_offset, continuous_parts.second.data(), continuous_parts.second.size());
    }
    return HAILO_SUCCESS;
}

hailo_status TransferBuffer::copy_from(const MemoryView buffer)
{
    CHECK(buffer.size() == m_size, HAILO_INTERNAL_FAILURE, "buffer size {} must be {}", buffer.size(), m_size);

    auto continuous_parts = get_continuous_parts();
    memcpy(continuous_parts.first.data(), buffer.data(), continuous_parts.first.size());
    if (!continuous_parts.second.empty()) {
        const size_t src_offset = continuous_parts.first.size();
        memcpy(continuous_parts.second.data(), buffer.data() + src_offset, continuous_parts.second.size());
    }

    return HAILO_SUCCESS;
}

bool TransferBuffer::is_wrap_around() const
{
    return (m_offset + m_size) > m_base_buffer.size();
}

std::pair<MemoryView, MemoryView> TransferBuffer::get_continuous_parts()
{
    if (is_wrap_around()) {
        const auto size_to_end = m_base_buffer.size() - m_offset;
        assert(size_to_end < m_size);
        return std::make_pair(
            MemoryView(m_base_buffer.data() + m_offset, size_to_end),
            MemoryView(m_base_buffer.data(), m_size - size_to_end)
        );

    } else {
        return std::make_pair(
            MemoryView(m_base_buffer.data() + m_offset, m_size),
            MemoryView()
        );
    }
}

} /* namespace hailort */
