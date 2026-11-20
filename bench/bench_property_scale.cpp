#include "bench.hpp"
#include "v8pp/class.hpp"

#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
#include "v8pp/dispatch_diagnostics.hpp"
#endif

#include <memory>
#include <stdexcept>

namespace {

constexpr std::string_view short_label = "Tab ";
constexpr std::string_view long_label = "Show advanced option ";
constexpr int reads = 1'000'000;

using clock_type = std::chrono::steady_clock;

struct scale_measurement
{
	int checksum;
	v8pp::bench::result samples{ "scaled property reads", reads, {} };
	clock_type::duration setup;
	clock_type::duration forced_gc;
	clock_type::time_point teardown_start;
#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
	v8pp::detail::dispatch_counts dispatch;
#endif
};

struct Control
{
	explicit Control(int initial)
		: value(initial) {}
	int get_value() const { return value; }
	void set_value(int next) { value = next; }
	int operation() { return value; }
	int value;
};

struct LabeledControl
{
	explicit LabeledControl(int index)
		: label(std::string(index % 2 == 0 ? short_label : long_label) + std::to_string(index)) {}
	std::string const& get_label() const { return label; }
	void set_label(std::string const& next) { label = next; }
	std::string label;
};

template<typename Base>
struct DerivedControl : Base
{
	using Base::Base;
};

struct ParentControl
{
	explicit ParentControl(std::shared_ptr<LabeledControl> child)
		: child(std::move(child)) {}
	std::shared_ptr<LabeledControl> get_child() const { return child; }
	std::shared_ptr<LabeledControl> child;
};

template<typename Base, typename Configure>
void register_controls(v8pp::context& context, Configure configure)
{
	v8pp::shared_class<Base> base(context.isolate());
	base.template ctor<int>();
	configure(base);
	v8pp::shared_class<DerivedControl<Base>> child(context.isolate());
	child.template ctor<int>().template inherit<Base>();
	context.class_("Control", base);
	context.class_("DerivedControl", child);
}

scale_measurement measure_property_scale(int count, std::string_view receiver, std::string_view order, std::string_view property)
{
	auto const setup_start = clock_type::now();
	auto context = v8pp::bench::make_context();
	auto* isolate = context.isolate();
	v8::HandleScope handles(isolate);
	bool const labels = property == "label";
	bool const returns_child = property == "child" || property == "child-label";
	bool const member_call = property == "member" || property == "fast-member";
	if (labels || returns_child)
		register_controls<LabeledControl>(context, [](auto& base)
		{
			base.property("label", &LabeledControl::get_label, &LabeledControl::set_label);
		});
	else
		register_controls<Control>(context, [&](auto& base)
		{
			if (property == "fast-member")
				base.function("operation", v8pp::fast_fn<&Control::operation>);
			else if (property == "member")
				base.function("operation", &Control::operation);
			else if (property == "fast-value")
				base.property("value", v8pp::fast_fn<&Control::get_value>, &Control::set_value);
			else
				base.property("value", &Control::get_value, &Control::set_value);
		});
	if (returns_child)
	{
		v8pp::shared_class<ParentControl> parent(isolate);
		parent.ctor<std::shared_ptr<LabeledControl>>().property("child", &ParentControl::get_child);
		v8pp::shared_class<DerivedControl<ParentControl>> derived(isolate);
		derived.ctor<std::shared_ptr<LabeledControl>>().inherit<ParentControl>();
		context.class_("Parent", parent);
		context.class_("DerivedParent", derived);
	}
	context.value("count", v8pp::to_v8(isolate, count));
	context.value("shape", v8pp::to_v8(isolate, std::string(receiver)));
	context.value("labels", v8pp::to_v8(isolate, labels));

	// Keep the order and property choices outside the timed JavaScript loop.
	std::string source = "(() => {";
	if (returns_child)
		source += R"JS(
            const useDerived = i => shape === 'derived' || (shape === 'mixed' && i % 2 === 1);
            const children = Array.from({ length: count }, (_, i) =>
                useDerived(i) ? new DerivedControl(i) : new Control(i));
            const controls = children.map((child, i) =>
                useDerived(i) ? new DerivedParent(child) : new Parent(child));
            // Keep the original JS wrappers live while the native owners return their children.
            globalThis.benchmarkChildren = children;
            for (let i = 0; i < count; ++i) {
                if (controls[i].child !== children[i])
                    throw new Error('The native getter did not return the registered child');
            }
        )JS";
	else
		source += R"JS(
            const controls = Array.from({ length: count }, (_, i) => {
                const Type = shape === 'derived' || (shape === 'mixed' && i % 2) ? DerivedControl : Control;
                return new Type(labels ? i : (i * 13 + 17) & 1023);
            });
        )JS";
	source += R"JS(
            const indices = Uint32Array.from({ length: count }, (_, i) => i);
            let state = 0x1234567;
            for (let i = count - 1; i > 0; --i) {
                state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
                const j = state % (i + 1);
                [indices[i], indices[j]] = [indices[j], indices[i]];
            }
            return passes => {
                let sum = 0;
                for (let pass = 0; pass < passes; ++pass) {
                    for (let i = 0; i < controls.length; ++i) {
                        sum += )JS";
	std::string const index = order == "shuffled" ? "indices[i]" : "i";
	source += "controls[" + index + "]";
	if (property == "child")
		source += ".child === children[" + index + "]";
	else if (property == "child-label")
		source += ".child.label.length";
	else if (member_call)
		source += ".operation()";
	else
		source += labels ? ".label.length" : ".value";
	source += R"JS(;
                    }
                }
                return sum;
            };
        })()
    )JS";

	v8::TryCatch caught(isolate);
	auto runner = context.run_script(source);
	if (runner.IsEmpty() || !runner->IsFunction())
		throw std::runtime_error("Could not construct the property benchmark population");
	auto const gc_start = clock_type::now();
	isolate->LowMemoryNotification();
	auto const gc_finish = clock_type::now();

	int const passes = reads / count;
	int sum = 0;
	for (int i = 0; i < count; ++i)
	{
		if (property == "child")
			++sum;
		else if (labels || property == "child-label")
			sum += static_cast<int>(std::to_string(i).size() + (i % 2 == 0 ? short_label.size() : long_label.size()));
		else
			sum += (i * 13 + 17) & 1023;
	}
	int const expected = sum * passes;

	scale_measurement measurement{.checksum = expected, .setup = gc_start - setup_start, .forced_gc = gc_finish - gc_start};
	for (int sample = 0; sample < 9; ++sample)
	{
#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
		if (sample == 2)
			v8pp::detail::reset_dispatch_counts();
#endif
		v8::HandleScope scope(isolate);
		v8::Local<v8::Value> argument = v8::Int32::New(isolate, passes);
		v8::Local<v8::Value> actual;
		auto start = clock_type::now();
		bool ok = runner.As<v8::Function>()->Call(isolate->GetCurrentContext(),
											   v8::Undefined(isolate), 1, &argument)
					  .ToLocal(&actual);
		auto stop = clock_type::now();
		if (!ok || caught.HasCaught() || !actual->IsNumber() || actual.As<v8::Number>()->Value() != expected)
			throw std::runtime_error("Property benchmark failed its checksum");
		if (sample >= 2)
			measurement.samples.samples_ns.push_back(std::chrono::duration<double, std::nano>(stop - start).count() / reads);
	}
