#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "v8pp/call_from_v8.hpp"
#include "v8pp/dispatch_diagnostics.hpp"
#include "v8pp/fast_api.hpp"
#include "v8pp/native_dispatch.hpp"
#include "v8pp/ptr_traits.hpp"
#include "v8pp/throw_ex.hpp"
#include "v8pp/utility.hpp"
#include "v8pp/v8_compat.hpp"

namespace v8pp::detail {

class external_data
{
	struct record;
	struct group_state
	{
		bool live = true;
		std::unordered_set<record*> records;
		v8::Global<v8::ObjectTemplate> carrier_template;
	};

	struct record
	{
		explicit record(v8::Isolate* isolate) : isolate(isolate) {}
		~record() { if (destroy) destroy(payload); }

		v8::Isolate* isolate;
		native_dispatch_state* dispatch = nullptr;
		std::shared_ptr<group_state> generation;
		std::shared_ptr<group_state> class_group;
		v8::Global<v8::Object> carrier;
		uintptr_t token = 0;
		size_t active = 0;
		void* payload = nullptr;
		void (*destroy)(void*) = nullptr;
		alignas(void*) unsigned char inline_payload[sizeof(void*)];

		bool live() const
		{
			return generation->live && (!class_group || class_group->live);
		}
	};

	using unpublished_record = std::unique_ptr<record, void (*)(record*)>;

public:
	class group
	{
	public:
		group() = default;
		~group();
		group(group const&) = delete;
		group& operator=(group const&) = delete;
		void revoke();

	private:
		friend class external_data;
		std::shared_ptr<group_state> state_ = std::make_shared<group_state>();
	};

	// Data is a private V8 object built at registration. A nulled slot is a
	// JS-owned tombstone which can survive resets without keeping native data.
	class pin
	{
	public:
		explicit pin(v8::Local<v8::Value> data)
			: record_(static_cast<record*>(aligned_pointer(data.As<v8::Object>(), 0)))
		{
			if (!record_ || !record_->live())
				throw std::runtime_error("native binding has been retired");
			++record_->active;
		}

		~pin()
		{
			if (!--record_->active && !record_->token)
				destroy_record(record_);
		}
		pin(pin const&) = delete;
		pin& operator=(pin const&) = delete;

		template<typename T>
		T& get() const { return *static_cast<T*>(record_->payload); }
		bool live() const { return record_->live(); }
		v8::Isolate* isolate() const { return record_->isolate; }

	private:
		record* record_;
	};

	class result_scope
	{
	public:
		explicit result_scope(pin const& callback)
			: previous_(std::exchange(current(), &callback)) {}
		~result_scope() { current() = previous_; }
		result_scope(result_scope const&) = delete;
		result_scope& operator=(result_scope const&) = delete;

		static void check(v8::Isolate* isolate)
		{
			auto const* callback = current();
			if (callback && callback->isolate() == isolate && !callback->live())
				throw std::runtime_error("native binding was retired while returning a wrapped value");
		}

	private:
		static pin const*& current();
		pin const* previous_;
	};

	template<typename T>
	static v8::Local<v8::Value> set(v8::Isolate* isolate, T&& value, group* owner = nullptr)
	{
		using value_type = std::decay_t<T>;
		auto entry = allocate_record(isolate);
		if constexpr (sizeof(value_type) <= sizeof(void*) && alignof(value_type) <= alignof(void*)
			&& std::is_trivially_copyable_v<value_type>)
		{
			entry->payload = std::construct_at(reinterpret_cast<value_type*>(entry->inline_payload),
				std::forward<T>(value));
		}
		else
		{
			entry->payload = new value_type(std::forward<T>(value));
			entry->destroy = [](void* payload) { delete static_cast<value_type*>(payload); };
		}
		return publish(std::move(entry), owner);
	}

