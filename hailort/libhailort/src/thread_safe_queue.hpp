/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file thread_safe_queue.hpp
 * @brief Thread safe queue taken from https://stackoverflow.com/a/16075550
 **/

#ifndef HAILO_THREAD_SAFE_QUEUE_HPP_
#define HAILO_THREAD_SAFE_QUEUE_HPP_

#include "hailo/expected.hpp"
#include "common/utils.hpp"
#include "hailo/event.hpp"
#include "common/logger_macros.hpp"
#include "event_internal.hpp"

// Define __unix__ for inclusion of readerwriterqueue.h because readerwriterqueue is implemented over POSIX standards 
// but checks __unix__ - otherwise QNX returns unsupported platform (need HAILO_UNDEF_UNIX_FLAG in order to undefine
// __unix__ only in case of defining it here)
#if defined(__QNX__) && !defined(__unix__)
#define __unix__
#define HAILO_UNDEF_UNIX_FLAG 
#endif

#include "readerwriterqueue.h"

#if defined(HAILO_UNDEF_UNIX_FLAG)
#undef __unix__
#undef HAILO_UNDEF_UNIX_FLAG
#endif

#include <queue>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <chrono>

namespace hailort
{

#define DEFAULT_TIMEOUT_MS (1000)

// A threadsafe-queue. - https://stackoverflow.com/a/16075550
template <class T>
class SafeQueue {
public:
    SafeQueue() : m_queue(), m_mutex(), m_queue_not_empty(), m_timeout(DEFAULT_TIMEOUT_MS) {}
    virtual ~SafeQueue() = default;

    // Add an element to the queue.
    virtual void push(T t) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(t);
        m_queue_not_empty.notify_one();
    }

    // Get the "front"-element.
    // If the queue is empty, wait till a element is available.
    virtual T pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_queue.empty()) {
            // release lock as long as the wait and require it afterwards.
            m_queue_not_empty.wait_for(lock, m_timeout);
        }
        T val = m_queue.front();
        m_queue.pop();
        return val;
    }

protected:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_queue_not_empty;
    const std::chrono::milliseconds m_timeout;
};

 template <class T>
 class SafeQueueMaxSize : public SafeQueue<T> {
 public:
    SafeQueueMaxSize(uint32_t max_size) :
        SafeQueue<T>::SafeQueue(),
        m_max_size(max_size),
        m_queue_not_full()
    {}
    virtual ~SafeQueueMaxSize() = default;

    virtual void push(T t) override {
        std::unique_lock<std::mutex> lock(this->m_mutex);
        m_queue_not_full.wait(lock, [&]{return this->m_queue.size() < m_max_size;});

        this->m_queue.push(t);
        this->m_queue_not_empty.notify_one();
    }

    virtual T pop() override {
        std::unique_lock<std::mutex> lock(this->m_mutex);
        this->m_queue_not_empty.wait(lock, [&]{return !this->m_queue.empty();});
        
        T val = this->m_queue.front();
        this->m_queue.pop();
        
        if (this->m_queue.size() < m_max_size) {
            m_queue_not_full.notify_one();
        }
        return val;
    }
protected:
    const uint32_t m_max_size;
    std::condition_variable m_queue_not_full;
};

// Single-Producer Single-Consumer Queue
// The queue's size is limited
template<typename T, size_t MAX_BLOCK_SIZE = 512>
class SpscQueue
{
private:
    typedef moodycamel::ReaderWriterQueue<T, MAX_BLOCK_SIZE> ReaderWriterQueue;

public:
    static constexpr auto INIFINITE_TIMEOUT() { return std::chrono::milliseconds(HAILO_INFINITE); }

    SpscQueue(size_t max_size, SemaphorePtr items_enqueued_sema, SemaphorePtr items_dequeued_sema,
              EventPtr shutdown_event, std::chrono::milliseconds default_timeout) :
        m_inner(max_size),
        m_items_enqueued_sema_or_shutdown(items_enqueued_sema, shutdown_event),
        m_items_enqueued_sema(items_enqueued_sema),
        m_items_dequeued_sema_or_shutdown(items_dequeued_sema, shutdown_event),
        m_items_dequeued_sema(items_dequeued_sema),
        m_default_timeout(default_timeout),
        m_size(max_size),
        m_enqueues_count(0),
        m_callback_mutex()
    {}

