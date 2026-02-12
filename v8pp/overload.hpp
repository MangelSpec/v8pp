#pragma once

#include <stdexcept>
#include <string>

#include "v8pp/call_from_v8.hpp"
#include "v8pp/function.hpp"

namespace v8pp {

/// Compile-time overload selector for free functions
template<typename Sig>
constexpr auto overload(Sig* f) -> Sig* { return f; }

/// Compile-time overload selector for member function pointers
template<typename MemFn>
	requires std::is_member_function_pointer_v<MemFn>
constexpr MemFn overload(MemFn f) { return f; }

/// Tag to detect overload_entry types
template<typename T>
struct is_overload_entry : std::false_type {};

/// An overload entry bundling one function, optionally with defaults
template<typename F, typename Defaults = void>
struct overload_entry
{
	F func;
};

template<typename F, typename... Defs>
struct overload_entry<F, defaults<Defs...>>
{
	F func;
	defaults<Defs...> defs;
};

template<typename F, typename D>
struct is_overload_entry<overload_entry<F, D>> : std::true_type {};

/// Helper: wrap a function with defaults into an overload_entry
template<typename F, typename... Defs>
auto with_defaults(F&& func, defaults<Defs...> defs)
{
	using F_type = std::decay_t<F>;
	return overload_entry<F_type, defaults<Defs...>>{std::forward<F>(func), std::move(defs)};
}

} // namespace v8pp

namespace v8pp::detail {

/// Compute min/max JS arg counts for one overload entry
template<typename Entry>
struct overload_arg_range;

template<typename F>
struct overload_arg_range<overload_entry<F, void>>
{
	static constexpr size_t offset = is_first_arg_isolate<F> ? 1 : 0;
	using traits = call_from_v8_traits<F, offset>;
	static constexpr size_t max_args = traits::arg_count;
	static constexpr size_t min_args = max_args - traits::optional_arg_count;
};

template<typename F, typename... Defs>
struct overload_arg_range<overload_entry<F, defaults<Defs...>>>
{
	static constexpr size_t offset = is_first_arg_isolate<F> ? 1 : 0;
	using traits = call_from_v8_traits<F, offset>;
	static constexpr size_t max_args = traits::arg_count;
	static constexpr size_t num_defaults = sizeof...(Defs);
	static constexpr size_t min_args = max_args - num_defaults - traits::optional_arg_count;
};

/// Check if a V8 value is valid for a given C++ arg type using convert::is_valid
template<typename ArgType, typename Traits>
bool arg_type_matches(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
	using T = std::remove_cv_t<std::remove_reference_t<ArgType>>;
	using U = std::remove_pointer_t<T>;
	using converter = typename std::conditional_t<
		is_wrapped_class<std::remove_cv_t<U>>::value,
		std::conditional_t<std::is_pointer_v<T>,
			typename Traits::template convert_ptr<U>,
			typename Traits::template convert_ref<U>>,
		convert<std::remove_cv_t<T>>>;
	return converter::is_valid(isolate, value);
}

/// Check provided JS args against a function's expected types.
/// Uses compile-time index sequence for all possible args, skips unprovided ones at runtime.
template<typename F, typename Traits, size_t Offset, size_t... Indices>
bool check_arg_types(v8::Isolate* isolate, v8::FunctionCallbackInfo<v8::Value> const& args,
	size_t provided_count, std::index_sequence<Indices...>)
{
	using traits = call_from_v8_traits<F, Offset>;
	// Only validate args that were actually provided by JS
	return ((Indices >= provided_count ||
		arg_type_matches<typename traits::template arg_type<Indices + Offset>, Traits>(
			isolate, args[Indices])) && ...);
}

/// Check if JS args match a function's expected types (arity already validated)
template<typename F, typename Traits>
bool overload_types_match(v8::Isolate* isolate, v8::FunctionCallbackInfo<v8::Value> const& args,
	size_t arg_count)
{
	constexpr size_t offset = is_first_arg_isolate<F> ? 1 : 0;
	using traits = call_from_v8_traits<F, offset>;

	if (arg_count == 0)
		return true;

	return check_arg_types<F, Traits, offset>(isolate, args, arg_count,
		std::make_index_sequence<traits::arg_count>{});
}

/// Try to invoke one overload entry (no defaults). Returns true on success.
template<typename Traits, typename F>
bool try_invoke_entry(overload_entry<F, void> const& entry,
	v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using FTraits = function_traits<F>;
	v8::Isolate* isolate = args.GetIsolate();
	// Copy func — entry is const& so entry.func is const, but call_from_v8
	// needs a non-const-qualified type for function_traits to work
	F func = entry.func;

	if constexpr (std::is_member_function_pointer_v<F>)
	{
		using class_type = std::decay_t<typename FTraits::class_type>;
		auto obj = class_<class_type, Traits>::unwrap_object(isolate, args.This());
		if (!obj) return false;

		if constexpr (std::same_as<typename FTraits::return_type, void>)
		{
			call_from_v8<Traits>(std::move(func), args, *obj);
		}
		else
		{
			using return_type = typename FTraits::return_type;
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			args.GetReturnValue().Set(converter::to_v8(isolate,
				call_from_v8<Traits>(std::move(func), args, *obj)));
		}
	}
	else
	{
		if constexpr (std::same_as<typename FTraits::return_type, void>)
		{
			call_from_v8<Traits>(std::move(func), args);
		}
		else
		{
			using return_type = typename FTraits::return_type;
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			args.GetReturnValue().Set(converter::to_v8(isolate,
				call_from_v8<Traits>(std::move(func), args)));
		}
	}
	return true;
}

/// Try to invoke one overload entry (with defaults). Returns true on success.
template<typename Traits, typename F, typename... Defs>
bool try_invoke_entry(overload_entry<F, defaults<Defs...>> const& entry,
	v8::FunctionCallbackInfo<v8::Value> const& args)
{
	using FTraits = function_traits<F>;
	v8::Isolate* isolate = args.GetIsolate();
	F func = entry.func;

	if constexpr (std::is_member_function_pointer_v<F>)
	{
		using class_type = std::decay_t<typename FTraits::class_type>;
		auto obj = class_<class_type, Traits>::unwrap_object(isolate, args.This());
		if (!obj) return false;

		if constexpr (std::same_as<typename FTraits::return_type, void>)
		{
			call_from_v8<Traits>(std::move(func), args, entry.defs, *obj);
		}
		else
		{
			using return_type = typename FTraits::return_type;
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			args.GetReturnValue().Set(converter::to_v8(isolate,
				call_from_v8<Traits>(std::move(func), args, entry.defs, *obj)));
		}
	}
	else
	{
		if constexpr (std::same_as<typename FTraits::return_type, void>)
		{
			call_from_v8<Traits>(std::move(func), args, entry.defs);
		}
		else
		{
			using return_type = typename FTraits::return_type;
			using converter = typename call_from_v8_traits<F>::template arg_converter<return_type, Traits>;
			args.GetReturnValue().Set(converter::to_v8(isolate,
				call_from_v8<Traits>(std::move(func), args, entry.defs)));
		}
	}
	return true;
}

/// Get the function type from an overload_entry
template<typename Entry>
struct entry_func_type;

template<typename F, typename D>
struct entry_func_type<overload_entry<F, D>>
{
	using type = F;
};

/// Holds a set of overload entries
template<typename... Entries>
struct overload_set
{
	std::tuple<Entries...> entries;
};

/// V8 callback that dispatches to the matching overload (first-match-wins)
template<typename Traits, typename OverloadSet>
void forward_overloaded_function(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	v8::Isolate* isolate = args.GetIsolate();
	v8::HandleScope scope(isolate);

	auto& set = external_data::get<OverloadSet>(args.Data());
	size_t const arg_count = args.Length();

	bool matched = false;
	std::string errors;

	std::apply([&](auto const&... entries)
	{
		// Fold: short-circuit on first match via (matched || try_one())
		((matched || [&]
		{
			using Entry = std::decay_t<decltype(entries)>;
			using F = typename entry_func_type<Entry>::type;

			// Arity check
			constexpr size_t min = overload_arg_range<Entry>::min_args;
			constexpr size_t max = overload_arg_range<Entry>::max_args;
			if (arg_count < min || arg_count > max)
				return false;

			// Type check
			if (arg_count > 0 && !overload_types_match<F, Traits>(isolate, args, arg_count))
				return false;

			// Try invoking
			try
			{
				matched = try_invoke_entry<Traits>(entries, args);
				return matched;
			}
			catch (std::exception const& ex)
			{
				if (!errors.empty()) errors += "; ";
				errors += ex.what();
				return false;
			}
		}()), ...);
	}, set.entries);

	if (!matched)
	{
		std::string msg = "No matching overload for " + std::to_string(arg_count) + " argument(s)";
		if (!errors.empty())
		{
			msg += ". Tried: " + errors;
		}
		args.GetReturnValue().Set(throw_ex(isolate, msg));
	}
}

/// Helper: wrap a plain callable into overload_entry<F, void>
template<typename F>
auto make_overload_entry(F&& func)
{
	if constexpr (is_overload_entry<std::decay_t<F>>::value)
	{
		return std::forward<F>(func);
	}
	else
	{
		return overload_entry<std::decay_t<F>, void>{std::forward<F>(func)};
	}
}

} // namespace v8pp::detail

