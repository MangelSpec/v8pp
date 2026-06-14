cmake_minimum_required(VERSION 3.12)

# CMP0091: use CMAKE_MSVC_RUNTIME_LIBRARY instead of compiler flags
if(POLICY CMP0091)
	cmake_policy(SET CMP0091 NEW)
endif()

project(v8pp
	VERSION 3.0.0
	DESCRIPTION "Bind C++ functions and classes into V8 JavaScript engine"
	HOMEPAGE_URL https://github.com/pmed/v8pp
	LANGUAGES CXX
)

# The C++ standard and /Zc:__cplusplus are requirements of the v8pp target
# itself (target_compile_features / target_compile_options in v8pp/CMakeLists.txt),
# not global state. Consuming v8pp via add_subdirectory therefore leaves the
# parent project's own standard and flags untouched.

if (MSVC)
	# static CRT when using a custom V8 monolith built with /MT
	if(DEFINED CUSTOM_V8_LIB_PATH OR DEFINED CUSTOM_V8_LIB_PATH_ABS)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()

option(BUILD_SHARED_LIBS "Build shared library" ON)
option(BUILD_TESTING "Build and run tests" OFF)
option(BUILD_DOCUMENTATION "Build documentation" OFF)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)

# Custom project options
option(V8PP_HEADER_ONLY "Header-only library" OFF)
set(V8PP_ISOLATE_DATA_SLOT 0 CACHE STRING "v8::Isolate data slot number, used in v8pp for shared data")
set(V8PP_PLUGIN_INIT_PROC_NAME "v8pp_module_init" CACHE STRING "v8pp plugin initialization procedure name")
set(V8PP_PLUGIN_SUFFIX ${CMAKE_SHARED_MODULE_SUFFIX} CACHE STRING "v8pp plugin filename suffix")
option(V8PP_PRETTIFY_TYPENAMES "v8pp registered classes will have prettier typeid names" ON)
option(V8_COMPRESS_POINTERS "Use new V8 ABI with V8_COMPRESS_POINTERS and V8_31BIT_SMIS_ON_64BIT_ARCH" ON)
option(V8_ENABLE_SANDBOX "Enable sandboxing in V8" OFF)

if(BUILD_SHARED_LIBS AND WIN32)
	set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS true)
endif()

# put all binaries into the output dir
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_subdirectory(v8pp)

if(BUILD_TESTING)
	enable_testing()
	# v8pp's public headers pull in V8 headers that check __cplusplus, which MSVC only
	# reports correctly with /Zc:__cplusplus. That flag is PRIVATE on the v8pp target, so
	# apply it to the test/plugin consumers here (does not affect v8pp's public interface).
	if(MSVC)
		add_compile_options(/Zc:__cplusplus)
	endif()
	add_subdirectory(plugins)
	add_subdirectory(test)
endif()

if(BUILD_BENCHMARKS)
	add_subdirectory(bench)
endif()

if(BUILD_DOCUMENTATION)
	add_subdirectory(docs)
endif()