	static void revoke_all(v8::Isolate* isolate);
	static std::vector<std::shared_ptr<void>> take_resources(v8::Isolate* isolate);
	static void add_owner(v8::Isolate* isolate);
	static bool remove_owner(v8::Isolate* isolate);
	static void retain_resource(v8::Isolate* isolate, std::shared_ptr<void> resource);

private:
	struct registry;
	static registry& instance();
	static unpublished_record allocate_record(v8::Isolate* isolate);
	static v8::Local<v8::Value> publish(unpublished_record entry, group* owner);
	static void revoke(group_state& state);
	static void clear(record* entry);
	static void destroy_record(record* entry);
	static record* find(v8::Isolate* isolate, uintptr_t token);
	static void weak_clear(v8::WeakCallbackInfo<void> const& info);
	static void weak_dispose(v8::WeakCallbackInfo<void> const& info);
};

/// Bundles a callable F with its default parameter values
template<typename F, typename Defaults>
struct function_with_defaults
{
	F func;
	Defaults defs;
};

template<typename Traits, typename F, typename Invoke>
void invoke_function(v8::FunctionCallbackInfo<v8::Value> const& args, v8::TryCatch& caught,
	external_data::pin const& callback, Invoke&& invoke)
{
	using traits = function_traits<F>;
	using return_type = typename traits::return_type;
	auto invoke_and_set = [&](auto&... obj)
	{
		if constexpr (std::same_as<return_type, void>)
		{
			invoke(obj...);
		}
		else
		{
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			decltype(auto) result = invoke(obj...);
			if (!caught.HasCaught())
			{
				external_data::result_scope conversion(callback);
				args.GetReturnValue().Set(converter::to_v8(args.GetIsolate(), std::forward<decltype(result)>(result)));
			}
		}
	};

	if constexpr (std::is_member_function_pointer_v<F>)
	{
		using class_type = std::decay_t<typename traits::class_type>;
		auto obj = class_<class_type, Traits>::unwrap_object(args.GetIsolate(), args.This());
		if (!obj) throw std::runtime_error("method called on null instance");
		count_dispatch(&dispatch_counts::checked_member_entries);
		invoke_and_set(*obj);
	}
	else
	{
		invoke_and_set();
	}
}

template<typename Traits, typename F>
void forward_function(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	static_assert(is_callable<F>::value || std::is_member_function_pointer_v<F>, "required callable F");

	v8::Isolate* isolate = args.GetIsolate();
	v8::HandleScope scope(isolate);
	v8::TryCatch try_catch(isolate);
	try
	{
		native_dispatch_scope dispatch(isolate);
		external_data::pin callback(args.Data());
		auto& func = callback.get<F>();
		invoke_function<Traits, F>(args, try_catch, callback, [&](auto&... obj) -> decltype(auto)
			{ return call_from_v8<Traits>(std::move(func), args, obj...); });
	}
	catch (std::exception const& ex)
	{
		if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
	}
	if (try_catch.HasCaught()) try_catch.ReThrow();
}

/// V8 callback adapter for functions with default parameter values
template<typename Traits, typename F, typename Defaults>
void forward_function_with_defaults(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	static_assert(is_callable<F>::value || std::is_member_function_pointer_v<F>, "required callable F");

	v8::Isolate* isolate = args.GetIsolate();
	v8::HandleScope scope(isolate);
	v8::TryCatch try_catch(isolate);
	try
	{
		native_dispatch_scope dispatch(isolate);
		external_data::pin callback(args.Data());
		auto& bundle = callback.get<function_with_defaults<F, Defaults>>();
		// A const defaults reference selects the defaults overload of call_from_v8.
		auto const& defs = bundle.defs;
		invoke_function<Traits, F>(args, try_catch, callback, [&](auto&... obj) -> decltype(auto)
			{ return call_from_v8<Traits>(std::move(bundle.func), args, defs, obj...); });
	}
	catch (std::exception const& ex)
	{
		if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
	}
	if (try_catch.HasCaught()) try_catch.ReThrow();
}

} // namespace v8pp::detail

namespace v8pp {

/// Wrap C++ function into new V8 function template
template<typename F, typename Traits = raw_ptr_traits>
v8::Local<v8::FunctionTemplate> wrap_function_template(v8::Isolate* isolate, F&& func,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect,
	detail::external_data::group* owner = nullptr)
{
	using F_type = typename std::decay_t<F>;
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_function<Traits, F_type>,
		detail::external_data::set(isolate, std::forward<F_type>(func), owner),
		v8::Local<v8::Signature>(), 0,
		v8::ConstructorBehavior::kAllow,
		side_effect_type);
}

