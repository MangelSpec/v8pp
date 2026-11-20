#include "v8pp/native_dispatch.hpp"

#include <stdexcept>
#include <utility>

namespace v8pp::detail {

V8PP_IMPL native_dispatch_state* native_dispatch_state::get(v8::Isolate* isolate)
{
	return static_cast<native_dispatch_state*>(isolate->GetData(V8PP_ISOLATE_DATA_SLOT));
}

V8PP_IMPL native_dispatch_state& native_dispatch_state::ensure(v8::Isolate* isolate)
{
	auto* state = get(isolate);
	if (!state)
	{
		state = new native_dispatch_state(isolate);
		isolate->SetData(V8PP_ISOLATE_DATA_SLOT, state);
	}
	return *state;
}

V8PP_IMPL void native_dispatch_state::drain()
{
	if (depth || draining || pending_destructions.empty() || !isolate->InContext()) return;
	v8::HandleScope scope(isolate);
	draining = true;
	while (!pending_destructions.empty())
	{
		auto pending = std::exchange(pending_destructions, {});
		for (auto& destroy : pending)
		{
			try
			{
				destroy();
			}
			catch (...)
			{
				if (!deferred_error) deferred_error = std::current_exception();
			}
			destroy = {};
		}
	}
	draining = false;
}

V8PP_IMPL void native_dispatch_state::release_if_idle()
{
	if (!depth && !draining && !disposing && !deferred_error && !callback_records &&
		pending_destructions.empty() && (!registrations || registrations->idle()))
	{
		isolate->SetData(V8PP_ISOLATE_DATA_SLOT, nullptr);
		delete this;
	}
}

V8PP_IMPL native_dispatch_scope::native_dispatch_scope(v8::Isolate* isolate)
	: state_(native_dispatch_state::ensure(isolate))
{
	++state_.depth;
}

V8PP_IMPL native_dispatch_scope::~native_dispatch_scope()
{
	if (--state_.depth == 0)
	{
		state_.drain();
		state_.release_if_idle();
	}
}

V8PP_IMPL native_disposal_scope::native_disposal_scope(v8::Isolate* isolate)
	: state_(native_dispatch_state::ensure(isolate))
{
	if (state_.depth || state_.draining || state_.disposing)
		throw std::runtime_error("cannot dispose bindings during native dispatch or destruction");
	state_.disposing = true;
}

V8PP_IMPL native_disposal_scope::~native_disposal_scope()
{
	state_.disposing = false;
	state_.release_if_idle();
}

V8PP_IMPL void check_native_publication(v8::Isolate* isolate)
{
	if (auto* state = native_dispatch_state::get(isolate); state && state->disposing)
		throw std::runtime_error("cannot register a native binding during final disposal");
}

V8PP_IMPL void defer_native_destruction(v8::Isolate* isolate, std::function<void()> destroy)
{
	native_dispatch_state::ensure(isolate).pending_destructions.push_back(std::move(destroy));
}

V8PP_IMPL void drain_native_destructions(v8::Isolate* isolate)
{
	if (auto* state = native_dispatch_state::get(isolate))
	{
		if (state->depth || state->draining) return;
		state->drain();
		auto error = std::exchange(state->deferred_error, {});
		state->release_if_idle();
		if (error) std::rethrow_exception(error);
	}
}

} // namespace v8pp::detail
