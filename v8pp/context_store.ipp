#include "v8pp/context_store.hpp"
#include "v8pp/json.hpp"

namespace v8pp {

V8PP_IMPL context_store::context_store(v8::Isolate* isolate)
	: isolate_(isolate)
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = v8::Context::New(isolate_);
	store_ctx_.Reset(isolate_, ctx);

	v8::Context::Scope context_scope(ctx);
	v8::Local<v8::Object> obj = v8::Object::New(isolate_);
	store_obj_.Reset(isolate_, obj);
}

V8PP_IMPL context_store::~context_store()
{
	store_obj_.Reset();
	store_ctx_.Reset();
	isolate_ = nullptr;
}

V8PP_IMPL context_store::context_store(context_store&& src) noexcept
	: isolate_(std::exchange(src.isolate_, nullptr))
	, store_ctx_(std::move(src.store_ctx_))
	, store_obj_(std::move(src.store_obj_))
{
}

V8PP_IMPL context_store& context_store::operator=(context_store&& src) noexcept
{
	if (this != &src)
	{
		store_obj_.Reset();
		store_ctx_.Reset();
		isolate_ = std::exchange(src.isolate_, nullptr);
		store_ctx_ = std::move(src.store_ctx_);
		store_obj_ = std::move(src.store_obj_);
	}
	return *this;
}

V8PP_IMPL bool context_store::ensure_subobjects(v8::Isolate* isolate, v8::Local<v8::Context> context,
	v8::Local<v8::Object>& obj, std::string_view& name)
{
	for (;;)
	{
		auto const dot = name.find('.');
		if (dot == name.npos)
		{
			return true;
		}

		auto const segment = name.substr(0, dot);
		v8::Local<v8::String> key = v8pp::to_v8(isolate, segment);
		v8::Local<v8::Value> part;
		if (obj->Get(context, key).ToLocal(&part) && part->IsObject())
		{
			obj = part.As<v8::Object>();
		}
		else
		{
			v8::Local<v8::Object> new_obj = v8::Object::New(isolate);
			if (!obj->Set(context, key, new_obj).FromMaybe(false))
			{
				return false;
			}
			obj = new_obj;
		}
		name.remove_prefix(dot + 1);
	}
}

V8PP_IMPL bool context_store::set(std::string_view name, v8::Local<v8::Value> value)
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
	if (!ensure_subobjects(isolate_, ctx, obj, name))
	{
		return false;
	}
	return obj->Set(ctx, v8pp::to_v8(isolate_, name), value).FromMaybe(false);
}

V8PP_IMPL bool context_store::get(std::string_view name, v8::Local<v8::Value>& out) const
{
	v8::EscapableHandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
	if (!traverse_subobjects(isolate_, ctx, obj, name))
	{
		return false;
	}

	v8::Local<v8::Value> val;
	if (!obj->Get(ctx, v8pp::to_v8(isolate_, name)).ToLocal(&val) || val->IsUndefined())
	{
		return false;
	}
	out = scope.Escape(val);
	return true;
}

V8PP_IMPL bool context_store::has(std::string_view name) const
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
	if (!traverse_subobjects(isolate_, ctx, obj, name))
	{
		return false;
	}

	v8::Local<v8::Value> val;
	if (!obj->Get(ctx, v8pp::to_v8(isolate_, name)).ToLocal(&val) || val->IsUndefined())
	{
		return false;
	}
	return true;
}

V8PP_IMPL bool context_store::remove(std::string_view name)
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
	if (!traverse_subobjects(isolate_, ctx, obj, name))
	{
		return false;
	}

	v8::Local<v8::String> key = v8pp::to_v8(isolate_, name);
	if (!obj->Has(ctx, key).FromMaybe(false))
	{
		return false;
	}
	return obj->Delete(ctx, key).FromMaybe(false);
}

V8PP_IMPL void context_store::clear()
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Object> obj = v8::Object::New(isolate_);
	store_obj_.Reset(isolate_, obj);
}

V8PP_IMPL size_t context_store::size() const
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Array> names;
	if (!to_local(isolate_, store_obj_)->GetOwnPropertyNames(ctx).ToLocal(&names))
	{
		return 0;
	}
	return names->Length();
}

