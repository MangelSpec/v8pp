#pragma once

#include <string>
#include <string_view>

#include "v8pp/config.hpp"

namespace v8pp {

#ifdef WIN32
std::wstring utf8_to_wide(std::string_view utf8);
std::string wide_to_utf8(std::wstring_view wide);
#endif

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/string_utils.ipp"
#endif
