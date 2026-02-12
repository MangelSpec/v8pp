#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <optional>
#include <type_traits>

namespace v8pp::detail {

template<typename T>
struct tuple_tail;

template<typename Head, typename... Tail>
struct tuple_tail<std::tuple<Head, Tail...>>
{
	using type = std::tuple<Tail...>;
};

struct none
{
};

template<typename Char>
concept WideChar = std::same_as<Char, char16_t> || std::same_as<Char, char32_t> || std::same_as<Char, wchar_t>;

/////////////////////////////////////////////////////////////////////////////
//
// is_string<T>
//
template<typename T>
struct is_string : std::false_type
{
};

template<typename Char, typename Traits, typename Alloc>
struct is_string<std::basic_string<Char, Traits, Alloc>> : std::true_type
{
};

template<typename Char, typename Traits>
struct is_string<std::basic_string_view<Char, Traits>> : std::true_type
{
};

template<>
struct is_string<char const*> : std::true_type
{
};

template<>
struct is_string<char16_t const*> : std::true_type
{
};

template<>
struct is_string<char32_t const*> : std::true_type
{
};

template<>
struct is_string<wchar_t const*> : std::true_type
{
};

/////////////////////////////////////////////////////////////////////////////
//
// is_mapping<T>
//
template<typename T>
concept mapping = requires(T t) {
	typename T::key_type;
	typename T::mapped_type;
	t.begin();
	t.end();
};

template<typename T>
struct is_mapping : std::bool_constant<mapping<T>>
{
};

/////////////////////////////////////////////////////////////////////////////
//
// is_sequence<T>
//
template<typename T>
concept sequence = !is_string<T>::value && requires(T t, typename T::value_type v) {
	t.begin();
	t.end();
	t.emplace_back(std::move(v));
};

template<typename T>
struct is_sequence : std::bool_constant<sequence<T>>
{
};

/////////////////////////////////////////////////////////////////////////////
//
// has_reserve<T>
//
template<typename T>
concept reservable = requires(T t) { t.reserve(size_t{}); };

template<typename T>
struct has_reserve : std::bool_constant<reservable<T>>
{
};

/////////////////////////////////////////////////////////////////////////////
//
// is_array<T>
//
template<typename T>
struct is_array : std::false_type
{
};

template<typename T, std::size_t N>
struct is_array<std::array<T, N>> : std::true_type
{
	static constexpr size_t length = N;
};

/////////////////////////////////////////////////////////////////////////////
//
// is_tuple<T>
//
template<typename T>
struct is_tuple : std::false_type
{
};

template<typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type
{
};

/////////////////////////////////////////////////////////////////////////////
//
// is_shared_ptr<T>
//
template<typename T>
struct is_shared_ptr : std::false_type
{
};

template<typename T>
struct is_shared_ptr<std::shared_ptr<T>> : std::true_type
{
};

/////////////////////////////////////////////////////////////////////////////
//
// is_optional<T>
//
template<typename T>
struct is_optional : std::false_type
{
};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type
{
};

/////////////////////////////////////////////////////////////////////////////
//
// Function traits
//
template<typename F>
struct function_traits;

template<>
struct function_traits<none>
{
	using return_type = void;
	using arguments = std::tuple<>;
	template<typename D>
	using pointer_type = void;
};

template<typename R, typename... Args>
struct function_traits<R (Args...)>
{
	using return_type = R;
	using arguments = std::tuple<Args...>;
	template<typename D>
	using pointer_type = R (*)(Args...);
};

// function pointer
template<typename R, typename... Args>
struct function_traits<R (*)(Args...)>
	: function_traits<R (Args...)>
{
};

// member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...)>
	: function_traits<R (C&, Args...)>
{
	using class_type = C;
	template<typename D>
	using pointer_type = R (D::*)(Args...);
};

// const member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const>
	: function_traits<R (C const&, Args...)>
{
	using class_type = C const;
	template<typename D>
	using pointer_type = R (D::*)(Args...) const;
};

// volatile member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) volatile>
	: function_traits<R (C volatile&, Args...)>
{
	using class_type = C volatile;
	template<typename D>
	using pointer_type = R (D::*)(Args...) volatile;
};

// const volatile member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const volatile>
	: function_traits<R (C const volatile&, Args...)>
{
	using class_type = C const volatile;
	template<typename D>
	using pointer_type = R (D::*)(Args...) const volatile;
};

// member object pointer
template<typename C, typename R>
struct function_traits<R (C::*)>
	: function_traits<R (C&)>
{
	using class_type = C;
	template<typename D>
	using pointer_type = R (D::*);
};

// const member object pointer
template<typename C, typename R>
struct function_traits<const R (C::*)>
	: function_traits<R (C const&)>
{
	using class_type = C const;
	template<typename D>
	using pointer_type = const R (D::*);
};

// volatile member object pointer
template<typename C, typename R>
struct function_traits<volatile R (C::*)>
	: function_traits<R (C volatile&)>
{
	using class_type = C volatile;
	template<typename D>
	using pointer_type = volatile R (D::*);
};

// const volatile member object pointer
template<typename C, typename R>
struct function_traits<const volatile R (C::*)>
	: function_traits<R (C const volatile&)>
{
	using class_type = C const volatile;
	template<typename D>
	using pointer_type = const volatile R (D::*);
};

// function object, std::function, lambda
template<typename F>
struct function_traits
{
	static_assert(!std::is_bind_expression<F>::value,
		"std::bind result is not supported yet");

private:
	using callable_traits = function_traits<decltype(&F::operator())>;

public:
	using return_type = typename callable_traits::return_type;
	using arguments = typename tuple_tail<typename callable_traits::arguments>::type;
	template<typename D>
	using pointer_type = typename callable_traits::template pointer_type<D>;
};

template<typename F>
struct function_traits<F&> : function_traits<F>
{
};

template<typename F>
struct function_traits<F&&> : function_traits<F>
{
};

template<typename F>
concept callable = std::is_function_v<std::remove_pointer_t<F>>
	|| requires { &F::operator(); };

template<typename F>
using is_callable = std::bool_constant<callable<F>>;

template<typename F>
inline constexpr bool is_const_member_function_v = false;

template<typename F>
	requires std::is_member_function_pointer_v<F>
inline constexpr bool is_const_member_function_v<F> =
	std::is_const_v<typename function_traits<F>::class_type>;

} // namespace v8pp::detail
