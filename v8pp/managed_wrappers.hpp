#pragma once

#include "v8pp/config.hpp"

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <v8-sandbox.h>
#include <v8-version.h>

#if !defined(_M_X64) || !defined(V8_COMPRESS_POINTERS) || defined(V8_ENABLE_SANDBOX) \
	|| defined(_DEBUG) || _ITERATOR_DEBUG_LEVEL != 0 \
	|| V8_MAJOR_VERSION != 12 || V8_MINOR_VERSION != 9 || V8_BUILD_NUMBER != 202 || V8_PATCH_LEVEL != 28
#error "Managed v8pp wrappers require the pinned Windows x64 V8 12.9.202.28 Release ABI."
#endif
#if !defined(CPPGC_CAGED_HEAP) || !defined(CPPGC_ENABLE_LARGER_CAGE) || !defined(CPPGC_POINTER_COMPRESSION) \
	|| !defined(CPPGC_SLIM_WRITE_BARRIER) || !defined(CPPGC_YOUNG_GENERATION) \
	|| defined(CPPGC_2GB_CAGE) || defined(CPPGC_SUPPORTS_OBJECT_NAMES)
#error "Managed v8pp wrappers require the pinned V8 cppgc ABI in every consumer."
#endif
#endif

namespace v8 {
class Isolate;
}

namespace v8pp::detail {

#if V8PP_ENABLE_MANAGED_WRAPPERS
static_assert(V8PP_MANAGED_WRAPPERS_DATA_SLOT != V8PP_ISOLATE_DATA_SLOT);

// Creation of a managed v8pp isolate reserves this tag for its wrapper cells.
inline constexpr auto managed_wrapper_tag = static_cast<v8::CppHeapPointerTag>(0x6fff);
static_assert(managed_wrapper_tag > v8::CppHeapPointerTag::kNullTag
	&& managed_wrapper_tag < v8::CppHeapPointerTag::kDefaultTag);

bool has_managed_wrappers(v8::Isolate* isolate) noexcept;
#else
inline bool has_managed_wrappers(v8::Isolate*) noexcept { return false; }
#endif

} // namespace v8pp::detail
