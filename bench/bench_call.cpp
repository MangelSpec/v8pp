#include <string>

#include "v8pp/context.hpp"
#include "v8pp/module.hpp"
#include "v8pp/fast_api.hpp"
#include "bench.hpp"

namespace {

void noop() {}
int noop_return() { return 0; }
int32_t add_ints(int32_t a, int32_t b) { return a + b; }
std::string concat(std::string const& a, std::string const& b) { return a + b; }
double compute(double a, double b, double c) { return a * b + c; }

} // anonymous namespace

void bench_call()
{
    v8pp::context context;
    v8::Isolate* isolate = context.isolate();
    v8::HandleScope scope(isolate);

    using namespace v8pp::bench;
    size_t const N = 10000;
    size_t const S = 20;

    // Bind functions into global scope
    context.function("noop", &noop);
    context.function("noop_return", &noop_return);
    context.function("add_ints", &add_ints);
    context.function("concat", &concat);
    context.function("compute", &compute);

#if V8_MAJOR_VERSION >= 10
    context.function("fast_add", v8pp::fast_fn<&add_ints>);
#endif

    // --- Baseline: empty function call overhead ---
    print_result(run_script_bench("JS->C++ void noop()",
        context, "noop()", N, S));

    print_result(run_script_bench("JS->C++ int noop_return()",
        context, "noop_return()", N, S));

    // --- Primitive argument passing ---
    print_result(run_script_bench("JS->C++ add_ints(int, int)",
        context, "add_ints(1, 2)", N, S));

#if V8_MAJOR_VERSION >= 10
    // --- Fast API vs slow path ---
    print_result(run_script_bench("JS->C++ fast_add(int, int)",
        context, "fast_add(1, 2)", N, S));
#endif

    // --- String arguments ---
    print_result(run_script_bench("JS->C++ concat(str, str)",
        context, "concat('hello', ' world')", N, S));

    // --- Multiple arguments ---
    print_result(run_script_bench("JS->C++ compute(dbl, dbl, dbl)",
        context, "compute(1.5, 2.5, 3.5)", N, S));

    // --- Tight loop from JS (V8 JIT) ---
    context.function("add", &add_ints);
    print_result(run_script_bench("JS loop: 1000x add_ints",
        context, "var s = 0; for (var i = 0; i < 1000; i++) s = add(s, 1); s",
        N / 100, S));

    // --- Module-scoped function call ---
    {
        v8pp::module m(isolate);
        m.function("add", &add_ints);
        context.module("mod", m);

        print_result(run_script_bench("JS->C++ mod.add(int, int)",
            context, "mod.add(1, 2)", N, S));
    }
}
