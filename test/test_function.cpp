#include "v8pp/function.hpp"
#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

#ifdef V8PP_TEST_CONSOLE_PLUGIN
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

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


namespace {

struct native_value
{
	int value = 7;
	int read() const { return value; }
	void write(int next) { value = next; }
};

void test_retained_bindings()
{
	auto main = make_test_context();
	auto* isolate = main.isolate();
	v8::HandleScope scope(isolate);
	v8pp::context auxiliary(isolate, nullptr, false, false);

	auto capture = std::make_shared<int>(28);
	std::weak_ptr<int> captured = capture;
	main.function("withCapture", [capture](int add) { return *capture + add; }, v8pp::defaults(1));
	main.function("withPointer", &f);
	v8pp::class_<native_value> binding(isolate);
	binding.ctor<>().function("read", &native_value::read)
		.property("value", &native_value::read, &native_value::write);
	main.class_("Original", binding);
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
	main.value("accessorDescriptors", v8::True(isolate));
#else
	main.value("accessorDescriptors", v8::False(isolate));
#endif
	capture.reset();

	auto saved = main.run_script(R"(
		(() => {
			const instance = new Original();
			const descriptor = accessorDescriptors ? Object.getOwnPropertyDescriptor(Original.prototype, "value") : {};
			if (accessorDescriptors && (typeof descriptor.get !== "function" || typeof descriptor.set !== "function"))
				throw new Error("missing accessors");
			return { instance, Constructor: Original, getter: descriptor.get,
				setter: descriptor.set, method: instance.read, withCapture, withPointer, plain: 45 };
		})()
	)");
	check("original callables are reachable", !saved.IsEmpty() && saved->IsObject());
	{
		v8::Context::Scope entered(auxiliary.impl());
		auxiliary.value("saved", saved);
		check_eq("retained function works before reset", run_script<int>(auxiliary, "saved.withCapture()"), 29);
		check_eq("retained getter works before reset",
			run_script<int>(auxiliary, "saved.getter ? saved.getter.call(saved.instance) : saved.instance.value"), 7);
	}

#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
	v8pp::class_<native_value>::destroy(isolate);
	v8pp::class_<native_value> rebound(isolate);
	rebound.ctor<>().property("value", &native_value::read, &native_value::write);
	main.class_("Rebound", rebound);
	auto rebound_object = main.run_script("new Rebound()");
	{
		v8::Context::Scope entered(auxiliary.impl());
		auxiliary.value("rebound", rebound_object);
		check("old class getter rejects a genuine receiver after class-only rebind",
			run_script<bool>(auxiliary, R"(
				try { saved.getter.call(rebound); false; }
				catch (e) { e.message.includes("retired") && rebound.value === 7; }
			)"));
		check_eq("unrelated callback survives class-only rebind",
			run_script<int>(auxiliary, "saved.withCapture()"), 29);
	}
#endif

	for (int generation = 1; generation <= 2; ++generation)
	{
		v8pp::cleanup(isolate);
		check("retired function releases capture while JS retains it", captured.expired());
		v8pp::class_<native_value> next_binding(isolate);
		next_binding.ctor<>().property("value", &native_value::read, &native_value::write);
		main.class_("Fresh", next_binding);
		main.function("freshFunction", [generation] { return generation; });
		auto fresh = main.run_script("new Fresh()");
		check("fresh receiver is registered", !fresh.IsEmpty() && fresh->IsObject());
		check_eq("new generation has working functions", run_script<int>(main, "freshFunction()"), generation);

		v8::Context::Scope entered(auxiliary.impl());
		auxiliary.value("fresh", fresh);
		check("callback and constructor aliases reject after reset", run_script<bool>(auxiliary, R"(
			[
				() => saved.withCapture(),
				() => saved.withPointer(3),
				() => new saved.Constructor(),
				saved.getter && (() => saved.getter.call(fresh)),
				saved.setter && (() => saved.setter.call(fresh, 99)),
				() => saved.method.call(fresh),
			].filter(Boolean).every(invoke => {
				try { invoke(); } catch (e) { return e.message.includes("retired"); }
				return false;
			})
		)"));
		check_eq("plain auxiliary data survives main reset", run_script<int>(auxiliary, "saved.plain"), 45);
		check_eq("retired setters do not change a fresh receiver", run_script<int>(auxiliary, "fresh.value"), 7);
	}
}

void test_reentrant_callback()
{
	auto context = make_test_context();
	auto* isolate = context.isolate();
	v8::HandleScope scope(isolate);
	auto scalar = std::make_shared<int>(19);
	std::weak_ptr<int> weak = scalar;
	context.function("finishScalar", [scalar, isolate]
		{
			v8pp::cleanup(isolate);
			return *scalar;
		});
	scalar.reset();

	check_eq("in-flight callable keeps its capture across reset",
		run_script<int>(context, "finishScalar()"), 19);
	check("in-flight capture releases after return", weak.expired());

	auto object = std::make_shared<native_value>();
	v8pp::shared_class<native_value> binding(isolate);
	binding.property("value", &native_value::read);
	context.value("old", v8pp::shared_class<native_value>::reference_external(isolate, object));
	context.function("finishWrapped", [object, &context, isolate]
		{
			v8pp::cleanup(isolate);
			v8pp::shared_class<native_value> replacement(isolate);
			replacement.property("value", &native_value::read);
			context.value("fresh", v8pp::shared_class<native_value>::reference_external(isolate, object));
			return object;
		});

	check("retired in-flight callable cannot return a new generation wrapper", run_script<bool>(context, R"(
		(() => {
			try { finishWrapped(); } catch (e) {
				return e.message.includes("retired") && fresh.value === 7;
			}
			return false;
		})()
	)"));
}


void test_retained_iterator()
{
	struct sequence
	{
		std::vector<int> values;
		auto begin() const { return values.begin(); }
		auto end() const { return values.end(); }
	};
	sequence first{ { 4, 5 } };
	sequence second{ { 6 } };
	auto context = make_test_context();
	auto* isolate = context.isolate();
	v8::HandleScope scope(isolate);
	v8pp::class_<sequence> binding(isolate);
	binding.iterable(&sequence::begin, &sequence::end);
	context.value("first", v8pp::class_<sequence>::reference_external(isolate, &first));
	context.value("second", v8pp::class_<sequence>::reference_external(isolate, &second));

	check_eq("extracted iterator next initially uses the original registration", run_script<int>(context, R"(
		var delegate = Object.create(first);
		var next = delegate[Symbol.iterator]().next;
		next().value
	)"), 4);
	v8pp::class_<sequence>::unreference_external(isolate, &first);
	context.value("freshFirst", v8pp::class_<sequence>::reference_external(isolate, &first));
	check("retired iterator stays retired across delegate retargeting and native address reuse",
		run_script<bool>(context, R"(
			Object.setPrototypeOf(delegate, second);
			try { next(); false; } catch (e) { e.message.includes("retired"); }
		)"));
	check_eq("a fresh iterator works after original registration retirement",
		run_script<int>(context, "freshFirst[Symbol.iterator]().next().value"), 4);
}

void test_borrowed_lifetime_handoff()
{
	auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
		v8::ArrayBuffer::Allocator::NewDefaultAllocator());
	auto* isolate = v8pp::context::create_isolate(allocator.get());
	auto image = std::make_shared<int>(1);
	std::weak_ptr<int> weak_image = image;
	std::vector<std::shared_ptr<void>> resources;
	{
		v8::Isolate::Scope entered(isolate);
		v8::HandleScope scope(isolate);
		v8::Context::Scope host(v8::Context::New(isolate));
		{
			v8pp::context borrower(isolate);
			v8pp::detail::external_data::retain_resource(isolate, std::move(image));
		}
		check("last borrowed context keeps the plugin resource rooted", !weak_image.expired());
		resources = v8pp::dispose_bindings(isolate);
		check_eq("borrowed host receives the retained plugin resource", resources.size(), size_t{1});
		check("final binding cleanup leaves the resource live", !weak_image.expired());
	}
	isolate->Dispose();
	check("borrowed host retains plugin resource through physical disposal", !weak_image.expired());
	resources.clear();
	check("borrowed host releases the resource after physical disposal", weak_image.expired());
}

void test_throwing_overloaded_factory()
{
	struct constructed
	{
		int& destructions;
		~constructed() { ++destructions; }
	};
	int destructions = 0;
	int fallback_calls = 0;
	auto context = make_test_context();
	auto* isolate = context.isolate();
	v8::HandleScope scope(isolate);
	v8pp::class_<constructed> binding(isolate);
	binding.ctor([&](v8::Isolate* current, v8::Local<v8::Value> error)
		{
			current->ThrowException(error);
			return new constructed{ destructions };
		}, [&]()
		{
			++fallback_calls;
			return new constructed{ destructions };
		});
	context.class_("ThrowingFactory", binding);
	check("overloaded factory preserves the original JS exception", run_script<bool>(context, R"(
		const original = {};
		try { new ThrowingFactory(original); false; } catch (e) { e === original; }
	)"));
	check_eq("raw result from throwing overloaded factory is destroyed once", destructions, 1);
	check_eq("throwing overloaded factory does not fall back", fallback_calls, 0);
}

#ifdef V8PP_TEST_CONSOLE_PLUGIN

#if defined(_WIN32) || defined(RTLD_NOLOAD)
bool console_image_loaded(std::filesystem::path const& plugin)
{
#ifdef _WIN32
	return GetModuleHandleW(plugin.c_str()) != nullptr;
#else
	auto* handle = dlopen(plugin.c_str(), RTLD_NOW | RTLD_NOLOAD);
	if (handle) dlclose(handle);
	return handle != nullptr;
#endif
}
#endif

void test_retained_plugin_callback()
{
	auto const plugin = std::filesystem::path(V8PP_TEST_CONSOLE_PLUGIN).make_preferred();
#if defined(_WIN32) || defined(RTLD_NOLOAD)
	check("console plugin is initially unloaded", !console_image_loaded(plugin));
#endif
	{
		auto owner = make_test_context();
		auto* isolate = owner.isolate();
		v8::HandleScope scope(isolate);
		v8pp::context auxiliary(isolate, nullptr, false, false);
		{
			v8pp::context loader(isolate, nullptr, true, false);
			loader.set_lib_path(plugin.parent_path().string());
			v8::Local<v8::Value> saved;
			{
				v8::Context::Scope entered(loader.impl());
				saved = loader.run_script("require('console').log");
				check("console plugin exports a callable", !saved.IsEmpty() && saved->IsFunction());
			}
			{
				v8::Context::Scope entered(auxiliary.impl());
				auxiliary.value("savedPlugin", saved);
			}
		}
#if defined(_WIN32) || defined(RTLD_NOLOAD)
		check("plugin image is rooted past loading context departure", console_image_loaded(plugin));
#endif
		{
			v8::Context::Scope entered(auxiliary.impl());
			check("retained console plugin still invokes", run_script<bool>(auxiliary, "savedPlugin() === undefined"));
		}
		v8pp::cleanup(isolate);
#if defined(_WIN32) || defined(RTLD_NOLOAD)
		check("plugin image remains loaded while a retired trampoline is callable", console_image_loaded(plugin));
#endif
		{
			v8::Context::Scope entered(auxiliary.impl());
			check("retired console plugin call makes a JS exception", run_script<bool>(auxiliary, R"(
				try { savedPlugin(); false; } catch (e) { e.message.includes("retired"); }
			)"));
		}
	}
#if defined(_WIN32) || defined(RTLD_NOLOAD)
	check("console plugin unloads after isolate disposal", !console_image_loaded(plugin));
#endif
}

#endif // V8PP_TEST_CONSOLE_PLUGIN

} // namespace

void test_function()
{
	auto context = make_test_context();
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

	test_retained_bindings();
	test_reentrant_callback();
	test_retained_iterator();
	test_borrowed_lifetime_handoff();
	test_throwing_overloaded_factory();
#ifdef V8PP_TEST_CONSOLE_PLUGIN
	test_retained_plugin_callback();
#endif
}
