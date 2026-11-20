#include "v8pp/class.hpp"
#include "v8pp/context.hpp"

#include "test.hpp"

#include <atomic>
#include <string>

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <thread>
#endif

namespace {

struct GCObj
{
	static std::atomic<int> instance_count;
	int value;

	explicit GCObj(int v = 0)
		: value(v) { ++instance_count; }
	~GCObj() { --instance_count; }

	int get() const { return value; }
};

std::atomic<int> GCObj::instance_count = 0;

struct GCBase
{
	static std::atomic<int> base_count;
	int x;

	explicit GCBase(int v = 0)
		: x(v) { ++base_count; }
	~GCBase() { --base_count; }

	int get_x() const { return x; }
};

std::atomic<int> GCBase::base_count = 0;

struct GCDerived : GCBase
{
	static std::atomic<int> derived_count;
	int y;

	explicit GCDerived(int v = 0)
		: GCBase(v), y(v * 2) { ++derived_count; }
	~GCDerived() { --derived_count; }

	int get_y() const { return y; }
};

std::atomic<int> GCDerived::derived_count = 0;

void force_gc(v8::Isolate* isolate, bool drain = true)
{
	// Force GC twice to ensure weak callbacks are processed
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
	if (drain) v8pp::detail::drain_native_destructions(isolate);
}

template<typename Traits>
void test_gc_stress_bulk()
{
	GCObj::instance_count = 0;

	auto context = make_test_context();
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<GCObj, Traits> GCObj_class(isolate);
	GCObj_class
		.template ctor<int>()
		.function("get", &GCObj::get);

	context.class_("GCObj", GCObj_class);

	// Create 10k objects in batches via JS, all unreferenced immediately
	for (int batch = 0; batch < 100; ++batch)
	{
		v8::HandleScope batch_scope(isolate);
		run_script<int>(context,
			"for (var i = 0; i < 100; i++) { new GCObj(i); } 0");
	}

	// Force GC to reclaim all unreferenced objects
	force_gc(isolate);

	check_eq("bulk 10k GC cleanup", GCObj::instance_count.load(), 0);
}

template<typename Traits>
void test_gc_stress_mixed_lifespan()
{
	GCObj::instance_count = 0;

	auto context = make_test_context();
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<GCObj, Traits> GCObj_class(isolate);
	GCObj_class
		.template ctor<int>()
		.function("get", &GCObj::get);

	context.class_("GCObj", GCObj_class);

	// Hold 100 objects alive via reference_external
	std::vector<typename Traits::template object_pointer_type<GCObj>> held;
	for (int i = 0; i < 100; ++i)
	{
		auto obj = Traits::template create<GCObj>(i);
		v8pp::class_<GCObj, Traits>::reference_external(isolate, obj);
		held.push_back(obj);
	}

	// Create 10k more objects via JS (all unreferenced)
	for (int batch = 0; batch < 100; ++batch)
	{
		v8::HandleScope batch_scope(isolate);
		run_script<int>(context,
			"for (var i = 0; i < 100; i++) { new GCObj(i); } 0");
	}

	force_gc(isolate);

	// The 100 held objects should survive GC
	check_eq("mixed lifespan after GC", GCObj::instance_count.load(), 100);

	// Unreference all held objects
	for (auto& obj : held)
	{
		v8pp::class_<GCObj, Traits>::unreference_external(isolate, obj);
	}
	bool constexpr use_shared_ptr = std::same_as<Traits, v8pp::shared_ptr_traits>;
	if constexpr (!use_shared_ptr)
	{
		check_eq("unreference leaves borrowed raw objects alive", GCObj::instance_count.load(), 100);
		for (auto obj : held)
		{
			Traits::destroy(obj);
		}
	}
	held.clear();
	force_gc(isolate);
	check_eq("mixed lifespan fully cleaned", GCObj::instance_count.load(), 0);
}

template<typename Traits>
void test_gc_stress_rapid_cycles()
{
	GCObj::instance_count = 0;

	auto context = make_test_context();
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<GCObj, Traits> GCObj_class(isolate);
	GCObj_class
		.template ctor<int>()
		.function("get", &GCObj::get);

	context.class_("GCObj", GCObj_class);

	// Rapid create-destroy cycles: 100 cycles of 100 objects each
	for (int cycle = 0; cycle < 100; ++cycle)
	{
		v8::HandleScope cycle_scope(isolate);
		run_script<int>(context,
			"for (var i = 0; i < 100; i++) { new GCObj(i); } 0");
		force_gc(isolate);
	}

	check_eq("rapid cycles cleanup", GCObj::instance_count.load(), 0);
}

template<typename Traits>
void test_gc_stress_inheritance()
{
	GCBase::base_count = 0;
	GCDerived::derived_count = 0;

	auto context = make_test_context();
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	v8pp::class_<GCBase, Traits> base_class(isolate);
	base_class
		.template ctor<int>()
		.function("get_x", &GCBase::get_x);

	v8pp::class_<GCDerived, Traits> derived_class(isolate);
	derived_class
		.template ctor<int>()
		.template inherit<GCBase>()
		.function("get_y", &GCDerived::get_y);

	context.class_("GCBase", base_class);
	context.class_("GCDerived", derived_class);

	// Create 5k derived objects (each also constructs a base)
	for (int batch = 0; batch < 50; ++batch)
	{
		v8::HandleScope batch_scope(isolate);
		run_script<int>(context,
			"for (var i = 0; i < 100; i++) { new GCDerived(i); } 0");
	}

	force_gc(isolate);

	check_eq("inheritance stress derived cleanup", GCDerived::derived_count.load(), 0);
	check_eq("inheritance stress base cleanup", GCBase::base_count.load(), 0);
}

template<typename Traits>
void test_pending_collection_with_explicit_destruction()
{
	using Binding = v8pp::class_<GCObj, Traits>;
	using Pointer = typename Traits::template object_pointer_type<GCObj>;
	int releases = 0;
	int reentrant_value = 0;
	auto context = make_test_context();
	auto* isolate = context.isolate();
	v8::HandleScope scope(isolate);
	Binding binding(isolate, [&](v8::Isolate*, Pointer const& ptr)
		{
			++releases;
			reentrant_value = run_script<int>(context, "gcControl");
			Traits::destroy(ptr); });
	binding.template ctor<int>();
	context.class_("Collected", binding);
	run_script<int>(context, "var gcControl = 42; new Collected(7); 0");
	force_gc(isolate, false);
	check_eq("weak GC queues native destruction for a safe drain", releases, 0);
	Binding::destroy_objects(isolate);
	check_eq("explicit cleanup drains already-collected native objects once", releases, 1);
	check_eq("GC destructor can reenter JavaScript", reentrant_value, 42);
	force_gc(isolate);
	check_eq("later collection cannot destroy the object again", releases, 1);
}

#if V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION == 9 && V8_BUILD_NUMBER == 202 && V8_PATCH_LEVEL == 28
template<typename Traits>
void test_explicit_destruction_between_weak_passes()
{
	struct WeakPasses
	{
		v8::Global<v8::Object>* handle;
		bool first = false;
		bool second = false;
	};
	using Binding = v8pp::class_<GCObj, Traits>;
	using Pointer = typename Traits::template object_pointer_type<GCObj>;
	int releases = 0;
	WeakPasses passes{};
	auto context = make_test_context();
	auto* isolate = context.isolate();
	v8::HandleScope scope(isolate);
	v8::Global<v8::Object> witness;
	passes.handle = &witness;
	Binding binding(isolate, [&](v8::Isolate*, Pointer const& ptr)
		{
			++releases;
			Traits::destroy(ptr); });
	{
		v8::HandleScope wrapper_scope(isolate);
		witness.Reset(isolate, Binding::create_object(isolate, 7));
		witness.SetWeak(&passes, [](v8::WeakCallbackInfo<WeakPasses> const& first)
			{
				auto* observed = first.GetParameter();
				observed->handle->Reset();
				observed->first = true;
				first.SetSecondPassCallback([](v8::WeakCallbackInfo<WeakPasses> const& second)
					{ second.GetParameter()->second = true; }); }, v8::WeakCallbackType::kParameter);
	}

	// Ordinary allocation can collect the wrapper without forcing the weak
	// second pass. Stop at the first observed collection and do not pump tasks.
	for (int allocation = 0; allocation < 4096 && !passes.first; ++allocation)
	{
		v8::HandleScope garbage_scope(isolate);
		v8::Array::New(isolate, 4096);
	}
	check("ordinary GC reached the first weak pass", passes.first);
	check("the weak second pass is still pending at the owner boundary", !passes.second);
	check_eq("first-pass collection leaves native destruction to the owner", releases, 0);

	Binding::destroy_objects(isolate);
	check("explicit retirement ran before the weak second pass", !passes.second);
	check_eq("explicit retirement releases the pending native object once", releases, 1);
	force_gc(isolate);
	check("the queued weak second pass was processed", passes.second);
	check_eq("the late weak callback does not repeat native destruction", releases, 1);
}
#endif

#if V8PP_ENABLE_MANAGED_WRAPPERS
void test_managed_wrapper_thread_transfer()
{
	using Binding = v8pp::class_<GCObj>;
	auto* platform = managed_test_platform();
	if (!platform) return;

	int releases = 0;
	auto managed = v8pp::context::create_isolate(*platform);
	auto* isolate = managed.get();
	check("thread-transfer test uses managed wrappers", v8pp::context::has_managed_wrappers(isolate));
	std::unique_ptr<v8pp::context> context;
	std::exception_ptr worker_error;
	std::thread worker([&]
		{
		v8::Locker lock(isolate);
		v8::Isolate::Scope isolate_scope(isolate);
		v8::HandleScope scope(isolate);
		try
		{
			auto bound = std::make_unique<v8pp::context>(isolate, nullptr, false, false);
			v8::Context::Scope context_scope(bound->impl());
			Binding binding(isolate, [&](v8::Isolate*, GCObj* const& ptr)
				{
					++releases;
					delete ptr;
				});
			binding.ctor<int>().function("get", &GCObj::get);
			bound->class_("TransferObject", binding);
			check_eq("worker registers a wrapper on a transferred isolate",
				run_script<int>(*bound, "var transferObject = new TransferObject(42); transferObject.get()"), 42);
			context = std::move(bound);
		}
		catch (...)
		{
			worker_error = std::current_exception();
		} });
	worker.join();
	if (worker_error) std::rethrow_exception(worker_error);

	{
		v8::Locker lock(isolate);
		v8::Isolate::Scope isolate_scope(isolate);
		v8::HandleScope scope(isolate);
		auto bound = std::move(context);
		v8::Context::Scope context_scope(bound->impl());
		check_eq("the same wrapper dispatches after returning to the creator thread",
			run_script<int>(*bound, "transferObject.get()"), 42);
		run_script<bool>(*bound, "transferObject = null; true");
		force_gc(isolate);
		check_eq("GC retires the transferred managed wrapper once", releases, 1);
		v8pp::dispose_bindings(isolate);
		check_eq("final disposal preserves the transferred wrapper's release count", releases, 1);
	}
}
#endif

} // anonymous namespace

