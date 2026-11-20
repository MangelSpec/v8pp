#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <v8.h>
#include <libplatform/libplatform.h>

#include "bench.hpp"
#include "v8pp/version.hpp"

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <cppgc/platform.h>
#endif

static v8::Platform* wrapper_platform;

v8pp::context v8pp::bench::make_context()
{
#if defined(V8PP_HAS_SAFE_BINDINGS)
    v8pp::context ctx(v8pp::context::options{.managed_wrapper_platform = wrapper_platform});
    if (ctx.has_managed_wrappers(ctx.isolate()) != (wrapper_platform != nullptr))
        throw std::logic_error("benchmark isolate did not select the requested wrapper backend");
    return ctx;
#else
    return v8pp::context{};
#endif
}

void bench_property_scale(int count, std::string_view receiver, std::string_view order, std::string_view property);

constexpr char property_scale_usage[] =
    "--property-scale <10000|100000> <base|derived|mixed> <sequential|shuffled> [value|label|child|child-label|fast-value|member|fast-member]";

bool is_scaled_property(std::string_view property)
{
    return property == "value" || property == "label" || property == "child" || property == "child-label" ||
        property == "fast-value" || property == "member" || property == "fast-member";
}

void run_benchmarks()
{
    void bench_convert();
    void bench_call();
    void bench_class();
    void bench_property();

    std::pair<char const*, void (*)()> benchmarks[] =
    {
        { "bench_convert",  bench_convert },
        { "bench_call",     bench_call },
        { "bench_class",    bench_class },
        { "bench_property", bench_property },
    };

    for (auto const& bench : benchmarks)
    {
        std::cout << "\n=== " << bench.first << " ===\n";
        try
        {
            bench.second();
        }
        catch (std::exception const& ex)
        {
            std::cerr << "  error: " << ex.what() << '\n';
        }
    }
}

int main(int argc, char const* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string const arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  --help,-h      Print this message and exit\n"
                << "  --version,-v   Print V8 and v8pp version\n"
                << "  " << property_scale_usage << '\n'
                << "                 Measure shared native property reads over a live population\n"
                ;
            return 0;
        }
        else if (arg == "-v" || arg == "--version")
        {
            std::cout << "V8 version " << v8::V8::GetVersion() << "\n";
            std::cout << "v8pp version " << v8pp::version() << "\n";
            return 0;
        }
    }

    bool const property_scale = argc > 1 && std::string(argv[1]) == "--property-scale";
    if (property_scale && ((argc != 5 && argc != 6) ||
        (std::string(argv[2]) != "10000" && std::string(argv[2]) != "100000") ||
        (std::string_view(argv[3]) != "base" && std::string_view(argv[3]) != "derived" && std::string_view(argv[3]) != "mixed") ||
        (std::string_view(argv[4]) != "sequential" && std::string_view(argv[4]) != "shuffled") ||
        (argc == 6 && !is_scaled_property(argv[5]))))
    {
        std::cerr << "Use " << property_scale_usage << '\n';
        return 1;
    }

    v8::V8::SetFlagsFromString("--expose_gc");
    v8::V8::InitializeExternalStartupData(argv[0]);

#if V8_MAJOR_VERSION >= 7
    std::unique_ptr<v8::Platform> platform(v8::platform::NewDefaultPlatform());
#else
    std::unique_ptr<v8::Platform> platform(v8::platform::CreateDefaultPlatform());
#endif

    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();
#if V8PP_ENABLE_MANAGED_WRAPPERS
    cppgc::InitializeProcess();
    wrapper_platform = platform.get();
#endif

    std::cout << "V8 version " << v8::V8::GetVersion() << "\n";
    std::cout << "v8pp version " << v8pp::version() << "\n";
    std::cout << "Wrapper backend: " << (wrapper_platform ? "managed" : "portable") << '\n';

    int status = 0;
    if (property_scale)
    {
        try
        {
            bench_property_scale(std::stoi(argv[2]), argv[3], argv[4], argc == 6 ? argv[5] : "value");
        }
        catch (std::exception const& ex)
        {
            std::cerr << ex.what() << '\n';
            status = 1;
        }
    }
    else
    {
        run_benchmarks();
    }

    std::cout << "\ndone.\n";

#if V8PP_ENABLE_MANAGED_WRAPPERS
    wrapper_platform = nullptr;
    cppgc::ShutdownProcess();
#endif
    v8::V8::Dispose();

#if V8_MAJOR_VERSION > 9 || (V8_MAJOR_VERSION == 9 && V8_MINOR_VERSION >= 8)
    v8::V8::DisposePlatform();
#else
    v8::V8::ShutdownPlatform();
#endif

    return status;
}
