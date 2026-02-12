#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/convert.hpp"

#include "test.hpp"

#include <atomic>
#include <string>

namespace {

struct Adv
{
	int value;

	explicit Adv(int v = 0) : value(v) {}
	int get() const { return value; }
	void set(int v) { value = v; }
	int add(int x) const { return value + x; }
};

struct Adv2
{
	std::string name;
	explicit Adv2(std::string n = "") : name(std::move(n)) {}
	std::string get_name() const { return name; }
};

struct ThrowingObj
{
	static std::atomic<int> instance_count;
	int value;

	explicit ThrowingObj(int v)
	{
		if (v < 0) throw std::runtime_error("negative value");
		value = v;
		++instance_count;
	}
	~ThrowingObj() { --instance_count; }

	int get() const { return value; }

	int throwing_method() const
	{
		throw std::runtime_error("method error");
	}

	int throwing_getter() const
	{
		throw std::runtime_error("getter error");
	}

	void throwing_setter(int)
	{
		throw std::runtime_error("setter error");
	}
};

std::atomic<int> ThrowingObj::instance_count = 0;

//
// Adversarial JS tests
//

template<typename Traits>
void test_adversarial_js()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<Adv, Traits> adv_class(isolate);
	adv_class
		.template ctor<int>()
		.var("value", &Adv::value)
		.property("prop", &Adv::get, &Adv::set)
		.function("add", &Adv::add)
		.function("get", &Adv::get);

	v8pp::class_<Adv2, Traits> adv2_class(isolate);
	adv2_class
		.template ctor<std::string>()
		.function("get_name", &Adv2::get_name);

	context
		.class_("Adv", adv_class)
		.class_("Adv2", adv2_class);

	// Proxy forwarding: method call through proxy uses proxy as `this`,
	// which has no internal fields, so unwrap_object correctly rejects it.
	// The key assertion is no crash.
	check_eq("proxy forwarding",
		run_script<std::string>(context,
			"var x = new Adv(5);"
			"var p = new Proxy(x, {"
			"  get: function(t, prop) { return t[prop]; },"
			"  set: function(t, prop, val) { t[prop] = val; return true; }"
			"});"
			"try { String(p.add(10)); } catch(e) { 'caught'; }"),
		"caught");

