#pragma once

#include <v8.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cmath>
#include <cstring>
#include <filesystem>
#ifdef WIN32
#include <stringapiset.h>
#endif
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <variant>
#include <optional>

#include "v8pp/ptr_traits.hpp"
#include "v8pp/utility.hpp"

namespace v8pp {

template<typename T, typename Traits>
class class_;

template<typename T>
struct is_wrapped_class;

// Generic convertor
/*
template<typename T, typename Enable = void>
struct convert
{
	using from_type = T;
	using to_type = v8::Local<v8::Value>;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value);

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value);
	static to_type to_v8(v8::Isolate* isolate, T const& value);
};
*/

// typed_array_trait specializations (requires V8 types)
namespace detail {
template<> struct typed_array_trait<uint8_t>  { using type = v8::Uint8Array; };
template<> struct typed_array_trait<int8_t>   { using type = v8::Int8Array; };
template<> struct typed_array_trait<uint16_t> { using type = v8::Uint16Array; };
template<> struct typed_array_trait<int16_t>  { using type = v8::Int16Array; };
template<> struct typed_array_trait<uint32_t> { using type = v8::Uint32Array; };
template<> struct typed_array_trait<int32_t>  { using type = v8::Int32Array; };
template<> struct typed_array_trait<float>    { using type = v8::Float32Array; };
template<> struct typed_array_trait<double>   { using type = v8::Float64Array; };
template<> struct typed_array_trait<int64_t>  { using type = v8::BigInt64Array; };
template<> struct typed_array_trait<uint64_t> { using type = v8::BigUint64Array; };
} // namespace detail

struct invalid_argument : std::invalid_argument
{
	invalid_argument(v8::Isolate* isolate, v8::Local<v8::Value> value, char const* expected_type);
};

struct runtime_error : std::runtime_error
{
	runtime_error(v8::Isolate* isolate, v8::Local<v8::Value> value, char const* message);
};

// converter specializations for string types
template<typename String>
	requires detail::is_string<String>::value
struct convert<String, void>
{
	using Char = typename String::value_type;
	using Traits = typename String::traits_type;

	static_assert(sizeof(Char) <= sizeof(uint16_t),
		"only UTF-8 and UTF-16 strings are supported");

	// A string that converts to Char const*
	struct convertible_string : std::basic_string<Char, Traits>
	{
		using base_class = std::basic_string<Char, Traits>;
		using base_class::base_class;
		operator Char const*() const { return this->c_str(); }
	};

	using from_type = convertible_string;
	using to_type = v8::Local<v8::String>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value))
		{
			return *std::move(result);
		}
		throw invalid_argument(isolate, value, "String");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty()) return std::nullopt;

		v8::HandleScope scope(isolate);
		v8::Local<v8::String> str;
		if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&str))
		{
			return std::nullopt;
		}

		return extract_string(isolate, str);
	}

	static to_type to_v8(v8::Isolate* isolate, std::basic_string_view<Char, Traits> value)
	{
		if constexpr (sizeof(Char) == 1)
		{
			return v8::String::NewFromUtf8(isolate,
				reinterpret_cast<char const*>(value.data()),
				v8::NewStringType::kNormal, static_cast<int>(value.size())).ToLocalChecked();
		}
		else
		{
			return v8::String::NewFromTwoByte(isolate,
				reinterpret_cast<uint16_t const*>(value.data()),
				v8::NewStringType::kNormal, static_cast<int>(value.size())).ToLocalChecked();
		}
	}

private:
	static from_type extract_string(v8::Isolate* isolate, v8::Local<v8::String> str)
	{
#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 3)
		if constexpr (sizeof(Char) == 1)
		{
			auto const len = str->Utf8LengthV2(isolate);
			from_type result(len, 0);
			result.resize(str->WriteUtf8V2(isolate, result.data(), len));
			return result;
		}
		else
		{
			auto const len = str->Length();
			from_type result(len, 0);
			str->WriteV2(isolate, 0, len, reinterpret_cast<uint16_t*>(result.data()));
			return result;
		}
#else
		if constexpr (sizeof(Char) == 1)
		{
			auto const len = str->Utf8Length(isolate);
			from_type result(len, 0);
			result.resize(str->WriteUtf8(isolate, result.data(), len, nullptr, v8::String::NO_NULL_TERMINATION | v8::String::REPLACE_INVALID_UTF8));
			return result;
		}
		else
		{
			auto const len = str->Length();
			from_type result(len, 0);
			result.resize(str->Write(isolate, reinterpret_cast<uint16_t*>(result.data()), 0, len, v8::String::NO_NULL_TERMINATION));
			return result;
		}
#endif
	}
};

// converter specializations for null-terminated strings
template<>
struct convert<char const*> : convert<std::basic_string_view<char>>
{
};

template<>
struct convert<char16_t const*> : convert<std::basic_string_view<char16_t>>
{
};

#ifdef WIN32
template<>
struct convert<wchar_t const*> : convert<std::basic_string_view<wchar_t>>
{
};
#endif

