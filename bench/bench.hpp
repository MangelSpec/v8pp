#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "v8pp/context.hpp"
#include "v8pp/convert.hpp"

namespace v8pp::bench {

struct result
{
    std::string name;
    size_t iterations;
    std::vector<double> samples_ns; // per-iteration time in nanoseconds

    double min_ns() const
    {
        return *std::min_element(samples_ns.begin(), samples_ns.end());
    }

    double max_ns() const
    {
        return *std::max_element(samples_ns.begin(), samples_ns.end());
    }

    double mean_ns() const
    {
        return std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0)
             / static_cast<double>(samples_ns.size());
    }

    double median_ns() const
    {
        std::vector<double> sorted = samples_ns;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        if (n % 2 == 0)
            return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
        return sorted[n / 2];
    }

    double ops_per_sec() const
    {
        double med = median_ns();
        return med > 0.0 ? 1'000'000'000.0 / med : 0.0;
    }
};

/// Run a benchmark: warmup, then collect timing samples.
/// Each sample times `iterations_per_sample` calls of `fn`.
inline result run(std::string_view name,
                  size_t iterations_per_sample,
                  size_t sample_count,
                  std::function<void()> const& fn)
{
    // Warmup: 10% of iterations, minimum 10
    size_t warmup = std::max<size_t>(iterations_per_sample / 10, 10);
    for (size_t i = 0; i < warmup; ++i)
    {
        fn();
    }

    result r;
    r.name = name;
    r.iterations = iterations_per_sample;
    r.samples_ns.reserve(sample_count);

    for (size_t s = 0; s < sample_count; ++s)
    {
        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iterations_per_sample; ++i)
        {
            fn();
        }
        auto end = std::chrono::steady_clock::now();

        double elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        r.samples_ns.push_back(elapsed_ns / static_cast<double>(iterations_per_sample));
    }

    return r;
}

/// Convenience: benchmark a JS script executed via context.run_script()
inline result run_script_bench(std::string_view name,
                               v8pp::context& ctx,
                               std::string_view script,
                               size_t iterations = 10000,
                               size_t samples = 20)
{
    v8::Isolate* isolate = ctx.isolate();
    return run(name, iterations, samples, [&]()
    {
        v8::HandleScope scope(isolate);
        v8::TryCatch try_catch(isolate);
        ctx.run_script(script);
    });
}

inline void print_result(result const& r)
{
    auto fmt_time = [](double ns) -> std::string
    {
        char buf[64];
        if (ns < 1'000.0)
            std::snprintf(buf, sizeof(buf), "%.1f ns", ns);
        else if (ns < 1'000'000.0)
            std::snprintf(buf, sizeof(buf), "%.2f us", ns / 1'000.0);
        else
            std::snprintf(buf, sizeof(buf), "%.2f ms", ns / 1'000'000.0);
        return buf;
    };

    auto fmt_ops = [](double ops) -> std::string
    {
        char buf[64];
        if (ops >= 1'000'000.0)
            std::snprintf(buf, sizeof(buf), "%.2f M", ops / 1'000'000.0);
        else if (ops >= 1'000.0)
            std::snprintf(buf, sizeof(buf), "%.2f K", ops / 1'000.0);
        else
            std::snprintf(buf, sizeof(buf), "%.0f", ops);
        return buf;
    };

    std::cout << std::left << std::setw(45) << r.name
              << "  median=" << std::setw(12) << fmt_time(r.median_ns())
              << "  min=" << std::setw(12) << fmt_time(r.min_ns())
              << "  max=" << std::setw(12) << fmt_time(r.max_ns())
              << "  ops/s=" << fmt_ops(r.ops_per_sec())
              << "\n";
}

} // namespace v8pp::bench
