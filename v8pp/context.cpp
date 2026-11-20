#include "v8pp/context.hpp"
#include "v8pp/config.hpp"
#include "v8pp/convert.hpp"
#include "v8pp/function.hpp"
#include "v8pp/module.hpp"
#include "v8pp/class.hpp"
#include "v8pp/throw_ex.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <utility>

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <v8-cppgc.h>
#endif

#if defined(_WIN32)
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static char const path_sep = '\\';
#else
#include <dlfcn.h>
static char const path_sep = '/';
#endif

namespace v8pp {

#if V8PP_ENABLE_MANAGED_WRAPPERS
static char managed_wrapper_isolate;

bool detail::has_managed_wrappers(v8::Isolate* isolate) noexcept
{
	// An external isolate can have an attached heap with an independent tag space.
	return isolate && isolate->GetData(V8PP_MANAGED_WRAPPERS_DATA_SLOT) == &managed_wrapper_isolate;
}
#endif

static std::mutex& callback_contexts_mutex()
{
	static std::mutex mtx;
	return mtx;
}

static std::unordered_map<uint64_t, context*>& callback_contexts()
{
	static std::unordered_map<uint64_t, context*> contexts;
	return contexts;
}

struct context::dynamic_module
{
	std::shared_ptr<void> handle;
	v8::Global<v8::Value> exports;
};

void context::load_module(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	v8::Isolate* isolate = args.GetIsolate();

	v8::EscapableHandleScope scope(isolate);
	v8::Local<v8::Value> result;
	v8::TryCatch try_catch(isolate);
	try
	{
		std::string const name = from_v8<std::string>(isolate, args[0], "");
		if (name.empty())
		{
			throw std::runtime_error("load_module: require module name string argument");
		}

		context* ctx;
		{
			std::lock_guard lock(callback_contexts_mutex());
			auto it = callback_contexts().find(args.Data().As<v8::BigInt>()->Uint64Value());
			if (it == callback_contexts().end())
			{
				throw std::runtime_error("require() called on destroyed context");
			}
			ctx = it->second;
		}

		// check if module is already loaded
		const auto it = ctx->modules_.find(name);
		if (it != ctx->modules_.end())
		{
			result = v8::Local<v8::Value>::New(isolate, it->second.exports);
		}
		else
		{
			std::string filename = name;
			if (!ctx->lib_path_.empty())
			{
				filename = ctx->lib_path_ + path_sep + name;
			}
			std::string const suffix = V8PP_PLUGIN_SUFFIX;
			if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
			{
				filename += suffix;
			}

			dynamic_module module;
#if defined(WIN32)
			UINT const prev_error_mode = SetErrorMode(SEM_NOOPENFILEERRORBOX);
			module.handle = std::shared_ptr<void>(LoadLibraryA(filename.c_str()), [](void* handle)
				{ if (handle) ::FreeLibrary(static_cast<HMODULE>(handle)); });
			::SetErrorMode(prev_error_mode);
#else
			module.handle = std::shared_ptr<void>(dlopen(filename.c_str(), RTLD_LAZY), [](void* handle)
				{ if (handle) dlclose(handle); });
#endif

			if (!module.handle)
			{
				throw std::runtime_error("load_module(" + name + "): could not load shared library " + filename);
			}
#if defined(WIN32)
			void* sym = ::GetProcAddress(static_cast<HMODULE>(module.handle.get()),
				V8PP_STRINGIZE(V8PP_PLUGIN_INIT_PROC_NAME));
#else
			void* sym = dlsym(module.handle.get(), V8PP_STRINGIZE(V8PP_PLUGIN_INIT_PROC_NAME));
#endif
			if (!sym)
			{
				throw std::runtime_error("load_module(" + name + "): initialization function " V8PP_STRINGIZE(V8PP_PLUGIN_INIT_PROC_NAME) " not found in " + filename);
			}

			using module_init_proc = v8::Local<v8::Value> (*)(v8::Isolate*);
			module_init_proc init_proc = reinterpret_cast<module_init_proc>(sym);
			detail::external_data::retain_resource(isolate, module.handle);
			result = init_proc(isolate);
			if (result.IsEmpty()) throw std::runtime_error("load_module(" + name + "): initialization failed");
			module.exports.Reset(isolate, result);
			ctx->modules_.emplace(name, std::move(module));
		}
	}
	catch (std::exception const& ex)
	{
		if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
	}
	if (try_catch.HasCaught())
		try_catch.ReThrow();
	else
		args.GetReturnValue().Set(scope.Escape(result));
}

void context::run_file(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	v8::Isolate* isolate = args.GetIsolate();

	v8::EscapableHandleScope scope(isolate);
	v8::Local<v8::Value> result;
	v8::TryCatch try_catch(isolate);
	try
	{
		std::string const filename = from_v8<std::string>(isolate, args[0], "");
		if (filename.empty())
		{
			throw std::runtime_error("run_file: require filename string argument");
		}

		context* ctx;
		{
			std::lock_guard lock(callback_contexts_mutex());
			auto it = callback_contexts().find(args.Data().As<v8::BigInt>()->Uint64Value());
			if (it == callback_contexts().end())
			{
				throw std::runtime_error("run() called on destroyed context");
			}
			ctx = it->second;
		}
		result = to_v8(isolate, ctx->run_file(filename));
	}
	catch (std::exception const& ex)
	{
		if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
	}
	if (try_catch.HasCaught())
		try_catch.ReThrow();
	else
		args.GetReturnValue().Set(scope.Escape(result));
}

v8::Isolate* context::create_isolate(v8::ArrayBuffer::Allocator* allocator)
{
	std::unique_ptr<v8::ArrayBuffer::Allocator> default_allocator;
	if (!allocator)
	{
		default_allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
		allocator = default_allocator.get();
	}

	v8::Isolate::CreateParams create_params;
	create_params.array_buffer_allocator = allocator;

	v8::Isolate* isolate = v8::Isolate::New(create_params);
	if (isolate) default_allocator.release();
	return isolate;
}

context::owned_managed_isolate context::create_isolate(v8::Platform& platform,
	v8::ArrayBuffer::Allocator* allocator)
{
#if V8PP_ENABLE_MANAGED_WRAPPERS
	v8::Isolate::CreateParams create_params;
	if (allocator)
	{
		create_params.array_buffer_allocator = allocator;
	}
	else
	{
		create_params.array_buffer_allocator_shared.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
	}

	auto heap = v8::CppHeap::Create(&platform, v8::CppHeapCreateParams({}));
	owned_managed_isolate isolate(v8::Isolate::New(create_params));
	if (!isolate) throw std::runtime_error("could not create V8 isolate");
	{
		v8::Isolate::Scope scope(isolate.get());
		// The pinned V8's CreateParams ownership path terminates the heap but
		// leaves its allocation behind. Retain ownership via AttachCppHeap.
		isolate->AttachCppHeap(heap.get());
		isolate->SetData(V8PP_MANAGED_WRAPPERS_DATA_SLOT, &managed_wrapper_isolate);
		isolate.get_deleter().heap_ = heap.release();
	}
	return isolate;
#else
	(void)platform;
	(void)allocator;
	throw std::logic_error("managed V8 wrappers are disabled in this build");
#endif
}

void context::managed_isolate_deleter::operator()(v8::Isolate* isolate) const noexcept
{
#if V8PP_ENABLE_MANAGED_WRAPPERS
	std::unique_ptr<v8::CppHeap> heap(heap_);
	if (heap)
	{
		v8::Locker lock(isolate);
		v8::Isolate::Scope scope(isolate);
		isolate->DetachCppHeap();
		isolate->SetData(V8PP_MANAGED_WRAPPERS_DATA_SLOT, nullptr);
		heap->Terminate();
	}
#endif
	isolate->Dispose();
}

context::context(v8::Isolate* isolate, v8::ArrayBuffer::Allocator* allocator,
	bool add_default_global_methods, bool enter_context,
	v8::Local<v8::ObjectTemplate> global)
	: context(options{isolate, allocator, global, add_default_global_methods, enter_context})
{
}

context::context(options const& opts)
	: own_isolate_(opts.isolate == nullptr)
	, enter_context_(opts.enter_context)
	, managed_isolate_(own_isolate_ && opts.managed_wrapper_platform
		? create_isolate(*opts.managed_wrapper_platform, opts.allocator) : owned_managed_isolate{})
	, isolate_(opts.isolate ? opts.isolate : managed_isolate_ ? managed_isolate_.get() : create_isolate(opts.allocator))
{
	if (own_isolate_)
	{
		if (!isolate_) throw std::runtime_error("could not create V8 isolate");
		isolate_->Enter();
		if (!opts.allocator && !managed_isolate_)
		{
			// take ownership over the new allocator created in create_isolate()
			array_buffer_allocator_.reset(isolate_->GetArrayBufferAllocator());
		}
	}

	try
	{
		v8::HandleScope scope(isolate_);
		auto global = opts.global;
		if (global.IsEmpty())
		{
			global = v8::ObjectTemplate::New(isolate_);
		}

		if (opts.add_default_global_methods)
		{
			static std::atomic<uint64_t> next_callback_id{ 1 };
			callback_id_ = next_callback_id.fetch_add(1, std::memory_order_relaxed);
			v8::Local<v8::Value> data = v8::BigInt::NewFromUnsigned(isolate_, callback_id_);
			global->Set(isolate_, "require",
				v8::FunctionTemplate::New(isolate_, context::load_module, data));
			global->Set(isolate_, "run",
				v8::FunctionTemplate::New(isolate_, context::run_file, data));
		}

		v8::Local<v8::Context> impl = v8::Context::New(isolate_, nullptr, global);
		if (impl.IsEmpty()) throw std::runtime_error("could not create V8 context");
		if (callback_id_)
		{
			std::lock_guard lock(callback_contexts_mutex());
			callback_contexts().emplace(callback_id_, this);
		}

		detail::external_data::add_owner(isolate_);
		impl_.Reset(isolate_, impl);
		if (enter_context_)
		{
			impl->Enter();
		}
	}
	catch (...)
	{
		if (callback_id_)
		{
			std::lock_guard lock(callback_contexts_mutex());
			callback_contexts().erase(callback_id_);
		}
		if (own_isolate_)
		{
			isolate_->Exit();
			if (managed_isolate_) managed_isolate_.reset();
			else isolate_->Dispose();
		}
		throw;
	}
}

context::context(context&& src) noexcept
	: own_isolate_(std::exchange(src.own_isolate_, false))
	, enter_context_(std::exchange(src.enter_context_, false))
	, managed_isolate_(std::move(src.managed_isolate_))
	, isolate_(std::exchange(src.isolate_, nullptr))
	, impl_(std::move(src.impl_))
	, array_buffer_allocator_(std::move(src.array_buffer_allocator_))
	, callback_id_(std::exchange(src.callback_id_, 0))
	, modules_(std::move(src.modules_))
	, lib_path_(std::move(src.lib_path_))
{
	if (callback_id_)
	{
		std::lock_guard lock(callback_contexts_mutex());
		callback_contexts().at(callback_id_) = this;
	}
}

context& context::operator=(context&& src) noexcept
{
	if (&src != this)
	{
		destroy();

		own_isolate_ = std::exchange(src.own_isolate_, false);
		enter_context_ = std::exchange(src.enter_context_, false);
		managed_isolate_ = std::move(src.managed_isolate_);
		isolate_ = std::exchange(src.isolate_, nullptr);
		array_buffer_allocator_ = std::move(src.array_buffer_allocator_);
		callback_id_ = std::exchange(src.callback_id_, 0);
		impl_ = std::move(src.impl_);
		modules_ = std::move(src.modules_);
		lib_path_ = std::move(src.lib_path_);
		if (callback_id_)
		{
			std::lock_guard lock(callback_contexts_mutex());
			callback_contexts().at(callback_id_) = this;
		}
	}
	return *this;
}

context::~context()
{
	destroy();
}

void context::destroy()
{
	if (isolate_ == nullptr && impl_.IsEmpty())
	{
		// moved out state
		return;
	}

	if (callback_id_)
	{
		std::lock_guard lock(callback_contexts_mutex());
		callback_contexts().erase(callback_id_);
		callback_id_ = 0;
	}

	bool const last_owner = detail::external_data::remove_owner(isolate_);
	std::vector<std::shared_ptr<void>> plugin_images;
	std::exception_ptr cleanup_error;
	if (own_isolate_)
	{
		v8::HandleScope handles(isolate_);
		v8::Context::Scope scope(impl());
		plugin_images = dispose_bindings(isolate_, &cleanup_error);
	}
	else if (last_owner)
	{
		v8::HandleScope handles(isolate_);
		v8::Context::Scope scope(impl());
		cleanup(isolate_);
	}

	for (auto& kv : modules_)
	{
		dynamic_module& module = kv.second;
		module.exports.Reset();
	}
	modules_.clear();

	if (enter_context_)
	{
		v8::HandleScope handles(isolate_);
		to_local(isolate_, impl_)->Exit();
	}
	impl_.Reset();

	if (own_isolate_)
	{
		isolate_->Exit();
		if (managed_isolate_) managed_isolate_.reset();
		else isolate_->Dispose();
	}
	isolate_ = nullptr;
	array_buffer_allocator_.reset();
	if (cleanup_error)
	{
		try
		{
			std::rethrow_exception(cleanup_error);
		}
		catch (std::exception const& error)
		{
			std::fprintf(stderr, "Native wrapper destruction failed: %s\n", error.what());
		}
		catch (...)
		{
			std::fputs("Native wrapper destruction failed\n", stderr);
		}
	}
}

context& context::value(std::string_view name, v8::Local<v8::Value> value)
{
	v8::HandleScope scope(isolate_);
	global()->Set(isolate_->GetCurrentContext(), to_v8(isolate_, name), value).FromJust();
	return *this;
}

context& context::module(std::string_view name, v8pp::module& m)
{
	return value(name, m.new_instance());
}

v8::Local<v8::Value> context::run_file(std::string const& filename)
{
	std::ifstream stream(filename.c_str());
	if (!stream)
	{
		throw std::runtime_error("could not locate file " + filename);
	}

	std::istreambuf_iterator<char> begin(stream), end;
	return run_script(std::string(begin, end), filename);
}

v8::Local<v8::Value> context::run_script(std::string_view source, std::string_view filename)
{
	v8::EscapableHandleScope scope(isolate_);
	v8::Local<v8::Context> context = isolate_->GetCurrentContext();
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 2)
	v8::ScriptOrigin origin(to_v8(isolate_, filename));
#elif V8_MAJOR_VERSION > 9 || (V8_MAJOR_VERSION == 9 && V8_MINOR_VERSION >= 7)
	v8::ScriptOrigin origin(isolate_, to_v8(isolate_, filename));
#else
	v8::ScriptOrigin origin(to_v8(isolate_, filename));
#endif
	v8::Local<v8::Script> script;
	bool const is_valid = v8::Script::Compile(context,
		to_v8(isolate_, source), &origin)
							  .ToLocal(&script);
	(void)is_valid;

	v8::Local<v8::Value> result;
	if (!script.IsEmpty())
	{
		bool const is_successful = script->Run(context).ToLocal(&result);
		(void)is_successful;
	}
	return scope.Escape(result);
}

} // namespace v8pp