// converter specializations for primitive types
template<>
struct convert<std::monostate>
{
	using from_type = std::monostate;
	using to_type = v8::Local<v8::Primitive>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return value.IsEmpty() || value->IsUndefined();
	}

	static std::monostate from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Undefined");
		}

		return std::monostate{};
	}

	static v8::Local<v8::Primitive> to_v8(v8::Isolate* isolate, std::monostate)
	{
		return v8::Undefined(isolate);
	}
};

template<>
struct convert<std::nullopt_t>
{
	using from_type = std::nullopt_t;
	using to_type = v8::Local<v8::Primitive>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return value.IsEmpty() || value->IsNull();
	}

	static std::nullopt_t from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Null");
		}

		return std::nullopt;
	}

	static v8::Local<v8::Primitive> to_v8(v8::Isolate* isolate, std::nullopt_t)
	{
		return v8::Null(isolate);
	}
};

template<>
struct convert<bool>
{
	using from_type = bool;
	using to_type = v8::Local<v8::Boolean>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsBoolean();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Boolean");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		return value->BooleanValue(isolate);
	}

	static to_type to_v8(v8::Isolate* isolate, bool value)
	{
		return v8::Boolean::New(isolate, value);
	}
};

// convert Number <-> integer types that fit in 32 bits
template<std::integral T>
	requires (sizeof(T) <= sizeof(uint32_t))
struct convert<T, void>
{
	using from_type = T;
	using to_type = v8::Local<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Number");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		if constexpr (std::is_signed_v<T>)
		{
			return static_cast<T>(value->Int32Value(isolate->GetCurrentContext()).FromJust());
		}
		else
		{
			return static_cast<T>(value->Uint32Value(isolate->GetCurrentContext()).FromJust());
		}
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		if constexpr (std::is_signed_v<T>)
		{
			return v8::Integer::New(isolate, static_cast<int32_t>(value));
		}
		else
		{
			return v8::Integer::NewFromUnsigned(isolate, static_cast<uint32_t>(value));
		}
	}
};

// convert Number <-> integer types larger than 32 bits (int64_t, uint64_t, etc.)
// to_v8 produces Number (double) for seamless JS arithmetic. Precision loss for
// values > 2^53, but this covers all practical use cases (timestamps, counters, IDs).
// from_v8 accepts both Number and BigInt for interop.
template<std::integral T>
	requires (sizeof(T) > sizeof(uint32_t))
struct convert<T, void>
{
	using from_type = T;
	using to_type = v8::Local<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && (value->IsNumber() || value->IsBigInt());
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Number");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		if (value->IsBigInt())
		{
			// Accept BigInt from JS side for interop
			auto bigint = value.As<v8::BigInt>();
			if constexpr (std::is_signed_v<T>)
			{
				return static_cast<T>(bigint->Int64Value());
			}
			else
			{
				return static_cast<T>(bigint->Uint64Value());
			}
		}
		else
		{
			return static_cast<T>(value->IntegerValue(isolate->GetCurrentContext()).FromJust());
		}
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		return v8::Number::New(isolate, static_cast<double>(value));
	}
};

template<typename T>
	requires std::is_enum_v<T>
struct convert<T, void>
{
	using underlying_type = typename std::underlying_type<T>::type;

	using from_type = T;
	using to_type = typename convert<underlying_type>::to_type;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return convert<underlying_type>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return static_cast<T>(convert<underlying_type>::from_v8(isolate, value));
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		auto result = convert<underlying_type>::try_from_v8(isolate, value);
		return result ? std::optional<from_type>{static_cast<T>(*result)} : std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		return convert<underlying_type>::to_v8(isolate,
			static_cast<underlying_type>(value));
	}
};

template<std::floating_point T>
struct convert<T, void>
{
	using from_type = T;
	using to_type = v8::Local<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Number");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		return static_cast<T>(value->NumberValue(isolate->GetCurrentContext()).FromJust());
	}

	static to_type to_v8(v8::Isolate* isolate, T value)
	{
		return v8::Number::New(isolate, value);
	}
};

// convert std::optional <-> value or undefined
template<typename T>
struct convert<std::optional<T>>
{
	using from_type = std::optional<T>;
	using to_type = v8::Local<v8::Value>;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return value.IsEmpty() || value->IsNullOrUndefined() || convert<T>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
	    if (value.IsEmpty() || value->IsNullOrUndefined())
		{
			return std::nullopt;
		}
		else if (convert<T>::is_valid(isolate, value))
		{
			return convert<T>::from_v8(isolate, value);
		}
		else
		{
		    throw invalid_argument(isolate, value, "Optional");
		}
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty() || value->IsNullOrUndefined())
		{
			return from_type{std::nullopt};
		}
		if (convert<T>::is_valid(isolate, value))
		{
			return from_type{convert<T>::from_v8(isolate, value)};
		}
		return std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, std::optional<T> const& value)
	{
		if (value)
		{
			return convert<T>::to_v8(isolate, *value);
		}
		else
		{
			return v8::Undefined(isolate);
		}
	}
};

