#include "v8pp/convert.hpp"
#include "v8pp/class.hpp"

#include "test.hpp"

#include <cmath>
#include <list>
#include <vector>
#include <map>

template<typename T, typename U>
void test_conv(v8::Isolate* isolate, T value, U expected)
{
	auto const obtained = v8pp::from_v8<U>(isolate, v8pp::to_v8(isolate, value));
	check_eq(v8pp::detail::type_id<T>().name(), obtained, expected);

	auto const obtained2 = v8pp::from_v8<T>(isolate, v8pp::to_v8(isolate, expected));
	check_eq(v8pp::detail::type_id<U>().name(), obtained2, value);
}

template<typename T>
void test_conv(v8::Isolate* isolate, T value)
{
	test_conv(isolate, value, value);
}

template<typename Char, size_t N>
void test_string_conv(v8::Isolate* isolate, Char const (&str)[N])
{
	std::basic_string<Char> const str2(str, 2);

	std::basic_string_view<Char> const sv(str);
	std::basic_string_view<Char> const sv2(str, 2);

	test_conv(isolate, str[0]);
	test_conv(isolate, str);
	test_conv(isolate, sv2);

	check_eq("string literal",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, str)), str);
	check_eq("string literal2",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, str, 2)), str2);
	check_eq("string view",
		v8pp::from_v8<std::basic_string_view<Char>>(isolate, v8pp::to_v8(isolate, sv)), sv);

	Char const* ptr = str;
	check_eq("string pointer",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, ptr)), str);
	check_eq("string pointer2",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, ptr, 2)), str2);

	Char const* empty = str + N - 1; // use last \0 in source string
	check_eq("empty string literal",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, "")), empty);
	check_eq("empty string literal0",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, "", 0)), empty);

	check_eq("empty string pointer",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, empty)), empty);
	check_eq("empty string pointer0",
		v8pp::from_v8<Char const*>(isolate, v8pp::to_v8(isolate, empty, 0)), empty);
}

struct address
{
	std::string zip;
	std::string city;
	std::string street;
	std::string house;
	std::optional<std::string> flat;

	//for test framework
	bool operator==(address const& other) const = default;

	friend std::ostream& operator<<(std::ostream& os, address const& a)
	{
		return os << "address: " << a.zip << " " << a.city << " " << a.street << " " << a.house << " " << a.flat;
	}
};

struct person
{
	std::string name;
	int age;
	std::optional<address> home;

	//for test framework
	bool operator==(person const& other) const = default;

	friend std::ostream& operator<<(std::ostream& os, person const& p)
	{
		return os << "person: " << p.name << " age: " << p.age << " home: " << p.home;
	}
};

namespace v8pp {

template<>
struct convert<address>
{
	using from_type = address;
	using to_type = v8::Local<v8::Object>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static to_type to_v8(v8::Isolate* isolate, address const& a)
	{
		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Object> obj = v8::Object::New(isolate);
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "zip"), v8pp::to_v8(isolate, a.zip)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "city"), v8pp::to_v8(isolate, a.city)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "street"), v8pp::to_v8(isolate, a.street)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "house"), v8pp::to_v8(isolate, a.house)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "flat"), v8pp::to_v8(isolate, a.flat)).FromJust();
		return scope.Escape(obj);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::runtime_error("expected object");
		}

		v8::HandleScope scope(isolate);
		v8::Local<v8::Object> obj = value.As<v8::Object>();

		address result;
		result.zip = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "zip")).ToLocalChecked());
		result.city = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "city")).ToLocalChecked());
		result.street = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "street")).ToLocalChecked());
		result.house = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "house")).ToLocalChecked());
		result.flat = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "flat")).ToLocalChecked());
		return result;
	}
};

