#include "v8pp/class.hpp"
#include "v8pp/context.hpp"

#include "test.hpp"

#include <atomic>
#include <string>

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

void force_gc(v8::Isolate* isolate)
{
	// Force GC twice to ensure weak callbacks are processed
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
}

template<typename Traits>
void test_gc_stress_bulk()
{
	GCObj::instance_count = 0;

	v8pp::context context;
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

	v8pp::context context;
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
	held.clear();

	force_gc(isolate);

	bool constexpr use_shared_ptr = std::same_as<Traits, v8pp::shared_ptr_traits>;
	// raw_ptr: unreference doesn't delete, so objects still exist until scope ends
	// shared_ptr: unreference removes registry entry, shared_ptr copies in held are cleared
	check_eq("mixed lifespan fully cleaned",
		GCObj::instance_count.load(), use_shared_ptr ? 0 : 100);
}

template<typename Traits>
void test_gc_stress_rapid_cycles()
{
	GCObj::instance_count = 0;

	v8pp::context context;
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

	v8pp::context context;
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

} // anonymous namespace

void test_gc_stress()
{
	test_gc_stress_bulk<v8pp::raw_ptr_traits>();
	test_gc_stress_bulk<v8pp::shared_ptr_traits>();

	test_gc_stress_mixed_lifespan<v8pp::raw_ptr_traits>();
	test_gc_stress_mixed_lifespan<v8pp::shared_ptr_traits>();

	test_gc_stress_rapid_cycles<v8pp::raw_ptr_traits>();
	test_gc_stress_rapid_cycles<v8pp::shared_ptr_traits>();

	test_gc_stress_inheritance<v8pp::raw_ptr_traits>();
	test_gc_stress_inheritance<v8pp::shared_ptr_traits>();
}
