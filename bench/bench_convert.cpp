#include <map>
#include <optional>
#include <string>
#include <vector>

#include "v8pp/context.hpp"
#include "v8pp/convert.hpp"
#include "bench.hpp"

void bench_convert()
{
    v8pp::context context;
    v8::Isolate* isolate = context.isolate();
    v8::HandleScope scope(isolate);

    using namespace v8pp::bench;
    size_t const N = 50000;
    size_t const S = 20;

    // --- Primitive to_v8 ---
    print_result(run("int32 to_v8", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, 42);
    }));

    print_result(run("double to_v8", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, 3.14);
    }));

    print_result(run("bool to_v8", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, true);
    }));

    // --- Primitive from_v8 ---
    v8::Local<v8::Value> v8_int = v8pp::to_v8(isolate, 42);
    print_result(run("int32 from_v8", N, S, [&]()
    {
        v8pp::from_v8<int>(isolate, v8_int);
    }));

    v8::Local<v8::Value> v8_double = v8pp::to_v8(isolate, 3.14);
    print_result(run("double from_v8", N, S, [&]()
    {
        v8pp::from_v8<double>(isolate, v8_double);
    }));

    v8::Local<v8::Value> v8_bool = v8pp::to_v8(isolate, true);
    print_result(run("bool from_v8", N, S, [&]()
    {
        v8pp::from_v8<bool>(isolate, v8_bool);
    }));

    // --- String conversions ---
    print_result(run("short string to_v8 (5 chars)", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, "hello");
    }));

    print_result(run("long string to_v8 (100 chars)", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, std::string(100, 'x'));
    }));

    v8::Local<v8::Value> v8_str = v8pp::to_v8(isolate, "hello world");
    print_result(run("string from_v8", N, S, [&]()
    {
        v8pp::from_v8<std::string>(isolate, v8_str);
    }));

    // --- Container conversions ---
    std::vector<int> vec100(100, 42);
    print_result(run("vector<int>(100) to_v8", N / 10, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, vec100);
    }));

    v8::Local<v8::Value> v8_arr = v8pp::to_v8(isolate, vec100);
    print_result(run("vector<int>(100) from_v8", N / 10, S, [&]()
    {
        v8pp::from_v8<std::vector<int>>(isolate, v8_arr);
    }));

    // --- Map conversion ---
    std::map<std::string, int> map10;
    for (int i = 0; i < 10; ++i) map10["key" + std::to_string(i)] = i;
    print_result(run("map<string,int>(10) to_v8", N / 10, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, map10);
    }));

    v8::Local<v8::Value> v8_map = v8pp::to_v8(isolate, map10);
    print_result(run("map<string,int>(10) from_v8", N / 10, S, [&]()
    {
        v8pp::from_v8<std::map<std::string, int>>(isolate, v8_map);
    }));

    // --- std::optional ---
    std::optional<int> opt_val = 42;
    print_result(run("optional<int> to_v8 (engaged)", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, opt_val);
    }));

    std::optional<int> opt_empty;
    print_result(run("optional<int> to_v8 (empty)", N, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8pp::to_v8(isolate, opt_empty);
    }));
}