template<>
struct convert<person>
{
	using from_type = person;
	using to_type = v8::Local<v8::Object>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsObject();
	}

	static to_type to_v8(v8::Isolate* isolate, person const& p)
	{
		v8::EscapableHandleScope scope(isolate);
		v8::Local<v8::Object> obj = v8::Object::New(isolate);
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "name"), v8pp::to_v8(isolate, p.name)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "age"), v8pp::to_v8(isolate, p.age)).FromJust();
		obj->Set(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "home"), v8pp::to_v8(isolate, p.home)).FromJust();
		/* Simpler after #include <v8pp/object.hpp>
		set_option(isolate, obj, "name", p.name);
		set_option(isolate, obj, "age", p.age);
		set_option(isolate, obj, "home", p.home);
		*/
		return scope.Escape(obj);
	}

	static from_type from_v8(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		if (!is_valid(isolate, value))
		{
			throw std::runtime_error("expected object");
		}

		v8::HandleScope scope(isolate);
		v8::Local<v8::Object> obj = value.As<v8::Object>();

		person result;
		result.name = v8pp::from_v8<std::string>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "name")).ToLocalChecked());
		result.age = v8pp::from_v8<int>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "age")).ToLocalChecked());
		result.home = v8pp::from_v8<std::optional<address>>(isolate,
			obj->Get(isolate->GetCurrentContext(), v8pp::to_v8(isolate, "home")).ToLocalChecked());

		/* Simpler after #include <v8pp/object.hpp>
		get_option(isolate, obj, "name", result.name);
		get_option(isolate, obj, "age", result.age);
		get_option(isolate, obj, "home", result.home);
		*/
		return result;
	}
};

} // namespace v8pp

void test_convert_user_type(v8::Isolate* isolate)
{
	person p;
	p.name = "Al"; p.age = 33;
	test_conv(isolate, p);
	p.home = { .zip = "90210", .city = "Beverly Hills", .street = "Main St", .house = "123", .flat = "B2" };
	test_conv(isolate, p);
}

void test_convert_optional(v8::Isolate* isolate)
{
	test_conv(isolate, std::optional<int>{42});
	test_conv(isolate, std::optional<int>{std::nullopt});

	check("null", v8pp::from_v8<std::optional<std::string>>(isolate, v8::Null(isolate)) == std::nullopt);
    check("undefined", v8pp::from_v8<std::optional<std::string>>(isolate, v8::Undefined(isolate)) == std::nullopt);

    check("nullopt", v8pp::to_v8(isolate, std::nullopt)->IsNull());
	check("monostate", v8pp::to_v8(isolate, std::monostate{})->IsUndefined());

	check_ex<v8pp::invalid_argument>("wrong optional type", [isolate]()
	{
		v8pp::from_v8<std::optional<int>>(isolate, v8pp::to_v8(isolate, std::optional<std::string>{"aa"}));
	});
}

void test_convert_tuple(v8::Isolate* isolate)
{
	std::tuple<size_t, bool> const tuple_1{ 2, true };
	test_conv(isolate, tuple_1);

	std::tuple<size_t, bool, std::string> const tuple_2{ 2, true, "test" };
	test_conv(isolate, tuple_2);

	std::tuple<size_t, size_t, size_t> const tuple_3{ 1, 2, 3 };
	test_conv(isolate, tuple_3);

	std::tuple<int, std::optional<int>, int, std::optional<int>> const tuple_4{ 1, 2, 3, std::nullopt };
	test_conv(isolate, tuple_4);

	check_ex<v8pp::invalid_argument>("Tuple", [isolate, &tuple_1]()
	{
		// incorrect number of elements
		v8::Local<v8::Array> tuple_1_ = v8pp::to_v8(isolate, tuple_1);
		v8pp::from_v8<std::tuple<size_t, bool, std::string>>(isolate, tuple_1_);
	});

	{
		// bool converts to string via ToString()
		v8::Local<v8::Array> tuple_1_ = v8pp::to_v8(isolate, tuple_1);
		auto result = v8pp::from_v8<std::tuple<size_t, std::string>>(isolate, tuple_1_);
		check_eq("tuple bool->string", std::get<1>(result), "true");
	}
}

template<typename... Ts>
struct variant_check
{
	using variant = std::variant<Ts...>;

	v8::Isolate* isolate;

	explicit variant_check(v8::Isolate* isolate) : isolate(isolate) {}

	template<typename T>
	static T const& get(T const& in) { return in; }

	template<typename T>
	static T const& get(variant const& in) { return std::get<T>(in); }

	template<typename T, typename From, typename To>
	void check(T const& value)
	{
		v8::Local<v8::Value> v8_value = v8pp::convert<To>::to_v8(isolate, value);
		auto const value2 = v8pp::convert<From>::from_v8(isolate, v8_value);
		::check_eq(v8pp::detail::type_id<variant>().name(), variant_check::get<T>(value2), value);
	}

