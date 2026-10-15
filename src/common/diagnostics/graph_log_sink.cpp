/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 */

#include "graph_log_sink.h"

#include "graph.h"

#include "../log.h"
#include "../memory.h"
#include "../utf.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace caspar { namespace diagnostics {

namespace {

constexpr auto kSummaryInterval = std::chrono::seconds(5);

class log_sink : public spi::graph_sink
{
    struct tag_state
    {
        tag_severity  severity = tag_severity::INFO;
        std::uint64_t count    = 0;
    };

    struct value_state
    {
        std::uint64_t count   = 0;
        double        minimum = 0;
        double        maximum = 0;
        double        mean    = 0;

        void add(double value)
        {
            minimum = count == 0 ? value : std::min(minimum, value);
            maximum = count == 0 ? value : std::max(maximum, value);
            mean += (value - mean) / static_cast<double>(++count);
        }
    };

    std::mutex                            mutex_;
    std::wstring                          text_;
    std::map<std::string, tag_state>      tags_;
    std::map<std::string, value_state>    values_;
    std::chrono::steady_clock::time_point last_report_ = std::chrono::steady_clock::now();

  public:
    void activate() override {}

    void set_text(const std::wstring& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ = value;
    }

    void set_value(const std::string& name, double value) override
    {
        if (!std::isfinite(value))
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        values_[name].add(value);
    }
    void set_color(const std::string& /*name*/, int /*color*/) override {}
    void auto_reset() override {}

    void set_tag(tag_severity severity, const std::string& name) override
    {
        if (severity == tag_severity::SILENT)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        auto&                       state = tags_[name];
        if (state.count == 0 || severity == tag_severity::WARNING)
            state.severity = severity;
        ++state.count;
    }

    void report(bool final)
    {
        std::wstring                       text;
        std::map<std::string, tag_state>   tags;
        std::map<std::string, value_state> values;
        double                             elapsed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto                  now = std::chrono::steady_clock::now();
            if (!final && now - last_report_ < kSummaryInterval)
                return;
            elapsed      = std::chrono::duration<double>(now - last_report_).count();
            last_report_ = now;
            text         = text_;
            tags.swap(tags_);
            values.swap(values_);
        }

        if (!tags.empty() || !values.empty()) {
            std::wostringstream message;
            message << text << L" diagnostics interval-seconds=" << std::fixed << std::setprecision(3) << elapsed;
            for (const auto& entry : tags) {
                message << L" " << u16(entry.first) << L"{severity="
                        << (entry.second.severity == tag_severity::WARNING ? L"warning" : L"info") << L",count="
                        << entry.second.count << L"}";
            }
            for (const auto& entry : values) {
                const auto& value = entry.second;
                message << L" " << u16(entry.first) << L"{min=" << value.minimum << L",avg=" << value.mean << L",max="
                        << value.maximum << L",samples=" << value.count << L"}";
            }
            CASPAR_LOG(debug) << message.str();
        }
    }
};

class log_reporter
{
    std::mutex                             mutex_;
    std::condition_variable                condition_;
    bool                                   stopping_ = false;
    std::vector<std::shared_ptr<log_sink>> sinks_;
    std::thread                            thread_;

  public:
    log_reporter()
        : thread_([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_) {
                condition_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
                auto sinks = sinks_;
                // Only the registry owns retired sinks before the snapshot is taken.
                sinks_.erase(std::remove_if(
                                 sinks_.begin(), sinks_.end(), [](const auto& sink) { return sink.use_count() == 2; }),
                             sinks_.end());
                const bool stopping = stopping_;
                lock.unlock();
                for (const auto& sink : sinks)
                    sink->report(stopping || sink.use_count() == 1);
                lock.lock();
            }
        })
    {
    }

    ~log_reporter()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        thread_.join();
    }

    spl::shared_ptr<spi::graph_sink> create_sink()
    {
        auto                        sink = std::make_shared<log_sink>();
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.push_back(sink);
        return spl::shared_ptr<spi::graph_sink>(std::move(sink));
    }
};

} // namespace

void register_log_sink()
{
    auto reporter = std::make_shared<log_reporter>();
    spi::register_sink_factory([reporter] { return reporter->create_sink(); });
}

}} // namespace caspar::diagnostics
