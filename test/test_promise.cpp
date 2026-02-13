#include "v8pp/promise.hpp"
#include "v8pp/class.hpp"
#include "v8pp/module.hpp"

#include "test.hpp"

void test_promise()
{
	// Test 1: immediate resolve with int
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("makePromise", [](v8::Isolate* isolate) -> v8pp::promise<int>
			{
			v8pp::promise<int> p(isolate);
			p.resolve(42);
			return p; });

		// V8 runs microtasks between top-level script evaluations.
		// First script sets up the .then(), second script reads the result.
		run_script<std::string>(context,
			"var intResult = 0;"
			"makePromise().then(function(v) { intResult = v; });"
			"''");
		check_eq("resolved int promise",
			run_script<int>(context, "intResult"),
			42);
	}

	// Test 2: immediate resolve with string
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("strPromise", [](v8::Isolate* isolate) -> v8pp::promise<std::string>
			{
			v8pp::promise<std::string> p(isolate);
			p.resolve("hello world");
			return p; });

		run_script<std::string>(context,
			"var strResult = '';"
			"strPromise().then(function(v) { strResult = v; });"
			"''");
		check_eq("resolved string promise",
			run_script<std::string>(context, "strResult"),
			"hello world");
	}

	// Test 3: immediate reject with error message
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("rejectPromise", [](v8::Isolate* isolate) -> v8pp::promise<int>
			{
			v8pp::promise<int> p(isolate);
			p.reject("something went wrong");
			return p; });

		run_script<std::string>(context,
			"var errMsg = '';"
			"rejectPromise().catch(function(e) { errMsg = e.message; });"
			"''");
		check_eq("rejected promise",
			run_script<std::string>(context, "errMsg"),
			"something went wrong");
	}

	// Test 4: reject with raw V8 value
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("rejectRaw", [](v8::Isolate* isolate) -> v8pp::promise<int>
			{
			v8pp::promise<int> p(isolate);
			p.reject(v8pp::to_v8(isolate, "raw rejection"));
			return p; });

		run_script<std::string>(context,
			"var rawErr = '';"
			"rejectRaw().catch(function(e) { rawErr = String(e); });"
			"''");
		check_eq("raw rejection",
			run_script<std::string>(context, "rawErr"),
			"raw rejection");
	}

	// Test 5: promise<void>
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("voidPromise", [](v8::Isolate* isolate) -> v8pp::promise<void>
			{
			v8pp::promise<void> p(isolate);
			p.resolve();
			return p; });

		run_script<std::string>(context,
			"var voidResult = 'not called';"
			"voidPromise().then(function() { voidResult = 'called'; });"
			"''");
		check_eq("void promise",
			run_script<std::string>(context, "voidResult"),
			"called");
	}

	// Test 6: void promise rejection
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("voidReject", [](v8::Isolate* isolate) -> v8pp::promise<void>
			{
			v8pp::promise<void> p(isolate);
			p.reject("void error");
			return p; });

		run_script<std::string>(context,
			"var voidErr = '';"
			"voidReject().catch(function(e) { voidErr = e.message; });"
			"''");
		check_eq("void promise rejection",
			run_script<std::string>(context, "voidErr"),
			"void error");
	}

	// Test 7: promise with double
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("piPromise", [](v8::Isolate* isolate) -> v8pp::promise<double>
			{
			v8pp::promise<double> p(isolate);
			p.resolve(3.14159);
			return p; });

		run_script<std::string>(context,
			"var piResult = 0;"
			"piPromise().then(function(v) { piResult = v; });"
			"''");
		check_eq("double promise",
			run_script<double>(context, "piResult"),
			3.14159);
	}

	// Test 8: promise chaining
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("chainPromise", [](v8::Isolate* isolate) -> v8pp::promise<int>
			{
			v8pp::promise<int> p(isolate);
			p.resolve(10);
			return p; });

		run_script<std::string>(context,
			"var chainResult = 0;"
			"chainPromise()"
			"  .then(function(v) { return v * 2; })"
			"  .then(function(v) { chainResult = v; });"
			"''");
		check_eq("promise chain",
			run_script<int>(context, "chainResult"),
			20);
	}

	// Test 9: verify return type is actually a Promise
	{
		v8pp::context context;
		v8::Isolate* isolate = context.isolate();
		v8::HandleScope scope(isolate);

		context.function("isPromiseTest", [](v8::Isolate* isolate) -> v8pp::promise<int>
			{
			v8pp::promise<int> p(isolate);
			p.resolve(1);
			return p; });

		check_eq("is a Promise",
			run_script<bool>(context, "isPromiseTest() instanceof Promise"),
			true);
	}
}
