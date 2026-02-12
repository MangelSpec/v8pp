#pragma once

#include <string_view>

#include <v8.h>

#include "v8pp/convert.hpp"

namespace v8pp {

/// Traverse dot-separated subobject path, updating options and name
/// to point to the final object and leaf property name.
/// Returns false if any intermediate path segment is missing or not an object.
inline bool traverse_subobjects(v8::Isolate* isolate, v8::Local<v8::Context> context,
	v8::Local<v8::Object>& options, std::string_view& name)
{
	for (;;)
	{
		std::string_view::size_type const dot_pos = name.find('.');
		if (dot_pos == name.npos)
		{
			return true;
		}

		v8::Local<v8::Value> part;
		if (!options->Get(context, v8pp::to_v8(isolate, name.substr(0, dot_pos))).ToLocal(&part)
			|| !part->IsObject())
		{
			return false;
		}
		options = part.As<v8::Object>();
		name.remove_prefix(dot_pos + 1);
	}
}

/// Get optional value from V8 object by name.
/// Dot symbols in option name delimits subobjects name.
/// return false if the value doesn't exist in the options object
template<typename T>
bool get_option(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T& value, bool support_subobjects = true)
{
	v8::Local<v8::Context> context = isolate->GetCurrentContext();

	if (support_subobjects && !traverse_subobjects(isolate, context, options, name))
	{
		return false;
	}

	v8::Local<v8::Value> val;
	if (!options->Get(context, v8pp::to_v8(isolate, name)).ToLocal(&val)
		|| val->IsUndefined())
	{
		return false;
	}
	value = from_v8<T>(isolate, val);
	return true;
}

/// Alias for get_option without subobjects.
template<typename T>
bool get_option_fast(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T& value)
{
	return get_option(isolate, options, name, value, false);
}

/// Set named value in V8 object
/// Dot symbols in option name delimits subobjects name.
template<typename T>
bool set_option(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T const& value, bool support_subobjects = true)
{
	v8::Local<v8::Context> context = isolate->GetCurrentContext();

	if (support_subobjects && !traverse_subobjects(isolate, context, options, name))
	{
		return false;
	}

	return options->Set(context, v8pp::to_v8(isolate, name), to_v8(isolate, value)).FromMaybe(false);
}

/// Alias for set_option without subobjects.
template<typename T>
bool set_option_fast(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T const& value)
{
	return set_option(isolate, options, name, value, false);
}

/// Set named value in V8 object as data property
template<typename T>
bool set_option_data(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T const& value, bool support_subobjects = true)
{
	v8::Local<v8::Context> context = isolate->GetCurrentContext();

	if (support_subobjects && !traverse_subobjects(isolate, context, options, name))
	{
		return false;
	}

	return options->CreateDataProperty(context,
		v8pp::to_v8(isolate, name).As<v8::Name>(),
		to_v8(isolate, value)).FromMaybe(false);
}

/// Alias for set_option_data without subobjects.
template<typename T>
bool set_option_data_fast(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T const& value)
{
	return set_option_data(isolate, options, name, value, false);
}

/// Set named constant in V8 object
/// Subobject names are not supported
template<typename T>
void set_const(v8::Isolate* isolate, v8::Local<v8::Object> options,
	std::string_view name, T const& value)
{
	options->DefineOwnProperty(isolate->GetCurrentContext(),
		v8pp::to_v8(isolate, name), to_v8(isolate, value),
		v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete)).FromJust();
}

} // namespace v8pp
