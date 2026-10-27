#pragma once

#include <cstring> // for memcpy
#include <cstdint>

#include <type_traits>

#include "v8pp/call_from_v8.hpp"
#include "v8pp/fast_api.hpp"
#include "v8pp/ptr_traits.hpp"
#include "v8pp/throw_ex.hpp"
#include "v8pp/utility.hpp"
#include "v8pp/v8_compat.hpp"

namespace v8pp::detail {

class external_data
{
public:
	// TODO: allow non-capturing lambdas
	template<typename T>
	static constexpr bool is_bitcast_allowed = sizeof(T) <= sizeof(void*) &&
		std::is_default_constructible_v<T> &&
		std::is_trivially_copyable_v<T> &&
		// Member pointers can be null (offset 0) -> Debug check failed: (value) != nullptr.
		!std::is_member_pointer_v<T>;
	template<typename T>
	static v8::Local<v8::Value> set(v8::Isolate* isolate, T&& value)
	{
		if constexpr (is_bitcast_allowed<T>)
		{
			void* ptr = nullptr;
			memcpy(&ptr, &value, sizeof value);
			return new_external(isolate, ptr);
		}
		else
		{
			using ExtValue = value_holder<T>;
			ExtValue* ext_value = new ExtValue(isolate, std::forward<T>(value));
			return v8::Local<v8::External>::New(isolate, ext_value->pext);
		}
	}

	template<typename T>
	static decltype(auto) get(v8::Local<v8::Value> value)
	{
		if constexpr (is_bitcast_allowed<T>)
		{
			void* ptr = external_value(value.As<v8::External>());
			T data;
			memcpy(&data, &ptr, sizeof data);
			return data;
		}
		else
		{
			using ExtValue = value_holder<T>;
			ExtValue* ext_value = static_cast<ExtValue*>(external_value(value.As<v8::External>()));
			return (ext_value->data()); // as reference
		}
	}

	static void destroy_all(v8::Isolate* isolate);
	static void add_owner(v8::Isolate* isolate);
	static bool remove_owner(v8::Isolate* isolate);
	static void retain_resource(v8::Isolate* isolate, std::shared_ptr<void> resource);

private:
	struct value_holder_base
	{
		virtual ~value_holder_base() = default;
	};
	struct registry;
	static registry& instance();
	static uintptr_t register_holder(v8::Isolate* isolate, value_holder_base* holder);
	static void unregister_holder(v8::Isolate* isolate, uintptr_t token);
	static value_holder_base* find_holder(v8::Isolate* isolate, uintptr_t token);

	template<typename T>
	struct value_holder final : value_holder_base
	{
		v8::Isolate* isolate_;
		uintptr_t token_ = 0;
		T value_;
		v8::Global<v8::External> pext;

		T& data() { return value_; }

		value_holder(v8::Isolate* isolate, T&& data)
			: isolate_(isolate)
			, value_(std::forward<T>(data))
		{
			pext.Reset(isolate, new_external(isolate, this));
			token_ = register_holder(isolate_, this);
			pext.SetWeak(reinterpret_cast<value_holder*>(token_), [](v8::WeakCallbackInfo<value_holder> const& info)
				{
					auto* holder = static_cast<value_holder*>(find_holder(info.GetIsolate(), reinterpret_cast<uintptr_t>(info.GetParameter())));
					if (holder) holder->pext.Reset();
					info.SetSecondPassCallback([](v8::WeakCallbackInfo<value_holder> const& second)
						{ delete find_holder(second.GetIsolate(), reinterpret_cast<uintptr_t>(second.GetParameter())); }); }, v8::WeakCallbackType::kParameter);
		}

