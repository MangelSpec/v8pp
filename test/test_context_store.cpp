#include "v8pp/context_store.hpp"
#include "v8pp/json.hpp"

#include "test.hpp"

#include <algorithm>
#include <type_traits>

static_assert(std::is_move_constructible_v<v8pp::context_store>);
static_assert(std::is_move_assignable_v<v8pp::context_store>);
static_assert(!std::is_copy_assignable_v<v8pp::context_store>);
static_assert(!std::is_copy_constructible_v<v8pp::context_store>);

void test_context_store()
{
	// Test 1: basic set/get with V8 value
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		check("store isolate", store.isolate() == isolate);
		check("store impl", !store.impl().IsEmpty());

		store.set("answer", v8pp::to_v8(isolate, 42));

		v8::Local<v8::Value> out;
		check("get existing", store.get("answer", out));
		check_eq("get value", v8pp::from_v8<int>(isolate, out), 42);

		check("get nonexistent", !store.get("missing", out));
	}

	// Test 2: typed set/get
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		store.set<int>("num", 42);
		store.set<std::string>("str", "hello");
		store.set<double>("pi", 3.14);
		store.set<bool>("flag", true);

		int num = 0;
		check("get int", store.get("num", num));
		check_eq("int value", num, 42);

		std::string str;
		check("get string", store.get("str", str));
		check_eq("string value", str, "hello");

		double pi = 0;
		check("get double", store.get("pi", pi));
		check_eq("double value", pi, 3.14);

		bool flag = false;
		check("get bool", store.get("flag", flag));
		check_eq("bool value", flag, true);
	}

	// Test 3: has / remove
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		check("has before set", !store.has("key"));

		store.set<int>("key", 1);
		check("has after set", store.has("key"));

		check("remove existing", store.remove("key"));
		check("has after remove", !store.has("key"));
		check("remove nonexistent", !store.remove("key"));
	}

	// Test 4: clear / size / keys
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		check_eq("empty size", store.size(), size_t(0));
		check_eq("empty keys", store.keys().size(), size_t(0));

		store.set<int>("a", 1);
		store.set<int>("b", 2);
		store.set<int>("c", 3);
		check_eq("size after set", store.size(), size_t(3));

		auto k = store.keys();
		check_eq("keys count", k.size(), size_t(3));
		std::sort(k.begin(), k.end());
		check_eq("key 0", k[0], "a");
		check_eq("key 1", k[1], "b");
		check_eq("key 2", k[2], "c");

		store.clear();
		check_eq("size after clear", store.size(), size_t(0));
		check("has after clear", !store.has("a"));
	}

	// Test 5: overwrite
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		store.set<int>("key", 1);
		store.set<int>("key", 2);

		int val = 0;
		check("get overwritten", store.get("key", val));
		check_eq("overwritten value", val, 2);
		check_eq("size after overwrite", store.size(), size_t(1));
	}

	// Test 6: dot-separated names
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		store.set<int>("a.b.c", 42);

		check("has a.b.c", store.has("a.b.c"));
		check("has a.b", store.has("a.b"));
		check("has a", store.has("a"));

		int val = 0;
		check("get a.b.c", store.get("a.b.c", val));
		check_eq("nested value", val, 42);
	}

	// Test 7: cross-context lifecycle (the critical test)
	{
		v8::Isolate* isolate = v8pp::context::create_isolate();
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope outer_scope(isolate);

			v8pp::context_store store(isolate);

			// Phase 1: create context, set values, save to store
			{
				v8pp::context ctx(isolate, nullptr, false, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());

				ctx.run_script("var state = 42; var config = 'hello';");
				store.save_from(ctx.impl(), {"state", "config"});
			}
			// ctx destroyed here

			// Phase 2: create new context, restore from store
			{
				v8pp::context ctx(isolate, nullptr, false, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());

				store.restore_to(ctx.impl(), {"state", "config"});

				auto state = ctx.run_script("state")->Int32Value(
					isolate->GetCurrentContext()).FromJust();
				check_eq("restored state", state, 42);

				auto config = v8pp::from_v8<std::string>(isolate, ctx.run_script("config"));
				check_eq("restored config", config, "hello");
			}
		}
		isolate->Dispose();
	}

	// Test 8: JS object survives context switch
	{
		v8::Isolate* isolate = v8pp::context::create_isolate();
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope outer_scope(isolate);

			v8pp::context_store store(isolate);

			// Save an object from ctx1
			{
				v8pp::context ctx(isolate, nullptr, false, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());

				v8::Local<v8::Value> obj = ctx.run_script("({x: 10, y: 20})");
				store.set("obj", obj);
			}

			// Retrieve in ctx2
			{
				v8pp::context ctx(isolate, nullptr, false, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());

				v8::Local<v8::Value> obj;
				check("get obj", store.get("obj", obj));
				check("obj is object", obj->IsObject());

				// Set it in the new context and verify properties
				ctx.impl()->Global()->Set(isolate->GetCurrentContext(),
					v8pp::to_v8(isolate, "obj"), obj).FromJust();
				auto x = ctx.run_script("obj.x")->Int32Value(
					isolate->GetCurrentContext()).FromJust();
				check_eq("obj.x", x, 10);

				auto y = ctx.run_script("obj.y")->Int32Value(
					isolate->GetCurrentContext()).FromJust();
				check_eq("obj.y", y, 20);
			}
		}
		isolate->Dispose();
	}

	// Test 9: bulk save_from / restore_to
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store(isolate);
		store.set<int>("a", 1);
		store.set<int>("b", 2);
		store.set<int>("c", 3);

		// save_from with nonexistent key
		auto saved = store.save_from(context.impl(), {"missing"});
		check_eq("save nonexistent", saved, size_t(0));

		// restore partial
		auto restored = store.restore_to(context.impl(), {"a", "b"});
		check_eq("restore count", restored, size_t(2));

		auto a = run_script<int>(context, "a");
		check_eq("restored a", a, 1);
		auto b = run_script<int>(context, "b");
		check_eq("restored b", b, 2);
	}

	// Test 10: JSON deep copy
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		// Create an object and store a JSON deep copy
		v8pp::context_store store(isolate);
		v8::Local<v8::Value> obj = context.run_script("({val: 100})");
		check("set_json", store.set_json("data", obj));

		// Modify the original
		context.run_script("// original is separate, store has its own copy");

		// Retrieve the deep copy
		v8::Local<v8::Value> copy;
		check("get_json", store.get_json("data", copy));
		check("copy is object", copy->IsObject());

		// Verify the copy has the original value
		context.impl()->Global()->Set(isolate->GetCurrentContext(),
			v8pp::to_v8(isolate, "copy"), copy).FromJust();
		check_eq("json copy value", run_script<int>(context, "copy.val"), 100);
	}

	// Test 11: move semantics
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::context_store store1(isolate);
		store1.set<int>("key", 42);

		v8pp::context_store store2(std::move(store1));
		check("moved-from isolate", store1.isolate() == nullptr);
		check("moved-to isolate", store2.isolate() == isolate);
		check("moved-to impl", !store2.impl().IsEmpty());

		int val = 0;
		check("get from moved-to", store2.get("key", val));
		check_eq("moved value", val, 42);
	}

	// Test 12: store outlives multiple contexts
	{
		v8::Isolate* isolate = v8pp::context::create_isolate();
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope outer_scope(isolate);

			v8pp::context_store store(isolate);
			store.set<int>("persistent", 99);

			// Create and destroy 3 contexts
			for (int i = 0; i < 3; ++i)
			{
				v8pp::context ctx(isolate, nullptr, false, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());

				// Verify store still works
				int val = 0;
				check("get persistent", store.get("persistent", val));
				check_eq("persistent value", val, 99);
			}
		}
		isolate->Dispose();
	}
}