// convert std::tuple <-> Array
template<typename... Ts>
struct convert<std::tuple<Ts...>>
{
	using from_type = std::tuple<Ts...>;
	using to_type = v8::Local<v8::Array>;

	static constexpr size_t N = sizeof...(Ts);

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray()
			&& value.As<v8::Array>()->Length() == N;
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Tuple");
		}
		return from_v8_impl(isolate, value, std::make_index_sequence<N>{});
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		return from_v8_impl(isolate, value, std::make_index_sequence<N>{});
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		return to_v8_impl(isolate, value, std::make_index_sequence<N>{});
	}

private:
	template<size_t... Is>
	static from_type from_v8_impl(v8::Isolate* isolate, v8::Local<v8::Value> value,
		std::index_sequence<Is...>)
	{
		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> array = value.As<v8::Array>();

		return std::tuple<Ts...>{ v8pp::convert<Ts>::from_v8(isolate, array->Get(context, Is).ToLocalChecked())... };
	}

	template<size_t... Is>
	static to_type to_v8_impl(v8::Isolate* isolate, std::tuple<Ts...> const& value, std::index_sequence<Is...>)
	{
		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> result = v8::Array::New(isolate, N);

		(void)std::initializer_list<bool>{ result->Set(context, Is, convert<Ts>::to_v8(isolate, std::get<Is>(value))).FromJust()... };

		return scope.Escape(result);
	}
};

// convert std::variant <-> Local
template<typename... Ts>
struct convert<std::variant<Ts...>>
{
public:
	using from_type = std::variant<Ts...>;
	using to_type = v8::Local<v8::Value>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Variant");
		}

		v8::HandleScope scope(isolate);

		if (value.IsEmpty() || value->IsNull())
		{
			return alternate<is_nullopt, detail::is_optional>(isolate, value);
		}
		else if (value->IsUndefined())
		{
			return alternate<is_monostate, detail::is_optional>(isolate, value);
		}
		if (value->IsBoolean())
		{
			return alternate<is_bool, detail::is_optional>(isolate, value);
		}
		else if (value->IsBigInt())
		{
			return alternate<is_large_integral, is_integral_not_bool, detail::is_optional>(isolate, value);
		}
		else if (value->IsInt32() || value->IsUint32())
		{
			return alternate<is_small_integral, is_large_integral, std::is_floating_point, detail::is_optional>(isolate, value);
		}
		else if (value->IsNumber())
		{
			return alternate<is_large_integral, std::is_floating_point, is_small_integral, detail::is_optional>(isolate, value);
		}
		else if (value->IsString())
		{
			return alternate<detail::is_string, detail::is_optional>(isolate, value);
		}
		else if (value->IsArray())
		{
			return alternate<detail::is_sequence, detail::is_array, detail::is_tuple, detail::is_optional>(isolate, value);
		}
		else if (value->IsObject())
		{
			if (is_map_object(isolate, value.As<v8::Object>()))
			{
				return alternate<detail::is_mapping, is_wrapped_class, detail::is_shared_ptr, detail::is_optional>(isolate, value);
			}
			else
			{
				return alternate<is_wrapped_class, detail::is_shared_ptr, detail::is_optional>(isolate, value);
			}
		}
		else if (value->IsNullOrUndefined())
		{
			return alternate<is_monostate>(isolate, value);
		}
		else
		{
			return alternate<is_any>(isolate, value);
		}
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		return std::visit([isolate](auto&& v) -> v8::Local<v8::Value>
			{
				using T = std::decay_t<decltype(v)>;
				return v8pp::convert<T>::to_v8(isolate, v);
			}, value);
	}

