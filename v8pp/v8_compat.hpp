#pragma once

#include <v8.h>

namespace v8pp::detail {

#if V8_MAJOR_VERSION >= 15
using setter_callback_info = v8::PropertyCallbackInfo<v8::Boolean>;
using accessor_setter_callback = v8::AccessorNameSetterCallbackV2;
#else
using setter_callback_info = v8::PropertyCallbackInfo<void>;
using accessor_setter_callback = v8::AccessorNameSetterCallback;
#endif

inline v8::Local<v8::External> new_external(v8::Isolate* isolate, void* value)
{
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 3)
	return v8::External::New(isolate, value, v8::kExternalPointerTypeTagDefault);
#else
	return v8::External::New(isolate, value);
#endif
}

inline void* external_value(v8::Local<v8::External> value)
{
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 3)
	return value->Value(v8::kExternalPointerTypeTagDefault);
#else
	return value->Value();
#endif
}

inline void* aligned_pointer(v8::Local<v8::Object> object, int index)
{
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 2)
	return object->GetAlignedPointerFromInternalField(index, v8::kEmbedderDataTypeTagDefault);
#else
	return object->GetAlignedPointerFromInternalField(index);
#endif
}

inline void set_aligned_pointer(v8::Local<v8::Object> object, int index, void* value)
{
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 2)
	object->SetAlignedPointerInInternalField(index, value, v8::kEmbedderDataTypeTagDefault);
#else
	object->SetAlignedPointerInInternalField(index, value);
#endif
}

template<typename T>
v8::Local<v8::Object> property_receiver(v8::PropertyCallbackInfo<T> const& info)
{
#if V8_MAJOR_VERSION > 14 || (V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 7)
	return info.Holder();
#elif V8_MAJOR_VERSION == 14 && V8_MINOR_VERSION >= 6
	return info.HolderV2();
#else
	return info.This();
#endif
}

} // namespace v8pp::detail