V8PP_IMPL std::vector<std::string> context_store::keys() const
{
	v8::HandleScope scope(isolate_);
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	std::vector<std::string> result;
	v8::Local<v8::Array> names;
	if (!to_local(isolate_, store_obj_)->GetOwnPropertyNames(ctx).ToLocal(&names))
	{
		return result;
	}

	result.reserve(names->Length());
	for (uint32_t i = 0; i < names->Length(); ++i)
	{
		v8::Local<v8::Value> key;
		if (names->Get(ctx, i).ToLocal(&key))
		{
			result.push_back(from_v8<std::string>(isolate_, key));
		}
	}
	return result;
}

V8PP_IMPL size_t context_store::save_from(v8::Local<v8::Context> source,
	std::initializer_list<std::string_view> names)
{
	v8::HandleScope scope(isolate_);
	size_t count = 0;

	for (auto const full_name : names)
	{
		v8::Local<v8::Value> val;

		// Read value from source context
		{
			v8::Context::Scope source_scope(source);
			v8::Local<v8::Object> src_global = source->Global();
			std::string_view leaf = full_name;

			if (!traverse_subobjects(isolate_, source, src_global, leaf))
			{
				continue;
			}
			if (!src_global->Get(source, v8pp::to_v8(isolate_, leaf)).ToLocal(&val)
				|| val->IsUndefined())
			{
				continue;
			}
		}

		// Store under the original full name
		if (set(full_name, val))
		{
			++count;
		}
	}
	return count;
}

V8PP_IMPL size_t context_store::restore_to(v8::Local<v8::Context> target,
	std::initializer_list<std::string_view> names) const
{
	v8::HandleScope scope(isolate_);
	size_t count = 0;

	for (auto name : names)
	{
		v8::Local<v8::Value> val;
		if (!get(name, val))
		{
			continue;
		}

		// Write value into target context
		{
			v8::Context::Scope target_scope(target);
			v8::Local<v8::Object> tgt_global = target->Global();
			std::string_view leaf = name;

			if (!ensure_subobjects(isolate_, target, tgt_global, leaf))
			{
				continue;
			}
			if (tgt_global->Set(target, v8pp::to_v8(isolate_, leaf), val).FromMaybe(false))
			{
				++count;
			}
		}
	}
	return count;
}

V8PP_IMPL bool context_store::set_json(std::string_view name, v8::Local<v8::Value> value)
{
	v8::HandleScope scope(isolate_);

	// Stringify in the caller's current context
	std::string json = json_str(isolate_, value);
	if (json.empty())
	{
		return false;
	}

	// Parse and store in the store's context
	v8::Local<v8::Context> ctx = impl();
	v8::Context::Scope context_scope(ctx);

	v8::Local<v8::Value> parsed = json_parse(isolate_, json);
	if (parsed.IsEmpty() || parsed->IsUndefined())
	{
		return false;
	}

	v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
	if (!ensure_subobjects(isolate_, ctx, obj, name))
	{
		return false;
	}
	return obj->Set(ctx, v8pp::to_v8(isolate_, name), parsed).FromMaybe(false);
}

V8PP_IMPL bool context_store::get_json(std::string_view name, v8::Local<v8::Value>& out) const
{
	v8::EscapableHandleScope scope(isolate_);

	// Read and stringify in the store's context
	std::string json;
	{
		v8::HandleScope inner_scope(isolate_);
		v8::Local<v8::Context> ctx = impl();
		v8::Context::Scope context_scope(ctx);

		v8::Local<v8::Object> obj = to_local(isolate_, store_obj_);
		if (!traverse_subobjects(isolate_, ctx, obj, name))
		{
			return false;
		}

		v8::Local<v8::Value> val;
		if (!obj->Get(ctx, v8pp::to_v8(isolate_, name)).ToLocal(&val) || val->IsUndefined())
		{
			return false;
		}
		json = json_str(isolate_, val);
	}

	if (json.empty())
	{
		return false;
	}

	// Parse in the caller's current context
	v8::Local<v8::Value> parsed = json_parse(isolate_, json);
	if (parsed.IsEmpty() || parsed->IsUndefined())
	{
		return false;
	}
	out = scope.Escape(parsed);
	return true;
}

} // namespace v8pp
