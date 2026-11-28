/*
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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace caspar { namespace proaudio {

struct probe_tap;

// Measures how far one audio input lags another, continuously, on programme material.
//
// Used to set the consumer's delay so that audio sent over the network lines up with the same
// audio embedded in the SDI output: feed the embedded (reference) audio to input A and the
// consumer's own output, looped back over the network, to input B. The result is the time by
// which B arrives after A.
//
// The estimate is the generalised cross-correlation with phase transform (GCC-PHAT): the cross
// spectrum is whitened so that every frequency contributes equally, which gives a sharp, single
// peak at the true lag on speech and music alike, independent of level and EQ differences between
// the two paths. The peak is refined to a fraction of a sample by parabolic interpolation.
class sync_probe
{
  public:
    struct result
    {
        bool   valid       = false;
        double offset_ms   = 0.0; // B after A; negative when B is early
        double confidence  = 0.0; // normalised peak height, 0..1; below ~0.1 is noise
        double level_a_dbfs = -200.0;
        double level_b_dbfs = -200.0;
        unsigned long long measurements = 0;
    };

    // A window of recent audio is analysed every second; lags up to +-max_lag_ms are searched.
    sync_probe(probe_tap& tap, int sample_rate, double max_lag_ms = 250.0);
    ~sync_probe();

    sync_probe(const sync_probe&)            = delete;
    sync_probe& operator=(const sync_probe&) = delete;

    result latest() const;

    // One measurement on two equal-length signals; exposed for tests.
    static result measure(const std::vector<float>& a, const std::vector<float>& b, int sample_rate, double max_lag_ms);

  private:
    void run();

    probe_tap& tap_;
    const int    sample_rate_;
    const double max_lag_ms_;

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    bool                    stopping_ = false;
    result                  latest_;
    std::vector<double>     recent_; // last few offsets, for a median
    std::thread             thread_;
};

}} // namespace caspar::proaudio