private:
	template<typename T>
	using is_bool = std::is_same<T, bool>;

	template<typename T>
	using is_monostate = std::is_same<T, std::monostate>;

	template<typename T>
	using is_nullopt = std::is_same<T, std::nullopt_t>;

	template<typename T>
	using is_integral_not_bool = std::bool_constant<std::is_integral<T>::value && !is_bool<T>::value>;

	template<typename T>
	using is_small_integral = std::bool_constant<std::is_integral<T>::value && !is_bool<T>::value && sizeof(T) <= sizeof(uint32_t)>;

	template<typename T>
	using is_large_integral = std::bool_constant<std::is_integral<T>::value && !is_bool<T>::value && (sizeof(T) > sizeof(uint32_t))>;

	template<typename T>
	using is_any = std::true_type;

	static bool is_map_object(v8::Isolate* isolate, v8::Local<v8::Object> obj)
	{
		v8::Local<v8::Array> prop_names;
		return obj->GetPropertyNames(isolate->GetCurrentContext()).ToLocal(&prop_names)
			&& prop_names->Length() > 0;
	}

	template<typename T, typename Number>
	static void get_number(v8::Isolate* isolate, v8::Local<v8::Value> value, std::optional<from_type>& result)
	{
		Number const number = v8pp::convert<Number>::from_v8(isolate, value);
		if constexpr (std::same_as<T, uint64_t>)
		{
			result = static_cast<T>(number);
		}
		else
		{
			Number const min = std::numeric_limits<T>::lowest();
			Number const max = std::numeric_limits<T>::max();
			if (number >= min && number <= max)
			{
				result = static_cast<T>(number);
			}
		}
	}

	template<typename T>
	static bool try_as(v8::Isolate* isolate, v8::Local<v8::Value> value, std::optional<from_type>& result)
	{
		if constexpr (std::is_same<T, std::monostate>::value)
		{
			if (v8pp::convert<std::monostate>::is_valid(isolate, value))
			{
				result = std::monostate{};
			}
		}
		else if constexpr (std::is_same<T, std::nullopt_t>::value)
		{
			if (v8pp::convert<std::nullopt_t>::is_valid(isolate, value))
			{
				result = std::nullopt;
			}
		}
		else if constexpr (detail::is_shared_ptr<T>::value)
		{
			using U = typename T::element_type;
			if (auto obj = v8pp::class_<U, v8pp::shared_ptr_traits>::unwrap_object(isolate, value))
			{
				result = obj;
			}
		}
		else if constexpr (v8pp::is_wrapped_class<T>::value)
		{
			if (auto obj = v8pp::class_<T, v8pp::raw_ptr_traits>::unwrap_object(isolate, value))
			{
				result = *obj;
			}
		}
		else if constexpr (is_integral_not_bool<T>::value)
		{
			if (value->IsBigInt())
			{
				auto bigint = value.As<v8::BigInt>();
				bool lossless = false;
				if constexpr (std::is_signed_v<T>)
				{
					int64_t val = bigint->Int64Value(&lossless);
					if (lossless)
					{
						if constexpr (sizeof(T) >= sizeof(int64_t))
							result = static_cast<T>(val);
						else if (val >= std::numeric_limits<T>::lowest() && val <= std::numeric_limits<T>::max())
							result = static_cast<T>(val);
					}
				}
				else
				{
					uint64_t val = bigint->Uint64Value(&lossless);
					if (lossless)
					{
						if constexpr (sizeof(T) >= sizeof(uint64_t))
							result = static_cast<T>(val);
						else if (val <= std::numeric_limits<T>::max())
							result = static_cast<T>(val);
					}
				}
			}
			else if constexpr (sizeof(T) > sizeof(uint32_t))
			{
				// For 64-bit integrals from Number, only match if the double
				// is an exact integer within safe range (±2^53)
				double d = value->NumberValue(isolate->GetCurrentContext()).FromJust();
				constexpr double safe_max = static_cast<double>(uint64_t{1} << std::numeric_limits<double>::digits);
				if (std::isfinite(d) && d == std::trunc(d))
				{
					if constexpr (std::is_signed_v<T>)
					{
						if (d >= -safe_max && d <= safe_max)
							result = static_cast<T>(static_cast<int64_t>(d));
					}
					else
					{
						if (d >= 0.0 && d <= safe_max)
							result = static_cast<T>(static_cast<uint64_t>(d));
					}
				}
			}
			else
			{
				get_number<T, int64_t>(isolate, value, result);
			}
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			get_number<T, double>(isolate, value, result);
		}
		else // if constexpr
		{
			result = v8pp::convert<T>::from_v8(isolate, value);
		}
		return result != std::nullopt;
	}

	template<template<typename T> typename... Conditions>
	static from_type alternate(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		std::optional<from_type> result;
		(get_object<Conditions>(isolate, value, result) || ...);
		return result ? *result : throw std::runtime_error("Unable to convert argument to variant.");
	}

	template<template<typename T> typename Condition>
	static bool get_object(v8::Isolate* isolate, v8::Local<v8::Value> value, std::optional<from_type>& result)
	{
		return ((Condition<Ts>::value && try_as<Ts>(isolate, value, result)) || ...);
	}
};

// convert Array <-> std::array, vector, deque, list
template<typename Sequence>
	requires (detail::sequence<Sequence> || detail::is_array<Sequence>::value)
struct convert<Sequence, void>
{
	using from_type = Sequence;
	using to_type = v8::Local<v8::Array>;
	using item_type = typename Sequence::value_type;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *std::move(result);
		throw invalid_argument(isolate, value, "Array");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> array = value.As<v8::Array>();

		from_type result{};

		constexpr bool is_array = detail::is_array<Sequence>::value;
		if constexpr (is_array)
		{
			constexpr size_t length = detail::is_array<Sequence>::length;
			if (array->Length() != length)
			{
				throw std::runtime_error("Invalid array length: expected "
					+ std::to_string(length) + " actual "
					+ std::to_string(array->Length()));
			}
		}
		else if constexpr (detail::has_reserve<Sequence>::value)
		{
			result.reserve(array->Length());
		}

