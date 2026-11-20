#include "v8pp/class.hpp"

#include <cassert>
#include <cstdio> // for snprintf
#include <tuple>
#include <utility>

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <cppgc/allocation.h>
#include <v8-cppgc.h>
#endif

namespace v8pp::detail {

static V8PP_IMPL std::string pointer_str(void const* ptr)
{
	std::string buf(sizeof(void*) * 2 + 3, 0); // +3 for 0x and \0 terminator
	int const len =
#if defined(_MSC_VER) && (_MSC_VER < 1900)
		sprintf_s(&buf[0], buf.size(), "%p", ptr);
#else
		snprintf(&buf[0], buf.size(), "%p", ptr);
#endif
	buf.resize(len < 0 ? 0 : len);
	return buf;
}

/////////////////////////////////////////////////////////////////////////////
//
// class_info
//
V8PP_IMPL class_info::class_info(type_info const& type, type_info const& traits)
	: type(type)
	, traits(traits)
{
}

V8PP_IMPL std::string class_info::class_name() const
{
	return class_name(type, traits);
}

V8PP_IMPL std::string class_info::class_name(type_info const& type, type_info const& traits)
{
	return "v8pp::class_<" + std::string(type.name()) + ", " + std::string(traits.name()) + ">";
}

static V8PP_IMPL v8::Local<v8::Private> wrapper_marker(v8::Isolate* isolate)
{
	return v8::Private::ForApi(isolate, v8pp::to_v8_name(isolate, "v8pp.wrapper"));
}

V8PP_IMPL weak_wrapper::weak_wrapper(classes& state)
	: state(state)
{
}

V8PP_IMPL void weak_wrapper::reset_weak_handle()
{
	if (auto* handle = std::exchange(weak_handle, nullptr))
	{
		handle->Reset();
	}
}

V8PP_IMPL void weak_wrapper::make_weak(v8::Global<v8::Object>& handle)
{
	assert(!weak_handle);
	weak_handle = &handle;
	handle.SetWeak(this, [](v8::WeakCallbackInfo<weak_wrapper> const& first)
		{
		auto& entry = *first.GetParameter();
		entry.reset_weak_handle();
		auto*& head = entry.state.pending_weak_;
		entry.previous = &head;
		entry.next = head;
		if (head) head->previous = &entry.next;
		head = &entry;
		first.SetSecondPassCallback([](v8::WeakCallbackInfo<weak_wrapper> const& second)
		{
			auto* entry = second.GetParameter();
			*entry->previous = entry->next;
			if (entry->next) entry->next->previous = entry->previous;
			entry->previous = nullptr;
			if (entry->detached)
			{
				auto& state = entry->state;
				delete entry;
				state.release_if_idle();
			}
			else
			{
				entry->collect();
			}
		}); }, v8::WeakCallbackType::kParameter);
}

/////////////////////////////////////////////////////////////////////////////
//
// object_registry
//
template<typename Traits>
V8PP_IMPL object_registry<Traits>::object_registry(classes& state, v8::Isolate* isolate, type_info const& type, dtor_function&& dtor)
	: class_info(type, type_id<Traits>())
	, state_(state)
	, isolate_(isolate)
	, dtor_(std::make_shared<dtor_function>(std::move(dtor)))
	, managed_wrappers_(has_managed_wrappers(isolate))
{
	v8::HandleScope scope(isolate_);

	v8::Local<v8::FunctionTemplate> func = v8::FunctionTemplate::New(isolate_);
	v8::Local<v8::FunctionTemplate> js_func = v8::FunctionTemplate::New(isolate_, [](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			v8::Isolate* isolate = args.GetIsolate();
			v8::TryCatch try_catch(isolate);
			try
			{
				native_dispatch_scope dispatch(isolate);
				external_data::pin callback(args.Data());
				if (!args.IsConstructCall()) throw std::runtime_error("must be called with new");
				auto* this_ = callback.get<object_registry*>();
				auto registry = this_->shared_from_this();
				auto wrapped = this_->wrap_object(args);
				if (!try_catch.HasCaught())
				{
					if (wrapped.IsEmpty()) throw std::runtime_error("wrapped nullptr");
					args.GetReturnValue().Set(wrapped);
				}
			}
			catch (std::exception const& ex)
			{
				if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
			}
			if (try_catch.HasCaught()) try_catch.ReThrow();
		}, external_data::set(isolate, this, &callbacks));

	func_.Reset(isolate, func);
	js_func_.Reset(isolate, js_func);

	// Reserve the API-wrapper layout used by both constructors. Native metadata
	// is published only after either constructor registers the actual instance.
	js_func->InstanceTemplate()->SetInternalFieldCount(2);
	func->InstanceTemplate()->SetInternalFieldCount(2);
	func->Inherit(js_func);
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>::~object_registry()
{
	assert(objects_.empty());
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::retire()
{
	retired = true;
	callbacks.revoke();
	for (auto& base : bases_)
	{
		std::erase(base.info->derivatives_, this);
	}
	for (auto* derived : derivatives_)
	{
		std::erase_if(derived->bases_, [this](base_class_info const& base)
			{ return base.info == this; });
	}
	bases_.clear();
	derivatives_.clear();
	remove_objects();
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>::wrapped_object::wrapped_object(object_registry& registry, object_id id, size_t size)
	: weak_wrapper(registry.state_)
	, registry(registry)
	, id(id)
	, size(size)
{
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::wrapped_object::collect()
{
	// An explicit wrap in progress will publish a replacement or retire this receipt.
	if (!replacement) registry.detach_object(id);
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>::external_replacement::external_replacement(
	wrapped_object& receipt)
	: registry(receipt.registry)
	, id(receipt.id)
{
	receipt.replacement = this;
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::external_replacement::cancel() const
{
	if (auto* receipt = find())
	{
		receipt->replacement = nullptr;
		if (!receipt->previous) registry.detach_object(id);
	}
}

template<typename Traits>
V8PP_IMPL typename object_registry<Traits>::wrapped_object*
object_registry<Traits>::external_replacement::find() const
{
	auto const position = registry.objects_.find(id);
	if (position == registry.objects_.end() || position->second.receipt->replacement != this)
	{
		return nullptr;
	}
	return position->second.receipt.get();
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>::factory_call::factory_call(object_registry& registry)
	: registry(registry)
	, previous(std::exchange(registry.factory_call_, this))
{
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>::factory_call::~factory_call()
{
	registry.factory_call_ = previous;
}

template<typename Traits>
V8PP_IMPL bool object_registry<Traits>::owns_new_factory_result(object_id id) const
{
	if (objects_.contains(id)) return false;
	auto const& retired_objects = factory_call_->retired_objects;
	if (std::find(retired_objects.begin(), retired_objects.end(), id) != retired_objects.end())
	{
		return false;
	}
	auto found = state_.classes_.find(type.id());
	return found == state_.classes_.end() || found->second->traits != traits ||
		!static_cast<object_registry const&>(*found->second).objects_.contains(id);
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::rollback_factory_result(pointer_type const& object, size_t size)
{
	if (object && size && owns_new_factory_result(Traits::pointer_id(object)))
	{
		defer_native_destruction(isolate_, [isolate = isolate_, destroy = dtor_, object]
			{ (*destroy)(isolate, object); });
	}
}

template<typename Traits>
V8PP_IMPL bool object_registry<Traits>::apply_const_properties(v8::Local<v8::Object> obj, pointer_type const& ext)
{
	// Converting a constant can run native code that retires its class or bases.
	if (!bases_.empty())
	{
		std::vector<std::pair<std::shared_ptr<class_info>, cast_function>> bases;
		bases.reserve(bases_.size());
		for (auto const& base : bases_)
		{
			bases.emplace_back(base.info->shared_from_this(), base.cast);
		}
		for (auto const& [base, cast] : bases)
		{
			if (retired || base->retired ||
				!static_cast<object_registry&>(*base).apply_const_properties(obj, cast(ext)))
			{
				return false;
			}
		}
	}
	std::vector<typename decltype(const_properties)::value_type const*> properties;
	properties.reserve(const_properties.size());
	for (auto const& property : const_properties)
	{
		properties.push_back(&property);
	}
	for (auto const* property : properties)
	{
		auto const& [name, func] = *property;
		auto value = func(isolate_, ext);
		if (retired || value.IsEmpty() ||
			!obj->DefineOwnProperty(isolate_->GetCurrentContext(), v8pp::to_v8_name(isolate_, name), value,
					v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete))
				.FromMaybe(false))
		{
			return false;
		}
	}
	return !retired;
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::add_base(object_registry& info, cast_function cast)
{
	auto it = std::find_if(bases_.begin(), bases_.end(),
		[&info](base_class_info const& base)
		{ return base.info == &info; });
	if (it != bases_.end())
	{
		// assert(false && "duplicated inheritance");
		throw std::runtime_error(class_name() + " is already inherited from " + info.class_name());
	}
	bases_.reserve(bases_.size() + 1);
	info.derivatives_.reserve(info.derivatives_.size() + 1);
	bases_.emplace_back(info, cast);
	info.derivatives_.emplace_back(this);
}

template<typename Traits>
V8PP_IMPL bool object_registry<Traits>::cast(pointer_type& ptr, type_info const& actual_type) const
{
	if (this->type == actual_type || !ptr)
	{
		return true;
	}

	// fast way - search a direct parent
	for (base_class_info const& base : bases_)
	{
		if (base.info->type == actual_type)
		{
			ptr = base.cast(ptr);
			return true;
		}
	}

	// slower way - walk on hierarhy
	for (base_class_info const& base : bases_)
	{
		pointer_type p = base.cast(ptr);
		if (base.info->cast(p, actual_type))
		{
			ptr = p;
			return true;
		}
	}
	return false;
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::remove_object(object_id const& obj)
{
	auto* isolate = isolate_;
	{
		native_dispatch_scope dispatch(isolate);
		detach_object(obj);
	}
	drain_native_destructions(isolate);
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::detach_object(object_id id)
{
	if (auto position = objects_.find(id); position != objects_.end())
	{
		auto receipt = std::move(position->second.receipt);
		receipt->registration->registry = nullptr;
		receipt->reset_weak_handle();
		objects_.erase(position);
		v8::HandleScope scope(isolate_);
		reset_object(std::move(receipt));
	}
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::remove_objects()
{
	auto* isolate = isolate_;
	{
		native_dispatch_scope dispatch(isolate);
		v8::HandleScope scope(isolate);
		for (auto& [id, entry] : objects_)
		{
			entry.receipt->registration->registry = nullptr;
			entry.receipt->reset_weak_handle();
		}
		decltype(objects_) objects;
		objects.swap(objects_);
		for (auto& [id, entry] : objects)
		{
			reset_object(std::move(entry.receipt));
		}
	}
	drain_native_destructions(isolate);
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::find_v8_object(pointer_type const& ptr) const
{
	return find_v8_object_by_id(Traits::pointer_id(ptr));
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::find_v8_object_by_id(object_id const& id) const
{
	auto it = objects_.find(id);
	if (it != objects_.end())
	{
		return to_local(isolate_, it->second.wrapper);
	}

	v8::Local<v8::Object> result;
	for (auto const info : derivatives_)
	{
		count_dispatch(&dispatch_counts::derived_registry_probes);
		result = info->find_v8_object_by_id(id);
		if (!result.IsEmpty()) break;
	}
	return result;
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::wrap_this(
	v8::Local<v8::Object> obj, pointer_type const& object, size_t size, external_replacement const* replacement)
{
	if (!object || obj.IsEmpty())
	{
		return {};
	}

	native_dispatch_scope dispatch(isolate_);
	auto self = shared_from_this();
	if (retired)
	{
		throw std::runtime_error("native class has been retired");
	}
	object_id const id = Traits::pointer_id(object);
	if (replacement ? !replacement->find() : objects_.contains(id))
	{
		throw std::runtime_error(class_name() + " duplicate object " + pointer_str(id));
	}

	v8::EscapableHandleScope scope(isolate_);
	if (obj->InternalFieldCount() != 2)
	{
		throw std::runtime_error("invalid native wrapper instance");
	}
	if (!apply_const_properties(obj, object) || (replacement ? !replacement->find() : objects_.contains(id)))
	{
		throw std::runtime_error("native wrapper registration changed during construction");
	}

	auto wrapped = std::make_unique<wrapped_object>(*this, id, size);
	v8::Local<v8::Object> portable_cell;
	v8::Local<v8::External> portable_registration;
#if V8PP_ENABLE_MANAGED_WRAPPERS
	managed_wrapper_registration* managed_cell = nullptr;
	if (managed_wrappers_)
	{
		managed_cell = cppgc::MakeGarbageCollected<managed_wrapper_registration>(
			isolate_->GetCppHeap()->GetAllocationHandle());
		wrapped->managed_registration = managed_cell;
		wrapped->registration = &managed_cell->registration;
	}
	else
#endif
	{
		wrapped->portable_registration = std::make_unique<wrapper_registration>();
		wrapped->registration = wrapped->portable_registration.get();
		v8::Local<v8::Context> context = isolate_->GetCurrentContext();
		portable_cell = state_.create_portable_cell(context);
		if (portable_cell.IsEmpty() ||
			!obj->SetPrivate(context, wrapper_marker(isolate_), v8::True(isolate_)).FromMaybe(false) ||
			!obj->SetPrivate(context, state_.portable_key(), portable_cell).FromMaybe(false))
		{
			throw std::runtime_error("native wrapper registration failed");
		}
		wrapped->portable_cell.Reset(isolate_, portable_cell);
		portable_registration = new_external(isolate_, wrapped->registration);
	}
	if (retired)
	{
		throw std::runtime_error("native wrapper registration changed during construction");
	}
	std::unique_ptr<wrapped_object> replaced;
	auto position = objects_.end();
	if (replacement)
	{
		position = objects_.find(id);
		if (position == objects_.end() || position->second.receipt->replacement != replacement)
		{
			throw std::runtime_error("native wrapper registration changed during construction");
		}
		replaced = std::exchange(position->second.receipt, std::move(wrapped));
		replaced->registration->registry = nullptr;
		if (replaced->size)
		{
			replaced->size = 0;
			size = 0; // The replacement inherits the existing external-memory charge.
		}
	}
	else
	{
		auto const [inserted_position, inserted] = objects_.try_emplace(id, std::move(wrapped));
		if (!inserted)
		{
			throw std::runtime_error("duplicate native wrapper registration");
		}
		position = inserted_position;
	}
	auto& slot = position->second;
	auto& entry = *slot.receipt;
	slot.wrapper.Reset(isolate_, obj);
	if (replaced)
	{
		entry.registration->owner = std::move(replaced->registration->owner);
	}
	else
	{
		entry.registration->owner.template emplace<pointer_type>(object);
	}
	entry.registration->registry = this;
#if V8PP_ENABLE_MANAGED_WRAPPERS
	if (managed_wrappers_)
	{
		v8::Object::Wrap<managed_wrapper_tag>(isolate_, obj, managed_cell);
	}
	else
#endif
	{
		portable_cell->SetInternalField(0, portable_registration);
	}
	// Rehash preserves the inline handle's address; detach unlinks it before erasure.
	entry.make_weak(slot.wrapper);
	if (replaced) reset_object(std::move(replaced));
	if (size)
	{
		increase_allocated_memory(size);
	}

	return scope.Escape(obj);
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::wrap_external(pointer_type const& object, size_t size)
{
	if constexpr (std::same_as<Traits, shared_ptr_traits>)
	{
		auto const position = size ? objects_.find(Traits::pointer_id(object)) : objects_.end();
		if (position != objects_.end())
		{
			auto& receipt = *position->second.receipt;
			auto const& owner = std::get<pointer_type>(receipt.registration->owner);
			if (!receipt.previous || receipt.replacement || !object.use_count() ||
				(receipt.size && receipt.size != size) ||
				object.owner_before(owner) || owner.owner_before(object))
			{
				throw std::runtime_error(class_name() + " duplicate object " + pointer_str(receipt.id));
			}
			native_dispatch_scope dispatch(isolate_);
			auto self = shared_from_this();
			external_replacement replacement(receipt);
			v8::Local<v8::Object> result;
			try
			{
				result = wrap_object(object, size, &replacement);
			}
			catch (...)
			{
				replacement.cancel();
				throw;
			}
			replacement.cancel();
			return result;
		}
	}
	return wrap_object(object, size);
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::wrap_object(
	pointer_type const& object, size_t size, external_replacement const* replacement)
{
	if (!object)
	{
		return {};
	}

	native_dispatch_scope dispatch(isolate_);
	auto self = shared_from_this();
	if (retired) throw std::runtime_error("native class has been retired");
	v8::EscapableHandleScope scope(isolate_);

	v8::Local<v8::Context> context = isolate_->GetCurrentContext();
	v8::Local<v8::Function> func;
	v8::Local<v8::Object> obj;
	if (class_function_template()->GetFunction(context).ToLocal(&func) && func->NewInstance(context).ToLocal(&obj))
	{
		return scope.Escape(wrap_this(obj, object, size, replacement));
	}

	return {};
}

template<typename Traits>
V8PP_IMPL v8::Local<v8::Object> object_registry<Traits>::wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args)
{
	v8::TryCatch factory_error(isolate_);
	native_dispatch_scope dispatch(isolate_);
	auto self = shared_from_this();
	auto ctor = ctor_;
	if (retired || !ctor || !*ctor)
	{
		// assert(false && "create not allowed");
		throw std::runtime_error(class_name() + " has no constructor");
	}
	factory_call factory(*this);
	pointer_type object = nullptr;
	size_t size = 0;
	try
	{
		std::tie(object, size) = (*ctor)(args);
		if (!factory_error.HasCaught()) return wrap_this(args.This(), object, size);
	}
	catch (...)
	{
		rollback_factory_result(object, size);
		if (factory_error.HasCaught()) factory_error.ReThrow();
		throw;
	}
	rollback_factory_result(object, size);
	factory_error.ReThrow();
	return {};
}

template<typename Traits>
V8PP_IMPL typename object_registry<Traits>::pointer_type
object_registry<Traits>::unwrap_object(v8::Local<v8::Value> value, v8::Local<v8::Object>* registered_wrapper)
{
	v8::EscapableHandleScope scope(isolate_);

	constexpr int max_prototype_depth = 16;
	for (int depth = 0; depth <= max_prototype_depth && !value.IsEmpty() && value->IsObject(); ++depth)
	{
		count_dispatch(&dispatch_counts::receiver_checks);
		auto obj = value.As<v8::Object>();
		if (obj->IsProxy())
		{
			return nullptr;
		}

		wrapper_registration* registration = nullptr;
#if V8PP_ENABLE_MANAGED_WRAPPERS
		if (managed_wrappers_)
		{
			if (obj->IsApiWrapper())
			{
				auto* cell = v8::Object::Unwrap<managed_wrapper_tag, managed_wrapper_registration>(isolate_, obj);
				if (!cell) return nullptr;
				registration = &cell->registration;
			}
		}
		else
#endif
		{
			auto context = isolate_->GetCurrentContext();
			v8::Local<v8::Value> cell;
			if (context.IsEmpty() || !obj->GetPrivate(context, state_.portable_key()).ToLocal(&cell))
			{
				return nullptr;
			}
			if (cell->IsObject())
			{
				auto entry = cell.As<v8::Object>()->GetInternalField(0).As<v8::Value>();
				if (!entry->IsExternal()) return nullptr;
				registration = static_cast<wrapper_registration*>(external_value(entry.As<v8::External>()));
			}
			else if (obj->HasPrivate(context, wrapper_marker(isolate_)).FromMaybe(true) ||
				obj->InternalFieldCount() != 0)
			{
				return nullptr;
			}
		}

		if (registration)
		{
			auto* owner = registration->registry;
			if (!owner || owner->retired || owner->traits != traits)
			{
				return nullptr;
			}
			auto* registry = static_cast<object_registry*>(owner);
			if constexpr (std::same_as<pointer_type, std::shared_ptr<void>>)
			{
				count_dispatch(&dispatch_counts::shared_owner_copies);
			}
			pointer_type ptr = std::get<pointer_type>(registration->owner);
			if constexpr (std::same_as<Traits, shared_ptr_traits>)
			{
				if (registry->type != type)
				{
					// The canonical owner pins the object while the cast graph adjusts its address.
					auto adjusted = Traits::key(Traits::pointer_id(ptr));
					if (!registry->cast(adjusted, type)) return nullptr;
					ptr = pointer_type(std::move(ptr), adjusted.get());
				}
			}
			else if (!registry->cast(ptr, type))
			{
				return nullptr;
			}
			if (registered_wrapper) *registered_wrapper = scope.Escape(obj);
			return ptr;
		}
		value = wrapper_prototype(obj);
	}
	return nullptr;
}

template<typename Traits>
V8PP_IMPL void object_registry<Traits>::reset_object(std::unique_ptr<wrapped_object> wrapped)
{
	assert(!wrapped->registration->registry);
	assert(!wrapped->weak_handle);
	pointer_type object = std::get<pointer_type>(std::move(wrapped->registration->owner));
	wrapped->registration->owner.template emplace<0>(nullptr);
	if (!wrapped->portable_cell.IsEmpty())
	{
		to_local(isolate_, wrapped->portable_cell)->SetInternalField(0, v8::Undefined(isolate_));
		wrapped->portable_cell.Reset();
	}
	wrapped->portable_registration.reset();
#if V8PP_ENABLE_MANAGED_WRAPPERS
	if (managed_wrappers_) wrapped->managed_registration.Clear();
#endif
	wrapped->registration = nullptr;
	auto const size = wrapped->size;
	if (wrapped->previous)
	{
		wrapped->detached = true;
		// The already-scheduled V8 second pass owns the detached receipt.
		wrapped.release();
	}
	if (size)
	{
		for (auto* factory = factory_call_; factory; factory = factory->previous)
		{
			factory->retired_objects.push_back(Traits::pointer_id(object));
		}
		decrease_allocated_memory(size);
	}
	if constexpr (std::is_same_v<pointer_type, void*>)
	{
		if (!size) return;
	}
	else if (!object && !size)
	{
		return;
	}
	defer_native_destruction(isolate_, [isolate = isolate_, destroy = dtor_, object = std::move(object), size]
		{
			if (size) (*destroy)(isolate, object); });
}

/////////////////////////////////////////////////////////////////////////////
//
// classes
//
template<typename Traits>
V8PP_IMPL object_registry<Traits>& classes::add(v8::Isolate* isolate, type_info const& type,
	typename object_registry<Traits>::dtor_function&& dtor)
{
	check_native_publication(isolate);
	native_dispatch_scope dispatch(isolate);
	classes* info = instance(operation::add, isolate);
	auto it = info->classes_.find(type.id());
	if (it != info->classes_.end())
	{
		// assert(false && "class already registred");
		throw std::runtime_error(it->second->class_name() + " is already exist in isolate " + pointer_str(isolate));
	}
	auto registry_ptr = std::make_shared<object_registry<Traits>>(*info, isolate, type, std::move(dtor));
	info->classes_[type.id()] = registry_ptr;
	return *registry_ptr;
}

template<typename Traits>
V8PP_IMPL void classes::remove(v8::Isolate* isolate, type_info const& type)
{
	native_dispatch_scope dispatch(isolate);
	classes* info = instance(operation::get, isolate);
	if (info)
	{
		auto it = info->classes_.find(type.id());
		if (it != info->classes_.end())
		{
			type_info const& traits = type_id<Traits>();
			if (it->second->traits != traits)
			{
				throw std::runtime_error(it->second->class_name() + " is already registered in isolate " + pointer_str(isolate) + " before of " + class_info::class_name(type, traits));
			}
			it->second->retired = true;
			auto registry = info->classes_.extract(it);
			registry.mapped()->retire();
		}
	}
}

template<typename Traits>
V8PP_IMPL object_registry<Traits>& classes::find(v8::Isolate* isolate, type_info const& type)
{
	classes* info = instance(operation::get, isolate);
	type_info const& traits = type_id<Traits>();
	if (info)
	{
		auto it = info->classes_.find(type.id());
		if (it != info->classes_.end())
		{
			if (it->second->traits != traits)
			{
				throw std::runtime_error(it->second->class_name() + " is already registered in isolate " + pointer_str(isolate) + " before of " + class_info::class_name(type, traits));
			}
			return *static_cast<object_registry<Traits>*>(it->second.get());
		}
	}
	// assert(false && "class not registered");
	throw std::runtime_error(class_info::class_name(type, traits) + " is not registered in isolate " + pointer_str(isolate));
}

V8PP_IMPL void classes::remove_all(v8::Isolate* isolate)
{
	native_dispatch_scope dispatch(isolate);
	auto* info = instance(operation::get, isolate);
	if (!info) return;
	for (auto& [id, registry] : info->classes_)
	{
		registry->retired = true;
	}
	classes_info retired;
	retired.swap(info->classes_);
	info->portable_key_.Reset();
	for (auto& [id, registry] : retired)
	{
		registry->retire();
	}
}

V8PP_IMPL classes* classes::instance(operation op, v8::Isolate* isolate)
{
	auto* state = op == operation::add
		? &native_dispatch_state::ensure(isolate) : native_dispatch_state::get(isolate);
	if (!state) return nullptr;
	if (!state->registrations && op == operation::add)
		state->registrations.reset(new classes(isolate));
	return static_cast<classes*>(state->registrations.get());
}

V8PP_IMPL v8::Local<v8::Private> classes::portable_key()
{
	if (portable_key_.IsEmpty())
	{
		portable_key_.Reset(isolate_, v8::Private::New(isolate_));
	}
	return to_local(isolate_, portable_key_);
}

V8PP_IMPL v8::Local<v8::Object> classes::create_portable_cell(v8::Local<v8::Context> context)
{
	if (portable_template_.IsEmpty())
	{
		auto cell_template = v8::ObjectTemplate::New(isolate_);
		cell_template->SetInternalFieldCount(1);
		portable_template_.Reset(isolate_, cell_template);
	}
	v8::Local<v8::Object> cell;
	if (!to_local(isolate_, portable_template_)->NewInstance(context).ToLocal(&cell))
	{
		return {};
	}
	return cell;
}

V8PP_IMPL void classes::release_if_idle()
{
	native_dispatch_state::get(isolate_)->release_if_idle();
}

V8PP_IMPL void dispose_native_registrations(v8::Isolate* isolate, std::exception_ptr* deferred_error)
{
	if (auto* state = native_dispatch_state::get(isolate); state && (state->depth || state->draining))
		throw std::runtime_error("cannot dispose bindings during native dispatch");

	if (auto* info = classes::instance(classes::operation::get, isolate); info && info->pending_weak_)
	{
		// Forced collection synchronously runs V8's queued second passes.
		native_dispatch_scope dispatch(isolate);
		isolate->LowMemoryNotification();
	}
	try
	{
		drain_native_destructions(isolate);
	}
	catch (...)
	{
		if (!deferred_error) throw;
		*deferred_error = std::current_exception();
	}
}

} // namespace v8pp::detail

namespace v8pp {

V8PP_IMPL void cleanup(v8::Isolate* isolate)
{
	detail::native_dispatch_scope dispatch(isolate);
	detail::external_data::revoke_all(isolate);
	detail::classes::remove_all(isolate);
}

V8PP_IMPL std::vector<std::shared_ptr<void>> dispose_bindings(v8::Isolate* isolate,
	std::exception_ptr* deferred_error)
{
	detail::native_disposal_scope disposing(isolate);
	cleanup(isolate);
	detail::dispose_native_registrations(isolate, deferred_error);
	return detail::external_data::take_resources(isolate);
}

} // namespace v8pp
