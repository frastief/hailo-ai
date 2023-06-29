/**
 * Copyright (c) 2020-2022 Hailo Technologies Ltd. All rights reserved.
 * Distributed under the MIT license (https://opensource.org/licenses/MIT)
 **/
/**
 * @file scheduler_profiler_handler.cpp
 * @brief Implementation of the scheduler profiler handlers base with HailoRT tracer mechanism
 **/

#include "scheduler_profiler_handler.hpp"

#include "common/logger_macros.hpp"

#include "utils/hailort_logger.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/android_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <iomanip>
#include <sstream>

#define SCHEDULER_PROFILER_NAME ("SchedulerProfiler")
#define SCHEDULER_PROFILER_LOGGER_FILENAME ("scheduler_profiler.json")
#define SCHEDULER_PROFILER_LOGGER_PATTERN ("%v")

#define SCHEDULER_PROFILER_LOGGER_PATH ("SCHEDULER_PROFILER_LOGGER_PATH")

namespace hailort
{

SchedulerProfilerHandler::SchedulerProfilerHandler(int64_t &start_time)
#ifndef __ANDROID__
    : m_file_sink(HailoRTLogger::create_file_sink(HailoRTLogger::get_log_path(SCHEDULER_PROFILER_LOGGER_PATH), SCHEDULER_PROFILER_LOGGER_FILENAME, false)),
      m_first_write(true)
#endif
{
#ifndef __ANDROID__
    spdlog::sinks_init_list sink_list = { m_file_sink };
    m_profiler_logger = make_shared_nothrow<spdlog::logger>(SCHEDULER_PROFILER_NAME, sink_list.begin(), sink_list.end());
    m_file_sink->set_level(spdlog::level::level_enum::info);
    m_file_sink->set_pattern(SCHEDULER_PROFILER_LOGGER_PATTERN);
    std::stringstream ss;
    ss << "{\"ns_since_epoch_zero_time\": \"" << start_time << "\",\n\"scheduler_actions\": [\n";
    m_profiler_logger->info(ss.str());
#else
    (void)start_time;
#endif
}

SchedulerProfilerHandler::~SchedulerProfilerHandler()
{
    m_profiler_logger->info("]\n}");
}

struct JSON
{
    std::unordered_map<std::string, std::string> members;
    JSON(const std::initializer_list<std::pair<const std::string, std::string>> &dict) : members{dict} {}
    JSON(const std::unordered_map<std::string, uint32_t> &dict) {
        for (auto &pair : dict) {
            members.insert({pair.first, std::to_string(pair.second)});
        }
    }
};

template<class T>
std::string json_to_string(const T &val) {
    return std::to_string(val);
}

template<>
std::string json_to_string(const std::string &val) {
    std::ostringstream os;
    os << std::quoted(val);
    return os.str();
}

template<>
std::string json_to_string(const bool &bool_val) {
    return bool_val ? "true" : "false";
}

template<>
std::string json_to_string(const JSON &json_val) {
    std::ostringstream os;
    os << "{\n";
    size_t i = 0;
    for (const auto &kv : json_val.members) {
        ++i;
        os << std::quoted(kv.first) << " : ";
        os << kv.second;
        if (i != json_val.members.size()) {
            os << ",\n";
        }
    }
    os << "\n}";
    return os.str();
}

bool SchedulerProfilerHandler::comma()
{
    auto result = !m_first_write;
    m_first_write = false;
    return result;
}

void SchedulerProfilerHandler::log(JSON json)
{
    m_profiler_logger->info("{}{}", comma() ? ",\n" : "", json_to_string(json));    
}

void SchedulerProfilerHandler::handle_trace(const AddCoreOpTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_name", json_to_string(trace.core_op_name)},
        {"core_op_handle", json_to_string(trace.core_op_handle)},
        {"timeout", json_to_string((uint64_t)trace.timeout)},
        {"threshold", json_to_string((uint64_t)trace.threshold)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const CreateCoreOpInputStreamsTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_name", json_to_string(trace.core_op_name)},
        {"stream_name", json_to_string(trace.stream_name)},
        {"queue_size", json_to_string(trace.queue_size)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const CreateCoreOpOutputStreamsTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_name", json_to_string(trace.core_op_name)},
        {"stream_name", json_to_string(trace.stream_name)},
        {"queue_size", json_to_string(trace.queue_size)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const WriteFrameTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_handle", json_to_string(trace.core_op_handle)},
        {"queue_name", json_to_string(trace.queue_name)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const InputVdmaDequeueTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_handle", json_to_string(trace.core_op_handle)},
        {"queue_name", json_to_string(trace.queue_name)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const ReadFrameTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_handle", json_to_string(trace.core_op_handle)},
        {"queue_name", json_to_string(trace.queue_name)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const OutputVdmaEnqueueTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_handle", json_to_string(trace.core_op_handle)},
        {"queue_name", json_to_string(trace.queue_name)},
        {"frames", json_to_string(trace.frames)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const ChooseCoreOpTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"chosen_core_op_handle", json_to_string(trace.core_op_handle)},
        {"threshold", json_to_string(trace.threshold)},
        {"timeout", json_to_string(trace.timeout)},
        {"priority", json_to_string(trace.priority)}
    }));
}

void SchedulerProfilerHandler::handle_trace(const SwitchCoreOpTrace &trace)
{
    log(JSON({
        {"action", json_to_string(trace.name)},
        {"timestamp", json_to_string(trace.timestamp)},
        {"device_id", json_to_string(trace.device_id)},
        {"core_op_handle", json_to_string(trace.core_op_handle)}
    }));
}

}