#include "v8pp/context.hpp"
#include "v8pp/object.hpp"
#include "v8pp/module.hpp"
#include "v8pp/class.hpp"

#include "test.hpp"

#include <type_traits>

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <v8-cppgc.h>
#endif

static_assert(std::is_move_constructible_v<v8pp::context>);
static_assert(std::is_move_assignable_v<v8pp::context>);
static_assert(!std::is_copy_assignable_v<v8pp::context>);
static_assert(!std::is_copy_constructible_v<v8pp::context>);

#if V8PP_ENABLE_MANAGED_WRAPPERS
namespace {

class counting_allocator : public v8::ArrayBuffer::Allocator
{
public:
	explicit counting_allocator(int& destroyed)
		: destroyed_(destroyed)
	{
	}

	~counting_allocator() override { ++destroyed_; }

	void* Allocate(size_t length) override { return allocator_->Allocate(length); }
	void* AllocateUninitialized(size_t length) override { return allocator_->AllocateUninitialized(length); }
	void Free(void* data, size_t length) override { allocator_->Free(data, length); }

private:
	int& destroyed_;
	std::unique_ptr<v8::ArrayBuffer::Allocator> allocator_{ v8::ArrayBuffer::Allocator::NewDefaultAllocator() };
};

struct heap_probe : cppgc::GarbageCollected<heap_probe>
{
	explicit heap_probe(int& destroyed) : destroyed(destroyed) {}
	~heap_probe() { ++destroyed; }
	void Trace(cppgc::Visitor*) const {}
	int& destroyed;
};

void test_managed_isolate_ownership()
{
	int allocator_destroyed = 0;
	auto allocator = std::make_unique<counting_allocator>(allocator_destroyed);
	int first_destroyed = 0;
	int second_destroyed = 0;
	auto const platform = managed_test_platform();
	auto first_isolate = v8pp::context::create_isolate(*platform, allocator.get());
	auto* const first_heap = first_isolate->GetCppHeap();
	{
		v8::Isolate::Scope first_scope(first_isolate.get());
		v8pp::context first(first_isolate.get(), nullptr, true, true);
		v8pp::context auxiliary(first_isolate.get(), nullptr, false, false);
		check("borrowed contexts share the opted-in isolate and heap",
			first.has_managed_wrappers(first.isolate()) && auxiliary.has_managed_wrappers(auxiliary.isolate())
			&& auxiliary.isolate()->GetCppHeap() == first_heap);
		check_eq("managed isolate uses the supplied allocator", first.isolate()->GetArrayBufferAllocator(),
			static_cast<v8::ArrayBuffer::Allocator*>(allocator.get()));
		check_eq("managed isolate allocates array buffers", run_script<int>(first, "new ArrayBuffer(4096).byteLength"), 4096);
		cppgc::MakeGarbageCollected<heap_probe>(first_heap->GetAllocationHandle(), first_destroyed);
		{
			auto second = make_test_context();
			check("managed isolates use independent heaps", second.isolate()->GetCppHeap() != first_heap);
			cppgc::MakeGarbageCollected<heap_probe>(second.isolate()->GetCppHeap()->GetAllocationHandle(), second_destroyed);
		}
		check_eq("second isolate releases its managed objects once", second_destroyed, 1);
		check_eq("first isolate keeps its managed objects", first_destroyed, 0);
		check_eq("first isolate remains usable", run_script<int>(first, "'4' * 10 + 2"), 42);
	}
	check_eq("destroying borrowed contexts leaves the managed heap alive", first_destroyed, 0);
	first_isolate.reset();
	check_eq("disposing the first isolate releases its managed objects once", first_destroyed, 1);
	if (allocator_destroyed)
	{
		allocator.release();
		throw std::runtime_error("managed isolate destroyed a borrowed allocator");
	}
	allocator.reset();
	check_eq("the caller releases its allocator once", allocator_destroyed, 1);

	// A host-owned CppHeap alone does not establish v8pp tag ownership.
	auto foreign_heap = v8::CppHeap::Create(platform, v8::CppHeapCreateParams({}));
	auto foreign_allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
	auto* foreign_isolate = v8pp::context::create_isolate(foreign_allocator.get());
	{
		v8::Isolate::Scope scope(foreign_isolate);
		foreign_isolate->AttachCppHeap(foreign_heap.get());
		{
			v8pp::context foreign(foreign_isolate);
			check("external isolate with a foreign heap uses portable bindings",
				!foreign.has_managed_wrappers(foreign.isolate()));
			check_eq("portable context with a foreign heap can run scripts", run_script<int>(foreign, "6 * 7"), 42);
		}
		foreign_isolate->DetachCppHeap();
	}
	foreign_heap->Terminate();
	foreign_isolate->Dispose();
}

} // namespace
#endif