	template<typename T>
	void check(T const& value)
	{
		this->check<T, variant, variant>(value); // variant to variant
		this->check<T, variant, T>(value); // variant to type
		this->check<T, T, variant>(value); // type to variant
	}

	template<typename T>
	void check_ex(T const& value)
	{
		v8::Local<v8::Value> v8_value = v8pp::convert<T>::to_v8(isolate, value);
		::check_ex<std::exception>("variant", [&]()
		{
			v8pp::convert<variant>::from_v8(isolate, v8_value);
		});
	}

	void operator()(Ts const&... values)
	{
		(check(values), ...);
	}
};

static int64_t const V8_MAX_INT = (uint64_t{1} << std::numeric_limits<double>::digits) - 1;
static int64_t const V8_MIN_INT = -V8_MAX_INT - 1;

template<typename T>
void check_range(v8::Isolate* isolate)
{
	variant_check<T> check_range{ isolate };

	T zero{ 0 };
	T min, max;
	if constexpr (std::same_as<T, int64_t>)
	{
		min = V8_MIN_INT;
		max = V8_MAX_INT;
	}
	else if constexpr (std::same_as<T, uint64_t>)
	{
		min = 0;
		max = V8_MAX_INT;
	}
	else
	{
		min = std::numeric_limits<T>::lowest();
		max = std::numeric_limits<T>::max();
	}

	check_range(zero);
	check_range(min);
	check_range(max);
	check_range.check_ex(std::nextafter(double(min), std::numeric_limits<double>::lowest())); // like min - 1 (out of range)
	check_range.check_ex(std::nextafter(double(max), std::numeric_limits<double>::max())); // like max + 1 (out of range)
}

template<typename... Ts>
void check_ranges(v8::Isolate* isolate)
{
	(check_range<Ts>(isolate), ...);
}

struct U
{
	int value = 1;
	//for test framework
	bool operator==(U const& other) const { return value == other.value; }
	bool operator!=(U const& other) const { return value != other.value; }
	friend std::ostream& operator<<(std::ostream& os, U const& val) { return os << val.value; }
};

struct U2
{
	double value = 2.0;
	//for test framework
	bool operator==(U2 const& other) const { return value == other.value; }
	bool operator!=(U2 const& other) const { return value != other.value; }
	friend std::ostream& operator<<(std::ostream& os, U2 const& val) { return os << val.value; }
};

struct V
{
	std::string value;
	//for test framework
	bool operator==(V const& other) const { return value == other.value; }
	bool operator!=(V const& other) const { return value != other.value; }
	friend std::ostream& operator<<(std::ostream& os, V const& val) { return os << val.value; }
};

struct V2
{
	std::string value;
	//for test framework
	bool operator==(V2 const& other) const { return value == other.value; }
	bool operator!=(V2 const& other) const { return value != other.value; }
	friend std::ostream& operator<<(std::ostream& os, V2 const& val) { return os << val.value; }
};

