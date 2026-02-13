#include "v8pp/context.hpp"
#include "v8pp/object.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

#include <type_traits>

static_assert(std::is_move_constructible_v<v8pp::context>);
static_assert(std::is_move_assignable_v<v8pp::context>);
static_assert(!std::is_copy_assignable_v<v8pp::context>);
static_assert(!std::is_copy_constructible_v<v8pp::context>);

void test_context()
{
	{
		v8pp::context context;

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
		check("returned context", context0.isolate() != nullptr && !context0.impl().IsEmpty());

		v8pp::context context = std::move(context0);
		check("moved from context", context0.isolate() == nullptr && context0.impl().IsEmpty());
		check("moved context", context.isolate() != nullptr && !context.impl().IsEmpty());

		v8::HandleScope scope(context.isolate());
		v8::Context::Scope context_scope(context.impl());

		v8::Local<v8::Object> global = context.isolate()->GetCurrentContext()->Global();
		v8::Local<v8::Value> value;
		check("no global require", !v8pp::get_option(context.isolate(), global, "require", value));
		check("no global run", !v8pp::get_option(context.isolate(), global, "run", value));

		int const r = context.run_script("'4' + 2")->Int32Value(context.isolate()->GetCurrentContext()).FromJust();

		check_eq("run_script with externally set up context", r, 42);
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
		v8::Isolate* isolate = v8pp::context::create_isolate();
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
		v8::Isolate* isolate = v8pp::context::create_isolate();
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
}
