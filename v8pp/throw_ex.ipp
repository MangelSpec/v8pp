#include "v8pp/throw_ex.hpp"

namespace v8pp {

V8PP_IMPL v8::Local<v8::Value> throw_ex(v8::Isolate* isolate, std::string_view str)
{
	v8::Local<v8::String> msg = v8::String::NewFromUtf8(isolate, str.data(),
		v8::NewStringType::kNormal, static_cast<int>(str.size())).ToLocalChecked();
	return isolate->ThrowException(msg);
}

V8PP_IMPL v8::Local<v8::Value> throw_ex(v8::Isolate* isolate, std::string_view str,
	v8::Local<v8::Value> (*exception_ctor)(v8::Local<v8::String>))
{
	v8::Local<v8::String> msg = v8::String::NewFromUtf8(isolate, str.data(),
		v8::NewStringType::kNormal, static_cast<int>(str.size())).ToLocalChecked();
	return isolate->ThrowException(exception_ctor(msg));
}

V8PP_IMPL v8::Local<v8::Value> throw_error(v8::Isolate* isolate, std::string_view str)
{
#if V8_MAJOR_VERSION >= 10
	v8::Local<v8::String> msg = v8::String::NewFromUtf8(isolate, str.data(),
		v8::NewStringType::kNormal, static_cast<int>(str.size()))
									.ToLocalChecked();
	return isolate->ThrowError(msg);
#else 
	return throw_ex(isolate, str, v8::Exception::Error);
#endif
}
} // namespace v8pp