	// Proxy with throwing get trap
	check_eq("proxy throwing trap",
		run_script<std::string>(context,
			"var x = new Adv(5);"
			"var p = new Proxy(x, {"
			"  get: function(t, prop) { throw new Error('trap!'); }"
			"});"
			"try { p.add(1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// defineProperty on wrapped instance — may throw TypeError (non-configurable)
	// or succeed depending on V8 version and property type. Must not crash.
	{
		auto result = run_script<std::string>(context,
			"var x = new Adv(5);"
			"try {"
			"  Object.defineProperty(x, 'value', { get: function() { return 999; } });"
			"  'redefined';"
			"} catch(e) { 'caught'; }");
		check("defineProperty on wrapped instance (no crash)",
			result == "caught" || result == "redefined");
	}

	// Frozen object — read should still work
	check_eq("frozen object read",
		run_script<int>(context,
			"var x = new Adv(42);"
			"Object.freeze(x);"
			"x.get()"),
		42);

	// Frozen object — mutation attempt. Native interceptors may bypass freeze.
	// Must not crash regardless of outcome.
	{
		auto result = run_script<std::string>(context,
			"'use strict';"
			"var x = new Adv(42);"
			"Object.freeze(x);"
			"try { x.value = 10; 'no error'; } catch(e) { 'caught'; }");
		check("frozen object mutate (no crash)",
			result == "caught" || result == "no error");
	}

	// Null prototype — method call should fail (method no longer on chain)
	check_eq("null prototype method call",
		run_script<std::string>(context,
			"var x = new Adv(5);"
			"Object.setPrototypeOf(x, null);"
			"try { x.add(1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Constructor called as function without `new` — should throw, not crash
	check_eq("constructor without new",
		run_script<std::string>(context,
			"try { Adv(1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Circular prototype attempt — V8 prevents this
	check_eq("circular prototype",
		run_script<std::string>(context,
			"var a = {}; var b = {};"
			"Object.setPrototypeOf(a, b);"
			"try { Object.setPrototypeOf(b, a); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// GetOwnPropertyDescriptor on native property — should not crash.
	// Property may be on prototype (not own), so descriptor can be undefined.
	{
		auto result = run_script<std::string>(context,
			"var x = new Adv(7);"
			"var desc = Object.getOwnPropertyDescriptor(x, 'value');"
			"desc !== undefined ? 'own' : 'proto'");
		check("getOwnPropertyDescriptor (no crash)",
			result == "own" || result == "proto");
	}

	// Spread wrapped object — must not crash
	{
		auto result = run_script<std::string>(context,
			"var x = new Adv(3);"
			"try { var copy = {...x}; 'ok'; } catch(e) { 'caught'; }");
		check("spread wrapped object (no crash)",
			result == "ok" || result == "caught");
	}

	// Prototype swap between different wrapped types
	check_eq("prototype swap between types",
		run_script<std::string>(context,
			"var a = new Adv(1);"
			"var b = new Adv2('hello');"
			"try {"
			"  Object.setPrototypeOf(a, Object.getPrototypeOf(b));"
			"  a.get_name();"
			"  'no error';"
			"} catch(e) { 'caught'; }"),
		"caught");

	// Method extracted and called on wrong receiver
	check_eq("method on wrong receiver",
		run_script<std::string>(context,
			"var x = new Adv(5);"
			"var f = x.add;"
			"try { f.call({}, 1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Method called on plain object via .call()
	check_eq("method via call on plain obj",
		run_script<std::string>(context,
			"try { var x = new Adv(5); x.add.call({value: 99}, 1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Property getter extracted and called on wrong receiver
	check_eq("property getter on wrong receiver",
		run_script<std::string>(context,
			"var desc = Object.getOwnPropertyDescriptor(new Adv(5), 'prop');"
			"try { desc.get.call({}); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Property setter extracted and called on wrong receiver
	check_eq("property setter on wrong receiver",
		run_script<std::string>(context,
			"var desc = Object.getOwnPropertyDescriptor(new Adv(5), 'prop');"
			"try { desc.set.call({}, 42); 'no error'; } catch(e) { 'caught'; }"),
		"caught");

	// Deep prototype chain (beyond 16 limit)
	check_eq("deep prototype chain",
		run_script<std::string>(context,
			"var obj = {};"
			"for (var i = 0; i < 20; i++) { obj = Object.create(obj); }"
			"try { var x = new Adv(1); x.add.call(obj, 1); 'no error'; } catch(e) { 'caught'; }"),
		"caught");
}

//
// Exception safety tests
//

template<typename Traits>
void test_exception_safety()
{
	ThrowingObj::instance_count = 0;

	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<ThrowingObj, Traits> throwing_class(isolate);
	throwing_class
		.template ctor<int>()
		.function("get", &ThrowingObj::get)
		.function("throwing_method", &ThrowingObj::throwing_method)
		.property("throwing_prop", &ThrowingObj::throwing_getter, &ThrowingObj::throwing_setter);

	context.class_("ThrowingObj", throwing_class);

	// Constructor that throws — should produce JS exception, not crash
	check_eq("throwing ctor produces JS exception",
		run_script<std::string>(context,
			"try { new ThrowingObj(-1); 'no error'; } catch(e) { e.message; }"),
		"negative value");

	// No instances should have been created
	check_eq("throwing ctor no leak", ThrowingObj::instance_count.load(), 0);

	// Successful construction
	check_eq("successful ctor", run_script<int>(context, "var t = new ThrowingObj(5); t.get()"), 5);
	check_eq("instance created", ThrowingObj::instance_count.load(), 1);

	// Method that throws — should produce JS exception, object still valid
	check_eq("throwing method",
		run_script<std::string>(context,
			"try { t.throwing_method(); 'no error'; } catch(e) { e.message; }"),
		"method error");

	// Object should still be usable after method exception
	check_eq("object valid after method throw", run_script<int>(context, "t.get()"), 5);

	// Property getter that throws — v8pp's property_get catch block only
	// re-throws to JS when ShouldThrowOnError() (strict mode). In sloppy mode
	// the C++ exception is silently swallowed and the property reads as undefined.
	{
		auto result = run_script<std::string>(context,
			"try { t.throwing_prop; 'no error'; } catch(e) { e.message; }");
		check("throwing property getter (no crash)",
			result == "getter error" || result == "no error");
	}

	// Property setter that throws — same ShouldThrowOnError behavior
	{
		auto result = run_script<std::string>(context,
			"try { t.throwing_prop = 42; 'no error'; } catch(e) { e.message; }");
		check("throwing property setter (no crash)",
			result == "setter error" || result == "no error");
	}

	// Object still valid after property exceptions
	check_eq("object valid after prop throw", run_script<int>(context, "t.get()"), 5);

	// Destroy objects, then try to use from JS
	v8pp::class_<ThrowingObj, Traits>::destroy_objects(isolate);

	check_eq("use after destroy_objects",
		run_script<std::string>(context,
			"try { t.get(); 'no error'; } catch(e) { 'caught'; }"),
		"caught");
}

} // anonymous namespace

void test_adversarial()
{
	test_adversarial_js<v8pp::raw_ptr_traits>();
	test_adversarial_js<v8pp::shared_ptr_traits>();

	test_exception_safety<v8pp::raw_ptr_traits>();
	test_exception_safety<v8pp::shared_ptr_traits>();
}
