#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include <v8.h>

#include "v8pp/config.hpp"

namespace v8pp::detail {

class registration_lifetime
{
public:
	virtual ~registration_lifetime() = default;
	virtual bool idle() const noexcept = 0;
};

struct native_dispatch_state
{
	explicit native_dispatch_state(v8::Isolate* isolate) : isolate(isolate) {}

	static native_dispatch_state* get(v8::Isolate* isolate);
	static native_dispatch_state& ensure(v8::Isolate* isolate);
	void drain();
	void release_if_idle();

	v8::Isolate* isolate;
	std::unique_ptr<registration_lifetime> registrations;
	std::vector<std::function<void()>> pending_destructions;
	std::exception_ptr deferred_error;
	size_t callback_records = 0;
	size_t depth = 0;
	bool draining = false;
	bool disposing = false;
};

class native_dispatch_scope
{
public:
	explicit native_dispatch_scope(v8::Isolate* isolate);
	~native_dispatch_scope();

	native_dispatch_scope(native_dispatch_scope const&) = delete;
	native_dispatch_scope& operator=(native_dispatch_scope const&) = delete;

private:
	native_dispatch_state& state_;
};

class native_disposal_scope
{
public:
	explicit native_disposal_scope(v8::Isolate* isolate);
	~native_disposal_scope();

	native_disposal_scope(native_disposal_scope const&) = delete;
	native_disposal_scope& operator=(native_disposal_scope const&) = delete;

private:
	native_dispatch_state& state_;
};

void check_native_publication(v8::Isolate* isolate);

// Weak callbacks queue destruction here. The isolate owner runs the queue
// outside GC and while a context is available to reentrant native destructors.
void defer_native_destruction(v8::Isolate* isolate, std::function<void()> destroy);
void drain_native_destructions(v8::Isolate* isolate);
void dispose_native_registrations(v8::Isolate* isolate, std::exception_ptr* deferred_error = nullptr);

} // namespace v8pp::detail

namespace v8pp {

/// Run native destruction queued by V8 collection. Call from the isolate
/// owner with its lock held and a context entered, after GC or message pumping.
using detail::drain_native_destructions;

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/native_dispatch.ipp"
#endif
