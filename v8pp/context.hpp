#pragma once

#include <string>
#include <map>
#include <cstdint>
#include <memory>

#include <v8.h>

#include "v8pp/convert.hpp"
#include "v8pp/function.hpp"
#include "v8pp/managed_wrappers.hpp"
#include "v8pp/overload.hpp"

namespace v8 {
class CppHeap;
}

namespace v8pp {

class module;

template<typename T, typename Traits>
class class_;

/// V8 isolate and context wrapper
class context
{
public:
	/// Create a portable isolate. The caller owns the allocator and the isolate.
	/// With a null allocator, retrieve the new allocator with GetArrayBufferAllocator().
	/// Call dispose_bindings in a live context before disposing the isolate.
	static v8::Isolate* create_isolate(v8::ArrayBuffer::Allocator* allocator = nullptr);

	struct managed_isolate_deleter
	{
		void operator()(v8::Isolate* isolate) const noexcept;

	private:
		friend class context;
		v8::CppHeap* heap_ = nullptr;
	};
	using owned_managed_isolate = std::unique_ptr<v8::Isolate, managed_isolate_deleter>;

	/// Explicitly reserve v8pp's wrapper tag and slot on a new isolate.
	/// The V8 platform owner must initialize cppgc before creation and shut it
	/// down after all managed isolates are destroyed. A supplied allocator is
	/// borrowed. The default allocator and managed heap belong to the result.
	/// Call dispose_bindings while a context is still usable, retain its result
	/// through isolate disposal, and leave all V8 scopes before destroying this owner.
	static owned_managed_isolate create_isolate(v8::Platform& platform,
		v8::ArrayBuffer::Allocator* allocator = nullptr);

	static bool has_managed_wrappers(v8::Isolate* isolate) noexcept
	{
		return detail::has_managed_wrappers(isolate);
	}

	struct options
	{
		v8::Isolate* isolate = nullptr;
		v8::ArrayBuffer::Allocator* allocator = nullptr;
		v8::Local<v8::ObjectTemplate> global = {};
		bool add_default_global_methods = true;
		bool enter_context = true;
		/// Opt in for a new isolate. A supplied isolate keeps its existing mode.
		v8::Platform* managed_wrapper_platform = nullptr;
	};

	/// Create context with optional existing v8::Isolate
	/// and v8::ArrayBuffer::Allocator,
	/// and add default global methods (`require()`, `run()`)
	/// and enter the created v8 context
	/// Bindings are shared by contexts and context stores on an isolate and are
	/// cleaned up when the last such owner is destroyed.
	/// A borrowed isolate's host calls dispose_bindings in a live context at final
	/// teardown and keeps the returned plugin handles through isolate disposal.
	explicit context(v8::Isolate* isolate = nullptr,
		v8::ArrayBuffer::Allocator* allocator = nullptr,
		bool add_default_global_methods = true,
		bool enter_context = true,
		v8::Local<v8::ObjectTemplate> global = {});

	explicit context(options const& opts);

	context(context const&) = delete;
	context& operator=(context const&) = delete;

	context(context&&) noexcept;
	context& operator=(context&&) noexcept;

	~context();

	/// V8 isolate associated with this context
	v8::Isolate* isolate() const { return isolate_; }

	/// V8 context implementation. Requires an active HandleScope on its isolate.
	v8::Local<v8::Context> impl() const { return to_local(isolate_, impl_); }

	/// Global object in this context
	v8::Local<v8::Object> global() { return impl()->Global(); }

	/// Library search path
	std::string const& lib_path() const { return lib_path_; }

	/// Set new library search path
	void set_lib_path(std::string const& lib_path) { lib_path_ = lib_path; }

	/// Run script file, returns script result
	/// or empty handle on failure, use v8::TryCatch around it to find out why.
	/// Must be invoked in a v8::HandleScope
	v8::Local<v8::Value> run_file(std::string const& filename);

	/// The same as run_file but uses string as the script source
	v8::Local<v8::Value> run_script(std::string_view source, std::string_view filename = "");

	/// Set a V8 value in the context global object with specified name
	context& value(std::string_view name, v8::Local<v8::Value> value);

	/// Set module to the context global object
	context& module(std::string_view name, v8pp::module& m);

	/// Set functions to the context global object
	template<typename Function, typename Traits = raw_ptr_traits>
	requires detail::is_callable<std::decay_t<Function>>::value
	context& function(std::string_view name, Function&& func)
	{
		return value(name, wrap_function<Function, Traits>(isolate_, name, std::forward<Function>(func)));
	}

	/// Set a Fast API function to the context global object
	template<auto FuncPtr, typename Traits = raw_ptr_traits>
	context& function(std::string_view name, fast_function<FuncPtr>)
	{
		return value(name, wrap_function<FuncPtr, Traits>(isolate_, name, fast_function<FuncPtr>{}));
	}

	/// Set functions with default parameter values to the context global object
	template<typename Function, typename... Defs, typename Traits = raw_ptr_traits>
	context& function(std::string_view name, Function&& func, v8pp::defaults<Defs...> defs)
	{
		using Fun = typename std::decay_t<Function>;
		static_assert(detail::is_callable<Fun>::value, "Function must be callable");
		return value(name, wrap_function<Function, Traits>(isolate_, name, std::forward<Function>(func), std::move(defs)));
	}

	/// Set multiple overloaded functions to the context global object
	template<typename F1, typename F2, typename... Fs, typename Traits = raw_ptr_traits>
	requires(detail::is_callable<std::decay_t<F2>>::value || std::is_member_function_pointer_v<std::decay_t<F2>> || is_overload_entry<std::decay_t<F2>>::value)
	context& function(std::string_view name, F1&& f1, F2&& f2, Fs&&... fs)
	{
		return value(name, wrap_overload<Traits>(isolate_, name, std::forward<F1>(f1), std::forward<F2>(f2), std::forward<Fs>(fs)...));
	}

	/// Set class to the context global object
	template<typename T, typename Traits>
	context& class_(std::string_view name, v8pp::class_<T, Traits>& cl)
	{
		v8::HandleScope scope(isolate_);
		cl.class_function_template()->SetClassName(v8pp::to_v8_name(isolate_, name));
		return value(name, cl.js_function_template()->GetFunction(isolate_->GetCurrentContext()).ToLocalChecked());
	}

private:
	void destroy();

	bool own_isolate_;
	bool enter_context_;
	owned_managed_isolate managed_isolate_;
	v8::Isolate* isolate_;
	v8::Global<v8::Context> impl_;
	std::unique_ptr<v8::ArrayBuffer::Allocator> array_buffer_allocator_;
	uint64_t callback_id_ = 0;

	static void load_module(v8::FunctionCallbackInfo<v8::Value> const& args);
	static void run_file(v8::FunctionCallbackInfo<v8::Value> const& args);

	struct dynamic_module;
	std::map<std::string, dynamic_module> modules_;
	std::string lib_path_;
};

} // namespace v8pp
