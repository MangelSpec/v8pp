#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <v8.h>
#include <libplatform/libplatform.h>

#include "v8pp/version.hpp"

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

    v8::V8::SetFlagsFromString("--expose_gc");
    v8::V8::InitializeExternalStartupData(argv[0]);

#if V8_MAJOR_VERSION >= 7
    std::unique_ptr<v8::Platform> platform(v8::platform::NewDefaultPlatform());
#else
    std::unique_ptr<v8::Platform> platform(v8::platform::CreateDefaultPlatform());
#endif

    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();

    std::cout << "V8 version " << v8::V8::GetVersion() << "\n";
    std::cout << "v8pp version " << v8pp::version() << "\n";

    run_benchmarks();

    std::cout << "\ndone.\n";

    v8::V8::Dispose();

#if V8_MAJOR_VERSION > 9 || (V8_MAJOR_VERSION == 9 && V8_MINOR_VERSION >= 8)
    v8::V8::DisposePlatform();
#else
    v8::V8::ShutdownPlatform();
#endif

    return 0;
}
