#pragma once

#include <functional>
#include <utility>

#include <v8.h>

#include "v8pp/convert.hpp"
#include "v8pp/utility.hpp"

namespace v8pp {

/// Tag type holding default parameter values for trailing arguments.
/// Defaults fill from the right side of the parameter list, like C++ defaults.
/// Usage: v8pp::defaults(1.0f, "black") for the last 2 parameters.
template<typename... Defs>
struct defaults
{
	std::tuple<Defs...> values;
	explicit constexpr defaults(Defs... args)
		: values(std::move(args)...) {}
};

template<typename... Defs>
defaults(Defs...) -> defaults<Defs...>;

/// Type trait to detect defaults<...>
template<typename T>
struct is_defaults : std::false_type
{
};

template<typename... Defs>
struct is_defaults<defaults<Defs...>> : std::true_type
{
};

} // namespace v8pp

namespace v8pp::detail {

struct argument_conversion_error : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

template<typename>
struct optional_count;

template<typename... Ts>
struct optional_count<std::tuple<Ts...>>
{
	static constexpr size_t value = (0 + ... + is_optional<Ts>::value);
};

template<typename F, size_t Offset = 0>
struct call_from_v8_traits
{
	static constexpr size_t offset = Offset;
	static constexpr bool is_mem_fun = std::is_member_function_pointer_v<F>;
	using arguments = typename function_traits<F>::arguments;
	static constexpr size_t optional_arg_count = optional_count<arguments>::value;

	static constexpr size_t arg_count = std::tuple_size_v<arguments> - is_mem_fun - offset;

	template<size_t Index, bool>
	struct tuple_element
	{
		using type = typename std::tuple_element<Index, arguments>::type;
	};

	template<size_t Index>
	struct tuple_element<Index, false>
	{
		using type = void;
	};

	template<size_t Index>
	using arg_type = typename tuple_element < Index + is_mem_fun,
		  Index<(arg_count + offset)>::type;

	template<size_t Count, size_t... Indices>
	static constexpr size_t required_arg_count_impl(std::index_sequence<Indices...>)
	{
		size_t result = 0;
		((!is_optional<std::remove_cvref_t<arg_type<Indices + offset>>>::value ? result = Indices + 1 : result), ...);
		return result;
	}

	template<size_t Count = arg_count>
	static constexpr size_t required_arg_count = required_arg_count_impl<Count>(std::make_index_sequence<Count>{});

	template<typename Arg, typename Traits,
		typename T = std::remove_reference_t<Arg>,
		typename U = std::remove_pointer_t<T>>
	using arg_converter = typename std::conditional_t<
		is_wrapped_class<std::remove_cv_t<U>>::value,
		std::conditional_t<std::is_pointer_v<T>,
			typename Traits::template convert_ptr<U>,
			typename Traits::template convert_ref<U>>,
		convert<std::remove_cv_t<T>>>;