void test_context()
{
	{
		v8pp::context context;
		check("default context stays portable", !context.has_managed_wrappers(context.isolate()));

		v8::HandleScope scope(context.isolate());
		v8::Local<v8::Value> result = context.run_script("42");
		int const r = result->Int32Value(context.isolate()->GetCurrentContext()).FromJust();
		check_eq("run_script", r, 42);

		v8::TryCatch try_catch(context.isolate());
		result = context.run_script("syntax error");
		check("script with syntax error", result.IsEmpty());
		check(" has caught", try_catch.HasCaught());
	}

	{
		v8::Isolate* isolate = nullptr;
		v8::ArrayBuffer::Allocator* allocator = nullptr;
		bool add_default_global_methods = false;
		v8pp::context context(isolate, allocator, add_default_global_methods);

		v8::HandleScope scope(context.isolate());
		v8::Local<v8::Object> global = context.isolate()->GetCurrentContext()->Global();
		v8::Local<v8::Value> value;
		check("no global require", !v8pp::get_option(context.isolate(), global, "require", value));
		check("no global run", !v8pp::get_option(context.isolate(), global, "run", value));

		int const r = context.run_script("'4' + 2")->Int32Value(context.isolate()->GetCurrentContext()).FromJust();

		check_eq("run_script", r, 42);
	}

	{
		v8pp::context::options options;
		options.add_default_global_methods = false;
		options.enter_context = false;
		v8pp::context context(options);

		v8::HandleScope scope(context.isolate());
		v8::Context::Scope context_scope(context.impl());

		v8::Local<v8::Object> global = context.isolate()->GetCurrentContext()->Global();
		v8::Local<v8::Value> value;
		check("no global require", !v8pp::get_option(context.isolate(), global, "require", value));
		check("no global run", !v8pp::get_option(context.isolate(), global, "run", value));

		int const r = context.run_script("'4' + 2")->Int32Value(context.isolate()->GetCurrentContext()).FromJust();

		check_eq("run_script with explicit context", r, 42);
	}

	{
		// Move constuctor allows to set up context inside function
		// also it allows to move class with v8pp::context as a member value
		auto setup_context = []()
		{
			v8pp::context::options options;
			options.add_default_global_methods = false;
			options.enter_context = false;
			v8pp::context context(options);
			return context;
		};

		v8pp::context context0 = setup_context();
		check("returned isolate", context0.isolate() != nullptr);
		{
			v8::HandleScope scope(context0.isolate());
			check("returned context", !context0.impl().IsEmpty());
		}

		v8pp::context context = std::move(context0);
		check("moved from context", context0.isolate() == nullptr && context0.impl().IsEmpty());
		check("moved isolate", context.isolate() != nullptr);

		v8::HandleScope scope(context.isolate());
		check("moved context", !context.impl().IsEmpty());
		v8::Context::Scope context_scope(context.impl());

		v8::Local<v8::Object> global = context.isolate()->GetCurrentContext()->Global();
		v8::Local<v8::Value> value;
		check("no global require", !v8pp::get_option(context.isolate(), global, "require", value));
		check("no global run", !v8pp::get_option(context.isolate(), global, "run", value));

		int const r = context.run_script("'4' + 2")->Int32Value(context.isolate()->GetCurrentContext()).FromJust();

		check_eq("run_script with externally set up context", r, 42);
	}

	{
		auto original = make_test_context();
		v8pp::context moved(std::move(original));
		v8::HandleScope scope(moved.isolate());
		check("require after move uses the live context", run_script<bool>(moved, "try { require('v8pp-missing-move-module'); false; } catch(e) { e.message.includes('could not load shared library'); }"));
		check("run after move uses the live context", run_script<bool>(moved, "try { run('v8pp-missing-move-file.js'); false; } catch(e) { e.message.includes('could not locate file'); }"));
	}

	{
		auto original = make_test_context();
		v8pp::context moved(original.isolate(), nullptr, false, false);
		moved = std::move(original);
		v8::HandleScope scope(moved.isolate());
		check("require after move assignment", run_script<bool>(moved, "try { require('v8pp-missing-move-module'); false; } catch(e) { e.message.includes('could not load shared library'); }"));
		check("run after move assignment", run_script<bool>(moved, "try { run('v8pp-missing-move-file.js'); false; } catch(e) { e.message.includes('could not locate file'); }"));
	}

	{
		struct Item
		{
			int value = 42;
		};
		auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
		v8::Isolate* isolate = v8pp::context::create_isolate(allocator.get());
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope scope(isolate);
			{
				v8pp::context first(isolate, nullptr, false, false);
				v8::Context::Scope context_scope(first.impl());
				v8pp::class_<Item> binding(isolate);
				binding.ctor<>().var("value", &Item::value);
				first.class_("Item", binding);
				{
					v8pp::context second(isolate, nullptr, false, false);
				}
				check_eq("destroying borrowed context preserves another context's bindings",
					run_script<int>(first, "new Item().value"), 42);
			}
			{
				v8pp::context replacement(isolate, nullptr, false, false);
				v8::Context::Scope context_scope(replacement.impl());
				v8pp::class_<Item> binding(isolate);
				binding.ctor<>().var("value", &Item::value);
				replacement.class_("Item", binding);
				check_eq("last borrowed context releases bindings for rebinding",
					run_script<int>(replacement, "new Item().value"), 42);
			}
		}
		isolate->Dispose();
	}

	{
		const auto init_global = [](v8::Isolate* isolate)
		{
			v8pp::module m(isolate);
			m.const_("value", 40);
			m.function("func", []()
				{ return 2; });
			return m.impl();
		};

		// Isolate and HandleScope shall exist before init_global
		auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
		v8::Isolate* isolate = v8pp::context::create_isolate(allocator.get());
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope scope(isolate);

			v8pp::context::options opt;
			opt.isolate = isolate; // use existing one
			opt.add_default_global_methods = false;
			opt.global = init_global(isolate);

			v8pp::context context(opt);
			int const r = context.run_script("value + func()")->Int32Value(context.isolate()->GetCurrentContext()).FromJust();
			check_eq("run_script with customized global", r, 42);
		}
		isolate->Dispose();
	}

	// Crash safety: require() after context destruction should throw, not crash
	{
		auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
		v8::Isolate* isolate = v8pp::context::create_isolate(allocator.get());
		{
			v8::Isolate::Scope isolate_scope(isolate);
			v8::HandleScope outer_scope(isolate);

			v8::Global<v8::Function> require_fn;
			v8::Global<v8::Context> saved_ctx;

			{
				v8pp::context ctx(isolate, nullptr, true, false);
				v8::HandleScope scope(isolate);
				v8::Context::Scope context_scope(ctx.impl());
				saved_ctx.Reset(isolate, ctx.impl());

				// Capture the require function
				v8::Local<v8::Value> require_val;
				check("get require",
					ctx.global()->Get(isolate->GetCurrentContext(),
									v8pp::to_v8(isolate, "require"))
						.ToLocal(&require_val));
				check("require is function", require_val->IsFunction());
				require_fn.Reset(isolate, require_val.As<v8::Function>());
			}
			// ctx is destroyed here, but isolate is still alive

			// Call require() from the saved context — should get an error, not crash
			{
				v8::HandleScope scope(isolate);
				v8::Local<v8::Context> local_ctx = saved_ctx.Get(isolate);
				v8::Context::Scope context_scope(local_ctx);
				v8::TryCatch try_catch(isolate);

				v8::Local<v8::Value> args[] = { v8pp::to_v8(isolate, "nonexistent") };
				v8::Local<v8::Function> fn = require_fn.Get(isolate);
				auto result = fn->Call(local_ctx, local_ctx->Global(), 1, args);

				check("require after destroy caught exception",
					try_catch.HasCaught() || result.IsEmpty());
			}

			saved_ctx.Reset();
			require_fn.Reset();
		}
		isolate->Dispose();
	}

#if V8PP_ENABLE_MANAGED_WRAPPERS
	test_managed_isolate_ownership();
#endif
}
