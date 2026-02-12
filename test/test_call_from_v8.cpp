#include "v8pp/call_from_v8.hpp"
#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/function.hpp"
#include "v8pp/module.hpp"
#include "v8pp/ptr_traits.hpp"
#include "v8pp/throw_ex.hpp"

#include "test.hpp"

static int x()
{
	return 0;
}
static int y(int a)
{
	return a;
}
static int z(v8::Isolate*, int a)
{
	return a;
}

static void w(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	return args.GetReturnValue().Set(args.Length());
}

struct X;
void class_ref(X&);
void class_ptr(X*);
void class_sptr(std::shared_ptr<X>);

using v8pp::detail::call_from_v8_traits;

using v8pp::raw_ptr_traits;
using v8pp::shared_ptr_traits;

template<typename F, size_t Index, typename Traits>
using arg_convert = typename call_from_v8_traits<F>::template arg_converter<
	typename call_from_v8_traits<F>::template arg_type<Index>, Traits>;

// fundamental type converters
static_assert(std::same_as<arg_convert<decltype(y), 0, raw_ptr_traits>, v8pp::convert<int>>);
static_assert(std::same_as<arg_convert<decltype(y), 0, shared_ptr_traits>, v8pp::convert<int>>);

static_assert(std::same_as<arg_convert<decltype(z), 1, raw_ptr_traits>, v8pp::convert<int>>);
static_assert(std::same_as<arg_convert<decltype(z), 1, shared_ptr_traits>, v8pp::convert<int>>);

// cv arg converters
static void s(std::string, std::vector<int>&, std::shared_ptr<int> const&, std::string*, std::string const*) {}

static_assert(std::same_as<arg_convert<decltype(s), 0, raw_ptr_traits>, v8pp::convert<std::string>>);
static_assert(std::same_as<arg_convert<decltype(s), 0, shared_ptr_traits>, v8pp::convert<std::string>>);

static_assert(std::same_as<arg_convert<decltype(s), 1, raw_ptr_traits>, v8pp::convert<std::vector<int>>>);
static_assert(std::same_as<arg_convert<decltype(s), 1, shared_ptr_traits>, v8pp::convert<std::vector<int>>>);

static_assert(std::same_as<arg_convert<decltype(s), 2, raw_ptr_traits>, v8pp::convert<std::shared_ptr<int>>>);
static_assert(std::same_as<arg_convert<decltype(s), 2, shared_ptr_traits>, v8pp::convert<std::shared_ptr<int>>>);

static_assert(std::same_as<arg_convert<decltype(s), 3, raw_ptr_traits>, v8pp::convert<std::string*>>);
static_assert(std::same_as<arg_convert<decltype(s), 3, shared_ptr_traits>, v8pp::convert<std::string*>>);

static_assert(std::same_as<arg_convert<decltype(s), 4, raw_ptr_traits>, v8pp::convert<std::string const*>>);
static_assert(std::same_as<arg_convert<decltype(s), 4, shared_ptr_traits>, v8pp::convert<std::string const*>>);

// fundamental types cv arg converters
static void t(int, char&, bool const&, float*, char const*) {}

static_assert(std::same_as<arg_convert<decltype(t), 0, raw_ptr_traits>, v8pp::convert<int>>);
static_assert(std::same_as<arg_convert<decltype(t), 0, shared_ptr_traits>, v8pp::convert<int>>);

static_assert(std::same_as<arg_convert<decltype(t), 1, raw_ptr_traits>, v8pp::convert<char>>);
static_assert(std::same_as<arg_convert<decltype(t), 1, shared_ptr_traits>, v8pp::convert<char>>);

static_assert(std::same_as<arg_convert<decltype(t), 2, raw_ptr_traits>, v8pp::convert<bool>>);
static_assert(std::same_as<arg_convert<decltype(t), 2, shared_ptr_traits>, v8pp::convert<bool>>);

static_assert(std::same_as<arg_convert<decltype(t), 3, raw_ptr_traits>, v8pp::convert<float*>>);
static_assert(std::same_as<arg_convert<decltype(t), 3, shared_ptr_traits>, v8pp::convert<float*>>);

