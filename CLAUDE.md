# CLAUDE.md — v8pp Project Guide

## Project Overview

v8pp (v2.1.1) is a C++20 library that binds C++ functions and classes into the V8 JavaScript engine.
It provides a fluent, type-safe API for exposing C++ types to JavaScript with automatic type conversion.

- **Upstream:** https://github.com/pmed/v8pp
- **Fork:** https://github.com/MangelSpec/v8pp
- **License:** Boost Software License 1.0
- **Minimum V8 version:** 9.0+ (with compatibility gates up to V8 13.3+)

## Repository Structure

```
v8pp/                 # Main library source
  *.hpp               # Public headers (class, module, context, convert, property, etc.)
  *.ipp               # Implementation files included in header-only mode
  *.cpp               # Compiled sources for non-header-only mode
  config.hpp.in       # CMake-generated config header
cmake/                # CMake modules (FindV8.cmake, Config.cmake.in)
test/                 # Test suite (custom framework, test_*.cpp pattern)
plugins/              # Sample plugin modules (console.cpp, file.cpp)
examples/             # 8 numbered example projects ("01 hello world" through "08 passing wrapped objects")
docs/                 # Documentation
.github/workflows/    # CI (GitHub Actions: Ubuntu, macOS, Windows matrix)
```

## Build System

### Requirements
- CMake 3.12+
- C++20 compiler (MSVC 2022, GCC, Clang)
- V8 JavaScript engine

