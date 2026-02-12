# benchmark target

add_executable(v8pp_bench
	main.cpp
	bench.hpp
	bench_convert.cpp
	bench_call.cpp
	bench_class.cpp
	bench_property.cpp
)

if(V8PP_HEADER_ONLY)
	target_sources(v8pp_bench PRIVATE ${PROJECT_SOURCE_DIR}/v8pp/context.cpp)
	if(APPLE)
		# see v8pp/CMakeLists.txt
		set_source_files_properties(${PROJECT_SOURCE_DIR}/v8pp/context.cpp PROPERTIES COMPILE_FLAGS -fno-rtti)
	endif()
endif()

target_link_libraries(v8pp_bench PRIVATE v8pp ${CMAKE_DL_LIBS})
