# v8pp target

include(GNUInstallDirs)

if(TARGET V8::V8)
	message(STATUS "Using existing V8::V8 target")
elseif(DEFINED CUSTOM_V8_LIB_PATH OR DEFINED CUSTOM_V8_LIB_PATH_ABS)
	# Resolve effective paths (_ABS = absolute, plain = relative as-is)
	if(DEFINED CUSTOM_V8_INCLUDE_PATH_ABS)
		set(_v8pp_inc "${CUSTOM_V8_INCLUDE_PATH_ABS}")
	elseif(DEFINED CUSTOM_V8_INCLUDE_PATH)
		set(_v8pp_inc "${CUSTOM_V8_INCLUDE_PATH}")
	endif()
	if(DEFINED CUSTOM_V8_LIB_PATH_ABS)
		set(_v8pp_lib "${CUSTOM_V8_LIB_PATH_ABS}")
	else()
		set(_v8pp_lib "${CUSTOM_V8_LIB_PATH}")
	endif()

	add_library(CustomV8 INTERFACE)
	add_library(V8::V8 ALIAS CustomV8)
	if(DEFINED _v8pp_inc)
		target_include_directories(CustomV8 INTERFACE
			$<BUILD_INTERFACE:${_v8pp_inc}>
		)
	endif()
	get_filename_component(_v8_ext "${_v8pp_lib}" EXT)
	if("${_v8_ext}" STREQUAL ".a" OR "${_v8_ext}" STREQUAL ".so" OR "${_v8_ext}" STREQUAL ".lib" OR "${_v8_ext}" STREQUAL ".dylib")
		target_link_libraries(CustomV8 INTERFACE "${_v8pp_lib}")
	else()
		# directory containing individual V8 static libs
		target_link_libraries(CustomV8 INTERFACE
			"${_v8pp_lib}/libv8_libbase.a"
			"${_v8pp_lib}/libv8_libplatform.a"
			"${_v8pp_lib}/libv8_monolith.a"
		)
	endif()
	if(WIN32)
		target_link_libraries(CustomV8 INTERFACE winmm dbghelp)
	endif()
	message(STATUS "Using custom V8 from ${_v8pp_lib}")
elseif(DEFINED VCPKG_TARGET_TRIPLET)
	find_package(V8 CONFIG REQUIRED)
else()
	list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
	find_package(V8 REQUIRED)
endif()

configure_file(config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/config.hpp)

if(V8_COMPRESS_POINTERS)
	list(APPEND V8PP_DEFINES V8_COMPRESS_POINTERS V8_31BIT_SMIS_ON_64BIT_ARCH)
endif()

if (V8_ENABLE_SANDBOX)
	list(APPEND V8PP_DEFINES V8_ENABLE_SANDBOX)
endif()

