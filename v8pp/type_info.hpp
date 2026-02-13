#pragma once

#include "v8pp/config.hpp"

#include <string_view>
#include <cstdint>

namespace v8pp::detail {

/// Type information for custom RTTI
class type_info
{
public:
	constexpr uintptr_t id() const { return id_; }
	constexpr std::string_view name() const { return name_; }

	constexpr bool operator==(type_info const& other) const { return id_ == other.id_; }
	constexpr bool operator!=(type_info const& other) const { return id_ != other.id_; }

private:
	template<typename T>
	friend type_info type_id();

	constexpr explicit type_info(uintptr_t id, std::string_view name)
		: id_(id)
		, name_(name)
	{
	}

	uintptr_t id_;
	std::string_view name_;
};

// Generate unique compile-time ID per type using static variable address
// The static variable ensures a single address per type across all translation units
template<typename T>
struct type_id_storage
{
	static constexpr char value = 0;
};

template<typename T>
constexpr char type_id_storage<T>::value;

/// Get type information for type T
/// The idea is borrowed from https://github.com/Manu343726/ctti
template<typename T>
inline type_info type_id()
{
	// Use the address of a static variable which is guaranteed to be unique per type
	uintptr_t unique_id = reinterpret_cast<uintptr_t>(&type_id_storage<T>::value);

#if defined(_MSC_VER) && !defined(__clang__)
	std::string_view name = __FUNCSIG__;
	const std::initializer_list<std::string_view> all_prefixes{ "type_id<", "struct ", "class " };
	const std::initializer_list<std::string_view> any_suffixes{ ">" };
#elif defined(__clang__) || defined(__GNUC__)
	std::string_view name = __PRETTY_FUNCTION__;
	const std::initializer_list<std::string_view> all_prefixes{ "T = " };
	const std::initializer_list<std::string_view> any_suffixes{ ";", "]" };
#else
#error "Unknown compiler"
#endif
#if V8PP_PRETTIFY_TYPENAMES
	for (auto&& prefix : all_prefixes)
	{
		const auto p = name.find(prefix);
		if (p != name.npos)
		{
			name.remove_prefix(p + prefix.size());
		}
	}

	for (auto&& suffix : any_suffixes)
	{
		const auto p = name.rfind(suffix);
		if (p != name.npos)
		{
			name.remove_suffix(name.size() - p);
			break;
		}
	}
#endif

	return type_info(unique_id, name);
}

} // namespace v8pp::detail
