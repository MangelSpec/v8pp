[![Build status](https://github.com/MangelSpec/v8pp/actions/workflows/cmake.yml/badge.svg)](https://github.com/MangelSpec/v8pp/actions/workflows/cmake.yml)

# v8pp

Header-only C++ library to expose C++ classes and functions into [V8](https://developers.google.com/v8/) JavaScript engine.

This is a fork of [pmed/v8pp](https://github.com/pmed/v8pp) with performance
optimizations, crash safety hardening, expanded type conversions, and new binding
features. The fork is fully merged with upstream — all upstream changes are
included here.

**Requirements:** C++20 compiler, V8 9.0+

Tested on:
  * MSVC 2022 (Windows)
  * GCC (Ubuntu)
  * Clang (macOS)

## What this fork adds

**Performance**
- Integer-based type IDs for O(1) type comparison (replaces string comparison)
- `unordered_map` class registry for O(1) class lookup (replaces linear scan)
- `SideEffectType` hints on function bindings for TurboFan optimization
- Internalized V8 strings for frequently-used property names
- `V8PP_PRETTIFY_TYPENAMES` toggle to skip string processing in production
- Iterative subobject traversal in `object.hpp` (replaces recursion)
- Fast aliases (`get_option_fast`, `set_option_fast`) that skip dot-path parsing

**Crash safety**
- Null checks on all `unwrap_object` results in property/member accessors
- Prototype chain depth limit (16) in `unwrap_object` prevents infinite loops
- Magic number validation (`0xC1A5517F`) on object registry before `static_cast`
- Use-after-free protection in `context::require()` via weak pointer pattern
- `try_from_v8<T>` — exception-free conversion returning `std::optional`
- Script-reachable `ToLocalChecked` paths replaced with `ToLocal` + proper error handling
- `FromJust` → `FromMaybe` on script-reachable paths
- Member pointer bitcast exclusion (prevents V8 debug assertion crash)

**New binding features**
- Function overloading with `v8pp::overload()`
- Default parameters with `v8pp::defaults()`
- V8 Fast API callbacks (auto-generated for compatible signatures, V8 10+)
- `const_property` — evaluated once at wrap time, stored as read-only own property
- `v8pp::promise<T>` — synchronous wrapper around `v8::Promise::Resolver`
- Symbol protocol support (`Symbol.toStringTag`, `Symbol.toPrimitive`, `Symbol.iterator`)
- Iterator protocol (`class_<T>::iterable(begin, end)` for `for...of` support)
- `context_store` — cross-context key-value persistence on the same isolate

**Expanded type conversions**
- `int64_t`/`uint64_t` ↔ `BigInt`
- `std::span<T>` → TypedArrays (`Uint8Array`, `Float32Array`, etc.)
- `std::vector<uint8_t>` ↔ `ArrayBuffer`
- `std::set`/`std::unordered_set` ↔ `Array`
- `std::pair<K,V>` ↔ two-element `Array`
- `std::filesystem::path` ↔ `String`
- `std::chrono::duration`/`time_point` ↔ `Number` (milliseconds)
- `std::monostate` in `std::variant` for `null`/`undefined`

**V8 API compatibility**
- V8 12.9+: `SetNativeDataProperty` on `InstanceTemplate` (replaces removed `SetAccessor`)
- V8 13.3+: `ExternalMemoryAccounter` for string conversion
- Fixed constructor object identity (`wrap_this` uses `args.This()` instead of creating a second object)
- All bindings registered on `js_function_template` (fixes inheritance and `super` calls)

**C++20 modernization**
- Concepts replace SFINAE for type dispatch (`mapping`, `sequence`, `set_like`, `callable`, etc.)
- Dead V8 < 9.0 code paths removed

For a detailed change-by-change analysis, see [docs/FORK_CHANGES.md](docs/FORK_CHANGES.md).

## Building and testing

The library has a set of tests that can be configured, built, and run with CMake:

```console
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release
cd build && ctest -C Release -V
```

### V8 installation

| Platform | Command |
|----------|---------|
| Windows  | `nuget install v8-v143-x64` or vcpkg |
| macOS    | `brew install v8` |
| Linux    | `sudo apt install libv8-dev` |

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHARED_LIBS` | ON | Build as shared library |
| `BUILD_TESTING` | OFF | Build and run tests |
| `V8PP_HEADER_ONLY` | 0 | Header-only library mode |
| `V8PP_ISOLATE_DATA_SLOT` | 0 | Isolate data slot for v8pp shared data |
| `V8PP_PRETTIFY_TYPENAMES` | 1 | Prettier type names (disable for max performance) |
| `V8_COMPRESS_POINTERS` | ON | V8 compressed pointers ABI |
| `V8_ENABLE_SANDBOX` | OFF | V8 sandboxing |

## Binding example

v8pp provides two main binding targets:

  * `v8pp::module`, a wrapper class around `v8::ObjectTemplate`
  * `v8pp::class_`, a template class wrapper around `v8::FunctionTemplate`

Both require a pointer to `v8::Isolate`. They allow binding variables, functions,
constants, and properties with a fluent `.set(name, item)` API:

```c++
v8::Isolate* isolate;

int var;
int get_var() { return var + 1; }
void set_var(int x) { var = x + 1; }

struct X
{
    X(int v, bool u) : var(v) {}
    int var;
    int get() const { return var; }
    void set(int x) { var = x; }
};

// bind free variables and functions
v8pp::module mylib(isolate);
mylib
    .const_("PI", 3.1415)
    .var("var", var)
    .function("fun", &get_var)
    .property("prop", get_var, set_var);

// bind class
v8pp::class_<X> X_class(isolate);
X_class
    .ctor<int, bool>()
    .var("var", &X::var)
    .function("fun", &X::set)
    .property("prop", &X::get);

// set class into the module template
mylib.class_("X", X_class);

// set bindings in global object as `mylib`
isolate->GetCurrentContext()->Global()->Set(
    v8::String::NewFromUtf8(isolate, "mylib"), mylib.new_instance());
```

After that bindings will be available in JavaScript:
```javascript
mylib.var = mylib.PI + mylib.fun();
var x = new mylib.X(1, true);
mylib.prop = x.prop + x.fun();
```

## Function overloading

```c++
v8pp::module m(isolate);
m.function("process", v8pp::overload(
    &process_int,
    &process_string,
    v8pp::with_defaults(&process_opts, v8pp::defaults(42, "default"))
));
```

## Default parameters

```c++
v8pp::module m(isolate);
m.function("create", v8pp::with_defaults(
    &create_widget,
    v8pp::defaults(100, 200, "untitled")  // fills from right
));
```

## Symbol protocols and iterators

```c++
v8pp::class_<MyClass> my_class(isolate);
my_class
    .ctor<int>()
    .to_string_tag("MyClass")                      // Symbol.toStringTag
    .to_primitive(&MyClass::value_of)               // Symbol.toPrimitive
    .iterable(&MyClass::begin, &MyClass::end);      // Symbol.iterator → for...of
```

## Promise support

```c++
v8pp::promise<int> p(isolate);
v8::Local<v8::Promise> js_promise = p.promise();  // return this to JS

// later, from C++:
p.resolve(42);
// or: p.reject("something went wrong");
```

## Type conversion table

| C++ Type | V8 / JS Type |
|----------|-------------|
| `bool`, integral, floating-point | `Boolean`, `Number` |
| `int64_t`, `uint64_t` | `BigInt` |
| `std::string`, `std::string_view`, `char const*` | `String` |
| `enum` / `enum class` | `Number` |
| `std::vector`, `std::list`, `std::deque`, etc. | `Array` |
| `std::set`, `std::unordered_set` | `Array` |
| `std::map`, `std::unordered_map` | `Object` |
| `std::array<T,N>` | `Array` |
| `std::tuple<Ts...>` | `Array` |
| `std::pair<K,V>` | `[key, value]` |
| `std::optional<T>` | `T` or `undefined` |
| `std::variant<Ts...>` | First matching type |
| `std::shared_ptr<T>` | Wrapped object |
| `std::vector<uint8_t>` | `ArrayBuffer` |
| `std::span<T>` | TypedArray (`Uint8Array`, `Float32Array`, etc.) |
| `std::filesystem::path` | `String` |
| `std::chrono::duration` | `Number` (milliseconds) |
| `std::chrono::time_point` | `Number` (epoch ms) |
| `v8::Local<T>`, `v8::Global<T>` | Pass-through |
| Any `class_<T>`-bound class | Wrapped object |
| `v8pp::promise<T>` | `Promise` |

## Class binding

```c++
v8pp::class_<MyClass> my_class(isolate);
my_class
    .ctor<int, std::string>()
    .function("method", &MyClass::method)
    .property("prop", &MyClass::get_prop, &MyClass::set_prop)
    .const_property("id", &MyClass::id)     // evaluated once, read-only
    .var("field", &MyClass::field)
    .const_("MAX", 100);
```

## Node.js and io.js addons

The library is suitable to make [Node.js](http://nodejs.org/) addons. See [addons](docs/addons.md) document.

```c++
void RegisterModule(v8::Local<v8::Object> exports)
{
    v8pp::module addon(v8::Isolate::GetCurrent());

    addon
        .function("fun", &function)
        .class_("cls", my_class)
        ;

    exports->SetPrototype(addon.new_instance());
}
```

## Plugins

v8pp provides a `require()` system for loading plugins from shared libraries:

```c++
#include <v8pp/context.hpp>

v8pp::context context;
context.set_lib_path("path/to/plugins/lib");
v8::HandleScope scope(context.isolate());
context.run_file("some_file.js");
```

### Plugin example

```c++
#include <v8pp/module.hpp>

namespace console {

void log(v8::FunctionCallbackInfo<v8::Value> const& args)
{
    v8::HandleScope handle_scope(args.GetIsolate());
    for (int i = 0; i < args.Length(); ++i)
    {
        if (i > 0) std::cout << ' ';
        v8::String::Utf8Value str(args[i]);
        std::cout << *str;
    }
    std::cout << std::endl;
}

v8::Local<v8::Value> init(v8::Isolate* isolate)
{
    v8pp::module m(isolate);
    m.function("log", &log);
    return m.new_instance();
}

} // namespace console

V8PP_PLUGIN_INIT(v8::Isolate* isolate)
{
    return console::init(isolate);
}
```

## External C++ objects

```c++
// Reference external — C++ object outlives JavaScript wrapper
v8::Local<v8::Value> val = v8pp::class_<my_class>::reference_external(
    isolate, &my_class::instance());

// Import external — JavaScript takes ownership via pointers / shared_ptr
v8::Local<v8::Value> val = v8pp::class_<my_class>::import_external(
    isolate, new my_class);
```

## Compile-time configuration

Defined in `v8pp/config.hpp`:

  * `V8PP_ISOLATE_DATA_SLOT` — v8::Isolate data slot for v8pp internal data
  * `V8PP_PLUGIN_INIT_PROC_NAME` — Plugin initialization procedure name
  * `V8PP_PLUGIN_SUFFIX` — Plugin filename suffix for `require()`
  * `V8PP_HEADER_ONLY` — Header-only mode (default)
  * `V8PP_PRETTIFY_TYPENAMES` — Pretty type names for debugging (disable for performance)

## License

[Boost Software License 1.0](LICENSE.md)

## Upstream

[pmed/v8pp](https://github.com/pmed/v8pp) — original project by [pmed](https://github.com/pmed)

## Alternatives

* [nbind](https://github.com/charto/nbind)
* [vu8](https://github.com/tsa/vu8)
* [v8-juice](http://code.google.com/p/v8-juice/)