    virtual ~SpscQueue() = default;
    SpscQueue(SpscQueue &&other) :
        m_inner(std::move(other.m_inner)),
        m_items_enqueued_sema_or_shutdown(std::move(other.m_items_enqueued_sema_or_shutdown)),
        m_items_enqueued_sema(std::move(other.m_items_enqueued_sema)),
        m_items_dequeued_sema_or_shutdown(std::move(other.m_items_dequeued_sema_or_shutdown)),
        m_items_dequeued_sema(std::move(other.m_items_dequeued_sema)),
        m_default_timeout(std::move(other.m_default_timeout)),
        m_size(std::move(other.m_size)),
        m_enqueues_count(std::move(other.m_enqueues_count.load())),
        m_cant_enqueue_callback(std::move(other.m_cant_enqueue_callback)),
        m_can_enqueue_callback(std::move(other.m_can_enqueue_callback)),
        m_callback_mutex()
    {}

    static Expected<SpscQueue> create(size_t max_size, const EventPtr& shutdown_event,
        std::chrono::milliseconds default_timeout = std::chrono::milliseconds(1000))
    {
        if (0 == max_size) {
            LOGGER__ERROR("Invalid queue max_size (must be greater than zero)");
            return make_unexpected(HAILO_INVALID_ARGUMENT);
        }

        // * items_enqueued_sema:
        //   +1 for each enqueued item
        //   -1 for each dequeued item
        //   Blocks when there are no items in the queue (hence when the queue is built it starts at zero)
        // * items_dequeued_sema:
        //   +1 for each dequeued item
        //   -1 for each enqueued item
        //   Blocks when the queue is full (which happens when it's value reaches zero, hence it starts at queue size)
        const auto items_enqueued_sema = Semaphore::create_shared(0);
        CHECK_AS_EXPECTED(nullptr != items_enqueued_sema, HAILO_OUT_OF_HOST_MEMORY, "Failed creating items_enqueued_sema semaphore");

        const auto items_dequeued_sema = Semaphore::create_shared(static_cast<uint32_t>(max_size));
        CHECK_AS_EXPECTED(nullptr != items_dequeued_sema, HAILO_OUT_OF_HOST_MEMORY, "Failed creating items_dequeued_sema semaphore");

        return SpscQueue(max_size, items_enqueued_sema, items_dequeued_sema, shutdown_event, default_timeout);
    }

    static std::shared_ptr<SpscQueue> create_shared(size_t max_size, const EventPtr& shutdown_event,
        std::chrono::milliseconds default_timeout = std::chrono::milliseconds(1000))
    {
        auto queue = create(max_size, shutdown_event, default_timeout);
        if (!queue) {
            LOGGER__ERROR("Failed creating queue. status={}", queue.status());
            return nullptr;
        }

        return make_shared_nothrow<SpscQueue>(queue.release());
    }

    static std::unique_ptr<SpscQueue> create_unique(size_t max_size, const EventPtr& shutdown_event,
        std::chrono::milliseconds default_timeout = std::chrono::milliseconds(1000))
    {
        auto queue = create(max_size, shutdown_event, default_timeout);
        if (!queue) {
            LOGGER__ERROR("Failed creating queue. status={}", queue.status());
            return nullptr;
        }

        return make_unique_nothrow<SpscQueue>(queue.release());
    }
    
    Expected<T> dequeue(std::chrono::milliseconds timeout, bool ignore_shutdown_event = false) AE_NO_TSAN
    {
        hailo_status wait_result = HAILO_UNINITIALIZED;
        if (ignore_shutdown_event) {
            wait_result = m_items_enqueued_sema->wait(timeout);
        } else {
            wait_result = m_items_enqueued_sema_or_shutdown.wait(timeout);
        }

        if (HAILO_SHUTDOWN_EVENT_SIGNALED == wait_result) {
            LOGGER__TRACE("Shutdown event has been signaled");
            return make_unexpected(wait_result);
        }
        if (HAILO_TIMEOUT == wait_result) {
            LOGGER__TRACE("Timeout, the queue is empty");
            return make_unexpected(wait_result);
        }
        if (HAILO_SUCCESS != wait_result) {
            LOGGER__WARNING("m_items_enqueued_sema received an unexpected failure");
            return make_unexpected(wait_result);
        }
        
        // The queue isn't empty
        T result{};
        const bool success = m_inner.try_dequeue(result);
        assert(success);
        AE_UNUSED(success);

        {
            std::unique_lock<std::mutex> lock(m_callback_mutex);
            if ((m_size == m_enqueues_count) && m_can_enqueue_callback) {
                m_can_enqueue_callback();
            }
            m_enqueues_count--;
        }

        const auto signal_result = m_items_dequeued_sema_or_shutdown.signal();
        if (HAILO_SUCCESS != signal_result) {
            return make_unexpected(signal_result);
        }
        return result;
    }

