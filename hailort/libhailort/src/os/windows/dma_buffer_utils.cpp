/**
 * Copyright (c) 2020-2024 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file dma_buffer_utils.cpp
 * @brief A module for managing DMA buffers on Windows (not supported)
 **/

#include "utils/dma_buffer_utils.hpp"


namespace hailort
{

Expected<MemoryView> DmaBufferUtils::mmap_dma_buffer_write(hailo_dma_buffer_t /*dma_buffer*/)
{
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}

hailo_status DmaBufferUtils::munmap_dma_buffer_write(hailo_dma_buffer_t /*dma_buffer*/, MemoryView /*dma_buffer_memview*/)
{
    return HAILO_NOT_IMPLEMENTED;
}

Expected<MemoryView> DmaBufferUtils::mmap_dma_buffer_read(hailo_dma_buffer_t /*dma_buffer*/)
{
    return make_unexpected(HAILO_NOT_IMPLEMENTED);
}

hailo_status DmaBufferUtils::munmap_dma_buffer_read(hailo_dma_buffer_t /*dma_buffer*/, MemoryView /*dma_buffer_memview*/)
{
    return HAILO_NOT_IMPLEMENTED;
}

} /* namespace hailort */
