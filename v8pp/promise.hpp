#pragma once

#include <string_view>

#include <v8.h>

#include "v8pp/convert.hpp"
#include "v8pp/throw_ex.hpp"

namespace v8pp {

/// Synchronous promise wrapper around v8::Promise::Resolver.
/// Resolve/reject must be called on the isolate's thread.
///
/// Usage:
///   v8pp::promise<int> make_value(v8::Isolate* isolate) {
///       v8pp::promise<int> p(isolate);
///       p.resolve(42);
///       return p;
///   }
///   module.function("makeValue", &make_value);
template<typename T>
class promise
{
public:
	explicit promise(v8::Isolate* isolate)
		: isolate_(isolate)
	{
		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver =
			v8::Promise::Resolver::New(context).ToLocalChecked();
		resolver_.Reset(isolate, resolver);
	}

	promise(promise const&) = delete;
	promise& operator=(promise const&) = delete;

	promise(promise&&) = default;
	promise& operator=(promise&&) = default;

	/// Resolve the promise with a C++ value (converted to V8 via convert<T>)
	void resolve(T const& value)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		resolver->Resolve(context, v8pp::to_v8(isolate_, value)).FromJust();
	}

	/// Reject the promise with an error message (creates a JS Error)
	void reject(std::string_view message)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		v8::Local<v8::Value> error = v8::Exception::Error(
			v8pp::to_v8(isolate_, message));
		resolver->Reject(context, error).FromJust();
	}

	/// Reject the promise with a raw V8 value
	void reject(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		resolver->Reject(context, value).FromJust();
	}

	/// Get the underlying v8::Promise (the "thenable" JS object)
	v8::Local<v8::Promise> get_promise() const
	{
		v8::EscapableHandleScope scope(isolate_);
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		return scope.Escape(resolver->GetPromise());
	}

	v8::Isolate* isolate() const { return isolate_; }

private:
	v8::Isolate* isolate_;
	v8::Global<v8::Promise::Resolver> resolver_;
};

/// Specialization for void promises (signal-only, no value)
template<>
class promise<void>
{
public:
	explicit promise(v8::Isolate* isolate)
		: isolate_(isolate)
	{
		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver =
			v8::Promise::Resolver::New(context).ToLocalChecked();
		resolver_.Reset(isolate, resolver);
	}

	promise(promise const&) = delete;
	promise& operator=(promise const&) = delete;

	promise(promise&&) = default;
	promise& operator=(promise&&) = default;

	/// Resolve the void promise (with undefined)
	void resolve()
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		resolver->Resolve(context, v8::Undefined(isolate_)).FromJust();
	}

	/// Reject the promise with an error message (creates a JS Error)
	void reject(std::string_view message)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		v8::Local<v8::Value> error = v8::Exception::Error(
			v8pp::to_v8(isolate_, message));
		resolver->Reject(context, error).FromJust();
	}

	/// Reject the promise with a raw V8 value
	void reject(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		resolver->Reject(context, value).FromJust();
	}

	/// Get the underlying v8::Promise (the "thenable" JS object)
	v8::Local<v8::Promise> get_promise() const
	{
		v8::EscapableHandleScope scope(isolate_);
		v8::Local<v8::Promise::Resolver> resolver = to_local(isolate_, resolver_);
		return scope.Escape(resolver->GetPromise());
	}

	v8::Isolate* isolate() const { return isolate_; }

private:
	v8::Isolate* isolate_;
	v8::Global<v8::Promise::Resolver> resolver_;
};

// Exclude promise<T> from is_wrapped_class so the convert system doesn't
// try to unwrap it as a bound C++ class
template<typename T>
struct is_wrapped_class<promise<T>> : std::false_type
{
};

/// convert<promise<T>> — to_v8 returns the JS promise object
template<typename T>
struct convert<promise<T>>
{
	using from_type = promise<T>;
	using to_type = v8::Local<v8::Promise>;

	static bool is_valid(v8::Isolate*, v8::Local<v8::Value> value)
	{
		return !value.IsEmpty() && value->IsPromise();
	}

	static to_type to_v8(v8::Isolate*, promise<T> const& value)
	{
		return value.get_promise();
	}
};

} // namespace v8pp