    Expected<T> dequeue() AE_NO_TSAN
    {
        return dequeue(m_default_timeout);
    }

    hailo_status enqueue(const T& result, std::chrono::milliseconds timeout) AE_NO_TSAN
    {
        const auto wait_result = m_items_dequeued_sema_or_shutdown.wait(timeout);
        if (HAILO_SHUTDOWN_EVENT_SIGNALED == wait_result) {
            LOGGER__TRACE("Shutdown event has been signaled");
            return wait_result;
        }
        if (HAILO_TIMEOUT == wait_result) {
            LOGGER__TRACE("Timeout, the queue is full");
            return wait_result;
        }
        if (HAILO_SUCCESS != wait_result) {
            LOGGER__WARNING("m_items_dequeued_sema received an unexpected failure");
            return wait_result;
        }

        // The queue isn't full
        const bool success = m_inner.try_enqueue(result);
        assert(success);
        AE_UNUSED(success);

        {
            std::unique_lock<std::mutex> lock(m_callback_mutex);
            m_enqueues_count++;
            if ((m_size == m_enqueues_count) && m_cant_enqueue_callback) {
                m_cant_enqueue_callback();
            }
        }

        return m_items_enqueued_sema_or_shutdown.signal();
    }

    inline hailo_status enqueue(const T& result) AE_NO_TSAN
    {
        return enqueue(result, m_default_timeout);
    }

    // TODO: Do away with two copies of this function? (SDK-16481)
    hailo_status enqueue(T&& result, std::chrono::milliseconds timeout, bool ignore_shutdown_event = false) AE_NO_TSAN
    {
        hailo_status wait_result = HAILO_UNINITIALIZED;
        if (ignore_shutdown_event) {
            wait_result = m_items_dequeued_sema->wait(timeout);
        } else {
            wait_result = m_items_dequeued_sema_or_shutdown.wait(timeout);
        }

        if (HAILO_SHUTDOWN_EVENT_SIGNALED == wait_result) {
            LOGGER__TRACE("Shutdown event has been signaled");
            return wait_result;
        }
        if (HAILO_TIMEOUT == wait_result) {
            LOGGER__TRACE("Timeout, the queue is full");
            return wait_result;
        }
        if (HAILO_SUCCESS != wait_result) {
            LOGGER__WARNING("m_items_dequeued_sema received an unexpected failure");
            return wait_result;
        }

        // The queue isn't full
        const bool success = m_inner.try_enqueue(std::move(result));
        assert(success);
        AE_UNUSED(success);

        {
            std::unique_lock<std::mutex> lock(m_callback_mutex);
            m_enqueues_count++;
            if ((m_size == m_enqueues_count) && m_cant_enqueue_callback) {
                m_cant_enqueue_callback();
            }
        }

        return m_items_enqueued_sema_or_shutdown.signal();
    }

    // TODO: HRT-3810, remove hacky argument ignore_shutdown_event
    inline hailo_status enqueue(T&& result, bool ignore_shutdown_event = false) AE_NO_TSAN
    {
        return enqueue(std::move(result), m_default_timeout, ignore_shutdown_event);
    }

    size_t size_approx()
    {
        return m_inner.size_approx();
    }

    hailo_status clear() AE_NO_TSAN
    {
        auto status = HAILO_SUCCESS;
        while (HAILO_SUCCESS == status) {
            auto output = dequeue(std::chrono::milliseconds(0), true);
            status = output.status();
        }

        if (HAILO_TIMEOUT == status) {
            return HAILO_SUCCESS;
        }
        return status;
    }

    void set_on_cant_enqueue_callback(std::function<void()> callback)
    {
        m_cant_enqueue_callback = callback;
    }

    void set_on_can_enqueue_callback(std::function<void()> callback)
    {
        m_can_enqueue_callback = callback;
    }

private:
    ReaderWriterQueue m_inner;
    WaitOrShutdown m_items_enqueued_sema_or_shutdown;
    SemaphorePtr m_items_enqueued_sema;
    WaitOrShutdown m_items_dequeued_sema_or_shutdown;
    SemaphorePtr m_items_dequeued_sema;
    std::chrono::milliseconds m_default_timeout;

    const size_t m_size;
    std::atomic_uint32_t m_enqueues_count;
    std::function<void()> m_cant_enqueue_callback;
    std::function<void()> m_can_enqueue_callback;
    std::mutex m_callback_mutex;
};

} /* namespace hailort */

#endif // HAILO_THREAD_SAFE_QUEUE_HPP_
