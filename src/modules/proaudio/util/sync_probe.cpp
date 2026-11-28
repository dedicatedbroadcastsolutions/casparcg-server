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

#include "sync_probe.h"

#include "../device/device_stream.h"

#include <common/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

// FFmpeg's headers do not build warning-free under MSVC's /WX; the OAL consumer does the same.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace caspar { namespace proaudio {

using namespace std::chrono_literals;

namespace {

// 2^16 samples is 1.37 s at 48 kHz: longer than a second, so a test signal that beeps once a
// second always has a beep in the window, and short enough to follow a change within seconds.
constexpr std::size_t window_frames = 1 << 16;
constexpr auto        interval      = 1000ms;
constexpr std::size_t median_of     = 5;
// Below this a channel is treated as silent and no lag is reported.
constexpr double silence_dbfs = -70.0;
constexpr double pi           = 3.14159265358979323846;

struct av_deleter
{
    void operator()(void* p) const { av_free(p); }
};
template <typename T>
using av_buffer = std::unique_ptr<T[], av_deleter>;

template <typename T>
av_buffer<T> av_alloc(std::size_t n)
{
    return av_buffer<T>(static_cast<T*>(av_mallocz(n * sizeof(T))));
}

struct tx_context
{
    AVTXContext* ctx = nullptr;
    av_tx_fn     fn  = nullptr;
    tx_context(int inverse, int len)
    {
        float scale = 1.0f;
        if (av_tx_init(&ctx, &fn, AV_TX_FLOAT_RDFT, inverse, len, &scale, 0) < 0)
            ctx = nullptr;
    }
    ~tx_context() { av_tx_uninit(&ctx); }
};

double rms_dbfs(const std::vector<float>& x)
{
    double sum = 0.0;
    for (auto v : x)
        sum += static_cast<double>(v) * v;
    auto rms = std::sqrt(sum / std::max<std::size_t>(1, x.size()));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -200.0;
}

} // namespace

sync_probe::sync_probe(probe_tap& tap, int sample_rate, double max_lag_ms)
    : tap_(tap)
    , sample_rate_(sample_rate)
    , max_lag_ms_(max_lag_ms)
    , thread_([this] { run(); })
{
}

sync_probe::~sync_probe()
{
    {
        std::lock_guard guard{mutex_};
        stopping_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

sync_probe::result sync_probe::latest() const
{
    std::lock_guard guard{mutex_};
    return latest_;
}

void sync_probe::run()
{
    std::vector<float> pair(window_frames * 2);
    std::vector<float> a(window_frames), b(window_frames);

    std::unique_lock lock{mutex_};
    while (!cv_.wait_for(lock, interval, [this] { return stopping_; })) {
        lock.unlock();

        // Analyse the most recent window only: drop anything older.
        auto queued = tap_.ring.size();
        if (queued >= window_frames) {
            if (queued > window_frames)
                tap_.ring.discard(queued - window_frames);
            tap_.ring.pop(pair.data(), window_frames);
            for (std::size_t i = 0; i < window_frames; ++i) {
                a[i] = pair[2 * i];
                b[i] = pair[2 * i + 1];
            }

            auto r = measure(a, b, sample_rate_, max_lag_ms_);

            lock.lock();
            r.measurements = latest_.measurements + 1;
            if (r.valid) {
                recent_.push_back(r.offset_ms);
                if (recent_.size() > median_of)
                    recent_.erase(recent_.begin());
                auto sorted = recent_;
                std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
                r.offset_ms = sorted[sorted.size() / 2];
            }
            latest_ = r;
            lock.unlock();
        }

        lock.lock();
    }
}

sync_probe::result sync_probe::measure(const std::vector<float>& a,
                                       const std::vector<float>& b,
                                       int                       sample_rate,
                                       double                    max_lag_ms)
{
    result r;
    r.level_a_dbfs = rms_dbfs(a);
    r.level_b_dbfs = rms_dbfs(b);
    if (r.level_a_dbfs < silence_dbfs || r.level_b_dbfs < silence_dbfs || a.size() != b.size() || a.empty())
        return r;

    // Zero-pad to twice the window so the circular correlation holds no wrapped-around lags.
    std::size_t n = 1;
    while (n < 2 * a.size())
        n <<= 1;
    const auto bins = n / 2 + 1;

    tx_context fwd(0, static_cast<int>(n));
    tx_context inv(1, static_cast<int>(n));
    if (!fwd.ctx || !inv.ctx)
        return r;

    auto ta = av_alloc<float>(n);
    auto tb = av_alloc<float>(n);
    auto fa = av_alloc<AVComplexFloat>(bins);
    auto fb = av_alloc<AVComplexFloat>(bins);
    auto xc = av_alloc<float>(n);

    // A Hann window keeps the window's edges from correlating with each other.
    const auto len = a.size();
    for (std::size_t i = 0; i < len; ++i) {
        auto w = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / (len - 1)));
        ta[i]  = a[i] * w;
        tb[i]  = b[i] * w;
    }

    fwd.fn(fwd.ctx, fa.get(), ta.get(), sizeof(float));
    fwd.fn(fwd.ctx, fb.get(), tb.get(), sizeof(float));

    // Cross spectrum B * conj(A), whitened (PHAT). Its inverse peaks at the lag of B behind A.
    for (std::size_t k = 0; k < bins; ++k) {
        auto re  = fb[k].re * fa[k].re + fb[k].im * fa[k].im;
        auto im  = fb[k].im * fa[k].re - fb[k].re * fa[k].im;
        auto mag = std::sqrt(re * re + im * im);
        if (mag > 1e-20f) {
            fa[k].re = re / mag;
            fa[k].im = im / mag;
        } else {
            fa[k].re = fa[k].im = 0.0f;
        }
    }
    inv.fn(inv.ctx, xc.get(), fa.get(), sizeof(AVComplexFloat));

    const auto max_lag = std::min<long>(static_cast<long>(max_lag_ms * sample_rate / 1000.0), static_cast<long>(len) - 1);
    auto       at      = [&](long lag) { return xc[static_cast<std::size_t>(lag >= 0 ? lag : static_cast<long>(n) + lag)]; };

    long  best      = 0;
    float best_val  = -1e30f;
    for (long lag = -max_lag; lag <= max_lag; ++lag) {
        if (at(lag) > best_val) {
            best_val = at(lag);
            best     = lag;
        }
    }

    // Parabolic interpolation through the peak and its neighbours for sub-sample precision.
    double frac = 0.0;
    if (best > -max_lag && best < max_lag) {
        double y0 = at(best - 1), y1 = at(best), y2 = at(best + 1);
        double den = y0 - 2.0 * y1 + y2;
        if (std::abs(den) > 1e-12)
            frac = 0.5 * (y0 - y2) / den;
    }

    // The whitened spectrum's inverse sums to n at a perfect, noise-free match.
    r.confidence = std::clamp(static_cast<double>(best_val) / static_cast<double>(bins), 0.0, 1.0);
    r.offset_ms  = (static_cast<double>(best) + frac) * 1000.0 / sample_rate;
    r.valid      = r.confidence >= 0.05;
    return r;
}

}} // namespace caspar::proaudio
