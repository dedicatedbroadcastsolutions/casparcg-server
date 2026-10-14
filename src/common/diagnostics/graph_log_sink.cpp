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

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace caspar { namespace diagnostics {

namespace {

// How long a tag can keep occurring before its next summary line is logged.
constexpr auto kSummaryInterval = std::chrono::seconds(5);

class log_sink : public spi::graph_sink
{
    struct tag_state
    {
        std::chrono::steady_clock::time_point last_logged;
        int                                    occurrences_since_log = 0;
    };

    mutable std::mutex                mutex_;
    std::wstring                      text_;
    std::map<std::string, tag_state>  tags_;

  public:
    void activate() override {}

    void set_text(const std::wstring& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text_ = value;
    }

    void set_value(const std::string& /*name*/, double /*value*/) override {}
    void set_color(const std::string& /*name*/, int /*color*/) override {}
    void auto_reset() override {}

    void set_tag(tag_severity severity, const std::string& name) override
    {
        // SILENT tags (e.g. html_producer's "nothing new this tick") are intentionally not
        // noteworthy - only WARNING/INFO are meant to flag an actual performance issue.
        if (severity == tag_severity::SILENT)
            return;

        std::lock_guard<std::mutex> lock(mutex_);

        auto  now   = std::chrono::steady_clock::now();
        auto& state = tags_[name];

        ++state.occurrences_since_log;

        // First time this tag has ever fired for this graph: log it immediately so an operator
        // sees it as soon as it happens, not only in a later periodic summary.
        bool first_occurrence = state.last_logged.time_since_epoch().count() == 0;
        if (!first_occurrence && (now - state.last_logged) < kSummaryInterval)
            return;

        log(severity, name, state.occurrences_since_log);
        state.last_logged          = now;
        state.occurrences_since_log = 0;
    }

  private:
    void log(tag_severity severity, const std::string& name, int occurrences)
    {
        std::wstring message = text_ + L" " + u16(name);
        if (occurrences > 1) {
            message += L" (x" + std::to_wstring(occurrences) + L" in the last " +
                      std::to_wstring(kSummaryInterval.count()) + L"s)";
        }

        if (severity == tag_severity::WARNING) {
            CASPAR_LOG(warning) << message;
        } else {
            CASPAR_LOG(info) << message;
        }
    }
};

} // namespace

void register_log_sink()
{
    spi::register_sink_factory([] { return spl::make_shared<log_sink>(); });
}

}} // namespace caspar::diagnostics