#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
	measurement.dispatch = v8pp::detail::read_dispatch_counts();
#endif
	measurement.teardown_start = clock_type::now();
	return measurement;
}

} // namespace

void bench_property_scale(int count, std::string_view receiver, std::string_view order, std::string_view property)
{
	auto const measurement = measure_property_scale(count, receiver, order, property);
	auto const teardown = clock_type::now() - measurement.teardown_start;
	using milliseconds = std::chrono::duration<double, std::milli>;

	std::cout << "objects,receiver,order,property,reads_per_sample,warmups,samples,checksum,min_ns,median_ns,max_ns,setup_ms,forced_gc_ms,teardown_ms\n"
			  << count << ',' << receiver << ',' << order << ',' << property
			  << ',' << reads << ",2,7," << measurement.checksum << ',' << measurement.samples.min_ns() << ','
			  << measurement.samples.median_ns() << ',' << measurement.samples.max_ns() << ','
			  << milliseconds(measurement.setup).count() << ',' << milliseconds(measurement.forced_gc).count() << ','
			  << milliseconds(teardown).count() << '\n';
#if V8PP_ENABLE_DISPATCH_DIAGNOSTICS
	auto const& counts = measurement.dispatch;
	std::cout << "receiver_checks,expected_class_lookups,derived_registry_probes,shared_owner_copies,string_conversions,checked_member_entries,optimized_member_entries\n"
			  << counts.receiver_checks << ',' << counts.expected_class_lookups << ',' << counts.derived_registry_probes << ','
			  << counts.shared_owner_copies << ',' << counts.string_conversions << ','
			  << counts.checked_member_entries << ',' << counts.optimized_member_entries << '\n';
#endif
}