### Key CMake Options
| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHARED_LIBS` | ON | Build as shared library |
| `BUILD_TESTING` | OFF | Build and run tests |
| `V8PP_HEADER_ONLY` | OFF | Header-only library mode |
| `V8PP_ISOLATE_DATA_SLOT` | 0 | Isolate data slot for v8pp shared data |
| `V8PP_PRETTIFY_TYPENAMES` | ON | Prettier typeid names for registered classes |
| `V8_COMPRESS_POINTERS` | ON | V8 compressed pointers ABI |
| `V8_ENABLE_SANDBOX` | OFF | V8 sandboxing |

### Building (Windows with Ninja)
```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release
```

### Running Tests
```bash
cd build
ctest -C Release -V
```
The test executable is `v8pp_test`. Run with `--version --run-tests` for the native tests.
When `BUILD_SHARED_LIBS=ON`, JavaScript tests (`test/*.js`) also run via `--lib-path`.

### V8 Installation by Platform
- **Windows:** NuGet (`nuget install v8-v143-x64`) or vcpkg
- **macOS:** Homebrew (`brew install v8`)
- **Linux:** apt (`sudo apt install libv8-dev`)

## Architecture & Key Abstractions

### Core Types (all in namespace `v8pp`)
- **`class_<T, Traits>`** — Binds a C++ class to V8: constructors, methods, properties, inheritance, symbol protocols, iterators
- **`module`** — Wraps `v8::ObjectTemplate`, binds functions/variables/classes into a JS module
- **`context`** — Manages `v8::Isolate` + `v8::Context`, provides `run_script()`, `require()`
- **`context_store`** — Cross-context key-value store backed by a dedicated V8 context; persists values across ephemeral contexts on the same isolate
- **`convert<T>`** — Template specializations for V8 ↔ C++ type conversion (30+ types)
- **`property<Get, Set>`** — Compile-time getter/setter property binding
- **`promise<T>`** — Synchronous wrapper around `v8::Promise::Resolver` with typed resolve/reject

### Internal Types (in `v8pp::detail`)
- **`object_registry<Traits>`** — Tracks wrapped C++ object lifecycles in V8 (magic number validation: `0xC1A5517F`)
- **`external_data`** — Stores C++ data in V8 Externals (bitcast optimization for small types)
- **`type_info`** — Lightweight compile-time RTTI using static variable addresses with integer-based IDs
- **`call_from_v8_traits`** — Parameter extraction with default parameter support
- **`overload_resolution`** — Runtime argument count + type dispatch for function overloading (first-match-wins)
- **`fast_callback<FuncPtr>`** — V8 10+ Fast API callback generation via NTTP

### Pointer Traits
- `raw_ptr_traits` — Raw pointer lifecycle (`new`/`delete`)
- `shared_ptr_traits` — `std::shared_ptr` lifecycle

### Design Patterns
- **Fluent API / Method chaining** — `.ctor<>().function().property().var()`
- **C++20 Concepts** — Compile-time dispatch for type traits (`mapping`, `sequence`, `set_like`, `callable`, etc.)
- **Template specialization** — Type conversion system
- **Header-only option** — `.ipp` files are included by headers when `V8PP_HEADER_ONLY` is defined

## Code Conventions

### Naming
- **Classes/types:** `snake_case` with trailing underscore for keyword conflicts (`class_<T>`)
- **Functions:** `snake_case` (`to_v8`, `from_v8`, `throw_ex`, `set_option`)
- **Member variables:** trailing underscore (`isolate_`, `obj_`, `impl_`)
- **Macros/constants:** `V8PP_` prefix, `UPPER_CASE`
- **Type aliases:** `snake_case` (`object_id`, `pointer_type`)
- **Namespaces:** `v8pp`, `v8pp::detail`

### Formatting (enforced by .clang-format)
- **Indent:** 4 spaces (tabs for continuation)
- **Braces:** Allman-like (open brace on new line for classes, functions, control statements)
- **Namespaces:** compact, no indentation, no brace on new line
- **Column limit:** none (0)
- **Pointer alignment:** left (`int* p`, not `int *p`)
- **Access modifiers:** flush with class keyword (offset -4)
- **Template declarations:** always break before
- **Short forms allowed:** short if-without-else on single line, inline-only short functions

### Coding Practices
- **Cache repeated lookups:** Extract `isolate()`, `GetCurrentContext()`, and similar
  accessor results into local variables when used more than once in a function.
  Avoids redundant calls and improves readability.
- **V8 MaybeLocal/Maybe handling:** Use `ToLocalChecked()`/`FromJust()` for internal
  operations that should never fail (crash is appropriate). Use `ToLocal()`/`FromMaybe()`
  only where failure is genuinely reachable from user script (e.g. `ToString()` via Proxy,
  `GetPropertyNames()` via Proxy traps) and there's a meaningful recovery path.
- **`try_from_v8<T>`:** Exception-free conversion returning `std::optional<T>`. Use
  instead of `is_valid()` + `from_v8()` two-step pattern for cleaner type dispatch.

### Headers
- Use `#pragma once` (no include guards)
- Include order: standard library → V8 headers → v8pp headers

### Modern C++ Features in Use
- C++20 concepts (`mapping`, `sequence`, `set_like`, `callable`, `typed_array_element`, etc.)
- `requires` clauses on convert specializations (replacing SFINAE `enable_if`)
- `std::optional` for optional function parameters and `try_from_v8` results
- `std::string_view` for string parameters
- Fold expressions, structured bindings
- NTTP (non-type template parameters) for Fast API callbacks

## Type Conversion System

The `convert<T>` system supports these C++ types:

| Category | Types | V8 Representation |
|----------|-------|-------------------|
| Numeric | `bool`, `char`, integral, floating-point | `Boolean`, `Number` |
| Large integers | `int64_t`, `uint64_t` (>32-bit) | `BigInt` |
| Strings | `std::string`, `std::string_view`, `char const*`, wide strings | `String` |
| Enums | any `enum` / `enum class` | `Number` (underlying type) |
| Containers | `std::vector`, `std::list`, `std::deque`, etc. | `Array` |
| Sets | `std::set`, `std::unordered_set` | `Array` |
| Mappings | `std::map`, `std::unordered_map` | `Object` |
| Arrays | `std::array<T,N>` | `Array` (fixed-length) |
| Tuples | `std::tuple<Ts...>` | `Array` |
| Pairs | `std::pair<K,V>` | `[key, value]` Array |
| Optional | `std::optional<T>` | `T` or `undefined` |
| Variant | `std::variant<Ts...>` | First matching type |
| Smart ptrs | `std::shared_ptr<T>` | Wrapped object |
| Binary | `std::vector<uint8_t>` | `ArrayBuffer` |
| TypedArrays | `std::span<T>` (to_v8 only) | `Uint8Array`, `Float32Array`, etc. |
| Filesystem | `std::filesystem::path` | `String` |
| Chrono | `std::chrono::duration`, `time_point` | `Number` (milliseconds / epoch ms) |
| V8 handles | `v8::Local<T>`, `v8::Global<T>` | Pass-through |
| Wrapped classes | Any class bound via `class_<T>` | Wrapped object |
| Promises | `v8pp::promise<T>` | `Promise` |

## Binding Features

### Class Binding (`class_<T>`)
```cpp
v8pp::class_<MyClass> my_class(isolate);
my_class
    .ctor<int, std::string>()
    .function("method", &MyClass::method)
    .property("prop", &MyClass::get_prop, &MyClass::set_prop)
    .var("field", &MyClass::field)
    .to_string_tag("MyClass")                          // Symbol.toStringTag
    .to_primitive(&MyClass::value_of)                   // Symbol.toPrimitive
    .iterable(&MyClass::begin, &MyClass::end);          // Symbol.iterator
```

### Function Overloading
```cpp
module.function("process", v8pp::overload(
    &process_int,
    &process_string,
    v8pp::with_defaults(&process_opts, v8pp::defaults(42, "default"))
));
```

### Default Parameters
```cpp
module.function("create", v8pp::with_defaults(
    &create_widget,
    v8pp::defaults(100, 200, "untitled")  // fills from right
));
```

### V8 Fast API Callbacks (V8 10+)
Automatically generated for functions with supported signatures (void, bool, int32_t,
uint32_t, float, double params/returns). Registered as dual slow+fast callbacks.

## V8 API Compatibility

The codebase tracks V8's evolving API with version guards:

```cpp
// V8 13.3+: New string conversion API with ExternalMemoryAccounter
#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 3)

// V8 12.9+: SetAccessor removed from FunctionTemplate, use SetNativeDataProperty
// V8 12.2+: CompileModule API changes
// V8 11.9+: Exception constructor takes options struct
// V8 10.5+: VisitHandlesWithClassIds removed
// V8 10.0+: Fast API callbacks available
```

When modifying V8 API calls, always check which version introduced/removed the API and add
appropriate `#if` version guards. Test against the CI matrix (Ubuntu/macOS/Windows with
varying V8 versions).

## Safety Features

- **Prototype chain depth limit** (16) in `unwrap_object` prevents infinite loops from circular prototypes
- **Magic number validation** (`0xC1A5517F`) on `object_registry` before `static_cast` catches corruption
- **Use-after-free protection** in `context::require()` callback via weak pointer pattern
- **Null checks** on `unwrap_object` results in property/member accessors
- **`try_from_v8<T>`** for exception-free conversion returning `std::optional`
- **`ToLocal()`/`FromMaybe()`** on script-reachable paths (Proxy traps, ToString, GetPropertyNames)

## Testing

### Framework
Custom lightweight framework in `test/test.hpp` (no external dependencies).

### Key Test Helpers
- `check(msg, condition)` — Assert, throws on failure
- `check_eq(msg, obtained, expected)` — Equality check with pretty-print
- `run_script<T>(context, code)` — Execute JS and convert result to C++ type

### Test Files (22 files)
Each test file covers one module: `test_class.cpp`, `test_convert.cpp`, `test_property.cpp`,
`test_overload.cpp`, `test_promise.cpp`, `test_context_store.cpp`, `test_fast_api.cpp`,
`test_symbol.cpp`, `test_adversarial.cpp`, `test_gc_stress.cpp`, `test_thread_safety.cpp`, etc.
Tests are registered in `test/main.cpp`. The full suite runs as a single executable.

## Compiler Flags

### MSVC
`/GR-` (no RTTI), `/EHsc` (structured exceptions), `/permissive-` (strict conformance),
`/W4` (high warnings), `/wd4190` (suppress C-linkage warning), `/Zc:__cplusplus`,
`/external:anglebrackets /external:W3` (reduced warnings for system headers)

### GCC/Clang
`-frtti`, `-fexceptions`, `-Wall -Wextra -Wpedantic`

Note: macOS may need `-fno-rtti` for `context.cpp` specifically.

## Git Conventions

- **Commit style:** Short subject line, no trailing period, lowercase start
- **Prefixes used:** `fix`, `add`, `use`, `improve`, or bare description
- **Examples:**
  - `fix const reference for _fast setters`
  - `add get_option/set_option/set_option_data fast aliases`
  - `Fix SetAccessor API for V8 12.9+`

## Common Tasks

### Adding a new type conversion
1. Add a `convert<T>` specialization in `v8pp/convert.hpp`
2. Implement `to_v8()` and `from_v8()` static methods
3. Add tests in `test/test_convert.cpp`

### Binding a new C++ class
Use `v8pp::class_<T>` with the fluent API:
```cpp
v8pp::class_<MyClass> my_class(isolate);
my_class
    .ctor<int, std::string>()
    .function("method", &MyClass::method)
    .property("prop", &MyClass::get_prop, &MyClass::set_prop)
    .var("field", &MyClass::field);
```

### Fixing V8 API breakage
1. Identify the V8 version that changed the API
2. Add `#if V8_MAJOR_VERSION > X || (V8_MAJOR_VERSION == X && V8_MINOR_VERSION >= Y)` guards
3. Implement both old and new code paths
4. Test across the CI matrix

## CI Matrix

GitHub Actions runs on push/PR with this matrix:
- **OS:** Ubuntu, macOS, Windows
- **Build types:** Release
- **Variants:** shared/static × header-only/compiled
- **V8 options:** compressed pointers and sandbox vary by platform
