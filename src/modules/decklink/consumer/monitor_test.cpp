#include "monitor.h"

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

using namespace caspar;
using namespace caspar::decklink;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class captured_logs
{
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::wtext_ostream_backend>;
    boost::shared_ptr<std::wostringstream> stream_ = boost::make_shared<std::wostringstream>();
    boost::shared_ptr<sink_t>              sink_   = boost::make_shared<sink_t>();

  public:
    captured_logs()
    {
        sink_->locked_backend()->add_stream(stream_);
        sink_->set_formatter(boost::log::expressions::stream << boost::log::expressions::wmessage);
        boost::log::core::get()->add_sink(sink_);
    }

    ~captured_logs() { boost::log::core::get()->remove_sink(sink_); }

    std::wstring text()
    {
        auto lock = sink_->locked_backend();
        return stream_->str();
    }

    size_t count(const std::wstring& text)
    {
        auto       lock     = sink_->locked_backend();
        const auto contents = stream_->str();
        size_t     count    = 0;
        for (auto pos = contents.find(text); pos != std::wstring::npos; pos = contents.find(text, pos + text.size()))
            ++count;
        return count;
    }
};

struct captured_graph : diagnostics::spi::graph_sink
{
    int failures = 0;

    void activate() override {}
    void set_text(const std::wstring&) override {}
    void set_value(const std::string&, double) override {}
    void set_color(const std::string&, int) override {}
    void auto_reset() override {}
    void set_tag(diagnostics::tag_severity severity, const std::string& name) override
    {
        if (severity == diagnostics::tag_severity::WARNING && name == "audio-schedule-failed")
            ++failures;
    }
};

void check_scheduling_diagnostics()
{
    captured_logs logs;
    const auto    sink = spl::make_shared<captured_graph>();
    diagnostics::spi::register_sink_factory([sink] { return sink; });
    diagnostics::graph     graph;
    audio_schedule_monitor monitor(L"DeckLink test");
    monitor.report(S_OK, 0, 1602, graph);
    monitor.report(S_FALSE, 1602, 1601, graph);
    require(logs.count(L"ScheduleAudioSamples") == 0, "Healthy submissions logged errors or recovery");

    for (int n = 0; n < 100; ++n)
        monitor.report(E_FAIL, 123456 + n * 1602, 1602, graph);
    require(sink->failures == 100, "Repeated failures were lost from diagnostics");
    require(logs.count(L"ScheduleAudioSamples failed") == 1, "Repeated failures flooded text logs");
    std::wostringstream expected_error;
    expected_error << L"HRESULT=0x" << std::hex << static_cast<std::uint32_t>(E_FAIL)
                   << L", stream-time=123456, sample-frames=1602";
    if (logs.count(expected_error.str()) != 1) {
        std::wcerr << logs.text();
        throw std::runtime_error("Failure omitted exact SDK error or submission context");
    }

    monitor.report(E_INVALIDARG, 300000, 1601, graph);
    monitor.report(E_INVALIDARG, 301601, 1602, graph);
    require(logs.count(L"ScheduleAudioSamples failed") == 2, "Changed HRESULT was not reported once");
    require(sink->failures == 102, "Changed HRESULT failure was not counted");
    monitor.report(S_OK, 303203, 1601, graph);
    monitor.report(S_OK, 304804, 1602, graph);
    require(logs.count(L"accepting audio again after 102 failed submissions") == 1,
            "Recovery did not summarize the complete failure burst once");
    require(logs.count(L"This does not confirm audible output") == 1, "Recovery overclaimed hardware health");

    monitor.report(E_FAIL, 306406, 1602, graph);
    monitor.report(S_OK, 308008, 1601, graph);
    require(logs.count(L"ScheduleAudioSamples failed") == 3, "New failure burst was suppressed");
    require(logs.count(L"accepting audio again after 1 failed submissions") == 1,
            "Recovery did not reset the burst counter");

    audio_schedule_monitor second(L"DeckLink second");
    second.report(E_FAIL, 0, 1602, graph);
    monitor.report(S_OK, 309609, 1602, graph);
    require(logs.count(L"DeckLink second ScheduleAudioSamples failed") == 1, "Device errors were not isolated");
    require(logs.count(L"accepting audio again") == 2, "Another device caused spurious recovery");
}

} // namespace

int main()
{
    try {
        check_scheduling_diagnostics();
        std::cout << "DeckLink scheduling diagnostic regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
