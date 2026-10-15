#include <common/diagnostics/graph.h>
#include <common/diagnostics/graph_log_sink.h>

#include <boost/log/core.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/value_ref.hpp>
#include <boost/make_shared.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class capture_backend : public boost::log::sinks::basic_sink_backend<boost::log::sinks::synchronized_feeding>
{
    std::mutex                mutex_;
    std::condition_variable   condition_;
    std::vector<std::wstring> messages_;

  public:
    void consume(const boost::log::record_view& record)
    {
        const auto message = boost::log::extract<std::wstring>("Message", record);
        if (message) {
            const auto severity = boost::log::extract<boost::log::trivial::severity_level>("Severity", record);
            if (!severity || severity.get() != boost::log::trivial::debug)
                throw std::runtime_error("Diagnostics must log at debug level");
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(message.get());
            condition_.notify_all();
        }
    }

    void expect(const std::wstring& expected, std::chrono::seconds timeout = std::chrono::seconds(8))
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, timeout, [&] {
                return std::any_of(messages_.begin(), messages_.end(), [&](const auto& message) {
                    return message.find(expected) != std::wstring::npos;
                });
            }))
            throw std::runtime_error("Expected diagnostic message was not reported");
    }

    void reject(const std::wstring& unexpected)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& message : messages_) {
            if (message.find(unexpected) != std::wstring::npos)
                throw std::runtime_error("Unexpected diagnostic message");
        }
    }

    void expect_count(const std::wstring& text, std::size_t expected)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  count = std::count_if(messages_.begin(), messages_.end(), [&](const auto& message) {
            return message.find(text) != std::wstring::npos;
        });
        if (static_cast<std::size_t>(count) != expected)
            throw std::runtime_error("Diagnostic line count exceeded the expected summary count");
    }
};

} // namespace

int main()
{
    using namespace caspar::diagnostics;
    auto backend = boost::make_shared<capture_backend>();
    auto sink    = boost::make_shared<boost::log::sinks::synchronous_sink<capture_backend>>(backend);
    boost::log::core::get()->add_sink(sink);
    register_log_sink();
    {
        graph active;
        active.set_text(L"channel-test");
        active.set_value("buffered-audio-ms", 20);
        active.set_value("buffered-audio-ms", 0);
        active.set_value("buffered-audio-ms", 10);
        active.set_value("buffered-audio-ms", std::numeric_limits<double>::quiet_NaN());
        active.set_tag(tag_severity::SILENT, "silent-test");
        for (int i = 0; i < 10000; ++i)
            active.set_tag(tag_severity::WARNING, "late-frame");

        // No further events arrive to trigger a summary.
        backend->expect(L"late-frame{severity=warning,count=10000}");
        backend->expect(L"buffered-audio-ms{min=0.000,avg=10.000,max=20.000,samples=3}");
        backend->reject(L"silent-test");
        backend->expect_count(L"channel-test", 1);

        std::thread producer([&] {
            for (int i = 0; i < 1000; ++i)
                active.set_value("concurrent", i);
        });
        for (int i = 0; i < 1000; ++i)
            active.set_value("concurrent", i);
        producer.join();
    }
    backend->expect(L"concurrent{min=0.000,avg=499.500,max=999.000,samples=2000}");
    {
        graph retired;
        retired.set_text(L"retired-test");
        retired.set_tag(tag_severity::INFO, "underflow");
        retired.set_value("queue", 2);
    }
    backend->expect(L"underflow{severity=info,count=1}");
    backend->expect(L"queue{min=2.000,avg=2.000,max=2.000,samples=1}");
    backend->expect_count(L"retired-test", 1);
    std::cout << "Performance diagnostics tests passed\n";
}