void test_convert_variant(v8::Isolate* isolate)
{
	v8pp::class_<U, v8pp::raw_ptr_traits> U_class(isolate);
	U_class.template ctor<>().auto_wrap_objects(true);

	v8pp::class_<U2, v8pp::raw_ptr_traits> U2_class(isolate);
	U2_class.template ctor<>().auto_wrap_objects(true);

	v8pp::class_<V, v8pp::shared_ptr_traits> V_class(isolate);
	V_class.template ctor<>().auto_wrap_objects(true);

	v8pp::class_<V2, v8pp::shared_ptr_traits> V2_class(isolate);
	V2_class.template ctor<>().auto_wrap_objects(true);

	auto const v = std::make_shared<V>(V{ "test" });
	auto const v2 = std::make_shared<V2>(V2{ "test2" });

	V_class.reference_external(isolate, v);
	V2_class.reference_external(isolate, v2);

	variant_check<U, std::shared_ptr<V>, int, std::string, U2, std::shared_ptr<V2>> check{ isolate };
	check(U{2}, v, -1, "Hello", U2{3.}, v2);

	variant_check<bool, float, int32_t> check_arithmetic{ isolate };
	check_arithmetic(true, 5.5f, 2);
	check_arithmetic(false, 1.1f, 0);

	variant_check<int32_t, float, bool> check_arithmetic_reversed{ isolate };
	check_arithmetic_reversed(2, 5.5f, true);
	check_arithmetic_reversed(-2, 2.2f, false);

	variant_check<std::vector<float>, float, std::optional<std::string>> check_vector{ isolate };
	check_vector({1.f, 2.f, 3.f}, 4.f, std::optional<std::string>("testing"));
	check_vector(std::vector<float>{}, 0.f, std::optional<std::string>{});

	// The order here matters
	variant_check<int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t, /*int64_t, uint64_t,*/ float, double> order_check{ isolate };
	order_check(
		std::numeric_limits<int8_t>::min(), std::numeric_limits<uint8_t>::max(),
		std::numeric_limits<int16_t>::min(), std::numeric_limits<uint16_t>::max(),
		std::numeric_limits<int32_t>::min(), std::numeric_limits<uint32_t>::max(),
		//TODO: V8_MIN_INT, V8_MAX_INT,
		std::numeric_limits<float>::lowest(), std::numeric_limits<double>::max());

	variant_check<bool, int8_t> simple_arithmetic{ isolate };
	simple_arithmetic.check_ex(std::numeric_limits<uint32_t>::max()); // does not fit into int8_t
	simple_arithmetic.check_ex(1.5); // is not integral
	simple_arithmetic.check_ex(v); // is not arithmetic

	variant_check<U, std::shared_ptr<V>, std::vector<float>> objects_only{ isolate };
	objects_only.check_ex(true);
	objects_only.check_ex(std::string{ "test" });
	objects_only.check_ex(1.);

	// Note: Not all values of uint64_t/int64_t are possible since v8 stores numeric values as doubles
	check_ranges<int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t, uint64_t>(isolate);

	// test map
	variant_check<U, std::map<size_t, U>> map_check{ isolate };
	map_check(U{3}, std::map<size_t, U>{ { 4, U{4} }, { 2, U{2} } });

	variant_check<U, std::unordered_map<int, U2>> unordered_map_check{ isolate };
	unordered_map_check(U{1}, std::unordered_map<int, U2>{ { 1, U2{1.0} }, { 2, U2{2.0} } });

	variant_check<U, std::multimap<std::string, U>> multimap_check{ isolate };
	multimap_check(U{2}, std::multimap<std::string, U>{ { "x", U{0} }, { "y", U{1} } });

	variant_check<U2, std::unordered_multimap<char, U>> unordered_multimap_check{ isolate };
	unordered_multimap_check(U2{3.0}, std::unordered_multimap<char, U>{ { 'a', U{1} }, { 'b', U{2} } });

	variant_check<int, std::optional<std::string>, bool> optional_check{ isolate };
	optional_check(true, "test", 1);
	optional_check(0, std::optional<std::string>{}, false);
}

