#include <string>

#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/property.hpp"
#include "bench.hpp"

namespace {

struct Widget
{
    int width_ = 100;
    int height_ = 200;
    std::string name_ = "widget";

    int get_width() const { return width_; }
    void set_width(int w) { width_ = w; }

    int get_height() const { return height_; }
    void set_height(int h) { height_ = h; }

    std::string const& get_name() const { return name_; }
    void set_name(std::string const& n) { name_ = n; }
};

} // anonymous namespace

void bench_property()
{
    v8pp::context context;
    v8::Isolate* isolate = context.isolate();
    v8::HandleScope scope(isolate);

    using namespace v8pp::bench;
    size_t const N = 10000;
    size_t const S = 20;

    v8pp::class_<Widget> widget_class(isolate);
    widget_class
        .ctor<>()
        .property("width", &Widget::get_width, &Widget::set_width)
        .property("height", &Widget::get_height, &Widget::set_height)
        .property("name", &Widget::get_name, &Widget::set_name);
    context.class_("Widget", widget_class);

    context.run_script("var w = new Widget()");

    // --- Property getter (int) ---
    print_result(run_script_bench("property: get int (w.width)",
        context, "w.width", N, S));

    // --- Property setter (int) ---
    print_result(run_script_bench("property: set int (w.width = 42)",
        context, "w.width = 42", N, S));

    // --- Property getter (string) ---
    print_result(run_script_bench("property: get string (w.name)",
        context, "w.name", N, S));

    // --- Property setter (string) ---
    print_result(run_script_bench("property: set string (w.name = 'x')",
        context, "w.name = 'test'", N, S));

    // --- Property read in tight loop ---
    print_result(run_script_bench("property: 100x get in loop",
        context, "var s = 0; for (var i = 0; i < 100; i++) s += w.width; s",
        N / 10, S));

    // --- Property write in tight loop ---
    print_result(run_script_bench("property: 100x set in loop",
        context, "for (var i = 0; i < 100; i++) w.width = i; w.width",
        N / 10, S));
}
