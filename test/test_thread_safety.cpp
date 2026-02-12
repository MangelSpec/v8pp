#include "v8pp/class.hpp"
#include "v8pp/context.hpp"

#include "test.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ThreadObj
{
	static std::atomic<int> total_created;
	static std::atomic<int> total_destroyed;
	int value;

	explicit ThreadObj(int v = 0) : value(v) { ++total_created; }
	~ThreadObj() { ++total_destroyed; }

	int get() const { return value; }
	int add(int x) const { return value + x; }
};

std::atomic<int> ThreadObj::total_created = 0;
std::atomic<int> ThreadObj::total_destroyed = 0;

struct SharedObj
{
	std::atomic<int> access_count{0};
	int value;

	explicit SharedObj(int v = 0) : value(v) {}

	int get()
	{
		++access_count;
		return value;
	}
};

void force_gc(v8::Isolate* isolate)
{
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
	isolate->RequestGarbageCollectionForTesting(
		v8::Isolate::GarbageCollectionType::kFullGarbageCollection);
}

//
// Test: multiple threads, each with own isolate and context
//

void test_concurrent_isolates()
{
	ThreadObj::total_created = 0;
	ThreadObj::total_destroyed = 0;

	constexpr int num_threads = 4;
	constexpr int objects_per_thread = 1000;
	std::atomic<int> errors{0};

	auto worker = [&errors](int thread_id)
	{
		try
		{
			v8pp::context context;
			v8::Isolate* isolate = context.isolate();
			v8::HandleScope scope(isolate);

			v8pp::class_<ThreadObj, v8pp::raw_ptr_traits> obj_class(isolate);
			obj_class
				.ctor<int>()
				.function("get", &ThreadObj::get)
				.function("add", &ThreadObj::add);

			context.class_("ThreadObj", obj_class);

			// Create objects in batches
			for (int batch = 0; batch < objects_per_thread / 100; ++batch)
			{
				v8::HandleScope batch_scope(isolate);
				std::string script =
					"for (var i = 0; i < 100; i++) {"
					"  var o = new ThreadObj(" + std::to_string(thread_id * 1000 + batch * 100) + " + i);"
					"  o.add(1);"
					"} 0";
				run_script<int>(context, script.c_str());
			}

			force_gc(isolate);
		}
		catch (std::exception const& ex)
		{
			++errors;
			std::cerr << "Thread " << thread_id << " error: " << ex.what() << '\n';
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(num_threads);
	for (int i = 0; i < num_threads; ++i)
	{
		threads.emplace_back(worker, i);
	}

	for (auto& t : threads)
	{
		t.join();
	}

	check_eq("concurrent isolates no errors", errors.load(), 0);
	check_eq("concurrent isolates all cleaned up",
		ThreadObj::total_created.load(), ThreadObj::total_destroyed.load());
}

//
// Test: cross-isolate shared_ptr (sequential)
//
// Wrap the same shared_ptr object in two different isolates sequentially.
//

void test_cross_isolate_shared_ptr_sequential()
{
	auto shared = std::make_shared<SharedObj>(42);
	check_eq("initial use_count", shared.use_count(), 1L);

	// Isolate A
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<SharedObj, v8pp::shared_ptr_traits> obj_class(isolate);
		obj_class
			.function("get", &SharedObj::get);

		context.class_("SharedObj", obj_class);

		v8::Local<v8::Object> js_obj =
			v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::reference_external(isolate, shared);
		check("isolate A wrap", !js_obj.IsEmpty());

		// Use the object from JS
		v8pp::set_option(isolate, context.global(), "obj", js_obj);
		check_eq("isolate A get", run_script<int>(context, "obj.get()"), 42);

		v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::unreference_external(isolate, shared);
	}

	// Object should still be alive (main thread holds shared_ptr)
	check_eq("shared survives isolate A", shared->value, 42);

	// Isolate B
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<SharedObj, v8pp::shared_ptr_traits> obj_class(isolate);
		obj_class
			.function("get", &SharedObj::get);

		context.class_("SharedObj", obj_class);

		v8::Local<v8::Object> js_obj =
			v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::reference_external(isolate, shared);
		check("isolate B wrap", !js_obj.IsEmpty());

		v8pp::set_option(isolate, context.global(), "obj", js_obj);
		check_eq("isolate B get", run_script<int>(context, "obj.get()"), 42);

		v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::unreference_external(isolate, shared);
	}

	// Only main thread's copy should remain
	check_eq("shared survives both isolates", shared->value, 42);
	check_eq("final use_count", shared.use_count(), 1L);
	check_eq("total access count", shared->access_count.load(), 2);
}

