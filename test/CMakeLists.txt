# test target

add_executable(v8pp_test
	main.cpp
	test.hpp
	test_adversarial.cpp
	test_call_from_v8.cpp
	test_call_v8.cpp
	test_class.cpp
	test_context.cpp
	test_context_store.cpp
	test_convert.cpp
	test_fast_api.cpp
	test_function.cpp
	test_gc_stress.cpp
	test_json.cpp
	test_module.cpp
	test_object.cpp
	test_overload.cpp
	test_promise.cpp
	test_property.cpp
	test_symbol.cpp
	test_thread_safety.cpp
	test_ptr_traits.cpp
	test_throw_ex.cpp
	test_type_info.cpp
	test_utility.cpp
)

if(V8PP_HEADER_ONLY)
	target_sources(v8pp_test PRIVATE ${PROJECT_SOURCE_DIR}/v8pp/context.cpp)
	if(APPLE)
		# see v8pp/CMakeLists.txt
		set_source_files_properties(${PROJECT_SOURCE_DIR}/v8pp/context.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
	endif()
endif()

target_link_libraries(v8pp_test PRIVATE v8pp ${CMAKE_DL_LIBS})

if(MSVC)
	target_compile_options(v8pp_test PRIVATE /EHsc)
endif()

set(_isolated_v8_runtime ON)
if(WIN32 AND BUILD_SHARED_LIBS)
	get_target_property(_v8_dependency_type V8::V8 TYPE)
	set(_plugin_test_v8_lib "${CUSTOM_V8_LIB_PATH_ABS}")
	if(NOT _plugin_test_v8_lib)
		set(_plugin_test_v8_lib "${CUSTOM_V8_LIB_PATH}")
	endif()
	if(_v8_dependency_type STREQUAL "STATIC_LIBRARY" OR _plugin_test_v8_lib MATCHES "v8_monolith[.]lib$")
		set(_isolated_v8_runtime OFF)
		message(STATUS "Skipping shared v8pp runtime tests: the Windows V8 monolith links separate V8 runtimes into the executable and DLLs.")
	endif()
endif()

if(_isolated_v8_runtime)
	add_test(NAME v8pp_test COMMAND v8pp_test --version --run-tests)
	if(TARGET console)
		add_dependencies(v8pp_test console)
		target_compile_definitions(v8pp_test PRIVATE "V8PP_TEST_CONSOLE_PLUGIN=\"$<TARGET_FILE:console>\"")
		file(GLOB JS_TESTS *.js)
		add_test(NAME v8pp_js_test COMMAND v8pp_test --lib-path "$<TARGET_FILE_DIR:console>" ${JS_TESTS})
	endif()
endif()