void test_convert_crash_safety(v8::Isolate* isolate)
{
	// from_v8<int> with non-numeric types should throw, not crash
	check_ex<v8pp::invalid_argument>("from_v8<int> undefined", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8::Undefined(isolate));
	});
	check_ex<v8pp::invalid_argument>("from_v8<int> null", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8::Null(isolate));
	});
	check_ex<v8pp::invalid_argument>("from_v8<int> string", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8pp::to_v8(isolate, "hello"));
	});
	check_ex<v8pp::invalid_argument>("from_v8<int> bool", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8pp::to_v8(isolate, true));
	});
	check_ex<v8pp::invalid_argument>("from_v8<int> object", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8::Object::New(isolate));
	});
	check_ex<v8pp::invalid_argument>("from_v8<int> empty handle", [isolate]()
	{
		v8pp::from_v8<int>(isolate, v8::Local<v8::Value>());
	});

	// from_v8<uint32_t> with non-numeric
	check_ex<v8pp::invalid_argument>("from_v8<uint32_t> string", [isolate]()
	{
		v8pp::from_v8<uint32_t>(isolate, v8pp::to_v8(isolate, "hello"));
	});

	// from_v8<double> with non-numeric
	check_ex<v8pp::invalid_argument>("from_v8<double> string", [isolate]()
	{
		v8pp::from_v8<double>(isolate, v8pp::to_v8(isolate, "hello"));
	});
	check_ex<v8pp::invalid_argument>("from_v8<double> undefined", [isolate]()
	{
		v8pp::from_v8<double>(isolate, v8::Undefined(isolate));
	});

	// from_v8<bool> with non-boolean
	check_ex<v8pp::invalid_argument>("from_v8<bool> int", [isolate]()
	{
		v8pp::from_v8<bool>(isolate, v8pp::to_v8(isolate, 42));
	});
	check_ex<v8pp::invalid_argument>("from_v8<bool> string", [isolate]()
	{
		v8pp::from_v8<bool>(isolate, v8pp::to_v8(isolate, "hello"));
	});
	check_ex<v8pp::invalid_argument>("from_v8<bool> undefined", [isolate]()
	{
		v8pp::from_v8<bool>(isolate, v8::Undefined(isolate));
	});

	// from_v8<string> with empty handle
	check_ex<v8pp::invalid_argument>("from_v8<string> empty handle", [isolate]()
	{
		v8pp::from_v8<std::string>(isolate, v8::Local<v8::Value>());
	});

	// from_v8<string> with object that has throwing toString (Phase 1a fix)
	{
		v8::TryCatch try_catch(isolate);
		v8::Local<v8::Context> ctx = isolate->GetCurrentContext();
		v8::Local<v8::Object> throwing_obj = v8::Object::New(isolate);
		auto throwing_fn = v8::Function::New(ctx, [](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			args.GetIsolate()->ThrowException(
				v8pp::to_v8(args.GetIsolate(), "toString throws!"));
		}).ToLocalChecked();
		throwing_obj->Set(ctx, v8pp::to_v8(isolate, "toString"), throwing_fn).FromJust();

		check_ex<v8pp::invalid_argument>("from_v8<string> throwing toString", [isolate, &throwing_obj]()
		{
			v8pp::from_v8<std::string>(isolate, throwing_obj);
		});
	}

	// from_v8<vector<int>> with non-array
	check_ex<v8pp::invalid_argument>("from_v8<vector<int>> int", [isolate]()
	{
		v8pp::from_v8<std::vector<int>>(isolate, v8pp::to_v8(isolate, 42));
	});
	check_ex<v8pp::invalid_argument>("from_v8<vector<int>> undefined", [isolate]()
	{
		v8pp::from_v8<std::vector<int>>(isolate, v8::Undefined(isolate));
	});
	check_ex<v8pp::invalid_argument>("from_v8<vector<int>> string", [isolate]()
	{
		v8pp::from_v8<std::vector<int>>(isolate, v8pp::to_v8(isolate, "hello"));
	});

	// from_v8<map> with non-object
	check_ex<v8pp::invalid_argument>("from_v8<map> int", [isolate]()
	{
		v8pp::from_v8<std::map<std::string, int>>(isolate, v8pp::to_v8(isolate, 42));
	});
	check_ex<v8pp::invalid_argument>("from_v8<map> array", [isolate]()
	{
		v8pp::from_v8<std::map<std::string, int>>(isolate, v8::Array::New(isolate));
	});
	check_ex<v8pp::invalid_argument>("from_v8<map> undefined", [isolate]()
	{
		v8pp::from_v8<std::map<std::string, int>>(isolate, v8::Undefined(isolate));
	});

	// from_v8 with default value — should return default on type mismatch, not crash
	check_eq("from_v8<int> default on undefined",
		v8pp::from_v8<int>(isolate, v8::Undefined(isolate), -1), -1);
	check_eq("from_v8<int> default on string",
		v8pp::from_v8<int>(isolate, v8pp::to_v8(isolate, "hello"), -1), -1);
	check_eq("from_v8<int> default on null",
		v8pp::from_v8<int>(isolate, v8::Null(isolate), -1), -1);
	check_eq("from_v8<bool> default on int",
		v8pp::from_v8<bool>(isolate, v8pp::to_v8(isolate, 42), false), false);
	check_eq("from_v8<double> default on string",
		v8pp::from_v8<double>(isolate, v8pp::to_v8(isolate, "hello"), -1.0), -1.0);

	// Enum with non-numeric should throw
	enum class color { red = 0, green = 1, blue = 2 };
	check_ex<v8pp::invalid_argument>("from_v8<enum> string", [isolate]()
	{
		v8pp::from_v8<color>(isolate, v8pp::to_v8(isolate, "red"));
	});
	check_ex<v8pp::invalid_argument>("from_v8<enum> undefined", [isolate]()
	{
		v8pp::from_v8<color>(isolate, v8::Undefined(isolate));
	});
}