/// Wrap C++ function with default parameter values into new V8 function template
template<typename F, typename Traits = raw_ptr_traits, typename... Defs>
v8::Local<v8::FunctionTemplate> wrap_function_template(v8::Isolate* isolate, F&& func,
	v8pp::defaults<Defs...> defs,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect,
	detail::external_data::group* owner = nullptr)
{
	using F_type = typename std::decay_t<F>;
	using Defaults = v8pp::defaults<Defs...>;
	using Bundle = detail::function_with_defaults<F_type, Defaults>;
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_function_with_defaults<Traits, F_type, Defaults>,
		detail::external_data::set(isolate, Bundle{ std::forward<F_type>(func), std::move(defs) }, owner),
		v8::Local<v8::Signature>(), 0,
		v8::ConstructorBehavior::kAllow,
		side_effect_type);
}

/// Wrap C++ function with default parameter values into new V8 function
template<typename F, typename Traits = raw_ptr_traits, typename... Defs>
v8::Local<v8::Function> wrap_function(v8::Isolate* isolate, std::string_view name, F&& func,
	v8pp::defaults<Defs...> defs,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	using F_type = typename std::decay_t<F>;
	using Defaults = v8pp::defaults<Defs...>;
	using Bundle = detail::function_with_defaults<F_type, Defaults>;
	v8::Local<v8::Function> fn;
	if (!v8::Function::New(isolate->GetCurrentContext(),
			&detail::forward_function_with_defaults<Traits, F_type, Defaults>,
			detail::external_data::set(isolate, Bundle{ std::forward<F_type>(func), std::move(defs) }),
			0, v8::ConstructorBehavior::kAllow,
			side_effect_type)
			.ToLocal(&fn))
	{
		return {};
	}
	if (!name.empty())
	{
		fn->SetName(to_v8_name(isolate, name));
	}
	return fn;
}

/// Wrap C++ function into new V8 function
/// Set nullptr or empty string for name
/// to make the function anonymous
template<typename F, typename Traits = raw_ptr_traits>
v8::Local<v8::Function> wrap_function(v8::Isolate* isolate, std::string_view name, F&& func,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	using F_type = typename std::decay_t<F>;
	v8::Local<v8::Function> fn;
	if (!v8::Function::New(isolate->GetCurrentContext(),
			&detail::forward_function<Traits, F_type>,
			detail::external_data::set(isolate, std::forward<F_type>(func)),
			0, v8::ConstructorBehavior::kAllow,
			side_effect_type)
			.ToLocal(&fn))
	{
		return {};
	}
	if (!name.empty())
	{
		fn->SetName(to_v8_name(isolate, name));
	}
	return fn;
}

/// Route fast_function through checked dispatch until a V8 fast entry can
/// authenticate the binding, pin its lifetime and adjust a native receiver.
template<auto FuncPtr, typename Traits = raw_ptr_traits>
v8::Local<v8::FunctionTemplate> wrap_function_template(v8::Isolate* isolate,
	fast_function<FuncPtr>,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect,
	detail::external_data::group* owner = nullptr)
{
	using F_type = typename fast_function<FuncPtr>::func_type;
#ifdef V8PP_HAS_FAST_API_HEADER
	constexpr auto behavior = fast_function<FuncPtr>::compatible
		? v8::ConstructorBehavior::kThrow : v8::ConstructorBehavior::kAllow;
#else
	constexpr auto behavior = v8::ConstructorBehavior::kAllow;
#endif
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_function<Traits, F_type>,
		detail::external_data::set(isolate, FuncPtr, owner),
		v8::Local<v8::Signature>(), 0, behavior, side_effect_type);
}

/// Wrap a fast_function into a V8 function (for context global bindings)
template<auto FuncPtr, typename Traits = raw_ptr_traits>
v8::Local<v8::Function> wrap_function(v8::Isolate* isolate, std::string_view name,
	fast_function<FuncPtr>,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	auto tmpl = wrap_function_template<FuncPtr, Traits>(isolate,
		fast_function<FuncPtr>{}, side_effect_type);
	v8::Local<v8::Function> fn;
	if (!tmpl->GetFunction(isolate->GetCurrentContext()).ToLocal(&fn))
	{
		return {};
	}
	if (!name.empty())
	{
		fn->SetName(to_v8_name(isolate, name));
	}
	return fn;
}

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/function.ipp"
#endif
