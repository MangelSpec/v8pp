#pragma once

#include <type_traits>

#include <v8.h>

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
struct is_fast_api_compatible : std::false_type {};

template<typename R, typename... Args>
struct is_fast_api_compatible<R(*)(Args...)>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)> {};

template<typename R, typename C, typename... Args>
struct is_fast_api_compatible<R(C::*)(Args...)>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)> {};

template<typename R, typename C, typename... Args>
struct is_fast_api_compatible<R(C::*)(Args...) const>
	: std::bool_constant<is_fast_return_type_v<R> && (is_fast_arg_type_v<Args> && ...)> {};

#ifdef V8PP_HAS_FAST_API_HEADER

/// Fast callback wrapper — generates a static function with the V8 fast API signature.
/// Uses NTTP to bake the function pointer at compile time so V8 can call it directly.
template<auto FuncPtr>
struct fast_callback;

/// Free function: R(*)(Args...)
template<typename R, typename... Args, R(*FuncPtr)(Args...)>
struct fast_callback<FuncPtr>
{
	static R call(v8::Local<v8::Object> /*receiver*/, Args... args,
		v8::FastApiCallbackOptions& /*options*/)
	{
		return FuncPtr(args...);
	}
};

/// Member function: R(C::*)(Args...) — extracts C++ object from receiver internal field 0
template<typename R, typename C, typename... Args, R(C::*MemPtr)(Args...)>
struct fast_callback<MemPtr>
{
	static R call(v8::Local<v8::Object> receiver, Args... args,
		v8::FastApiCallbackOptions& options)
	{
		void* ptr = receiver->GetAlignedPointerFromInternalField(0);
		if (!ptr)
		{
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
			options.isolate->ThrowError("Invalid receiver: null C++ object");
#else
			options.fallback = true;
#endif
			if constexpr (std::is_void_v<R>) return;
			else return R{};
		}
		return (static_cast<C*>(ptr)->*MemPtr)(args...);
	}
};

/// Const member function: R(C::*)(Args...) const
template<typename R, typename C, typename... Args, R(C::*MemPtr)(Args...) const>
struct fast_callback<MemPtr>
{
	static R call(v8::Local<v8::Object> receiver, Args... args,
		v8::FastApiCallbackOptions& options)
	{
		void* ptr = receiver->GetAlignedPointerFromInternalField(0);
		if (!ptr)
		{
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
			options.isolate->ThrowError("Invalid receiver: null C++ object");
#else
			options.fallback = true;
#endif
			if constexpr (std::is_void_v<R>) return;
			else return R{};
		}
		return (static_cast<C const*>(ptr)->*MemPtr)(args...);
	}
};

#endif // V8PP_HAS_FAST_API_HEADER

} // namespace detail

/// Tag to detect fast_function types
template<typename T>
struct is_fast_function : std::false_type {};

/// Compile-time Fast API function wrapper.
/// Wraps a function pointer as a non-type template parameter to enable V8 Fast API callbacks.
/// Compatible signatures get both a fast callback (called directly by V8's JIT) and
/// the standard slow callback. Incompatible signatures silently fall back to slow-only.
/// Usage: module.function("add", v8pp::fast_fn<&add>);
template<auto FuncPtr>
struct fast_function
{
	using func_type = decltype(FuncPtr);
	static constexpr func_type ptr = FuncPtr;
	static constexpr bool compatible = detail::is_fast_api_compatible<func_type>::value;
};

template<auto FuncPtr>
struct is_fast_function<fast_function<FuncPtr>> : std::true_type {};

/// Variable template for convenient use
template<auto FuncPtr>
inline constexpr fast_function<FuncPtr> fast_fn{};

} // namespace v8pp