	template<size_t Index, typename Traits>
	static decltype(auto) arg_from_v8(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		// might be reference
		return (arg_converter<arg_type<Index>, Traits>::from_v8(args.GetIsolate(), args[Index - offset]));
	}
};

template<typename F, size_t Offset, typename CallTraits = call_from_v8_traits<F>>
inline constexpr bool is_direct_args = CallTraits::arg_count == (Offset + 1) &&
	std::same_as<typename CallTraits::template arg_type<Offset>, v8::FunctionCallbackInfo<v8::Value> const&>;

template<typename F, size_t Offset = 0, typename CallTraits = call_from_v8_traits<F>>
inline constexpr bool is_first_arg_isolate = CallTraits::arg_count != (Offset + 0) &&
	std::same_as<typename CallTraits::template arg_type<Offset>, v8::Isolate*>;

template<typename Traits, bool MarkConversion, typename F, typename CallTraits, size_t... Indices, typename... ObjArg>
decltype(auto) call_from_v8_impl(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	CallTraits, std::index_sequence<Indices...>, ObjArg&&... obj)
{
	(void)args;
	if constexpr (MarkConversion)
	{
		auto converted = [&]
		{
			try
			{
				return std::tuple<decltype(CallTraits::template arg_from_v8<Indices + CallTraits::offset, Traits>(args))...>{
					CallTraits::template arg_from_v8<Indices + CallTraits::offset, Traits>(args)...
				};
			}
			catch (std::exception const& ex)
			{
				throw argument_conversion_error(ex.what());
			}
		}();
		return std::apply([&](auto&&... values) -> decltype(auto)
			{ return std::invoke(func, std::forward<ObjArg>(obj)..., std::forward<decltype(values)>(values)...); }, std::move(converted));
	}
	else
	{
		return (std::invoke(func, std::forward<ObjArg>(obj)...,
			CallTraits::template arg_from_v8<Indices + CallTraits::offset, Traits>(args)...));
	}
}

template<typename Traits, bool MarkConversion = false, typename F, typename... ObjArg>
requires(sizeof...(ObjArg) == 0 || (!v8pp::is_defaults<std::remove_cvref_t<ObjArg>>::value && ...))
decltype(auto) call_from_v8(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args, ObjArg&... obj)
{
	constexpr bool with_isolate = is_first_arg_isolate<F>;
	if constexpr (is_direct_args<F, with_isolate>)
	{
		if constexpr (with_isolate)
		{
			return (std::invoke(func, obj..., args.GetIsolate(), args));
		}
		else
		{
			return (std::invoke(func, obj..., args));
		}
	}
	else
	{
		using call_traits = call_from_v8_traits<F, with_isolate>;
		using indices = std::make_index_sequence<call_traits::arg_count>;

		size_t const arg_count = args.Length();
		if (arg_count > call_traits::arg_count || arg_count < call_traits::template required_arg_count<>)
		{
			throw std::runtime_error(
				"Argument count does not match function definition. Expected " +
				std::to_string(call_traits::arg_count) + " but got " +
				std::to_string(args.Length()));
		}

		if constexpr (with_isolate)
		{
			return (call_from_v8_impl<Traits, MarkConversion>(std::forward<F>(func), args,
				call_traits{}, indices{}, obj..., args.GetIsolate()));
		}
		else
		{
			return (call_from_v8_impl<Traits, MarkConversion>(std::forward<F>(func), args,
				call_traits{}, indices{}, obj...));
		}
	}
}

/// Extract a single argument: from V8 args if provided, from defaults tuple otherwise.
/// DefaultsStart is the first arg index that has a default value.
template<typename Traits, typename CallTraits, size_t Index, size_t DefaultsStart,
	typename DefaultsTuple>
decltype(auto) arg_or_default(v8::FunctionCallbackInfo<v8::Value> const& args,
	DefaultsTuple const& defaults_tuple)
{
	if constexpr (Index < DefaultsStart)
	{
		// No default available — always convert from V8
		return CallTraits::template arg_from_v8<Index + CallTraits::offset, Traits>(args);
	}
	else
	{
		// This parameter has a default value available.
		using arg_type = typename CallTraits::template arg_type<Index + CallTraits::offset>;
		using value_type = std::remove_cv_t<std::remove_reference_t<arg_type>>;

		if constexpr (is_string<value_type>::value)
		{
			// Keep the converted string alive until invocation, including views and pointers.
			using converter = convert<value_type>;
			if (static_cast<size_t>(args.Length()) <= Index || args[Index]->IsUndefined())
			{
				using string_type = typename converter::from_type;
				using view_type = std::basic_string_view<typename string_type::value_type, typename string_type::traits_type>;
				return string_type(view_type(std::get<Index - DefaultsStart>(defaults_tuple)));
			}
			return CallTraits::template arg_from_v8<Index + CallTraits::offset, Traits>(args);
		}
		else
		{
			if (static_cast<size_t>(args.Length()) <= Index || args[Index]->IsUndefined())
			{
				return static_cast<value_type>(std::get<Index - DefaultsStart>(defaults_tuple));
			}
			return static_cast<value_type>(
				CallTraits::template arg_from_v8<Index + CallTraits::offset, Traits>(args));
		}
	}
}

template<typename Traits, bool MarkConversion, typename F, typename CallTraits, typename DefaultsTuple,
	size_t DefaultsStart, size_t... Indices, typename... ObjArg>
decltype(auto) call_from_v8_defaults_impl(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	DefaultsTuple const& defaults_tuple,
	CallTraits, std::index_sequence<Indices...>, ObjArg&&... obj)
{
	(void)args;
	if constexpr (MarkConversion)
	{
		auto converted = [&]
		{
			try
			{
				return std::tuple<decltype(arg_or_default<Traits, CallTraits, Indices, DefaultsStart>(args, defaults_tuple))...>{
					arg_or_default<Traits, CallTraits, Indices, DefaultsStart>(args, defaults_tuple)...
				};
			}
			catch (std::exception const& ex)
			{
				throw argument_conversion_error(ex.what());
			}
		}();
		return std::apply([&](auto&&... values) -> decltype(auto)
			{ return std::invoke(func, std::forward<ObjArg>(obj)..., std::forward<decltype(values)>(values)...); }, std::move(converted));
	}
	else
	{
		return (std::invoke(func, std::forward<ObjArg>(obj)...,
			arg_or_default<Traits, CallTraits, Indices, DefaultsStart>(args, defaults_tuple)...));
	}
}

/// call_from_v8 with default parameter values
template<typename Traits, bool MarkConversion = false, typename F, typename... Defs, typename... ObjArg>
decltype(auto) call_from_v8(F&& func, v8::FunctionCallbackInfo<v8::Value> const& args,
	v8pp::defaults<Defs...> const& defs, ObjArg&... obj)
{
	constexpr bool with_isolate = is_first_arg_isolate<F>;

	if constexpr (is_direct_args<F, with_isolate>)
	{
		// direct FunctionCallbackInfo — defaults don't apply
		if constexpr (with_isolate)
			return (std::invoke(func, obj..., args.GetIsolate(), args));
		else
			return (std::invoke(func, obj..., args));
	}
	else
	{
		using call_traits = call_from_v8_traits<F, with_isolate>;
		using indices = std::make_index_sequence<call_traits::arg_count>;

		constexpr size_t num_defaults = sizeof...(Defs);
		static_assert(num_defaults <= call_traits::arg_count,
			"More defaults than function parameters");

		constexpr size_t defaults_start = call_traits::arg_count - num_defaults;
		constexpr size_t min_args = call_traits::template required_arg_count<defaults_start>;

		size_t const arg_count = args.Length();
		if (arg_count > call_traits::arg_count || arg_count < min_args)
		{
			throw std::runtime_error(
				"Argument count does not match function definition. Expected " +
				std::to_string(min_args) + ".." +
				std::to_string(call_traits::arg_count) + " but got " +
				std::to_string(arg_count));
		}

		if constexpr (with_isolate)
		{
			return (call_from_v8_defaults_impl<Traits, MarkConversion, F, call_traits, decltype(defs.values),
				defaults_start>(std::forward<F>(func), args,
				defs.values, call_traits{}, indices{}, obj..., args.GetIsolate()));
		}
		else
		{
			return (call_from_v8_defaults_impl<Traits, MarkConversion, F, call_traits, decltype(defs.values),
				defaults_start>(std::forward<F>(func), args,
				defs.values, call_traits{}, indices{}, obj...));
		}
	}
}

} // namespace v8pp::detail