if(V8PP_ENABLE_MANAGED_WRAPPERS)
	if(V8PP_HEADER_ONLY)
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires the compiled v8pp library to share one managed isolate identity across modules.")
	endif()
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires the verified V8 Release CRT and iterator ABI.")
	endif()
	if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
		OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"
		OR NOT V8_COMPRESS_POINTERS OR V8_ENABLE_SANDBOX)
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires MSVC Windows x64 and V8 pointer compression with the sandbox disabled.")
	endif()
	if(NOT V8PP_MANAGED_WRAPPERS_DATA_SLOT MATCHES "^[0-3]$"
		OR V8PP_MANAGED_WRAPPERS_DATA_SLOT EQUAL V8PP_ISOLATE_DATA_SLOT)
		message(FATAL_ERROR "V8PP_MANAGED_WRAPPERS_DATA_SLOT must be a V8 isolate slot (0-3) distinct from V8PP_ISOLATE_DATA_SLOT.")
	endif()

	get_target_property(_v8pp_managed_links V8::V8 INTERFACE_LINK_LIBRARIES)
	if(NOT IS_ABSOLUTE "${CUSTOM_V8_LIB_PATH_ABS}" OR NOT IS_ABSOLUTE "${CUSTOM_V8_INCLUDE_PATH_ABS}"
		OR NOT CUSTOM_V8_LIB_PATH_ABS IN_LIST _v8pp_managed_links
		OR NOT EXISTS "${CUSTOM_V8_LIB_PATH_ABS}"
		OR NOT EXISTS "${CUSTOM_V8_INCLUDE_PATH_ABS}/v8-version.h")
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires the pinned GN-built V8 monolith and headers linked by V8::V8. Set CUSTOM_V8_LIB_PATH_ABS and CUSTOM_V8_INCLUDE_PATH_ABS.")
	endif()

	file(STRINGS "${CUSTOM_V8_INCLUDE_PATH_ABS}/v8-version.h" _v8pp_v8_version
		REGEX "^#define V8_(MAJOR_VERSION|MINOR_VERSION|BUILD_NUMBER|PATCH_LEVEL) [0-9]+$")
	if(NOT _v8pp_v8_version STREQUAL "#define V8_MAJOR_VERSION 12;#define V8_MINOR_VERSION 9;#define V8_BUILD_NUMBER 202;#define V8_PATCH_LEVEL 28")
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires V8 12.9.202.28.")
	endif()

	get_filename_component(_v8pp_monolith_dir "${CUSTOM_V8_LIB_PATH_ABS}" DIRECTORY)
	set(_v8pp_gn_metadata "${_v8pp_monolith_dir}/v8_monolith.ninja")
	if(NOT EXISTS "${_v8pp_gn_metadata}")
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires the V8 monolith's GN metadata at ${_v8pp_gn_metadata} to verify its ABI.")
	endif()
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_v8pp_gn_metadata}"
		"${CUSTOM_V8_INCLUDE_PATH_ABS}/v8-version.h")
	file(STRINGS "${_v8pp_gn_metadata}" _v8pp_gn_defines LIMIT_COUNT 1 REGEX "^defines = ")
	string(REGEX MATCHALL "-DCPPGC_[A-Z0-9_]+" _v8pp_cppgc_abi "${_v8pp_gn_defines}")
	list(SORT _v8pp_cppgc_abi)
	if(NOT _v8pp_cppgc_abi STREQUAL "-DCPPGC_CAGED_HEAP;-DCPPGC_ENABLE_LARGER_CAGE;-DCPPGC_POINTER_COMPRESSION;-DCPPGC_SLIM_WRITE_BARRIER;-DCPPGC_YOUNG_GENERATION"
		OR NOT _v8pp_gn_defines MATCHES " -DV8_COMPRESS_POINTERS "
		OR NOT _v8pp_gn_defines MATCHES " -DV8_COMPRESS_POINTERS_IN_SHARED_CAGE "
		OR NOT _v8pp_gn_defines MATCHES " -DV8_31BIT_SMIS_ON_64BIT_ARCH "
		OR NOT _v8pp_gn_defines MATCHES " -DV8_TARGET_ARCH_X64( |$)"
		OR _v8pp_gn_defines MATCHES " -D(V8_ENABLE_SANDBOX|V8_ENABLE_CHECKS|DEBUG)([ =]|$)")
		message(FATAL_ERROR "V8PP_ENABLE_MANAGED_WRAPPERS requires the verified V8 Release x64 pointer-compression and cppgc ABI. The linked monolith's GN definitions differ.")
	endif()
	string(REPLACE "-D" "" _v8pp_cppgc_defines "${_v8pp_cppgc_abi}")
	list(APPEND V8PP_DEFINES V8_COMPRESS_POINTERS_IN_SHARED_CAGE ${_v8pp_cppgc_defines})
endif()

set(V8PP_HEADERS
	${CMAKE_CURRENT_BINARY_DIR}/config.hpp
	call_from_v8.hpp
	call_v8.hpp
	class.hpp
	context.hpp
	context_store.hpp
	convert.hpp
	dispatch_diagnostics.hpp
	fast_api.hpp
	function.hpp
	json.hpp
	managed_wrappers.hpp
	module.hpp
	native_dispatch.hpp
	object.hpp
	overload.hpp
	promise.hpp
	property.hpp
	ptr_traits.hpp
	string_utils.hpp
	throw_ex.hpp
	type_info.hpp
	utility.hpp
	v8_compat.hpp
	version.hpp
)

