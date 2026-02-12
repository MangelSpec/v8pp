#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/fast_api.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

// Free functions for fast API testing
static int32_t fast_add(int32_t a, int32_t b) { return a + b; }
static double fast_mul(double a, double b) { return a * b; }
static bool fast_negate(bool x) { return !x; }
static uint32_t fast_square(uint32_t x) { return x * x; }

// Non-compatible functions (should silently fall back to slow-only)
static std::string slow_greet(std::string name) { return "hello " + name; }
static int isolate_func(v8::Isolate*, int x) { return x; }

// Compile-time compatibility checks
static_assert(v8pp::detail::is_fast_api_compatible<decltype(&fast_add)>::value);
static_assert(v8pp::detail::is_fast_api_compatible<decltype(&fast_mul)>::value);
static_assert(v8pp::detail::is_fast_api_compatible<decltype(&fast_negate)>::value);
static_assert(v8pp::detail::is_fast_api_compatible<decltype(&fast_square)>::value);
static_assert(!v8pp::detail::is_fast_api_compatible<decltype(&slow_greet)>::value);
static_assert(!v8pp::detail::is_fast_api_compatible<decltype(&isolate_func)>::value);

// Return type checks
static_assert(v8pp::detail::is_fast_return_type_v<void>);
static_assert(v8pp::detail::is_fast_return_type_v<bool>);
static_assert(v8pp::detail::is_fast_return_type_v<int32_t>);
static_assert(v8pp::detail::is_fast_return_type_v<float>);
static_assert(v8pp::detail::is_fast_return_type_v<double>);
static_assert(!v8pp::detail::is_fast_return_type_v<int64_t>);
static_assert(!v8pp::detail::is_fast_return_type_v<std::string>);

// Arg type checks
static_assert(v8pp::detail::is_fast_arg_type_v<int32_t>);
static_assert(v8pp::detail::is_fast_arg_type_v<int64_t>);
static_assert(v8pp::detail::is_fast_arg_type_v<uint64_t>);
static_assert(!v8pp::detail::is_fast_arg_type_v<std::string>);
static_assert(!v8pp::detail::is_fast_arg_type_v<v8::Isolate*>);

void test_fast_api()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	// --- Free function: int32_t + int32_t ---
	context.function("fast_add", v8pp::fast_fn<&fast_add>);
	check_eq("fast_api: add ints", run_script<int>(context, "fast_add(10, 20)"), 30);
	check_eq("fast_api: add negative", run_script<int>(context, "fast_add(-5, 3)"), -2);

	// --- Free function: double * double ---
	context.function("fast_mul", v8pp::fast_fn<&fast_mul>);
	check_eq("fast_api: mul doubles", run_script<double>(context, "fast_mul(1.5, 3.0)"), 4.5);

	// --- Bool return ---
	context.function("fast_negate", v8pp::fast_fn<&fast_negate>);
	check_eq("fast_api: negate true", run_script<bool>(context, "fast_negate(true)"), false);
	check_eq("fast_api: negate false", run_script<bool>(context, "fast_negate(false)"), true);

	// --- uint32_t ---
	context.function("fast_square", v8pp::fast_fn<&fast_square>);
	check_eq("fast_api: square", run_script<int>(context, "fast_square(7)"), 49);

	// --- Non-compatible function silently falls back to slow ---
	context.function("slow_greet", v8pp::fast_fn<&slow_greet>);
	check_eq("fast_api: slow fallback", run_script<std::string>(context, "slow_greet('world')"), "hello world");

	// --- Module function with fast API ---
	{
		v8pp::module m(isolate);
		m.function("compute", v8pp::fast_fn<&fast_add>);
		context.module("fast_mod", m);

		check_eq("fast_api: module func", run_script<int>(context, "fast_mod.compute(3, 4)"), 7);
	}

	// --- Class member function with fast API ---
	{
		struct Vec
		{
			int32_t x = 0;
			int32_t y = 0;
			int32_t sum() const { return x + y; }
			int32_t dot(int32_t ox, int32_t oy) const { return x * ox + y * oy; }
		};

		v8pp::class_<Vec> vec_class(isolate);
		vec_class
			.ctor<>()
			.var("x", &Vec::x)
			.var("y", &Vec::y)
			.function("sum", v8pp::fast_fn<&Vec::sum>)
			.function("dot", v8pp::fast_fn<&Vec::dot>);
		context.class_("Vec", vec_class);

		check_eq("fast_api: member sum", run_script<int>(context, "var v = new Vec(); v.x = 3; v.y = 4; v.sum()"), 7);
		check_eq("fast_api: member dot", run_script<int>(context, "v.dot(2, 3)"), 18);
	}

	// --- Lambda as fast_fn is not supported (lambdas can't be NTTPs) ---
	// This is by design: fast_fn requires a function pointer known at compile time.
	// Use regular .function() for lambdas.
}