namespace v8pp {

/// Wrap multiple overloaded functions into a single V8 function template
template<typename Traits = raw_ptr_traits, typename... Funcs>
v8::Local<v8::FunctionTemplate> wrap_overload_template(v8::Isolate* isolate, Funcs&&... funcs)
{
	using Set = detail::overload_set<decltype(detail::make_overload_entry(std::forward<Funcs>(funcs)))...>;
	Set set{std::make_tuple(detail::make_overload_entry(std::forward<Funcs>(funcs))...)};
	return v8::FunctionTemplate::New(isolate,
		&detail::forward_overloaded_function<Traits, Set>,
		detail::external_data::set(isolate, std::move(set)),
		v8::Local<v8::Signature>(), 0,
		v8::ConstructorBehavior::kAllow,
		v8::SideEffectType::kHasSideEffect);
}

/// Wrap multiple overloaded functions into a single V8 function
template<typename Traits = raw_ptr_traits, typename... Funcs>
v8::Local<v8::Function> wrap_overload(v8::Isolate* isolate, std::string_view name, Funcs&&... funcs)
{
	using Set = detail::overload_set<decltype(detail::make_overload_entry(std::forward<Funcs>(funcs)))...>;
	Set set{std::make_tuple(detail::make_overload_entry(std::forward<Funcs>(funcs))...)};
	v8::Local<v8::Function> fn;
	if (!v8::Function::New(isolate->GetCurrentContext(),
		&detail::forward_overloaded_function<Traits, Set>,
		detail::external_data::set(isolate, std::move(set)),
		0, v8::ConstructorBehavior::kAllow,
		v8::SideEffectType::kHasSideEffect).ToLocal(&fn))
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