static_assert(std::same_as<arg_convert<decltype(t), 4, raw_ptr_traits>, v8pp::convert<char const*>>);
static_assert(std::same_as<arg_convert<decltype(t), 4, shared_ptr_traits>, v8pp::convert<char const*>>);

void test_call_from_v8()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	(void)&s; //context.function("s", s);
	(void)&t; //context.function("t", t);
	context.function("x", x);
	context.function("y", y);
	context.function("z", z);
	context.function("w", w);

	check_eq("x", run_script<int>(context, "x()"), 0);
	check_eq("y", run_script<int>(context, "y(1)"), 1);
	check_eq("z", run_script<int>(context, "z(2)"), 2);
	check_eq("w", run_script<int>(context, "w(2, 'd', true, null)"), 4);

	// --- Default parameter tests ---

	// Free function with 1 default
	static auto add_default = [](int a, int b) { return a + b; };
	context.function("add_default", add_default, v8pp::defaults(10));

	check_eq("defaults: all args provided", run_script<int>(context, "add_default(3, 7)"), 10);
	check_eq("defaults: 1 default used", run_script<int>(context, "add_default(5)"), 15);

	// Free function with 2 defaults
	static auto three_args = [](int a, int b, int c) { return a + b + c; };
	context.function("three_args", three_args, v8pp::defaults(20, 30));

	check_eq("defaults: 2 defaults, all provided", run_script<int>(context, "three_args(1, 2, 3)"), 6);
	check_eq("defaults: 2 defaults, 1 used", run_script<int>(context, "three_args(1, 2)"), 33);
	check_eq("defaults: 2 defaults, both used", run_script<int>(context, "three_args(1)"), 51);

	// Too few args should throw
	check_ex<std::runtime_error>("defaults: too few args", [&context]
	{
		run_script<int>(context, "three_args()");
	});

	// Too many args should throw
	check_ex<std::runtime_error>("defaults: too many args", [&context]
	{
		run_script<int>(context, "three_args(1, 2, 3, 4)");
	});

	// String default
	static auto greet = [](std::string name, std::string greeting) { return greeting + " " + name; };
	context.function("greet", greet, v8pp::defaults(std::string("hello")));

	check_eq("defaults: string default used", run_script<std::string>(context, "greet('world')"), "hello world");
	check_eq("defaults: string default overridden", run_script<std::string>(context, "greet('world', 'hi')"), "hi world");

	// Module function with defaults
	{
		v8pp::module m(context.isolate());
		m.function("multiply", [](int a, int b) { return a * b; }, v8pp::defaults(2));
		context.module("def_mod", m);

		check_eq("module defaults: provided", run_script<int>(context, "def_mod.multiply(3, 4)"), 12);
		check_eq("module defaults: default used", run_script<int>(context, "def_mod.multiply(5)"), 10);
	}

	// Class member function with defaults
	{
		struct Counter
		{
			int value = 0;
			int add(int n) { value += n; return value; }
		};

		v8pp::class_<Counter> counter_class(context.isolate());
		counter_class
			.ctor()
			.function("add", &Counter::add, v8pp::defaults(1));
		context.class_("Counter", counter_class);

		check_eq("class defaults: provided", run_script<int>(context, "var c = new Counter(); c.add(5)"), 5);
		check_eq("class defaults: default used", run_script<int>(context, "c.add()"), 6);
	}

	// Constructor with defaults
	{
		struct Named
		{
			std::string name;
			int value;
			Named(std::string n, int v) : name(std::move(n)), value(v) {}
		};

		v8pp::class_<Named> named_class(context.isolate());
		named_class
			.ctor<std::string, int>(v8pp::defaults(42))
			.var("name", &Named::name)
			.var("value", &Named::value);
		context.class_("Named", named_class);

		check_eq("ctor defaults: all provided", run_script<int>(context, "var n1 = new Named('test', 7); n1.value"), 7);
		check_eq("ctor defaults: default used", run_script<int>(context, "var n2 = new Named('test'); n2.value"), 42);
		check_eq("ctor defaults: name correct", run_script<std::string>(context, "n2.name"), "test");
	}
}
