#include "v8pp/throw_ex.hpp"

namespace v8pp {

V8PP_IMPL v8::Local<v8::Value> throw_ex(v8::Isolate* isolate, std::string_view message, exception_ctor ctor, v8::Local<v8::Value> exception_options)
{
	v8::Local<v8::String> msg = v8::String::NewFromUtf8(isolate, message.data(),
		v8::NewStringType::kNormal, static_cast<int>(message.size())).ToLocalChecked();

// if constexpr (exception_ctor_with_options) doesn't work win VC++ 2022
#if V8_MAJOR_VERSION > 11 || (V8_MAJOR_VERSION == 11 && V8_MINOR_VERSION >= 9)
	v8::Local<v8::Value> ex = ctor(msg, exception_options);
#else
	(void)exception_options;
	v8::Local<v8::Value> ex = ctor(msg);
#endif

	return isolate->ThrowException(ex);
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
