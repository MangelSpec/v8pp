#include "v8pp/function.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace v8pp::detail {

struct external_data::registry
{
	struct isolate_data
	{
		size_t owners = 0;
		std::unordered_map<uintptr_t, value_holder_base*> holders;
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

V8PP_IMPL uintptr_t external_data::register_holder(v8::Isolate* isolate, value_holder_base* holder)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	if (!data.next_token) throw std::runtime_error("native callback token limit exceeded");
	uintptr_t const token = data.next_token++;
	data.isolates[isolate].holders.emplace(token, holder);
	return token;
}

V8PP_IMPL void external_data::unregister_holder(v8::Isolate* isolate, uintptr_t token)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto it = data.isolates.find(isolate);
	if (it != data.isolates.end())
	{
		it->second.holders.erase(token);
		if (!it->second.owners && it->second.holders.empty() && it->second.resources.empty())
		{
			data.isolates.erase(it);
		}
	}
}

V8PP_IMPL external_data::value_holder_base* external_data::find_holder(v8::Isolate* isolate, uintptr_t token)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	auto it = data.isolates.find(isolate);
	if (it == data.isolates.end()) return nullptr;
	auto holder = it->second.holders.find(token);
	return holder != it->second.holders.end() ? holder->second : nullptr;
}

V8PP_IMPL void external_data::add_owner(v8::Isolate* isolate)
{
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
	{
		return false;
	}
	return --it->second.owners == 0;
}

V8PP_IMPL void external_data::retain_resource(v8::Isolate* isolate, std::shared_ptr<void> resource)
{
	auto& data = instance();
	std::lock_guard lock(data.mutex);
	data.isolates[isolate].resources.push_back(std::move(resource));
}

V8PP_IMPL void external_data::destroy_all(v8::Isolate* isolate)
{
	auto& data = instance();
	std::vector<std::shared_ptr<void>> resources;
	for (;;)
	{
		value_holder_base* holder;
		{
			std::lock_guard lock(data.mutex);
			auto it = data.isolates.find(isolate);
			if (it == data.isolates.end()) break;
			if (it->second.holders.empty())
			{
				resources = std::move(it->second.resources);
				if (!it->second.owners) data.isolates.erase(it);
				break;
			}
			auto first = it->second.holders.begin();
			holder = first->second;
			it->second.holders.erase(first);
		}
		delete holder;
	}
	// Native modules must remain loaded until their callback destructors finish.
}

} // namespace v8pp::detail
