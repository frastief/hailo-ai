/**
 * Copyright (c) 2024 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
**/
/**
 * @file raw_connection.hpp
 * @brief Raw Connection Header
 **/

#ifndef _RAW_CONNECTION_HPP_
#define _RAW_CONNECTION_HPP_

#include "hailo/expected.hpp"
#include "vdma/pcie_session.hpp"

#include <memory>

using namespace hailort;

namespace hrpc
{

class ConnectionContext
{
public:
    static Expected<std::shared_ptr<ConnectionContext>> create_shared(bool is_accepting);

    bool is_accepting() const { return m_is_accepting; }

    ConnectionContext(bool is_accepting) : m_is_accepting(is_accepting) {}
    virtual ~ConnectionContext() = default;

protected:
    bool m_is_accepting;
};


class RawConnection
{
public:
    static Expected<std::shared_ptr<RawConnection>> create_shared(std::shared_ptr<ConnectionContext> context);

    RawConnection() = default;
    virtual ~RawConnection() = default;

    virtual Expected<std::shared_ptr<RawConnection>> accept() = 0;
    virtual hailo_status connect() = 0;
    virtual hailo_status write(const uint8_t *buffer, size_t size) = 0;
    virtual hailo_status read(uint8_t *buffer, size_t size) = 0;
    virtual hailo_status close() = 0;

protected:
    std::chrono::milliseconds m_timeout = std::chrono::milliseconds(HAILO_INFINITE);
};

} // namespace hrpc

#endif // _RAW_CONNECTION_HPP_