//
// Test: cross-isolate shared_ptr (concurrent)
//
// Multiple threads each wrap the same shared_ptr in their own isolate.
//

void test_cross_isolate_shared_ptr_concurrent()
{
	auto shared = std::make_shared<SharedObj>(99);

	constexpr int num_threads = 4;
	std::atomic<int> errors{0};

	auto worker = [&shared, &errors](int)
	{
		try
		{
			// Each thread gets its own copy of the shared_ptr
			auto local_copy = shared;

			v8pp::context context;
			v8::Isolate* isolate = context.isolate();
			v8::HandleScope scope(isolate);

			v8pp::class_<SharedObj, v8pp::shared_ptr_traits> obj_class(isolate);
			obj_class
				.function("get", &SharedObj::get);

			context.class_("SharedObj", obj_class);

			v8::Local<v8::Object> js_obj =
				v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::reference_external(isolate, local_copy);

			v8pp::set_option(isolate, context.global(), "obj", js_obj);

			int result = run_script<int>(context, "obj.get()");
			if (result != 99)
			{
				++errors;
			}

			v8pp::class_<SharedObj, v8pp::shared_ptr_traits>::unreference_external(isolate, local_copy);
		}
		catch (std::exception const& ex)
		{
			++errors;
			std::cerr << "Thread error: " << ex.what() << '\n';
		}
	};

	std::vector<std::thread> threads;
	threads.reserve(num_threads);
	for (int i = 0; i < num_threads; ++i)
	{
		threads.emplace_back(worker, i);
	}

	for (auto& t : threads)
	{
		t.join();
	}

	check_eq("concurrent shared_ptr no errors", errors.load(), 0);
	check_eq("concurrent shared_ptr use_count", shared.use_count(), 1L);
	check("concurrent shared_ptr accessed", shared->access_count.load() >= num_threads);
}

//
// Test: isolate independence
//
// Two threads register classes with the same name but different types.
// Each should only see its own registration.
//

struct IsoTypeA
{
	int get() const { return 111; }
};

struct IsoTypeB
{
	int get() const { return 222; }
};

void test_isolate_independence()
{
	std::atomic<int> errors{0};
	std::atomic<int> result_a{0};
	std::atomic<int> result_b{0};

	auto worker_a = [&]()
	{
		try
		{
			v8pp::context context;
			v8::Isolate* isolate = context.isolate();
			v8::HandleScope scope(isolate);

			v8pp::class_<IsoTypeA, v8pp::raw_ptr_traits> cls(isolate);
			cls.ctor<>().function("get", &IsoTypeA::get);
			context.class_("MyClass", cls);

			result_a = run_script<int>(context, "var x = new MyClass(); x.get()");
		}
		catch (std::exception const&) { ++errors; }
	};

	auto worker_b = [&]()
	{
		try
		{
			v8pp::context context;
			v8::Isolate* isolate = context.isolate();
			v8::HandleScope scope(isolate);

			v8pp::class_<IsoTypeB, v8pp::raw_ptr_traits> cls(isolate);
			cls.ctor<>().function("get", &IsoTypeB::get);
			context.class_("MyClass", cls);

			result_b = run_script<int>(context, "var x = new MyClass(); x.get()");
		}
		catch (std::exception const&) { ++errors; }
	};

	std::thread ta(worker_a);
	std::thread tb(worker_b);
	ta.join();
	tb.join();

	check_eq("isolate independence no errors", errors.load(), 0);
	check_eq("isolate A sees its own type", result_a.load(), 111);
	check_eq("isolate B sees its own type", result_b.load(), 222);
}

} // anonymous namespace

void test_thread_safety()
{
	test_concurrent_isolates();
	test_cross_isolate_shared_ptr_sequential();
	test_cross_isolate_shared_ptr_concurrent();
	test_isolate_independence();
}
