#pragma once

#include <type_traits>

#include <v8.h>
#include "v8pp/v8_compat.hpp"

// V8 10+ supports Fast API callbacks, but some distributions (e.g. Ubuntu libv8-dev)
// ship V8 10+ without the v8-fast-api-calls.h header. Guard on both version and header.
#if V8_MAJOR_VERSION >= 10 && __has_include(<v8-fast-api-calls.h>)
#include <v8-fast-api-calls.h>
#define V8PP_HAS_FAST_API_HEADER 1
#endif

namespace v8pp {

namespace detail {

/// Check if T is a supported Fast API return type
/// V8 10.x supports: void, bool, int32_t, uint32_t, float, double
template<typename T>
constexpr bool is_fast_return_type_v =
	std::is_void_v<T> ||
	std::is_same_v<T, bool> ||
	std::is_same_v<T, int32_t> ||
	std::is_same_v<T, uint32_t> ||
	std::is_same_v<T, float> ||
	std::is_same_v<T, double>;

/// Check if T is a supported Fast API argument type
/// V8 10.x supports: bool, int32_t, uint32_t, int64_t, uint64_t, float, double
template<typename T>
constexpr bool is_fast_arg_type_v =
	std::is_same_v<T, bool> ||
	std::is_same_v<T, int32_t> ||
	std::is_same_v<T, uint32_t> ||
	std::is_same_v<T, int64_t> ||
	std::is_same_v<T, uint64_t> ||
	std::is_same_v<T, float> ||
	std::is_same_v<T, double>;

/// Check if a function signature is Fast API compatible
template<typename F>
struct is_fast_api_compatible : std::false_type
{
};

template<typename R, typename... Args>
struct is_fast_api_compatible<R (*)(Args...)>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)>
{
};

template<typename R, typename C, typename... Args>
struct is_fast_api_compatible<R (C::*)(Args...)>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)>
{
};

template<typename R, typename C, typename... Args>
struct is_fast_api_compatible<R (C::*)(Args...) const>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)>
{
};

} // namespace detail

/// Tag to detect fast_function types
template<typename T>
struct is_fast_function : std::false_type
{
};

/// Compile-time Fast API function wrapper.
/// Wraps a function pointer as a non-type template parameter. The binding
/// uses checked dispatch so it can authenticate receivers and retire callbacks.
/// Usage: module.function("add", v8pp::fast_fn<&add>);
template<auto FuncPtr>
struct fast_function
{
	using func_type = decltype(FuncPtr);
	static constexpr func_type ptr = FuncPtr;
	static constexpr bool compatible = detail::is_fast_api_compatible<func_type>::value;
};

template<auto FuncPtr>
struct is_fast_function<fast_function<FuncPtr>> : std::true_type
{
};

/// Variable template for convenient use
template<auto FuncPtr>
inline constexpr fast_function<FuncPtr> fast_fn{};

} // namespace v8pp