void test_convert_try_from_v8(v8::Isolate* isolate)
{
	// Primitives: valid conversions return value
	auto int_result = v8pp::try_from_v8<int>(isolate, v8pp::to_v8(isolate, 42));
	check("try int valid", int_result.has_value());
	check_eq("try int value", *int_result, 42);

	auto uint_result = v8pp::try_from_v8<uint32_t>(isolate, v8pp::to_v8(isolate, 100u));
	check("try uint valid", uint_result.has_value());
	check_eq("try uint value", *uint_result, 100u);

	auto double_result = v8pp::try_from_v8<double>(isolate, v8pp::to_v8(isolate, 3.14));
	check("try double valid", double_result.has_value());
	check_eq("try double value", *double_result, 3.14);

	auto bool_result = v8pp::try_from_v8<bool>(isolate, v8pp::to_v8(isolate, true));
	check("try bool valid", bool_result.has_value());
	check_eq("try bool value", *bool_result, true);

	// Primitives: type mismatch returns nullopt
	check("try int from string", !v8pp::try_from_v8<int>(isolate, v8pp::to_v8(isolate, "hello")));
	check("try int from bool", !v8pp::try_from_v8<int>(isolate, v8pp::to_v8(isolate, true)));
	check("try bool from int", !v8pp::try_from_v8<bool>(isolate, v8pp::to_v8(isolate, 42)));
	check("try int from undefined", !v8pp::try_from_v8<int>(isolate, v8::Undefined(isolate)));
	check("try int from null", !v8pp::try_from_v8<int>(isolate, v8::Null(isolate)));
	check("try int from empty", !v8pp::try_from_v8<int>(isolate, v8::Local<v8::Value>()));

	// Strings: valid conversion
	auto str_result = v8pp::try_from_v8<std::string>(isolate, v8pp::to_v8(isolate, "hello"));
	check("try string valid", str_result.has_value());
	check_eq("try string value", *str_result, std::string("hello"));

	// Strings: any non-empty value converts to string (via toString)
	auto str_from_int = v8pp::try_from_v8<std::string>(isolate, v8pp::to_v8(isolate, 42));
	check("try string from int", str_from_int.has_value());
	check_eq("try string from int value", *str_from_int, std::string("42"));

	// Strings: empty handle returns nullopt
	check("try string from empty", !v8pp::try_from_v8<std::string>(isolate, v8::Local<v8::Value>()));

	// Enums
	enum class color { red = 0, green = 1, blue = 2 };
	auto enum_result = v8pp::try_from_v8<color>(isolate, v8pp::to_v8(isolate, 1));
	check("try enum valid", enum_result.has_value());
	check_eq("try enum value", *enum_result, color::green);
	check("try enum from string", !v8pp::try_from_v8<color>(isolate, v8pp::to_v8(isolate, "red")));

	// Sequences
	auto vec_result = v8pp::try_from_v8<std::vector<int>>(isolate,
		v8pp::to_v8(isolate, std::vector<int>{1, 2, 3}));
	check("try vector valid", vec_result.has_value());
	check_eq("try vector value", *vec_result, std::vector<int>({1, 2, 3}));
	check("try vector from int", !v8pp::try_from_v8<std::vector<int>>(isolate, v8pp::to_v8(isolate, 42)));

	// Maps
	check("try map from int", !v8pp::try_from_v8<std::map<std::string, int>>(isolate, v8pp::to_v8(isolate, 42)));

	// Tuples
	auto tuple_val = std::tuple<int, bool>{42, true};
	auto tuple_result = v8pp::try_from_v8<std::tuple<int, bool>>(isolate, v8pp::to_v8(isolate, tuple_val));
	check("try tuple valid", tuple_result.has_value());
	check_eq("try tuple value", *tuple_result, tuple_val);
	check("try tuple from int", !v8pp::try_from_v8<std::tuple<int, bool>>(isolate, v8pp::to_v8(isolate, 42)));

	// Optional: undefined/null returns optional containing nullopt
	auto opt_undef = v8pp::try_from_v8<std::optional<int>>(isolate, v8::Undefined(isolate));
	check("try optional undef valid", opt_undef.has_value());
	check("try optional undef is nullopt", !opt_undef->has_value());

	auto opt_val = v8pp::try_from_v8<std::optional<int>>(isolate, v8pp::to_v8(isolate, 42));
	check("try optional<int> valid", opt_val.has_value());
	check("try optional<int> has value", opt_val->has_value());
	check_eq("try optional<int> value", **opt_val, 42);

	// Optional: wrong type returns outer nullopt
	check("try optional<int> from string",
		!v8pp::try_from_v8<std::optional<int>>(isolate, v8pp::to_v8(isolate, "hello")));

	// Wrapped class: valid unwrap (class_<U> already registered by test_convert_variant)
	U u_obj{42};
	auto u_v8 = v8pp::class_<U, v8pp::raw_ptr_traits>::reference_external(isolate, &u_obj);

	auto u_ptr_result = v8pp::try_from_v8<U*>(isolate, u_v8);
	check("try U* valid", u_ptr_result.has_value());
	check_eq("try U* value", (*u_ptr_result)->value, 42);

	// Wrapped class: wrong object type returns nullopt
	check("try U* from int", !v8pp::try_from_v8<U*>(isolate, v8pp::to_v8(isolate, 42)));
	check("try U* from plain object", !v8pp::try_from_v8<U*>(isolate, v8::Object::New(isolate)));

	// Wrapped class via shared_ptr (class_<V> already registered by test_convert_variant)
	auto v_obj = std::make_shared<V>(V{"test"});
	v8pp::class_<V, v8pp::shared_ptr_traits>::reference_external(isolate, v_obj);
	auto v_v8 = v8pp::class_<V, v8pp::shared_ptr_traits>::find_object(isolate, v_obj);

	auto v_result = v8pp::try_from_v8<std::shared_ptr<V>>(isolate, v_v8);
	check("try shared_ptr<V> valid", v_result.has_value());
	check_eq("try shared_ptr<V> value", (*v_result)->value, std::string("test"));

	check("try shared_ptr<V> from int", !v8pp::try_from_v8<std::shared_ptr<V>>(isolate, v8pp::to_v8(isolate, 42)));
}

