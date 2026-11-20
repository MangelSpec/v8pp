#include "v8pp/function.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace v8pp::detail {

struct external_data::registry
{
	struct isolate_data
	{
		size_t owners = 0;
		std::shared_ptr<group_state> generation;
		std::unordered_map<uintptr_t, record*> records;
		std::vector<std::shared_ptr<void>> resources;
	};

	std::mutex mutex;
	uintptr_t next_token = 1;
	std::unordered_map<v8::Isolate*, isolate_data> isolates;
};

V8PP_IMPL external_data::registry& external_data::instance()
{
	static registry data;
	return data;
}

V8PP_IMPL external_data::pin const*& external_data::result_scope::current()
{
	static thread_local pin const* active_result = nullptr;
	return active_result;
}

V8PP_IMPL external_data::group::~group()
{
	revoke();
}

V8PP_IMPL void external_data::group::revoke()
{
	// A destructor released by revocation can itself reenter binding cleanup.
	auto state = state_;
	external_data::revoke(*state);
}

V8PP_IMPL external_data::unpublished_record external_data::allocate_record(v8::Isolate* isolate)
{
	check_native_publication(isolate);
	// Allocate and free the record in the same module when a plugin has a separate CRT.
	return unpublished_record(new record(isolate), [](record* entry) { delete entry; });
}

V8PP_IMPL v8::Local<v8::Value> external_data::publish(unpublished_record entry, group* owner)
{
	auto* isolate = entry->isolate;
	auto& data = instance();
	{
		std::lock_guard lock(data.mutex);
		auto& state = data.isolates[isolate];
		if (!state.generation) state.generation = std::make_shared<group_state>();
		entry->generation = state.generation;
	}
	if (owner) entry->class_group = owner->state_;

	v8::EscapableHandleScope scope(isolate);
	auto& template_handle = entry->generation->carrier_template;
	if (template_handle.IsEmpty())
	{
		auto carrier_template = v8::ObjectTemplate::New(isolate);
		carrier_template->SetInternalFieldCount(1);
		template_handle.Reset(isolate, carrier_template);
	}

	auto context = isolate->GetCurrentContext();
	if (context.IsEmpty()) context = v8::Context::New(isolate);
	v8::Local<v8::Object> carrier;
	if (!template_handle.Get(isolate)->NewInstance(context).ToLocal(&carrier))
		throw std::runtime_error("could not create native callback data");
	if (!entry->live()) throw std::runtime_error("native binding has been retired");

	try
	{
		entry->dispatch = &native_dispatch_state::ensure(isolate);
		entry->generation->records.insert(entry.get());
		if (entry->class_group) entry->class_group->records.insert(entry.get());
		std::lock_guard lock(data.mutex);
		if (!data.next_token) throw std::runtime_error("native callback token limit exceeded");
		entry->token = data.next_token++;
		data.isolates.at(isolate).records.emplace(entry->token, entry.get());
		++entry->dispatch->callback_records;
	}
	catch (...)
	{
		entry->generation->records.erase(entry.get());
		if (entry->class_group) entry->class_group->records.erase(entry.get());
		if (entry->dispatch) entry->dispatch->release_if_idle();
		throw;
	}

	set_aligned_pointer(carrier, 0, entry.get());
	entry->carrier.Reset(isolate, carrier);
	entry->carrier.SetWeak(reinterpret_cast<void*>(entry->token), &weak_clear, v8::WeakCallbackType::kParameter);
	entry.release();
	return scope.Escape(carrier);
}

V8PP_IMPL external_data::record* external_data::find(v8::Isolate* isolate, uintptr_t token)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto it = data.isolates.find(isolate);
	if (it == data.isolates.end()) return nullptr;
	auto record = it->second.records.find(token);
	return record != it->second.records.end() ? record->second : nullptr;
}

V8PP_IMPL void external_data::weak_clear(v8::WeakCallbackInfo<void> const& info)
{
	auto* entry = find(info.GetIsolate(), reinterpret_cast<uintptr_t>(info.GetParameter()));
	if (entry) entry->carrier.Reset();
	info.SetSecondPassCallback(&weak_dispose);
}

V8PP_IMPL void external_data::weak_dispose(v8::WeakCallbackInfo<void> const& info)
{
	if (auto* entry = find(info.GetIsolate(), reinterpret_cast<uintptr_t>(info.GetParameter())))
		clear(entry);
}

V8PP_IMPL void external_data::clear(record* entry)
{
	auto* isolate = entry->isolate;
	if (!entry->carrier.IsEmpty())
	{
		v8::HandleScope scope(isolate);
		set_aligned_pointer(entry->carrier.Get(isolate), 0, nullptr);
		entry->carrier.Reset();
	}

	entry->generation->records.erase(entry);
	if (entry->class_group) entry->class_group->records.erase(entry);
	auto& data = instance();
	{
		std::lock_guard lock(data.mutex);
		data.isolates.at(isolate).records.erase(entry->token);
		entry->token = 0;
	}

	auto& dispatch = *entry->dispatch;
	--dispatch.callback_records;
	if (!entry->active) destroy_record(entry);
	dispatch.release_if_idle();
}

V8PP_IMPL void external_data::destroy_record(record* entry)
{
	if (entry->destroy)
		defer_native_destruction(entry->isolate, [entry] { delete entry; });
	else
		delete entry;
}

V8PP_IMPL void external_data::revoke(group_state& state)
{
	state.live = false;
	while (!state.records.empty()) clear(*state.records.begin());
	state.carrier_template.Reset();
}

V8PP_IMPL void external_data::add_owner(v8::Isolate* isolate)
{
	check_native_publication(isolate);
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	++data.isolates[isolate].owners;
}

V8PP_IMPL bool external_data::remove_owner(v8::Isolate* isolate)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto it = data.isolates.find(isolate);
	if (it == data.isolates.end() || !it->second.owners)
		return false;
	return --it->second.owners == 0;
}

V8PP_IMPL void external_data::retain_resource(v8::Isolate* isolate, std::shared_ptr<void> resource)
{
	check_native_publication(isolate);
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto& resources = data.isolates[isolate].resources;
	auto same_handle = [&](auto const& held) { return held.get() == resource.get(); };
	if (std::none_of(resources.begin(), resources.end(), same_handle))
		resources.push_back(std::move(resource));
}

V8PP_IMPL void external_data::revoke_all(v8::Isolate* isolate)
{
	auto& data = instance();
	std::shared_ptr<group_state> generation;
	{
		std::lock_guard lock(data.mutex);
		auto it = data.isolates.find(isolate);
		if (it == data.isolates.end()) return;
		generation = std::move(it->second.generation);
	}
	if (generation) revoke(*generation);
	{
		std::lock_guard lock(data.mutex);
		auto it = data.isolates.find(isolate);
		if (it != data.isolates.end() && !it->second.owners && !it->second.generation
			&& it->second.records.empty() && it->second.resources.empty())
			data.isolates.erase(it);
	}
}

V8PP_IMPL std::vector<std::shared_ptr<void>> external_data::take_resources(v8::Isolate* isolate)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto it = data.isolates.find(isolate);
	if (it == data.isolates.end()) return {};
	auto resources = std::move(it->second.resources);
	if (!it->second.owners && !it->second.generation && it->second.records.empty())
		data.isolates.erase(it);
	return resources;
}

} // namespace v8pp::detail