		for (uint32_t i = 0, count = array->Length(); i < count; ++i)
		{
			v8::Local<v8::Value> item = array->Get(context, i).ToLocalChecked();
			if constexpr (is_array)
			{
				result[i] = convert<item_type>::from_v8(isolate, item);
			}
			else
			{
				result.emplace_back(convert<item_type>::from_v8(isolate, item));
			}
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		constexpr int max_size = std::numeric_limits<int>::max();
		if (value.size() > max_size)
		{
			throw std::runtime_error("Invalid array length: actual "
				+ std::to_string(value.size()) + " exceeds maximal "
				+ std::to_string(max_size));
		}

		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> result = v8::Array::New(isolate, static_cast<int>(value.size()));
		uint32_t i = 0;
		for (item_type const& item : value)
		{
			result->Set(context, i++, convert<item_type>::to_v8(isolate, item)).FromJust();
		}
		return scope.Escape(result);
	}
};

// convert Object <-> std::{unordered_}{multi}map
template<detail::mapping Mapping>
struct convert<Mapping, void>
{
	using from_type = Mapping;
	using to_type = v8::Local<v8::Object>;

	using Key = typename Mapping::key_type;
	using Value = typename Mapping::mapped_type;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject() && !value->IsArray();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *std::move(result);
		throw invalid_argument(isolate, value, "Object");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Object> object = value.As<v8::Object>();
		v8::Local<v8::Array> prop_names;
		if (!object->GetPropertyNames(context).ToLocal(&prop_names))
		{
			return std::nullopt;
		}

		from_type result{};
		for (uint32_t i = 0, count = prop_names->Length(); i < count; ++i)
		{
			v8::Local<v8::Value> key = prop_names->Get(context, i).ToLocalChecked();
			v8::Local<v8::Value> val = object->Get(context, key).ToLocalChecked();
			const auto k = convert<Key>::from_v8(isolate, key);
			const auto v = convert<Value>::from_v8(isolate, val);
			result.emplace(k, v);
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Object> result = v8::Object::New(isolate);
		for (auto const& item : value)
		{
			const auto k = convert<Key>::to_v8(isolate, item.first);
			const auto v = convert<Value>::to_v8(isolate, item.second);
			result->Set(context, k, v).FromJust();
		}
		return scope.Escape(result);
	}
};

// convert Array <-> std::set, std::unordered_set
template<detail::set_like Set>
struct convert<Set, void>
{
	using from_type = Set;
	using to_type = v8::Local<v8::Array>;
	using item_type = typename Set::value_type;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *std::move(result);
		throw invalid_argument(isolate, value, "Array");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> array = value.As<v8::Array>();

		from_type result{};
		if constexpr (detail::reservable<Set>)
		{
			result.reserve(array->Length());
		}

		for (uint32_t i = 0, count = array->Length(); i < count; ++i)
		{
			v8::Local<v8::Value> item = array->Get(context, i).ToLocalChecked();
			result.insert(convert<item_type>::from_v8(isolate, item));
		}
		return result;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		constexpr int max_size = std::numeric_limits<int>::max();
		if (value.size() > static_cast<size_t>(max_size))
		{
			throw std::runtime_error("Invalid array length: actual "
				+ std::to_string(value.size()) + " exceeds maximal "
				+ std::to_string(max_size));
		}

		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> result = v8::Array::New(isolate, static_cast<int>(value.size()));
		uint32_t i = 0;
		for (item_type const& item : value)
		{
			result->Set(context, i++, convert<item_type>::to_v8(isolate, item)).FromJust();
		}
		return scope.Escape(result);
	}
};

// convert [first, second] Array <-> std::pair
template<typename K, typename V>
struct convert<std::pair<K, V>, void>
{
	using from_type = std::pair<K, V>;
	using to_type = v8::Local<v8::Array>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsArray()
			&& value.As<v8::Array>()->Length() == 2;
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *std::move(result);
		throw invalid_argument(isolate, value, "Array[2]");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;

		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> array = value.As<v8::Array>();

		v8::Local<v8::Value> first = array->Get(context, 0).ToLocalChecked();
		v8::Local<v8::Value> second = array->Get(context, 1).ToLocalChecked();
		return std::pair{ convert<K>::from_v8(isolate, first), convert<V>::from_v8(isolate, second) };
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> result = v8::Array::New(isolate, 2);
		result->Set(context, 0, convert<K>::to_v8(isolate, value.first)).FromJust();
		result->Set(context, 1, convert<V>::to_v8(isolate, value.second)).FromJust();
		return scope.Escape(result);
	}
};

// convert string <-> std::filesystem::path
template<>
struct convert<std::filesystem::path, void>
{
	using from_type = std::filesystem::path;
	using to_type = v8::Local<v8::String>;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return convert<std::string>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		std::string str = convert<std::string>::from_v8(isolate, value);
#ifdef WIN32
		int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
		std::wstring w(sz, 0);
		MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), w.data(), sz);
		return std::filesystem::path(w);
#else
		return std::filesystem::path(str);
#endif
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto str = convert<std::string>::try_from_v8(isolate, value))
		{
			std::string s = *std::move(str);
#ifdef WIN32
			int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			std::wstring w(sz, 0);
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), sz);
			return std::filesystem::path(w);
#else
			return std::filesystem::path(s);
#endif
		}
		return std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