void test_convert()
{
	v8pp::context context;
	v8::Isolate* isolate = context.isolate();
	v8::HandleScope scope(isolate);

	test_conv(isolate, 1);
	test_conv(isolate, 2.2);
	test_conv(isolate, true);

	enum old_enum { A = 1, B = 5, C = -1 };
	test_conv(isolate, B);

	enum class new_enum { X = 'a', Y = 'b', Z = 'c' };
	test_conv(isolate, new_enum::Z);

	test_string_conv(isolate, "qaz");
	test_string_conv(isolate, u"qaz");
#ifdef WIN32
	test_string_conv(isolate, L"qaz");
#endif
	// numeric string
	test_string_conv(isolate, "0");

	const std::vector<int> vector{ 1, 2, 3 };
	test_conv(isolate, vector);
	test_conv(isolate, std::deque<unsigned>{ 1, 2, 3 }, vector);
	test_conv(isolate, std::list<int>{ 1, 2, 3 }, vector);

	const std::array<int, 3> array{ 1, 2, 3 };
	test_conv(isolate, array);
	check_ex<std::runtime_error>("wrong array length", [isolate, &array]()
	{
		v8::Local<v8::Array> arr = v8pp::to_v8(isolate, array);
		v8pp::from_v8<std::array<int, 2>>(isolate, arr);
	});

	test_conv(isolate, std::map<char, int>{ { 'a', 1 }, { 'b', 2 }, { 'c', 3 } });
	test_conv(isolate, std::multimap<int, int>{ { 1, -1 }, { 2, -2 } });
	test_conv(isolate, std::unordered_map<char, std::string>{ { 'x', "1" }, { 'y', "2" } });
	test_conv(isolate, std::unordered_multimap<std::string, int>{ { "0", 0 }, { "a", 1 }, { "b", 2 } });

	check_eq("initializer list to array",
		v8pp::from_v8<std::vector<int>>(isolate, v8pp::to_v8(isolate, { 1, 2, 3 })), vector);

	std::list<int> list = { 1, 2, 3 };
	check_eq("pair of iterators to array",
		v8pp::from_v8<std::vector<int>>(isolate, v8pp::to_v8(isolate, list.begin(), list.end())), vector);

	test_convert_user_type(isolate);
	test_convert_optional(isolate);
	test_convert_tuple(isolate);
	test_convert_variant(isolate);
	test_convert_crash_safety(isolate);
	test_convert_try_from_v8(isolate);
}
