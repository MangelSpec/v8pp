#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <v8.h>

#include "v8pp/config.hpp"
#include "v8pp/convert.hpp"
#include "v8pp/object.hpp"

namespace v8pp {

/// A key-value store backed by a dedicated V8 context.
///
/// Designed to hold named V8 values that outlive ephemeral contexts
/// on the same isolate. Values are stored as live V8 object references
/// (no serialization), so wrapped C++ objects, functions, and complex
/// JS structures survive intact.
///
/// The store creates and owns a lightweight V8 context internally.
/// All stored values live in a dedicated storage object (not the global).
///
/// Thread safety: same as V8 — single-threaded per isolate.
///
/// Usage:
///   v8::Isolate* isolate = ...;
///   v8pp::context_store store(isolate);
///
///   // Save values before destroying a context
///   store.save_from(old_ctx->impl(), {"state", "config"});
///
///   // ... destroy and recreate context ...
///
///   // Restore values into the new context
///   store.restore_to(new_ctx->impl(), {"state", "config"});
class context_store
{
public:
	/// Create a store on the given isolate.
	/// The isolate must outlive this store.
	explicit context_store(v8::Isolate* isolate);

	~context_store();

	context_store(context_store const&) = delete;
	context_store& operator=(context_store const&) = delete;

	context_store(context_store&&) noexcept;
	context_store& operator=(context_store&&) noexcept;

	/// The isolate this store belongs to
	v8::Isolate* isolate() const { return isolate_; }

	/// The internal V8 context (for advanced use)
	v8::Local<v8::Context> impl() const { return to_local(isolate_, store_ctx_); }

	/// Store a value under the given name.
	/// Dot-separated names create/traverse subobjects.
	/// Overwrites existing values. Returns true on success.
	bool set(std::string_view name, v8::Local<v8::Value> value);

	/// Store a C++ value (converted via convert<T>).
	template<typename T>
	bool set(std::string_view name, T const& value)
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Value> v8_value = to_v8(isolate_, value);
		return set(name, v8_value);
	}

	/// Retrieve a value by name.
	/// Returns true if the name exists and value is not undefined.
	/// The returned handle is valid in the caller's HandleScope.
	bool get(std::string_view name, v8::Local<v8::Value>& out) const;

	/// Retrieve and convert a value to C++ type T.
	template<typename T>
	bool get(std::string_view name, T& out) const
	{
		v8::HandleScope scope(isolate_);
		v8::Local<v8::Value> val;
		if (!get(name, val))
		{
			return false;
		}
		out = from_v8<T>(isolate_, val);
		return true;
	}

	/// Check whether a name exists in the store.
	bool has(std::string_view name) const;

	/// Remove a value from the store. Returns true if it existed.
	bool remove(std::string_view name);

	/// Remove all stored values.
	void clear();

	/// Number of top-level stored values.
	size_t size() const;

	/// Names of all top-level stored values.
	std::vector<std::string> keys() const;

	/// Save named values from a source context into this store.
	/// Returns the number of values successfully saved.
	size_t save_from(v8::Local<v8::Context> source,
		std::initializer_list<std::string_view> names);

	/// Restore named values from this store into a target context.
	/// Returns the number of values successfully restored.
	size_t restore_to(v8::Local<v8::Context> target,
		std::initializer_list<std::string_view> names) const;

	/// Store a JSON-serialized deep copy of a value.
	/// Returns false if serialization fails (e.g., circular references).
	bool set_json(std::string_view name, v8::Local<v8::Value> value);

	/// Retrieve a deep copy via JSON into the caller's context.
	/// Returns false if the name doesn't exist or parsing fails.
	bool get_json(std::string_view name, v8::Local<v8::Value>& out) const;

private:
	v8::Isolate* isolate_;
	v8::Global<v8::Context> store_ctx_;
	v8::Global<v8::Object> store_obj_;

	/// Like traverse_subobjects but creates missing intermediate objects.
	static bool ensure_subobjects(v8::Isolate* isolate, v8::Local<v8::Context> context,
		v8::Local<v8::Object>& obj, std::string_view& name);
};

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/context_store.ipp"
#endif