void test_gc_stress()
{
	{
		std::weak_ptr<int> captured;
		{
			auto context = make_test_context();
			v8::HandleScope scope(context.isolate());
			auto value = std::make_shared<int>(42);
			captured = value;
			context.function("captured", [value]
				{ return *value; });
			value.reset();
			check_eq("captured callback works before teardown", run_script<int>(context, "captured()"), 42);
		}
		check("context teardown releases native callback captures without GC", captured.expired());
	}

	{
		struct Left
		{
			int value = 11;
		};
		struct Right
		{
			int value = 22;
			int get() const { return value; }
		};
		struct Both : Left
			, Right
		{
		};
		auto check_const_property = []<typename Traits>()
		{
			auto context = make_test_context();
			auto* isolate = context.isolate();
			v8::HandleScope scope(isolate);
			v8pp::class_<Right, Traits> base(isolate);
			base.const_property("constant", &Right::get);
			v8pp::class_<Both, Traits> derived(isolate);
			derived.template inherit<Right>().template ctor<>();
			context.class_("Both", derived);
			check_eq("inherited constant property adjusts the base pointer",
				run_script<int>(context, "new Both().constant"), 22);
		};
		check_const_property.template operator()<v8pp::raw_ptr_traits>();
		check_const_property.template operator()<v8pp::shared_ptr_traits>();
	}

	test_gc_stress_bulk<v8pp::raw_ptr_traits>();
	test_gc_stress_bulk<v8pp::shared_ptr_traits>();

	test_gc_stress_mixed_lifespan<v8pp::raw_ptr_traits>();
	test_gc_stress_mixed_lifespan<v8pp::shared_ptr_traits>();

	test_gc_stress_rapid_cycles<v8pp::raw_ptr_traits>();
	test_gc_stress_rapid_cycles<v8pp::shared_ptr_traits>();

	test_gc_stress_inheritance<v8pp::raw_ptr_traits>();
	test_gc_stress_inheritance<v8pp::shared_ptr_traits>();
	test_pending_collection_with_explicit_destruction<v8pp::raw_ptr_traits>();
	test_pending_collection_with_explicit_destruction<v8pp::shared_ptr_traits>();
#if V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION == 9 && V8_BUILD_NUMBER == 202 && V8_PATCH_LEVEL == 28
	test_explicit_destruction_between_weak_passes<v8pp::raw_ptr_traits>();
	test_explicit_destruction_between_weak_passes<v8pp::shared_ptr_traits>();
#endif
#if V8PP_ENABLE_MANAGED_WRAPPERS
	test_managed_wrapper_thread_transfer();
#endif
}