#ifdef WIN32
		std::wstring w = value.generic_wstring();
		int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string utf8(sz, 0);
		WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), utf8.data(), sz, nullptr, nullptr);
		return convert<std::string>::to_v8(isolate, utf8);
#else
		return convert<std::string>::to_v8(isolate, value.string());
#endif
	}
};

// convert Number (milliseconds) <-> std::chrono::duration
template<typename Rep, typename Period>
struct convert<std::chrono::duration<Rep, Period>, void>
{
	using duration_type = std::chrono::duration<Rep, Period>;
	using from_type = duration_type;
	using to_type = v8::Local<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Number");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		double ms = value->NumberValue(isolate->GetCurrentContext()).FromJust();
		return std::chrono::duration_cast<duration_type>(
			std::chrono::duration<double, std::milli>(ms));
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(value);
		return v8::Number::New(isolate, ms.count());
	}
};

// convert Number (epoch milliseconds) <-> std::chrono::time_point
template<typename Clock, typename Duration>
struct convert<std::chrono::time_point<Clock, Duration>, void>
{
	using time_point_type = std::chrono::time_point<Clock, Duration>;
	using from_type = time_point_type;
	using to_type = v8::Local<v8::Number>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsNumber();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *result;
		throw invalid_argument(isolate, value, "Number");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		double ms = value->NumberValue(isolate->GetCurrentContext()).FromJust();
		// Convert via integer milliseconds to avoid floating-point precision loss
		// when scaling large epoch timestamps to finer-grained durations (e.g. nanoseconds)
		auto epoch_duration = std::chrono::duration_cast<Duration>(
			std::chrono::milliseconds(std::llround(ms)));
		return time_point_type(epoch_duration);
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			value.time_since_epoch());
		return v8::Number::New(isolate, static_cast<double>(epoch_ms.count()));
	}
};

// convert ArrayBuffer <-> std::vector<uint8_t>
template<>
struct convert<std::vector<uint8_t>, void>
{
	using from_type = std::vector<uint8_t>;
	using to_type = v8::Local<v8::ArrayBuffer>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && (value->IsArrayBuffer() || value->IsArrayBufferView());
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (auto result = try_from_v8(isolate, value)) return *std::move(result);
		throw invalid_argument(isolate, value, "ArrayBuffer");
	}

	static std::optional<from_type> try_from_v8(v8::Isolate*, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty() || (!value->IsArrayBuffer() && !value->IsArrayBufferView()))
		{
			return std::nullopt;
		}

		v8::Local<v8::ArrayBuffer> buffer;
		size_t offset = 0;
		size_t length = 0;
		if (value->IsArrayBufferView())
		{
			auto view = value.As<v8::ArrayBufferView>();
			buffer = view->Buffer();
			offset = view->ByteOffset();
			length = view->ByteLength();
		}
		else
		{
			buffer = value.As<v8::ArrayBuffer>();
			length = buffer->ByteLength();
		}

		auto const* data = static_cast<uint8_t const*>(buffer->GetBackingStore()->Data()) + offset;
		return std::vector<uint8_t>(data, data + length);
	}

	static to_type to_v8(v8::Isolate* isolate, from_type const& value)
	{
		v8::EscapableHandleScope scope(isolate);
		auto backing = v8::ArrayBuffer::NewBackingStore(isolate, value.size());
		std::memcpy(backing->Data(), value.data(), value.size());
		return scope.Escape(v8::ArrayBuffer::New(isolate, std::move(backing)));
	}
};

// convert TypedArray <- std::span<T> (to_v8 only — span is non-owning)
template<detail::typed_array_element T>
struct convert<std::span<T>, void>
{
	using from_type = std::span<T>;
	using to_type = v8::Local<v8::Value>;

	static to_type to_v8(v8::Isolate* isolate, std::span<T> value)
	{
		v8::EscapableHandleScope scope(isolate);
		size_t byte_length = value.size_bytes();
		auto backing = v8::ArrayBuffer::NewBackingStore(isolate, byte_length);
		std::memcpy(backing->Data(), value.data(), byte_length);
		auto buffer = v8::ArrayBuffer::New(isolate, std::move(backing));
		using TypedArrayType = typename detail::typed_array_trait<T>::type;
		auto typed_array = TypedArrayType::New(buffer, 0, value.size());
		return scope.Escape(typed_array);
	}
};

template<typename T>
struct convert<v8::Local<T>>
{
	using from_type = v8::Local<T>;
	using to_type = v8::Local<T>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.As<T>().IsEmpty();
	}

	static v8::Local<T> from_v8(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return value.As<T>();
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value)) return std::nullopt;
		return value.As<T>();
	}

	static v8::Local<T> to_v8(v8::Isolate*, v8::Local<T> value)
	{
		return value;
	}
};

template<typename T>
struct is_wrapped_class : std::conjunction<
	std::is_class<T>,
	std::negation<detail::is_string<T>>,
	std::negation<detail::is_mapping<T>>,
	std::negation<detail::is_sequence<T>>,
	std::negation<detail::is_array<T>>,
	std::negation<detail::is_tuple<T>>,
	std::negation<detail::is_shared_ptr<T>>,
	std::negation<detail::is_optional<T>>,
	std::negation<detail::is_set<T>>,
	std::negation<detail::is_pair<T>>,
	std::negation<detail::is_duration<T>>,
	std::negation<detail::is_time_point<T>>>
{
};