if(V8PP_HEADER_ONLY)
	list(APPEND V8PP_HEADERS
		class.ipp
		context_store.ipp
		function.ipp
		native_dispatch.ipp
		json.ipp
		string_utils.ipp
		throw_ex.ipp
		version.ipp
	)
	set(V8PP_SOURCES)
else()
	set(V8PP_SOURCES
		class.cpp
		context.cpp
		context_store.cpp
		convert.cpp
		function.cpp
		string_utils.cpp
		json.cpp
		throw_ex.cpp
		version.cpp
	)
endif()

# Flags for compiling v8pp's own sources. Applied PRIVATE below so they never
# propagate to consumers: RTTI, warning levels and exception model are build
# choices, not usage requirements. v8pp uses no typeid/dynamic_cast, so it is
# built RTTI-off to match V8's -fno-rtti ABI, consistent with the MSVC /GR-.
if(MSVC)
	set(V8PP_COMPILE_OPTIONS /GR- /EHsc /permissive- /W4 /Zc:__cplusplus)
	# disable specific warnings
	list(APPEND V8PP_COMPILE_OPTIONS /wd4190)
	# set warning level 3 for system headers
	list(APPEND V8PP_COMPILE_OPTIONS /external:anglebrackets /external:W3)
else()
	set(V8PP_COMPILE_OPTIONS -fno-rtti -fexceptions -Wall -Wextra -Wpedantic)
endif()

if(V8PP_HEADER_ONLY)
	add_library(v8pp INTERFACE)
	target_compile_features(v8pp INTERFACE cxx_std_20)
	target_compile_definitions(v8pp INTERFACE ${V8PP_DEFINES})
	target_include_directories(v8pp INTERFACE
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
	target_link_libraries(v8pp INTERFACE V8::V8)
else()
	add_library(v8pp ${V8PP_HEADERS} ${V8PP_SOURCES})
	# Consumers need at least C++20; v8pp's own sources are pinned to C++20 so
	# they build identically regardless of the consumer's (possibly newer) standard.
	target_compile_features(v8pp PUBLIC cxx_std_20)
	set_target_properties(v8pp PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)
	target_compile_definitions(v8pp PUBLIC ${V8PP_DEFINES})
	target_compile_options(v8pp PRIVATE ${V8PP_COMPILE_OPTIONS})
	target_include_directories(v8pp PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
		$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
	target_link_libraries(v8pp PUBLIC V8::V8)
	if(BUILD_SHARED_LIBS)
		target_link_libraries(v8pp PUBLIC ${CMAKE_DL_LIBS})
	endif()
endif()

# V8 headers check __cplusplus, which MSVC reports accurately only with this flag.
if(MSVC)
	target_compile_options(v8pp INTERFACE /Zc:__cplusplus)
endif()

# Install (skipped when consumed as a subdirectory of another package)
if(NOT V8PP_SKIP_INSTALL)
	include(CMakePackageConfigHelpers)

	set(targets_export_name v8pp_Targets)

	write_basic_package_version_file("${PROJECT_BINARY_DIR}/ConfigVersion.cmake" COMPATIBILITY SameMajorVersion)
	configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/Config.cmake.in" "${PROJECT_BINARY_DIR}/Config.cmake"
		INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/v8pp/cmake)

	if(TARGET CustomV8)
		install(TARGETS CustomV8 EXPORT ${targets_export_name})
	endif()
	install(TARGETS v8pp EXPORT ${targets_export_name})
	install(EXPORT ${targets_export_name}
		NAMESPACE v8pp::
		FILE ${targets_export_name}.cmake
		DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/v8pp")

	install(FILES "${PROJECT_BINARY_DIR}/ConfigVersion.cmake" "${PROJECT_BINARY_DIR}/Config.cmake"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/v8pp")

	install(FILES ${V8PP_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/v8pp)
endif()
