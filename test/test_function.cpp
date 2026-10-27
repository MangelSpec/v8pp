#include "v8pp/function.hpp"
#include "v8pp/context.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

static int f(int const& x)
{
	return x;
}
static std::string g(char const* s)
{
	return s ? s : "";
}
static int h(v8::Isolate*, int x, int y)
{
	return x + y;
}

struct X
{
	int operator()(int x) const { return -x; }
};

void test_function()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	context.function("f", f);
	check_eq("f", run_script<int>(context, "f(1)"), 1);

	context.function("g", &g);
	check_eq("g", run_script<std::string>(context, "g('abc')"), "abc");

	context.function("h", h);
	check_eq("h", run_script<int>(context, "h(1, 2)"), 3);

	int x = 1, y = 2;
	struct moveonly
	{
		double v = 3;
		moveonly() = default;
		moveonly(moveonly const&) = delete;
		moveonly& operator=(moveonly const&) = delete;
		moveonly(moveonly&&) = default;
		moveonly& operator=(moveonly&&) = default;
	};
	moveonly z;
	context.function("lambda", [x, y, z = std::move(z)](int a)
		{ return a + x + y + z.v; });
	check_eq("lambda", run_script<int>(context, "lambda(3)"), 9);

	auto lambda2 = []()
	{
		return 99;
	};
	// TODO: static_assert(v8pp::detail::external_data::is_bitcast_allowed<decltype(lambda2)>::value);

	context.function("lambda2", lambda2);
	check_eq("lambda2", run_script<int>(context, "lambda2()"), 99);

	X xfun;
	context.function("xfun", xfun);
	check_eq("xfun", run_script<int>(context, "xfun(5)"), -5);

	std::function<int(int)> fun = f;
	context.function("fun", fun);
	check_eq("fun", run_script<int>(context, "fun(42)"), 42);
	check("function preserves original conversion exception", run_script<bool>(context, R"(
		(() => { const original = {}; try { g({ toString() { throw original; } }); }
		catch (e) { return e === original; } return false; })()
	)"));
	context.function("default_exception", [](std::string value)
		{ return value; }, v8pp::defaults("default"));
	check("function with defaults preserves original exception", run_script<bool>(context, R"(
		(() => { const original = {}; try { default_exception({ toString() { throw original; } }); }
		catch (e) { return e === original; } return false; })()
	)"));

	int module_value = 7;
	std::string module_string;
	v8pp::module variables(isolate);
	variables.var("value", module_value).var("text", module_string).property("throwing", []() -> int
		{ throw std::runtime_error("getter failed"); }, [](int)
		{ throw std::runtime_error("setter failed"); });
	context.module("variables", variables);
	check("module variable conversion failures are JavaScript exceptions", run_script<bool>(context, "try { variables.value = 'invalid'; false; } catch (e) { e instanceof Error; }"));
	check_eq("failed variable conversion preserves value", module_value, 7);
	check_eq("property getter failure propagates in sloppy mode", run_script<std::string>(context, "try { variables.throwing; 'missing'; } catch (e) { e.message; }"), "getter failed");
	check_eq("property setter failure propagates in sloppy mode", run_script<std::string>(context, "try { variables.throwing = 1; 'missing'; } catch (e) { e.message; }"), "setter failed");
	check("module variable preserves original conversion exception", run_script<bool>(context, R"(
		(() => { const original = {}; try { variables.text = { toString() { throw original; } }; }
		catch (e) { return e === original; } return false; })()
	)"));
}