		~value_holder()
		{
			pext.Reset();
			unregister_holder(isolate_, token_);
		}
	};
};

/// Bundles a callable F with its default parameter values
template<typename F, typename Defaults>
struct function_with_defaults
{
	F func;
	Defaults defs;
};

template<typename Traits, typename F, typename FTraits>
decltype(auto) invoke(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	if constexpr (std::is_member_function_pointer<F>())
	{
		using class_type = std::decay_t<typename FTraits::class_type>;
		auto obj = class_<class_type, Traits>::unwrap_object(args.GetIsolate(), args.This());
		if (!obj)
		{
			throw std::runtime_error("method called on null instance");
		}
		return (call_from_v8<Traits>(std::forward<F>(external_data::get<F>(args.Data())), args, *obj));
	}
	else
	{
		return (call_from_v8<Traits>(std::forward<F>(external_data::get<F>(args.Data())), args));
	}
}

template<typename Traits, typename F>
void forward_function(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using FTraits = function_traits<F>;

	static_assert(is_callable<F>::value || std::is_member_function_pointer_v<F>, "required callable F");

	v8::Isolate* isolate = args.GetIsolate();
	v8::HandleScope scope(isolate);
	v8::TryCatch try_catch(isolate);
	try
	{
		if constexpr (std::same_as<typename FTraits::return_type, void>)
		{
			invoke<Traits, F, FTraits>(args);
		}
		else
		{
			using return_type = typename FTraits::return_type;
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			decltype(auto) result = invoke<Traits, F, FTraits>(args);
			if (!try_catch.HasCaught()) args.GetReturnValue().Set(converter::to_v8(isolate, std::forward<decltype(result)>(result)));
		}
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
	using FTraits = function_traits<F>;
	using Bundle = function_with_defaults<F, Defaults>;

	static_assert(is_callable<F>::value || std::is_member_function_pointer_v<F>, "required callable F");

	v8::Isolate* isolate = args.GetIsolate();
	v8::HandleScope scope(isolate);
	v8::TryCatch try_catch(isolate);
	try
	{
		auto& bundle = external_data::get<Bundle>(args.Data());

		if constexpr (std::is_member_function_pointer<F>())
		{
			using class_type = std::decay_t<typename FTraits::class_type>;
			auto obj = class_<class_type, Traits>::unwrap_object(isolate, args.This());
			if (!obj)
			{
				throw std::runtime_error("method called on null instance");
			}
			if constexpr (std::same_as<typename FTraits::return_type, void>)
			{
				call_from_v8<Traits>(std::forward<F>(bundle.func), args, bundle.defs, *obj);
			}
			else
			{
				using return_type = typename FTraits::return_type;
				using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
				decltype(auto) result = call_from_v8<Traits>(std::forward<F>(bundle.func), args, bundle.defs, *obj);
				if (!try_catch.HasCaught()) args.GetReturnValue().Set(converter::to_v8(isolate, std::forward<decltype(result)>(result)));
			}
		}
		else
		{
			if constexpr (std::same_as<typename FTraits::return_type, void>)
			{
				call_from_v8<Traits>(std::forward<F>(bundle.func), args, bundle.defs);
			}
			else
			{
				using return_type = typename FTraits::return_type;
				using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
				decltype(auto) result = call_from_v8<Traits>(std::forward<F>(bundle.func), args, bundle.defs);
				if (!try_catch.HasCaught()) args.GetReturnValue().Set(converter::to_v8(isolate, std::forward<decltype(result)>(result)));
			}
		}
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
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	using F_type = typename std::decay_t<F>;
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_function<Traits, F_type>,
		detail::external_data::set(isolate, std::forward<F_type>(func)),
		v8::Local<v8::Signature>(), 0,
		v8::ConstructorBehavior::kAllow,
		side_effect_type);
}

/// Wrap C++ function with default parameter values into new V8 function template
template<typename F, typename Traits = raw_ptr_traits, typename... Defs>
v8::Local<v8::FunctionTemplate> wrap_function_template(v8::Isolate* isolate, F&& func,
	v8pp::defaults<Defs...> defs,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	using F_type = typename std::decay_t<F>;
	using Defaults = v8pp::defaults<Defs...>;
	using Bundle = detail::function_with_defaults<F_type, Defaults>;
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_function_with_defaults<Traits, F_type, Defaults>,
		detail::external_data::set(isolate, Bundle{ std::forward<F_type>(func), std::move(defs) }),
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

/// Wrap a fast_function into a V8 function template with optional Fast API callback.
/// Compatible signatures get a CFunction for the V8 JIT fast path.
/// Incompatible signatures or V8 < 10 silently fall back to slow-only.
template<auto FuncPtr, typename Traits = raw_ptr_traits>
v8::Local<v8::FunctionTemplate> wrap_function_template(v8::Isolate* isolate,
	fast_function<FuncPtr>,
	v8::SideEffectType side_effect_type = v8::SideEffectType::kHasSideEffect)
{
	using F_type = typename fast_function<FuncPtr>::func_type;
#ifdef V8PP_HAS_FAST_API_HEADER
	if constexpr (fast_function<FuncPtr>::compatible)
	{
		auto c_func = v8::CFunction::Make(&detail::fast_callback<FuncPtr>::call);
		return v8::FunctionTemplate::New(isolate,
			&detail::forward_function<Traits, F_type>,
			detail::external_data::set(isolate, FuncPtr),
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kThrow,
			side_effect_type,
			&c_func);
	}
	else
#endif
	{
		return v8::FunctionTemplate::New(isolate,
			&detail::forward_function<Traits, F_type>,
			detail::external_data::set(isolate, FuncPtr),
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kAllow,
			side_effect_type);
	}
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
