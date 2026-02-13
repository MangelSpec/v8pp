#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/module.hpp"
#include "v8pp/overload.hpp"

#include "test.hpp"

// Free functions for overload testing
static int add_int(int a, int b)
{
	return a + b;
}
static double add_double(double a, double b)
{
	return a + b;
}
static std::string add_string(std::string a, std::string b)
{
	return a + b;
}
static int negate_int(int a)
{
	return -a;
}

void test_overload()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	// --- Arity-based dispatch ---
	// f(int) vs f(int, int)
	context.function("arity_test",
		static_cast<int (*)(int)>(&negate_int),
		static_cast<int (*)(int, int)>(&add_int));

	check_eq("overload: arity 1 arg", run_script<int>(context, "arity_test(5)"), -5);
	check_eq("overload: arity 2 args", run_script<int>(context, "arity_test(3, 7)"), 10);

	// --- Type-based dispatch ---
	// f(int, int) vs f(string, string)
	context.function("type_test",
		static_cast<int (*)(int, int)>(&add_int),
		static_cast<std::string (*)(std::string, std::string)>(&add_string));

	check_eq("overload: type int", run_script<int>(context, "type_test(10, 20)"), 30);
	check_eq("overload: type string", run_script<std::string>(context, "type_test('hello', ' world')"), "hello world");

	// --- Mixed arity + type ---
	// f(int) vs f(double, double)
	context.function("mixed_test",
		static_cast<int (*)(int)>(&negate_int),
		static_cast<double (*)(double, double)>(&add_double));

	check_eq("overload: mixed 1 arg", run_script<int>(context, "mixed_test(42)"), -42);
	check_eq("overload: mixed 2 args", run_script<double>(context, "mixed_test(1.5, 2.5)"), 4.0);

	// --- Lambda overloads ---
	context.function("lambda_test", [](int x)
		{ return x * 2; }, [](std::string s)
		{ return s + s; });

	check_eq("overload: lambda int", run_script<int>(context, "lambda_test(7)"), 14);
	check_eq("overload: lambda string", run_script<std::string>(context, "lambda_test('ab')"), "abab");

	// --- No match → error ---
	check_ex<std::runtime_error>("overload: no match", [&context]
		{ run_script<int>(context, "arity_test()"); });

	// --- Module function overloads ---
	{
		v8pp::module m(isolate);
		m.function("compute", [](int x)
			{ return x * x; }, [](int x, int y)
			{ return x + y; });
		context.module("ovl_mod", m);

		check_eq("overload: module 1 arg", run_script<int>(context, "ovl_mod.compute(5)"), 25);
		check_eq("overload: module 2 args", run_script<int>(context, "ovl_mod.compute(3, 4)"), 7);
	}

	// --- Overload with defaults ---
	{
		context.function("defaults_overload",
			v8pp::with_defaults([](int a, int b)
				{ return a + b; }, v8pp::defaults(10)),
			[](std::string s)
			{ return s; });

		check_eq("overload: defaults int both", run_script<int>(context, "defaults_overload(3, 7)"), 10);
		check_eq("overload: defaults int default", run_script<int>(context, "defaults_overload(5)"), 15);
		check_eq("overload: defaults string", run_script<std::string>(context, "defaults_overload('hi')"), "hi");
	}

	// --- Class member overloads ---
	{
		struct Calc
		{
			int value = 0;
			int add_one(int n)
			{
				value += n;
				return value;
			}
			int add_two(int a, int b)
			{
				value += a + b;
				return value;
			}
		};

		v8pp::class_<Calc> calc_class(isolate);
		calc_class
			.ctor()
			.function("add", &Calc::add_one, &Calc::add_two);
		context.class_("Calc", calc_class);

		check_eq("overload: class 1 arg", run_script<int>(context, "var calc = new Calc(); calc.add(5)"), 5);
		check_eq("overload: class 2 args", run_script<int>(context, "calc.add(3, 7)"), 15);
	}

	// --- v8pp::overload<Sig> selector ---
	{
		struct Multi
		{
			int compute(int x) { return x * 2; }
			int compute(int x, int y) { return x + y; }
		};

		v8pp::class_<Multi> multi_class(isolate);
		multi_class
			.ctor()
			.function("compute",
				v8pp::overload<int (Multi::*)(int)>(&Multi::compute),
				v8pp::overload<int (Multi::*)(int, int)>(&Multi::compute));
		context.class_("Multi", multi_class);

		check_eq("overload: selector 1 arg", run_script<int>(context, "var m = new Multi(); m.compute(5)"), 10);
		check_eq("overload: selector 2 args", run_script<int>(context, "m.compute(3, 4)"), 7);
	}
}