// convert specialization for wrapped user classes
template<typename T>
struct is_wrapped_class<v8::Local<T>> : std::false_type
{
};

template<typename T>
struct is_wrapped_class<v8::Global<T>> : std::false_type
{
};

template<typename... Ts>
struct is_wrapped_class<std::variant<Ts...>> : std::false_type
{
};

template<>
struct is_wrapped_class<std::filesystem::path> : std::false_type
{
};

template<typename T>
	requires is_wrapped_class<T>::value
struct convert<T*, void>
{
	using from_type = T*;
	using to_type = v8::Local<v8::Object>;
	using class_type = typename std::remove_cv<T>::type;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			return nullptr;
		}
		return class_<class_type, raw_ptr_traits>::unwrap_object(isolate, value);
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		// from_v8 returns nullptr without throwing on failure
		auto ptr = from_v8(isolate, value);
		return ptr ? std::optional<from_type>{ptr} : std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, T const* value)
	{
		return class_<class_type, raw_ptr_traits>::find_object(isolate, value);
	}
};

template<typename T>
	requires is_wrapped_class<T>::value
struct convert<T, void>
{
	using from_type = T&;
	using to_type = v8::Local<v8::Object>;
	using class_type = typename std::remove_cv_t<T>;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return convert<T*>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Object");
		}
		T* object = class_<class_type, raw_ptr_traits>::unwrap_object(isolate, value);
		if (object)
		{
			return *object;
		}
		throw std::runtime_error("failed to unwrap C++ object");
	}

	static std::optional<class_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty() || !value->IsObject()) return std::nullopt;
		T* object = class_<class_type, raw_ptr_traits>::unwrap_object(isolate, value);
		return object ? std::optional<class_type>{*object} : std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, T const& value)
	{
		v8::Local<v8::Object> result = class_<class_type, raw_ptr_traits>::find_object(isolate, value);
		if (!result.IsEmpty()) return result;
		throw std::runtime_error("failed to wrap C++ object");
	}
};

template<typename T>
	requires is_wrapped_class<T>::value
struct convert<std::shared_ptr<T>, void>
{
	using from_type = std::shared_ptr<T>;
	using to_type = v8::Local<v8::Object>;
	using class_type = typename std::remove_cv_t<T>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			return nullptr;
		}
		return class_<class_type, shared_ptr_traits>::unwrap_object(isolate, value);
	}

	static std::optional<from_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		// from_v8 returns empty shared_ptr without throwing on failure
		auto ptr = from_v8(isolate, value);
		return ptr ? std::optional<from_type>{std::move(ptr)} : std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, std::shared_ptr<T> const& value)
	{
		return class_<class_type, shared_ptr_traits>::find_object(isolate, value);
	}
};

template<typename T>
struct convert<T, ref_from_shared_ptr>
{
	using from_type = T&;
	using to_type = v8::Local<v8::Object>;
	using class_type = typename std::remove_cv_t<T>;

	static bool is_valid(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return convert<std::shared_ptr<T>>::is_valid(isolate, value);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw invalid_argument(isolate, value, "Object");
		}
		std::shared_ptr<T> object = class_<class_type, shared_ptr_traits>::unwrap_object(isolate, value);
		if (object)
		{
			//assert(object.use_count() > 1);
			return *object;
		}
		throw std::runtime_error("failed to unwrap C++ object");
	}

	static std::optional<class_type> try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (value.IsEmpty() || !value->IsObject()) return std::nullopt;
		std::shared_ptr<T> object = class_<class_type, shared_ptr_traits>::unwrap_object(isolate, value);
		return object ? std::optional<class_type>{*object} : std::nullopt;
	}

	static to_type to_v8(v8::Isolate* isolate, T const& value)
	{
		v8::Local<v8::Object> result = class_<class_type, shared_ptr_traits>::find_object(isolate, value);
		if (!result.IsEmpty()) return result;
		throw std::runtime_error("failed to wrap C++ object");
	}
};

template<typename T>
struct convert<T&> : convert<T>
{
};

template<typename T>
struct convert<T const&> : convert<T>
{
};

template<typename T>
auto from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	-> decltype(convert<T>::from_v8(isolate, value))
{
	return convert<T>::from_v8(isolate, value);
}

template<typename T, typename U>
auto from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value, U const& default_value)
	-> decltype(convert<T>::from_v8(isolate, value))
{
	using return_type = decltype(convert<T>::from_v8(isolate, value));
	return convert<T>::is_valid(isolate, value) ?
		convert<T>::from_v8(isolate, value) : static_cast<return_type>(default_value);
}

