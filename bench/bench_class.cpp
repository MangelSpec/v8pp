#include <cmath>

#include "v8pp/class.hpp"
#include "v8pp/context.hpp"
#include "v8pp/module.hpp"
#include "bench.hpp"

namespace {

struct Point
{
    double x, y;
    Point() : x(0), y(0) {}
    Point(double x, double y) : x(x), y(y) {}
    double length() const { return std::sqrt(x * x + y * y); }
    double dot(Point const& other) const { return x * other.x + y * other.y; }
    void translate(double dx, double dy) { x += dx; y += dy; }
};

struct Base
{
    int value;
    explicit Base(int v) : value(v) {}
    int get_value() const { return value; }
};

struct Derived : Base
{
    int extra;
    explicit Derived(int v) : Base(v), extra(v * 2) {}
    int get_extra() const { return extra; }
};

} // anonymous namespace

void bench_class()
{
    v8pp::context context;
    v8::Isolate* isolate = context.isolate();
    v8::HandleScope scope(isolate);

    using namespace v8pp::bench;
    size_t const N = 10000;
    size_t const S = 20;

    // --- Point class: construction, methods, var access ---
    {
        v8pp::class_<Point> point_class(isolate);
        point_class
            .ctor<double, double>()
            .var("x", &Point::x)
            .var("y", &Point::y)
            .function("length", &Point::length)
            .function("dot", &Point::dot)
            .function("translate", &Point::translate);
        context.class_("Point", point_class);

        // Object construction from JS
        print_result(run_script_bench("class: new Point(x,y)",
            context, "var p = new Point(3.0, 4.0); p.x", N, S));

        // Set up a persistent object for method/var benchmarks
        context.run_script("var pt = new Point(3.0, 4.0)");

        print_result(run_script_bench("class: pt.length()",
            context, "pt.length()", N, S));

        print_result(run_script_bench("class: pt.dot(pt)",
            context, "pt.dot(pt)", N, S));

        print_result(run_script_bench("class: pt.translate(dx,dy)",
            context, "pt.translate(0.1, 0.1); pt.x", N, S));

        // Direct member variable access via var()
        print_result(run_script_bench("class: pt.x (get var)",
            context, "pt.x", N, S));

        print_result(run_script_bench("class: pt.x = val (set var)",
            context, "pt.x = 5.0", N, S));
    }

    // --- C++ side wrap/unwrap ---
    {
        v8pp::class_<Base> base_class(isolate);
        base_class
            .ctor<int>()
            .function("get_value", &Base::get_value);
        context.class_("Base", base_class);

        // Measure wrap cost from C++ side
        print_result(run("class: wrap_object (C++ side)", N, S, [&]()
        {
            v8::HandleScope hs(isolate);
            Base* obj = new Base(42);
            v8pp::class_<Base>::reference_external(isolate, obj);
            v8pp::class_<Base>::unreference_external(isolate, obj);
            delete obj;
        }));

        // Measure unwrap from a JS value
        context.run_script("var b = new Base(42)");
        v8::Local<v8::Value> b_val = context.run_script("b");
        print_result(run("class: unwrap_object (C++ side)", N, S, [&]()
        {
            v8pp::class_<Base>::unwrap_object(isolate, b_val);
        }));
    }

    // --- Inheritance ---
    {
        v8pp::class_<Derived> derived_class(isolate);
        derived_class
            .ctor<int>()
            .inherit<Base>()
            .function("get_extra", &Derived::get_extra);
        context.class_("Derived", derived_class);

        print_result(run_script_bench("class: new Derived (with inherit)",
            context, "var d = new Derived(10); d.get_value()", N, S));

        context.run_script("var dd = new Derived(10)");
        print_result(run_script_bench("class: base method via derived",
            context, "dd.get_value()", N, S));
    }

    // --- Bulk object creation (GC pressure) ---
    print_result(run("class: 100x new Point from JS", N / 100, S, [&]()
    {
        v8::HandleScope hs(isolate);
        v8::TryCatch try_catch(isolate);
        context.run_script("for (var i = 0; i < 100; i++) new Point(i, i)");
    }));
}
