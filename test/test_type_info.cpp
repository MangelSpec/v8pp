#include "v8pp/type_info.hpp"
#include "test.hpp"

struct some_struct {};
namespace test { class some_class {}; }
namespace { using other_class = test::some_class; }

void test_type_info()
{
	using v8pp::detail::type_id;

#if V8PP_PRETTIFY_TYPENAMES
	// When prettification is enabled, we get clean type names
	check_eq("type_id int", type_id<int>().name(), "int");
	check_eq("type_id bool", type_id<bool>().name(), "bool");
	check_eq("type_id some_struct", type_id<some_struct>().name(), "some_struct");
	check_eq("type_id test::some_class", type_id<test::some_class>().name(), "test::some_class");
	check_eq("type_id other_class", type_id<other_class>().name(), "test::some_class");
#else
	// When prettification is disabled, names are raw compiler output.
	// Verify names are non-empty and contain the type name substring.
	check("type_id<int> non-empty", !type_id<int>().name().empty());
	check("type_id<bool> non-empty", !type_id<bool>().name().empty());
	check("type_id<some_struct> non-empty", !type_id<some_struct>().name().empty());

	check("type_id<int> contains 'int'",
		type_id<int>().name().find("int") != std::string_view::npos);
	check("type_id<bool> contains 'bool'",
		type_id<bool>().name().find("bool") != std::string_view::npos);
	check("type_id<some_struct> contains 'some_struct'",
		type_id<some_struct>().name().find("some_struct") != std::string_view::npos);
	check("type_id<some_class> contains 'some_class'",
		type_id<test::some_class>().name().find("some_class") != std::string_view::npos);
#endif

	// These checks must pass regardless of V8PP_PRETTIFY_TYPENAMES

	// IDs are unique per type
	check("int != bool", type_id<int>() != type_id<bool>());
	check("int != some_struct", type_id<int>() != type_id<some_struct>());
	check("bool != some_struct", type_id<bool>() != type_id<some_struct>());
	check("some_struct != some_class", type_id<some_struct>() != type_id<test::some_class>());

	// IDs are stable (same call returns same ID)
	check("int == int", type_id<int>() == type_id<int>());
	check("some_struct == some_struct", type_id<some_struct>() == type_id<some_struct>());

	// Type alias produces same ID as original type
	check("other_class == some_class", type_id<other_class>() == type_id<test::some_class>());

	// ID values are non-zero
	check("int id nonzero", type_id<int>().id() != 0);
	check("bool id nonzero", type_id<bool>().id() != 0);
}
