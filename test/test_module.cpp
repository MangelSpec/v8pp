#include "v8pp/module.hpp"
#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/property.hpp"

#include "test.hpp"

#include <type_traits>

static std::string var;

static int fun(int x)
{
	return x + 1;
}

static int x = 1;
static int get_x()
{
	return x + 1;
}
static void set_x(int v)
{
	x = v - 1;
}

static_assert(std::is_move_constructible_v<v8pp::module>);
static_assert(std::is_move_assignable_v<v8pp::module>);
static_assert(!std::is_copy_assignable_v<v8pp::module>);
static_assert(!std::is_copy_constructible_v<v8pp::module>);

void test_module()
{
	auto context = make_test_context();

	v8::HandleScope scope(context.isolate());

	v8pp::module module(context.isolate());
	v8pp::module consts(context.isolate());

	consts
		.const_("bool", true)
		.const_("char", 'Z')
		.const_("int", 100)
		.const_("str", "str")
		.const_("num", 99.9);

	module
		.submodule("consts", consts)
		.var("var", var)
		.function("fun", &fun)
		.value("empty", v8::Null(context.isolate()))
		.property("rprop", get_x)
		.property("wprop", get_x, set_x);

	context.module("module", module);

	check_eq("module.consts.bool",
		run_script<bool>(context, "module.consts.bool"), true);
	check_eq("module.consts.char",
		run_script<char>(context, "module.consts.char"), 'Z');
	check_eq("module.consts.int",
		run_script<char>(context, "module.consts.int"), 100);
	check_eq("module.consts.str",
		run_script<std::string>(context, "module.consts.str"), "str");

	check_eq("module.var", run_script<std::string>(context, "module.var = 'test'; module.var"), "test");
	check_eq("var", var, "test");

	check_eq("module.fun",
		run_script<int>(context, "module.fun(100)"), 101);

	check_eq("module.rprop",
		run_script<int>(context, "module.rprop"), 2);
	check_eq("module.wrop",
		run_script<int>(context, "++module.wprop"), 3);
	check_eq("x", x, 2);

	auto captured = std::make_shared<int>(17);
	std::weak_ptr<int> weak = captured;
	v8pp::module accessors(context.isolate());
	int reads = 0;
	accessors.property("tracked", [captured, &reads] { ++reads; return *captured; },
		[captured](int value) { *captured = value; });
	context.module("accessors", accessors);
	captured.reset();
	check("module descriptor exposes the getter and setter", run_script<bool>(context, R"(
		var saved = Object.getOwnPropertyDescriptor(accessors, "tracked");
		typeof saved.get === "function" && typeof saved.set === "function"
	)"));
	check_eq("module descriptor lookup does not invoke getter", reads, 0);
	check_eq("module descriptors dispatch normally", run_script<int>(context, "saved.set(22); saved.get()"), 22);
	v8pp::cleanup(context.isolate());
	check("module accessors release captured data", weak.expired());
	check("retained module accessors reject after reset", run_script<bool>(context, R"(
		[() => saved.get(), () => saved.set(33)].every(invoke => {
			try { invoke(); } catch (e) { return e.message.includes("retired"); }
			return false;
		})
	)"));
	check_eq("retired module getter does not access native state", reads, 1);

	struct wrapped_result
	{
		int value = 63;
	};
	v8pp::module direct(context.isolate());
	direct.property("wrapped", [&context](v8::Local<v8::Name>, v8::PropertyCallbackInfo<v8::Value> const& info)
		{
			auto* isolate = info.GetIsolate();
			v8pp::cleanup(isolate);
			v8pp::class_<wrapped_result> binding(isolate);
			binding.ctor<>().var("value", &wrapped_result::value);
			context.class_("WrappedResult", binding);
			auto fresh = context.run_script("globalThis.fresh = new WrappedResult(); fresh");
			auto* result = v8pp::class_<wrapped_result>::unwrap_object(isolate, fresh);
			info.GetReturnValue().Set(v8pp::to_v8(isolate, result));
		});
	context.module("directAccess", direct);
	check("in-flight direct getter cannot return a fresh generation wrapper", run_script<bool>(context, R"(
		try { directAccess.wrapped; false; }
		catch (e) { e.message.includes("retired") && fresh.value === 63; }
	)"));
}