namespace detail {

// Fallback for user-defined converters that don't implement try_from_v8.
// Uses is_valid + from_v8 (double validation, potential exception on type mismatch).
template<typename T>
[[deprecated("convert<T> specialization should implement try_from_v8() for optimal exception-free conversion")]]
auto try_from_v8_fallback(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
	using result_type = std::decay_t<typename convert<T>::from_type>;
	if (!convert<T>::is_valid(isolate, value)) return std::optional<result_type>{};
	return std::optional<result_type>{convert<T>::from_v8(isolate, value)};
}

} // namespace detail

// Exception-free conversion: returns std::optional with the converted value,
// or std::nullopt if the value cannot be converted to type T.
// Distinct from convert<std::optional<T>> which handles missing/undefined JS arguments.
// Delegates to convert<T>::try_from_v8 when available, falls back to is_valid + from_v8.
template<typename T>
auto try_from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
	if constexpr (requires { convert<T>::try_from_v8(isolate, value); })
	{
		return convert<T>::try_from_v8(isolate, value);
	}
	else
	{
		return detail::try_from_v8_fallback<T>(isolate, value);
	}
}

inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, char const* str)
{
	return convert<std::string_view>::to_v8(isolate, std::string_view(str));
}

inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, char const* str, size_t len)
{
	return convert<std::string_view>::to_v8(isolate, std::string_view(str, len));
}

template<int N>
v8::Local<v8::String> to_v8(v8::Isolate* isolate, char const (&str)[N])
{
	return v8::String::NewFromUtf8Literal(isolate, str);
}

inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, char16_t const* str)
{
	return convert<std::u16string_view>::to_v8(isolate, std::u16string_view(str));
}

inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, char16_t const* str, size_t len)
{
	return convert<std::u16string_view>::to_v8(isolate, std::u16string_view(str, len));
}

template<size_t N>
v8::Local<v8::String> to_v8(v8::Isolate* isolate,
	char16_t const (&str)[N], size_t len = N - 1)
{
	return convert<std::u16string_view>::to_v8(isolate, std::u16string_view(str, len));
}

#ifdef WIN32
inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, wchar_t const* str)
{
	return convert<std::wstring_view>::to_v8(isolate, std::wstring_view(str));
}

inline v8::Local<v8::String> to_v8(v8::Isolate* isolate, wchar_t const* str, size_t len)
{
	return convert<std::wstring_view>::to_v8(isolate, std::wstring_view(str, len));
}

template<size_t N>
v8::Local<v8::String> to_v8(v8::Isolate* isolate,
	wchar_t const (&str)[N], size_t len = N - 1)
{
	return convert<std::wstring_view>::to_v8(isolate, std::wstring_view(str, len));
}
#endif

/// Create an internalized V8 string, optimized for property/method names
inline v8::Local<v8::String> to_v8_name(v8::Isolate* isolate, std::string_view name)
{
	return v8::String::NewFromUtf8(isolate, name.data(),
		v8::NewStringType::kInternalized, static_cast<int>(name.size())).ToLocalChecked();
}

template<typename T>
v8::Local<v8::Value> to_v8(v8::Isolate* isolate, std::optional<T> const& value)
{
	if (value)
	{
		return convert<T>::to_v8(isolate, *value);
	}
	else
	{
		return v8::Undefined(isolate);
	}
}

template<typename T>
auto to_v8(v8::Isolate* isolate, T const& value)
{
	return convert<T>::to_v8(isolate, value);
}

template<typename Iterator>
v8::Local<v8::Array> to_v8(v8::Isolate* isolate, Iterator begin, Iterator end)
{
	v8::EscapableHandleScope scope(isolate);
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Array> result = v8::Array::New(isolate);
	for (uint32_t idx = 0; begin != end; ++begin, ++idx)
	{
		result->Set(context, idx, to_v8(isolate, *begin)).FromJust();
	}
	return scope.Escape(result);
}

template<typename T>
v8::Local<v8::Array> to_v8(v8::Isolate* isolate, std::initializer_list<T> const& init)
{
	return to_v8(isolate, init.begin(), init.end());
}

template<typename T>
v8::Local<T> to_local(v8::Isolate* isolate, v8::PersistentBase<T> const& handle)
{
	if (handle.IsWeak())
	{
		return v8::Local<T>::New(isolate, handle);
	}
	else
	{
		return *reinterpret_cast<v8::Local<T>*>(
			const_cast<v8::PersistentBase<T>*>(&handle));
	}
}

inline invalid_argument::invalid_argument(v8::Isolate* isolate, v8::Local<v8::Value> value, char const* expected_type)
	: std::invalid_argument(std::string("expected ")
		+ expected_type
		+ ", typeof="
		+ (value.IsEmpty() ? std::string("<empty>") : v8pp::from_v8<std::string>(isolate, value->TypeOf(isolate))))
{
}

inline runtime_error::runtime_error(v8::Isolate* isolate, v8::Local<v8::Value> value, char const* message)
	: std::runtime_error(std::string("runtime error: ") + message
		+ ", typeof="
		+ (value.IsEmpty() ? std::string("<empty>") : v8pp::from_v8<std::string>(isolate, value->TypeOf(isolate))))
{
}

} // namespace v8pp
