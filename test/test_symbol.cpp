#include "v8pp/class.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

namespace {

struct Widget
{
	int value = 42;
};

struct NumericValue
{
	double val;
	explicit NumericValue(double v)
		: val(v) {}
	double to_number(std::string_view) const { return val; }
};

struct Tag
{
	std::string name;
	explicit Tag(std::string n)
		: name(std::move(n)) {}
};

struct NumberList
{
	std::vector<int> numbers;

	auto begin() const { return numbers.begin(); }
	auto end() const { return numbers.end(); }
};

struct WordList
{
	std::vector<std::string> words;

	auto begin() const { return words.begin(); }
	auto end() const { return words.end(); }
};

} // namespace

void test_symbol()
{
	// Test to_string_tag
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<Widget> widget_class(isolate);
		widget_class
			.ctor<>()
			.var("value", &Widget::value)
			.to_string_tag("Widget");

		context.class_("Widget", widget_class);

		check_eq("to_string_tag",
			run_script<std::string>(context,
				"let w = new Widget(); Object.prototype.toString.call(w)"),
			"[object Widget]");
	}

	// Test to_primitive with member function
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<NumericValue> nv_class(isolate);
		nv_class
			.ctor<double>()
			.to_primitive(&NumericValue::to_number);

		context.class_("NumericValue", nv_class);

		check_eq("to_primitive +",
			run_script<double>(context, "let nv = new NumericValue(10); nv + 5"),
			15.0);

		check_eq("to_primitive *",
			run_script<double>(context, "nv * 3"),
			30.0);
	}

	// Test to_primitive with lambda
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<Tag> tag_class(isolate);
		tag_class
			.ctor<std::string>()
			.var("name", &Tag::name)
			.to_primitive([](Tag const& t, std::string_view) -> std::string
				{ return t.name; });

		context.class_("Tag", tag_class);

		check_eq("to_primitive string concat",
			run_script<std::string>(context,
				"let t = new Tag('hello'); '' + t"),
			"hello");
	}

	// Test iterable with int vector (member begin/end)
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<NumberList> nl_class(isolate);
		nl_class
			.ctor<>()
			.iterable(&NumberList::begin, &NumberList::end);

		context.class_("NumberList", nl_class);

		auto* nl = new NumberList{ { 1, 2, 3, 4, 5 } };
		auto nl_obj = v8pp::class_<NumberList>::import_external(isolate, nl);
		v8pp::set_option(isolate, context.isolate()->GetCurrentContext()->Global(), "nl", nl_obj);

		check_eq("for...of sum",
			run_script<int>(context,
				"let sum = 0; for (const n of nl) sum += n; sum"),
			15);

		check_eq("spread to array",
			run_script<std::string>(context,
				"JSON.stringify([...nl])"),
			"[1,2,3,4,5]");

		check_eq("Array.from",
			run_script<int>(context,
				"Array.from(nl).length"),
			5);
	}

	// Test iterable with string vector
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<WordList> wl_class(isolate);
		wl_class
			.ctor<>()
			.iterable(&WordList::begin, &WordList::end);

		context.class_("WordList", wl_class);

		auto* wl = new WordList{ { "hello", "world" } };
		auto wl_obj = v8pp::class_<WordList>::import_external(isolate, wl);
		v8pp::set_option(isolate, context.isolate()->GetCurrentContext()->Global(), "wl", wl_obj);

		check_eq("string iterable",
			run_script<std::string>(context,
				"let parts = []; for (const w of wl) parts.push(w); parts.join(' ')"),
			"hello world");
	}

	// Test iterable with empty container
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<NumberList> nl_class(isolate);
		nl_class
			.ctor<>()
			.iterable(&NumberList::begin, &NumberList::end);

		context.class_("NumberList", nl_class);

		auto* nl = new NumberList{ {} };
		auto nl_obj = v8pp::class_<NumberList>::import_external(isolate, nl);
		v8pp::set_option(isolate, context.isolate()->GetCurrentContext()->Global(), "empty_nl", nl_obj);

		check_eq("empty iterable",
			run_script<int>(context,
				"let count = 0; for (const n of empty_nl) count++; count"),
			0);
	}

	// Test iterable with lambda begin/end
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<NumberList> nl_class(isolate);
		nl_class
			.ctor<>()
			.iterable(
				[](NumberList const& nl)
				{ return nl.numbers.begin(); },
				[](NumberList const& nl)
				{ return nl.numbers.end(); });

		context.class_("NumberList", nl_class);

		auto* nl = new NumberList{ { 10, 20, 30 } };
		auto nl_obj = v8pp::class_<NumberList>::import_external(isolate, nl);
		v8pp::set_option(isolate, context.isolate()->GetCurrentContext()->Global(), "nl2", nl_obj);

		check_eq("lambda iterable",
			run_script<int>(context,
				"let s = 0; for (const n of nl2) s += n; s"),
			60);
	}

	// Test combined: to_string_tag + iterable
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		v8pp::class_<NumberList> nl_class(isolate);
		nl_class
			.ctor<>()
			.to_string_tag("NumberList")
			.iterable(&NumberList::begin, &NumberList::end);

		context.class_("NumberList", nl_class);

		auto* nl = new NumberList{ { 1, 2 } };
		auto nl_obj = v8pp::class_<NumberList>::import_external(isolate, nl);
		v8pp::set_option(isolate, context.isolate()->GetCurrentContext()->Global(), "nl3", nl_obj);

		check_eq("tag + iterable tag",
			run_script<std::string>(context,
				"Object.prototype.toString.call(nl3)"),
			"[object NumberList]");

		check_eq("tag + iterable spread",
			run_script<int>(context,
				"[...nl3].reduce((a, b) => a + b, 0)"),
			3);
	}
}
