#include "v8pp/string_utils.hpp"

#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace v8pp {

#ifdef WIN32
V8PP_IMPL std::wstring utf8_to_wide(std::string_view utf8)
{
	int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring w(sz, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), sz);
	return w;
}

V8PP_IMPL std::string wide_to_utf8(std::wstring_view wide)
{
	int sz = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string utf8(sz, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), sz, nullptr, nullptr);
	return utf8;
}
#endif

} // namespace v8pp
