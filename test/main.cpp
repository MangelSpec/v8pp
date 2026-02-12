#include <iostream>
#include <algorithm>
#include <memory>
#include <vector>
#include <exception>
#include <string>

#include <v8.h>
#include <libplatform/libplatform.h>

#include "v8pp/context.hpp"
#include "v8pp/version.hpp"

void run_tests()
{
	void test_type_info();
	void test_utility();
	void test_context();
	void test_context_store();
	void test_convert();
	void test_throw_ex();
	void test_call_v8();
	void test_call_from_v8();
	void test_function();
	void test_ptr_traits();
	void test_module();
	void test_class();
	void test_property();
	void test_object();
	void test_json();
	void test_overload();
	void test_fast_api();
	void test_symbol();
	void test_promise();
	void test_gc_stress();
	void test_adversarial();
	void test_thread_safety();

	std::pair<char const*, void (*)()> tests[] =
	{
		{ "test_type_info", test_type_info },
		{"test_utility", test_utility},
		{"test_context", test_context},
		{"test_context_store", test_context_store},
		{"test_convert", test_convert},
		{"test_throw_ex", test_throw_ex},
		{"test_function", test_function},
		{"test_ptr_traits", test_ptr_traits},
		{"test_call_v8", test_call_v8},
		{"test_call_from_v8", test_call_from_v8},
		{"test_module", test_module},
		{"test_class", test_class},
		{"test_property", test_property},
		{"test_object", test_object},
		{"test_json", test_json},
		{"test_overload", test_overload},
		{"test_fast_api", test_fast_api},
		{"test_symbol", test_symbol},
		{"test_promise", test_promise},
		{"test_gc_stress", test_gc_stress},
		{"test_adversarial", test_adversarial},
		{"test_thread_safety", test_thread_safety},
	};

	for (auto const& test : tests)
	{
		std::cout << test.first;
		try
		{
			test.second();
			std::cout << " ok";
		}
		catch (std::exception const& ex)
		{
			std::cerr << " error: " << ex.what() << '\n';
			exit(EXIT_FAILURE);
		}
		std::cout << std::endl;
	}
}

int main(int argc, char const* argv[])
{
	std::cerr << "[debug] main() entered" << std::endl;

	std::vector<std::string> scripts;
	std::string lib_path;
	bool do_tests = false;

	for (int i = 1; i < argc; ++i)
	{
		std::string const arg = argv[i];
		if (arg == "-h" || arg == "--help")
		{
			std::cout << "Usage: " << argv[0] << " [arguments] [script]\n"
				<< "Arguments:\n"
				<< "  --help,-h           Print this message and exit\n"
				<< "  --version,-v        Print V8 version\n"
				<< "  --lib-path <dir>    Set <dir> for plugins library path\n"
				<< "  --run-tests         Run library tests\n"
				;
			return EXIT_SUCCESS;
		}
		else if (arg == "-v" || arg == "--version")
		{
			std::cout << "V8 version " << v8::V8::GetVersion() << std::endl;
			std::cout << "v8pp version " << v8pp::version()
				<< " (major=" << v8pp::version_major()
				<< " minor=" << v8pp::version_minor()
				<< " patch=" << v8pp::version_patch()
				<< ")\n";
			std::cout << "v8pp build options " << v8pp::build_options() << std::endl;
		}
		else if (arg == "--lib-path")
		{
			++i;
			if (i < argc) lib_path = argv[i];
		}
		else if (arg == "--run-tests")
		{
			do_tests = true;
		}
		else
		{
			scripts.push_back(arg);
		}
	}

	std::cerr << "[debug] SetFlagsFromString" << std::endl;
	// allow Isolate::RequestGarbageCollectionForTesting() before Initialize()
	// for v8pp::class_ tests
	v8::V8::SetFlagsFromString("--expose_gc");

	std::cerr << "[debug] InitializeExternalStartupData" << std::endl;
	//v8::V8::InitializeICU();
	v8::V8::InitializeExternalStartupData(argv[0]);

	std::cerr << "[debug] NewDefaultPlatform" << std::endl;
#if V8_MAJOR_VERSION >= 7
	std::unique_ptr<v8::Platform> platform(v8::platform::NewDefaultPlatform());
#else
	std::unique_ptr<v8::Platform> platform(v8::platform::CreateDefaultPlatform());
#endif

	std::cerr << "[debug] InitializePlatform" << std::endl;
	v8::V8::InitializePlatform(platform.get());

	std::cerr << "[debug] V8::Initialize" << std::endl;
	v8::V8::Initialize();

	std::cerr << "[debug] V8 initialized, do_tests=" << do_tests << " scripts=" << scripts.size() << std::endl;

	if (do_tests || scripts.empty())
	{
		std::cerr << "[debug] running tests" << std::endl;
		run_tests();
		std::cerr << "[debug] tests finished" << std::endl;
	}

	int result = EXIT_SUCCESS;
	try
	{
		std::cerr << "[debug] creating v8pp::context" << std::endl;
		v8pp::context context;
		std::cerr << "[debug] v8pp::context created" << std::endl;

		if (!lib_path.empty())
		{
			context.set_lib_path(lib_path);
		}
		for (std::string const& script : scripts)
		{
			v8::HandleScope scope(context.isolate());
			context.run_file(script);
		}
		std::cerr << "[debug] destroying v8pp::context" << std::endl;
	}
	catch (std::exception const& ex)
	{
		std::cerr << ex.what() << std::endl;
		result = EXIT_FAILURE;
	}
	std::cerr << "[debug] v8pp::context destroyed" << std::endl;

	std::cerr << "[debug] V8::Dispose" << std::endl;
	v8::V8::Dispose();

	std::cerr << "[debug] ShutdownPlatform" << std::endl;
#if V8_MAJOR_VERSION > 9 || (V8_MAJOR_VERSION == 9 && V8_MINOR_VERSION >= 8)
	v8::V8::DisposePlatform();
#else
	v8::V8::ShutdownPlatform();
#endif

	std::cerr << "[debug] clean exit" << std::endl;
	return result;
}
