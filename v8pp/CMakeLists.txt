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

set(V8PP_HEADERS
	${CMAKE_CURRENT_BINARY_DIR}/config.hpp
	call_from_v8.hpp
	call_v8.hpp
	class.hpp
	context.hpp
	context_store.hpp
	convert.hpp
	function.hpp
	json.hpp
	module.hpp
	object.hpp
	overload.hpp
	promise.hpp
	property.hpp
	ptr_traits.hpp
	string_utils.hpp
	throw_ex.hpp
	type_info.hpp
	utility.hpp
	version.hpp
)

if(V8PP_HEADER_ONLY)
	list(APPEND V8PP_HEADERS
		class.ipp
		context_store.ipp
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
