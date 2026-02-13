#pragma once

#include <algorithm>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "v8pp/config.hpp"
#include "v8pp/function.hpp"
#include "v8pp/overload.hpp"
#include "v8pp/property.hpp"
#include "v8pp/ptr_traits.hpp"
#include "v8pp/type_info.hpp"

#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 3)
#include <v8-external-memory-accounter.h>
#endif
#include "v8pp/object.hpp"

namespace v8pp::detail {

struct class_info
{
	static constexpr uint32_t kMagic = 0xC1A5517F;

	uint32_t const magic = kMagic;
	type_info const type;
	type_info const traits;

	class_info(type_info const& type, type_info const& traits);

	virtual ~class_info()
	{
		const_cast<uint32_t&>(magic) = 0;
	}

	bool is_valid() const { return magic == kMagic; }

	std::string class_name() const;
};

template<typename Traits>
class object_registry final : public class_info
{
public:
	using pointer_type = typename Traits::pointer_type;
	using const_pointer_type = typename Traits::const_pointer_type;
	using object_id = typename Traits::object_id;

	using ctor_function = std::function<std::pair<pointer_type, size_t>(v8::FunctionCallbackInfo<v8::Value> const& args)>;
	using dtor_function = std::function<void(v8::Isolate*, pointer_type const&)>;
	using cast_function = pointer_type (*)(pointer_type const&);

	object_registry(v8::Isolate* isolate, type_info const& type, dtor_function&& dtor);

	object_registry(object_registry const&) = delete;
	object_registry(object_registry&&) = default;

	object_registry& operator=(object_registry const&) = delete;
	object_registry& operator=(object_registry&&) = delete;

	~object_registry();

	v8::Isolate* isolate() { return isolate_; }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return to_local(isolate_, func_);
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return to_local(isolate_, js_func_);
	}

	void set_auto_wrap_objects(bool auto_wrap) { auto_wrap_objects_ = auto_wrap; }
	bool auto_wrap_objects() const { return auto_wrap_objects_; }

	void set_ctor(ctor_function&& ctor) { ctor_ = std::move(ctor); }

	void add_base(object_registry& info, cast_function cast);
	bool cast(pointer_type& ptr, type_info const& actual_type) const;

	void remove_object(object_id const& obj);
	void remove_objects();

	pointer_type find_object(object_id id, type_info const& actual_type) const;
	v8::Local<v8::Object> find_v8_object(pointer_type const& ptr) const;

	v8::Local<v8::Object> wrap_this(v8::Local<v8::Object> obj, pointer_type const& object, size_t size);
	v8::Local<v8::Object> wrap_object(pointer_type const& object, size_t size);
	v8::Local<v8::Object> wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args);
	pointer_type unwrap_object(v8::Local<v8::Value> value);

	std::unordered_map<std::string, std::function<v8::Local<v8::Value>(v8::Isolate*, pointer_type)>> const_properties;

	void apply_const_properties(v8::Isolate* isolate, v8::Local<v8::Object> obj, pointer_type const& ext)
	{
		// Set constant properties
		for (const auto& base : bases_)
		{
			base.info.apply_const_properties(isolate, obj, ext);
		}

		for (const auto& [name, func] : const_properties)
		{
			v8pp::set_const(isolate, obj, name, func(isolate, ext));
		}
	}

private:
	struct wrapped_object
	{
		v8::Global<v8::Object> pobj;
		size_t size; // 0 for referenced objects
	};

	void reset_object(pointer_type const& object, wrapped_object& wrapped);

	struct base_class_info
	{
		object_registry& info;
		cast_function cast;

		base_class_info(object_registry& info, cast_function cast)
			: info(info)
			, cast(cast)
		{
		}
	};

	std::vector<base_class_info> bases_;
	std::vector<object_registry*> derivatives_;
	std::unordered_map<pointer_type, wrapped_object> objects_;

	v8::Isolate* isolate_;
	v8::Global<v8::FunctionTemplate> func_;
	v8::Global<v8::FunctionTemplate> js_func_;

#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 3)
	v8::ExternalMemoryAccounter external_memory_accounter_;

	void increase_allocated_memory(size_t size)
	{
		external_memory_accounter_.Increase(isolate_, size);
	}

	void decrease_allocated_memory(size_t size)
	{
		external_memory_accounter_.Decrease(isolate_, size);
	}
#else
	void increase_allocated_memory(size_t size)
	{
		isolate_->AdjustAmountOfExternalAllocatedMemory(static_cast<int64_t>(size));
	}
	void decrease_allocated_memory(size_t size)
	{
		isolate_->AdjustAmountOfExternalAllocatedMemory(-static_cast<int64_t>(size));
	}
#endif

	ctor_function ctor_;
	dtor_function dtor_;
	bool auto_wrap_objects_;
};

class classes
{
public:
	template<typename Traits>
	static object_registry<Traits>& add(v8::Isolate* isolate, type_info const& type,
		typename object_registry<Traits>::dtor_function&& dtor);

	template<typename Traits>
	static void remove(v8::Isolate* isolate, type_info const& type);

	template<typename Traits>
	static object_registry<Traits>& find(v8::Isolate* isolate, type_info const& type);

	static void remove_all(v8::Isolate* isolate);

private:
	using classes_info = std::unordered_map<uintptr_t, std::unique_ptr<class_info>>;
	classes_info classes_;

	enum class operation
	{
		get,
		add,
		remove
	};
	static classes* instance(operation op, v8::Isolate* isolate);
};

} // namespace v8pp::detail

namespace v8pp {

template<typename T, typename Traits>
class class_;

namespace detail {

/// Holds begin/end function objects and provides V8 callbacks for the iterator protocol.
/// Used by class_<T>::iterable() to implement Symbol.iterator.
template<typename T, typename Traits, typename BeginFn, typename EndFn>
struct iterator_factory
{
	BeginFn begin_fn;
	EndFn end_fn;

	using begin_result = std::invoke_result_t<BeginFn, T const&>;
	using end_result = std::invoke_result_t<EndFn, T const&>;

	struct state
	{
		begin_result current;
		end_result end;
		v8::Global<v8::Object> container_ref; // prevent GC of container during iteration
	};

	static void iterator_callback(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();
		v8::HandleScope scope(isolate);

		try
		{
			auto self = class_<T, Traits>::unwrap_object(isolate, args.This());
			if (!self)
			{
				args.GetReturnValue().Set(
					throw_ex(isolate, "calling [Symbol.iterator] on null instance"));
				return;
			}

			decltype(auto) factory = external_data::get<iterator_factory>(args.Data());

			auto* iter_state = new state{
				std::invoke(factory.begin_fn, std::as_const(*self)),
				std::invoke(factory.end_fn, std::as_const(*self)),
				v8::Global<v8::Object>(isolate, args.This().As<v8::Object>())
			};

			v8::Local<v8::Context> context = isolate->GetCurrentContext();
			v8::Local<v8::Object> iter_obj = v8::Object::New(isolate);

			v8::Local<v8::External> state_ext = v8::External::New(isolate, iter_state);
			v8::Local<v8::Function> next_fn;
			if (!v8::Function::New(context, &next_callback, state_ext).ToLocal(&next_fn))
			{
				delete iter_state;
				args.GetReturnValue().Set(throw_ex(isolate, "failed to create iterator next()"));
				return;
			}

			iter_obj->Set(context, v8pp::to_v8_name(isolate, "next"), next_fn).FromJust();

			// weak ref to clean up state when iterator object is GC'd
			auto* weak_data = new weak_ref_data{ iter_state, v8::Global<v8::Object>(isolate, iter_obj) };
			weak_data->handle.SetWeak(weak_data, weak_callback, v8::WeakCallbackType::kParameter);

			args.GetReturnValue().Set(iter_obj);
		}
		catch (std::exception const& ex)
		{
			args.GetReturnValue().Set(throw_ex(isolate, ex.what()));
		}
	}

	static void next_callback(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();
		v8::HandleScope scope(isolate);

		try
		{
			auto* iter_state = static_cast<state*>(
				args.Data().As<v8::External>()->Value());

			v8::Local<v8::Context> context = isolate->GetCurrentContext();
			v8::Local<v8::Object> result = v8::Object::New(isolate);

			if (iter_state->current == iter_state->end)
			{
				result->Set(context,
						  v8pp::to_v8_name(isolate, "value"),
						  v8::Undefined(isolate))
					.FromJust();
				result->Set(context,
						  v8pp::to_v8_name(isolate, "done"),
						  v8::Boolean::New(isolate, true))
					.FromJust();
			}
			else
			{
				result->Set(context,
						  v8pp::to_v8_name(isolate, "value"),
						  v8pp::to_v8(isolate, *iter_state->current))
					.FromJust();
				result->Set(context,
						  v8pp::to_v8_name(isolate, "done"),
						  v8::Boolean::New(isolate, false))
					.FromJust();
				++iter_state->current;
			}

			args.GetReturnValue().Set(result);
		}
		catch (std::exception const& ex)
		{
			args.GetReturnValue().Set(throw_ex(isolate, ex.what()));
		}
	}

	struct weak_ref_data
	{
		state* iter_state;
		v8::Global<v8::Object> handle;
	};

	static void weak_callback(v8::WeakCallbackInfo<weak_ref_data> const& info)
	{
		auto* ref = info.GetParameter();
		delete ref->iter_state;
		ref->handle.Reset();
		delete ref;
	}
};

/// Wraps a non-member callable for Symbol.toPrimitive, unwrapping `this` from args.This().
/// The callable signature: ReturnType(T const&, std::string_view hint)
template<typename T, typename Traits, typename Function>
struct to_primitive_invoker
{
	Function func;

	static void callback(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();
		v8::HandleScope scope(isolate);

		try
		{
			auto self = class_<T, Traits>::unwrap_object(isolate, args.This());
			if (!self)
			{
				args.GetReturnValue().Set(
					throw_ex(isolate, "calling [Symbol.toPrimitive] on null instance"));
				return;
			}

			decltype(auto) invoker = external_data::get<to_primitive_invoker>(args.Data());

			std::string hint;
			if (args.Length() > 0)
			{
				hint = from_v8<std::string>(isolate, args[0]);
			}

			using return_type = typename function_traits<Function>::return_type;
			if constexpr (std::same_as<return_type, void>)
			{
				std::invoke(invoker.func, std::as_const(*self), std::string_view(hint));
			}
			else
			{
				args.GetReturnValue().Set(
					to_v8(isolate, std::invoke(invoker.func, std::as_const(*self), std::string_view(hint))));
			}
		}
		catch (std::exception const& ex)
		{
			args.GetReturnValue().Set(throw_ex(isolate, ex.what()));
		}
	}
};

} // namespace detail

/// Interface to access C++ classes bound to V8
template<typename T, typename Traits = raw_ptr_traits>
class class_
{
	static_assert(is_wrapped_class<T>::value, "T must be a user-defined class");

	using object_registry = detail::object_registry<Traits>;
	object_registry& class_info_;

	using object_id = typename object_registry::object_id;
	using pointer_type = typename object_registry::pointer_type;
	using const_pointer_type = typename object_registry::const_pointer_type;

public:
	using object_pointer_type = typename Traits::template object_pointer_type<T>;
	using object_const_pointer_type = typename Traits::template object_const_pointer_type<T>;

	using ctor_function = std::function<object_pointer_type(v8::FunctionCallbackInfo<v8::Value> const& args)>;
	using dtor_function = std::function<void(v8::Isolate* isolate, object_pointer_type const& obj)>;

private:
	template<typename... Args>
	static object_pointer_type object_create(Args&&... args)
	{
		return Traits::template create<T>(std::forward<Args>(args)...);
	}

	template<typename... Args>
	struct object_create_from_v8
	{
		static object_pointer_type call(v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			return detail::call_from_v8<Traits>(Traits::template create<T, Args...>, args);
		}
	};

	static void object_destroy(v8::Isolate*, pointer_type const& ptr)
	{
		Traits::destroy(Traits::template static_pointer_cast<T>(ptr));
	}

	explicit class_(v8::Isolate* isolate, detail::type_info const& existing)
		: class_info_(detail::classes::find<Traits>(isolate, existing))
	{
	}

public:
	explicit class_(v8::Isolate* isolate, dtor_function destroy = &object_destroy)
		: class_info_(detail::classes::add<Traits>(isolate, detail::type_id<T>(),
			  [destroy = std::move(destroy)](v8::Isolate* isolate, pointer_type const& obj)
			  {
				  destroy(isolate, Traits::template static_pointer_cast<T>(obj));
			  }))
	{
	}

	class_(class_ const&) = delete;
	class_& operator=(class_ const&) = delete;

	class_(class_&&) = default;
	class_& operator=(class_&&) = delete;

	/// Find existing class_ to extend bindings
	static class_ extend(v8::Isolate* isolate)
	{
		return class_(isolate, detail::type_id<T>());
	}

	/// Set class constructor signature
	template<typename... Args, typename Create = object_create_from_v8<Args...>>
	class_& ctor(ctor_function create = &Create::call)
	{
		class_info_.set_ctor([create = std::move(create)](v8::FunctionCallbackInfo<v8::Value> const& args)
			{
			auto object = create(args);
			return std::make_pair(object, Traits::object_size(object)); });
		return *this;
	}

	/// Set class constructor signature with default parameter values
	template<typename... Args, typename... Defs>
	class_& ctor(v8pp::defaults<Defs...> defs)
	{
		class_info_.set_ctor([defs = std::move(defs)](v8::FunctionCallbackInfo<v8::Value> const& args)
			{
			auto object = detail::call_from_v8<Traits>(Traits::template create<T, Args...>, args, defs);
			return std::make_pair(object, Traits::object_size(object)); });
		return *this;
	}

	/// Set class constructor from a factory function with default parameter values.
	/// The factory must return object_pointer_type (T* for raw_ptr_traits, shared_ptr<T> for shared_ptr_traits).
	template<typename Function, typename... Defs>
	requires(detail::is_callable<std::decay_t<Function>>::value && !v8pp::is_defaults<std::decay_t<Function>>::value)
	class_& ctor(Function&& func, v8pp::defaults<Defs...> defs)
	{
		using F = std::decay_t<Function>;
		static_assert(std::is_convertible_v<typename detail::function_traits<F>::return_type, object_pointer_type>,
			"Constructor factory must return object_pointer_type");

		class_info_.set_ctor(
			[func = F(std::forward<Function>(func)), defs = std::move(defs)](v8::FunctionCallbackInfo<v8::Value> const& args)
			{
				F f = func; // copy from captured const
				auto object = detail::call_from_v8<Traits>(std::move(f), args, defs);
				return std::make_pair(pointer_type(object), Traits::object_size(object));
			});
		return *this;
	}

	/// Set class constructor with multiple overloaded factory functions (multi-dispatch).
	/// Each factory must return object_pointer_type. Dispatched by arity + type matching (first-match-wins).
	/// Accepts plain callables and v8pp::with_defaults() entries.
	template<typename F1, typename F2, typename... Fs>
	requires((detail::is_callable<std::decay_t<F1>>::value || is_overload_entry<std::decay_t<F1>>::value) && (detail::is_callable<std::decay_t<F2>>::value || is_overload_entry<std::decay_t<F2>>::value) && !std::is_member_function_pointer_v<std::decay_t<F1>> && !std::is_member_function_pointer_v<std::decay_t<F2>>)
	class_& ctor(F1&& f1, F2&& f2, Fs&&... fs)
	{
		using Set = detail::overload_set<
			decltype(detail::make_overload_entry(std::forward<F1>(f1))),
			decltype(detail::make_overload_entry(std::forward<F2>(f2))),
			decltype(detail::make_overload_entry(std::forward<Fs>(fs)))...>;

		class_info_.set_ctor(
			[set = Set{ std::make_tuple(
				 detail::make_overload_entry(std::forward<F1>(f1)),
				 detail::make_overload_entry(std::forward<F2>(f2)),
				 detail::make_overload_entry(std::forward<Fs>(fs))...) }](v8::FunctionCallbackInfo<v8::Value> const& args)
			{
				v8::Isolate* isolate = args.GetIsolate();
				size_t const arg_count = args.Length();
				bool matched = false;
				object_pointer_type result{};
				std::string errors;

				std::apply([&](auto const&... entries)
					{ ((matched || [&]
						   {
							   using Entry = std::decay_t<decltype(entries)>;
							   using F = typename detail::entry_func_type<Entry>::type;

							   constexpr size_t min = detail::overload_arg_range<Entry>::min_args;
							   constexpr size_t max = detail::overload_arg_range<Entry>::max_args;
							   if (arg_count < min || arg_count > max)
								   return false;

							   if (arg_count > 0 && !detail::overload_types_match<F, Traits>(isolate, args, arg_count))
								   return false;

							   try
							   {
								   result = object_pointer_type(detail::call_ctor_entry<Traits>(entries, args));
								   matched = true;
								   return true;
							   }
							   catch (std::exception const& ex)
							   {
								   if (!errors.empty()) errors += "; ";
								   errors += ex.what();
								   return false;
							   }
						   }()),
						  ...); }, set.entries);

				if (!matched)
				{
					std::string msg = "No matching constructor overload for " + std::to_string(arg_count) + " argument(s)";
					if (!errors.empty())
					{
						msg += ". Tried: " + errors;
					}
					throw std::runtime_error(msg);
				}

				return std::make_pair(pointer_type(result), Traits::object_size(result));
			});
		return *this;
	}

	/// Inhert from C++ class U
	template<typename U>
	class_& inherit()
	{
		static_assert(std::derived_from<T, U>, "Class U should be base for class T");
		// TODO: std::is_convertible<T*, U*> and check for duplicates in hierarchy?
		auto& base = detail::classes::find<Traits>(isolate(), detail::type_id<U>());
		class_info_.add_base(base, [](pointer_type const& ptr)
			{ return pointer_type{ Traits::template static_pointer_cast<U>(
				  Traits::template static_pointer_cast<T>(ptr)) }; });
		class_info_.js_function_template()->Inherit(base.class_function_template());
		return *this;
	}

	/// Enable new C++ objects auto-wrapping
	class_& auto_wrap_objects(bool auto_wrap = true)
	{
		class_info_.set_auto_wrap_objects(auto_wrap);
		return *this;
	}

	/// Set class member function, or static function, or lambda
	/// Const member functions are automatically tagged as side-effect-free
	template<typename Function>
	class_& function(std::string_view name, Function&& func, v8::PropertyAttribute attr = v8::None)
	{
		constexpr auto effect = detail::is_const_member_function_v<std::decay_t<Function>> ? v8::SideEffectType::kHasNoSideEffect : v8::SideEffectType::kHasSideEffect;
		return function_impl(name, std::forward<Function>(func), effect, attr);
	}

	/// Set class member function with explicit side-effect type
	template<typename Function>
	class_& function(std::string_view name, Function&& func,
		v8::SideEffectType side_effect, v8::PropertyAttribute attr = v8::None)
	{
		return function_impl(name, std::forward<Function>(func), side_effect, attr);
	}

	/// Set class member function with default parameter values
	template<typename Function, typename... Defs>
	class_& function(std::string_view name, Function&& func, v8pp::defaults<Defs...> defs,
		v8::PropertyAttribute attr = v8::None)
	{
		constexpr auto effect = detail::is_const_member_function_v<std::decay_t<Function>> ? v8::SideEffectType::kHasNoSideEffect : v8::SideEffectType::kHasSideEffect;
		return function_impl(name, std::forward<Function>(func), std::move(defs), effect, attr);
	}

	/// Set class function with Fast API callback
	template<auto FuncPtr>
	class_& function(std::string_view name, fast_function<FuncPtr>, v8::PropertyAttribute attr = v8::None)
	{
		using F = typename fast_function<FuncPtr>::func_type;
		constexpr bool is_mem_fun = std::is_member_function_pointer_v<F>;
		constexpr auto side_effect = detail::is_const_member_function_v<F> ? v8::SideEffectType::kHasNoSideEffect : v8::SideEffectType::kHasSideEffect;

		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto wrapped_fun = wrap_function_template<FuncPtr, Traits>(isolate(),
			fast_function<FuncPtr>{}, side_effect);

		v8::Local<v8::FunctionTemplate> js_func = class_info_.js_function_template();
		js_func->PrototypeTemplate()->Set(v8_name, wrapped_fun, attr);
		if constexpr (!is_mem_fun)
		{
			js_func->Set(v8_name, wrapped_fun, attr);
		}
		return *this;
	}

	/// Set multiple overloaded member/static functions
	template<typename F1, typename F2, typename... Fs>
	requires(detail::is_callable<std::decay_t<F2>>::value || std::is_member_function_pointer_v<std::decay_t<F2>> || is_overload_entry<std::decay_t<F2>>::value)
	class_& function(std::string_view name, F1&& f1, F2&& f2, Fs&&... fs)
	{
		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::Local<v8::Data> wrapped_fun = wrap_overload_template<Traits>(isolate(),
			std::forward<F1>(f1), std::forward<F2>(f2), std::forward<Fs>(fs)...);

		v8::Local<v8::FunctionTemplate> js_func = class_info_.js_function_template();
		js_func->PrototypeTemplate()->Set(v8_name, wrapped_fun);
		// Also on constructor for static access
		js_func->Set(v8_name, wrapped_fun);
		return *this;
	}

	/// Set class member variable
	template<typename Attribute>
	class_& var(std::string_view name, Attribute attribute)
	{
		static_assert(std::is_member_object_pointer_v<Attribute>, "Attribute must be pointer to member data");

		v8::HandleScope scope(isolate());

		using attribute_type = typename detail::function_traits<Attribute>::template pointer_type<T>;
		attribute_type attr = attribute;

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::AccessorNameGetterCallback getter = &member_get<attribute_type>;
		v8::AccessorNameSetterCallback setter = &member_set<attribute_type>;
		v8::Local<v8::Value> data = detail::external_data::set(isolate(), std::forward<attribute_type>(attr));
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
		// SetAccessor removed from ObjectTemplate in V8 12.9+
		class_info_.js_function_template()
			->InstanceTemplate()
			->SetNativeDataProperty(v8_name, getter, setter, data,
				v8::PropertyAttribute(v8::DontDelete),
				v8::SideEffectType::kHasNoSideEffect,
				v8::SideEffectType::kHasSideEffectToReceiver);
#else
		class_info_.js_function_template()
			->PrototypeTemplate()
			->SetAccessor(v8_name, getter, setter, data,
				v8::DEFAULT, v8::PropertyAttribute(v8::DontDelete),
				v8::SideEffectType::kHasNoSideEffect,
				v8::SideEffectType::kHasSideEffectToReceiver);
#endif
		return *this;
	}

	/// Set read/write class property with getter and setter
	template<typename GetFunction, typename SetFunction = detail::none>
	requires(!is_fast_function<std::decay_t<GetFunction>>::value)
	class_& property(std::string_view name, GetFunction&& get, SetFunction&& set = {})
	{
		using Getter = typename std::conditional_t<std::is_member_function_pointer_v<GetFunction>,
			typename detail::function_traits<GetFunction>::template pointer_type<T>,
			typename std::decay_t<GetFunction>>;

		using Setter = typename std::conditional_t<std::is_member_function_pointer_v<SetFunction>,
			typename detail::function_traits<SetFunction>::template pointer_type<T>,
			typename std::decay_t<SetFunction>>;

		static_assert(std::is_member_function_pointer_v<GetFunction> || detail::is_callable<Getter>::value, "GetFunction must be callable");
		static_assert(std::is_member_function_pointer_v<SetFunction> || detail::is_callable<Setter>::value || std::same_as<Setter, detail::none>, "SetFunction must be callable");

		using GetClass = std::conditional_t<detail::function_with_object<Getter, T>, T, detail::none>;
		using SetClass = std::conditional_t<detail::function_with_object<Setter, T>, T, detail::none>;

		using property_type = v8pp::property<Getter, Setter, GetClass, SetClass>;

		v8::HandleScope scope(isolate());

		v8::AccessorNameGetterCallback getter = property_type::template get<Traits>;
		v8::AccessorNameSetterCallback setter = property_type::is_readonly ? nullptr : property_type::template set<Traits>;
		v8::Local<v8::String> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::Local<v8::Value> data = detail::external_data::set(isolate(), property_type(std::move(get), std::move(set)));

		v8::SideEffectType setter_effect = property_type::is_readonly ? v8::SideEffectType::kHasSideEffect : v8::SideEffectType::kHasSideEffectToReceiver;
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
		// SetAccessor removed from ObjectTemplate in V8 12.9+
		class_info_.js_function_template()->InstanceTemplate()->SetNativeDataProperty(v8_name, getter, setter, data,
			v8::PropertyAttribute(v8::DontDelete),
			v8::SideEffectType::kHasNoSideEffect, setter_effect);
#else
		class_info_.js_function_template()->PrototypeTemplate()->SetAccessor(v8_name, getter, setter, data,
			v8::DEFAULT, v8::PropertyAttribute(v8::DontDelete),
			v8::SideEffectType::kHasNoSideEffect, setter_effect);
#endif
		return *this;
	}

	/// Set read-only class property with V8 Fast API getter
	template<auto GetPtr>
	class_& property(std::string_view name, fast_function<GetPtr>)
	{
		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto getter_template = wrap_function_template<GetPtr, Traits>(
			isolate(), fast_function<GetPtr>{},
			v8::SideEffectType::kHasNoSideEffect);

		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template,
			v8::Local<v8::FunctionTemplate>(),
			v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete));

		return *this;
	}

	/// Set read/write class property with V8 Fast API getter and setter
	template<auto GetPtr, auto SetPtr>
	class_& property(std::string_view name, fast_function<GetPtr>, fast_function<SetPtr>)
	{
		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto getter_template = wrap_function_template<GetPtr, Traits>(
			isolate(), fast_function<GetPtr>{},
			v8::SideEffectType::kHasNoSideEffect);
		auto setter_template = wrap_function_template<SetPtr, Traits>(
			isolate(), fast_function<SetPtr>{},
			v8::SideEffectType::kHasSideEffectToReceiver);

		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template, setter_template,
			v8::PropertyAttribute(v8::DontDelete));

		return *this;
	}

	/// Set value as a read-only constant, once by the class instance
	template<typename GetFunction>
	class_& const_property(std::string_view name, GetFunction&& get)
	{
		using Getter = typename std::conditional<
			std::is_member_function_pointer<GetFunction>::value,
			typename detail::function_traits<GetFunction>::template pointer_type<T>,
			typename std::decay<GetFunction>::type>::type;

		static_assert(std::is_member_function_pointer<GetFunction>::value || detail::is_callable<Getter>::value, "GetFunction must be callable");

		v8::HandleScope scope(isolate());

		// Store the native function for the constant property in object_registry
		class_info_.const_properties.emplace(name, [get = std::move(get)](v8::Isolate* isolate, pointer_type obj)
			{
				auto typed_obj = Traits::template static_pointer_cast<T>(obj);
				return to_v8(isolate, ((*typed_obj).*get)()); });
		return *this;
	}

	/// Set value as a read-only constant
	template<typename Value>
	class_& const_(std::string_view name, Value const& value)
	{
		v8::HandleScope scope(isolate());

		class_info_.js_function_template()->PrototypeTemplate()->Set(v8pp::to_v8_name(isolate(), name), to_v8(isolate(), value),
			v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete));
		return *this;
	}

	/// Set Symbol.toStringTag on prototype for custom [object Tag] output
	class_& to_string_tag(std::string_view tag)
	{
		v8::Isolate* iso = isolate();
		v8::HandleScope scope(iso);

		class_info_.js_function_template()->PrototypeTemplate()->Set(
			v8::Symbol::GetToStringTag(iso), v8pp::to_v8(iso, tag),
			v8::PropertyAttribute(v8::ReadOnly | v8::DontEnum | v8::DontDelete));
		return *this;
	}

	/// Set Symbol.toPrimitive on prototype for custom type coercion
	/// func signature: ReturnType(std::string_view hint) as member, or
	///                 ReturnType(T const&, std::string_view hint) as free function/lambda
	template<typename Function>
	class_& to_primitive(Function&& func)
	{
		constexpr bool is_mem_fun = std::is_member_function_pointer_v<Function>;

		static_assert(is_mem_fun || detail::is_callable<Function>::value,
			"Function must be pointer to member function or callable object");

		v8::Isolate* iso = isolate();
		v8::HandleScope scope(iso);

		v8::Local<v8::Data> wrapped_fun;
		if constexpr (is_mem_fun)
		{
			using mem_func_type = typename detail::function_traits<Function>::template pointer_type<T>;
			wrapped_fun = wrap_function_template<mem_func_type, Traits>(iso,
				mem_func_type(std::forward<Function>(func)),
				v8::SideEffectType::kHasNoSideEffect);
		}
		else
		{
			using Invoker = detail::to_primitive_invoker<T, Traits, std::decay_t<Function>>;
			v8::Local<v8::Value> data = detail::external_data::set(iso,
				Invoker{ std::decay_t<Function>(std::forward<Function>(func)) });
			wrapped_fun = v8::FunctionTemplate::New(iso, &Invoker::callback, data,
				v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
				v8::SideEffectType::kHasNoSideEffect);
		}

		class_info_.js_function_template()->PrototypeTemplate()->Set(
			v8::Symbol::GetToPrimitive(iso), wrapped_fun,
			v8::PropertyAttribute(v8::DontEnum | v8::DontDelete));
		return *this;
	}

	/// Make this class iterable in JavaScript (for...of, spread, Array.from, etc.)
	/// begin_fn and end_fn: member functions or callables taking T const& and returning iterators
	template<typename BeginFn, typename EndFn>
	class_& iterable(BeginFn&& begin_fn, EndFn&& end_fn)
	{
		v8::Isolate* iso = isolate();
		v8::HandleScope scope(iso);

		using Factory = detail::iterator_factory<T, Traits,
			std::decay_t<BeginFn>, std::decay_t<EndFn>>;

		v8::Local<v8::Value> data = detail::external_data::set(iso,
			Factory{ std::decay_t<BeginFn>(std::forward<BeginFn>(begin_fn)),
				std::decay_t<EndFn>(std::forward<EndFn>(end_fn)) });

		v8::Local<v8::FunctionTemplate> iter_tmpl = v8::FunctionTemplate::New(iso,
			&Factory::iterator_callback, data);

		class_info_.js_function_template()->PrototypeTemplate()->Set(
			v8::Symbol::GetIterator(iso), iter_tmpl,
			v8::PropertyAttribute(v8::DontEnum | v8::DontDelete));
		return *this;
	}

	/// Set value as a class static property
	template<typename Value>
	class_& static_(std::string_view const& name, Value const& value, bool readonly = false)
	{
		v8::Isolate* iso = isolate();
		v8::HandleScope scope(iso);
		v8::Local<v8::Context> context = iso->GetCurrentContext();

		class_info_.js_function_template()->GetFunction(context).ToLocalChecked()->DefineOwnProperty(context, v8pp::to_v8_name(iso, name), to_v8(iso, value),
																					 v8::PropertyAttribute(v8::DontDelete | (readonly ? v8::ReadOnly : 0)))
			.FromJust();
		return *this;
	}

	/// v8::Isolate where the class bindings belongs
	v8::Isolate* isolate() { return class_info_.isolate(); }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return class_info_.class_function_template();
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return class_info_.js_function_template();
	}

	/// Create JavaScript object which references externally created C++ class.
	/// It will not take ownership of the C++ pointer.
	static v8::Local<v8::Object> reference_external(v8::Isolate* isolate, object_pointer_type const& ext)
	{
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).wrap_object(ext, 0);
	}

	/// Remove external reference from JavaScript
	static void unreference_external(v8::Isolate* isolate, object_pointer_type const& ext)
	{
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).remove_object(Traits::pointer_id(ext));
	}

	/// As reference_external but delete memory for C++ object
	/// when JavaScript object is deleted. You must use `Traits::create<T>()`
	/// to allocate `ext`
	static v8::Local<v8::Object> import_external(v8::Isolate* isolate, object_pointer_type const& ext)
	{
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).wrap_object(ext, Traits::object_size(ext));
	}

	/// Get wrapped object from V8 value, may return nullptr on fail.
	static object_pointer_type unwrap_object(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		return Traits::template static_pointer_cast<T>(
			detail::classes::find<Traits>(isolate, detail::type_id<T>()).unwrap_object(value));
	}

	/// Create a wrapped C++ object and import it into JavaScript
	template<typename... Args>
	static v8::Local<v8::Object> create_object(v8::Isolate* isolate, Args&&... args)
	{
		return import_external(isolate, object_create(std::forward<Args>(args)...));
	}

	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail.
	static v8::Local<v8::Object> find_object(v8::Isolate* isolate, object_const_pointer_type const& obj)
	{
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).find_v8_object(Traits::const_pointer_cast(obj));
	}

	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail
	/// or wrap a copy of the obj if class_.auto_wrap_objects()
	static v8::Local<v8::Object> find_object(v8::Isolate* isolate, T const& obj)
	{
		auto& class_info = detail::classes::find<Traits>(isolate, detail::type_id<T>());
		v8::Local<v8::Object> wrapped_object = class_info.find_v8_object(Traits::key(const_cast<T*>(&obj)));
		if (wrapped_object.IsEmpty() && class_info.auto_wrap_objects())
		{
			object_pointer_type clone = Traits::clone(obj);
			if (clone)
			{
				wrapped_object = class_info.wrap_object(clone, Traits::object_size(clone));
			}
		}
		return wrapped_object;
	}

	/// Destroy wrapped C++ object
	static void destroy_object(v8::Isolate* isolate, object_pointer_type const& obj)
	{
		detail::classes::find<Traits>(isolate, detail::type_id<T>()).remove_object(Traits::pointer_id(obj));
	}

	/// Destroy all wrapped C++ objects of this class
	static void destroy_objects(v8::Isolate* isolate)
	{
		detail::classes::find<Traits>(isolate, detail::type_id<T>()).remove_objects();
	}

	/// Destroy all wrapped C++ objects and this binding class_
	static void destroy(v8::Isolate* isolate)
	{
		detail::classes::remove<Traits>(isolate, detail::type_id<T>());
	}

private:
	template<typename Function>
	class_& function_impl(std::string_view name, Function&& func,
		v8::SideEffectType side_effect, v8::PropertyAttribute attr)
	{
		constexpr bool is_mem_fun = std::is_member_function_pointer_v<Function>;

		static_assert(is_mem_fun || detail::is_callable<Function>::value,
			"Function must be pointer to member function or callable object");

		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::Local<v8::Data> wrapped_fun;

		if constexpr (is_mem_fun)
		{
			using mem_func_type = typename detail::function_traits<Function>::template pointer_type<T>;
			wrapped_fun = wrap_function_template<mem_func_type, Traits>(isolate(),
				mem_func_type(std::forward<Function>(func)), side_effect);
		}
		else
		{
			wrapped_fun = wrap_function_template<Function, Traits>(isolate(),
				std::forward<Function>(func), side_effect);
		}

		v8::Local<v8::FunctionTemplate> js_func = class_info_.js_function_template();
		js_func->PrototypeTemplate()->Set(v8_name, wrapped_fun, attr);
		if constexpr (!is_mem_fun)
		{
			// non-member functions are also accessible on the constructor (e.g. X.static_fun())
			js_func->Set(v8_name, wrapped_fun, attr);
		}
		return *this;
	}

	template<typename Function, typename... Defs>
	class_& function_impl(std::string_view name, Function&& func,
		v8pp::defaults<Defs...> defs, v8::SideEffectType side_effect, v8::PropertyAttribute attr)
	{
		constexpr bool is_mem_fun = std::is_member_function_pointer_v<Function>;

		static_assert(is_mem_fun || detail::is_callable<Function>::value,
			"Function must be pointer to member function or callable object");

		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::Local<v8::Data> wrapped_fun;

		if constexpr (is_mem_fun)
		{
			using mem_func_type = typename detail::function_traits<Function>::template pointer_type<T>;
			wrapped_fun = wrap_function_template<mem_func_type, Traits>(isolate(),
				mem_func_type(std::forward<Function>(func)), std::move(defs), side_effect);
		}
		else
		{
			wrapped_fun = wrap_function_template<Function, Traits>(isolate(),
				std::forward<Function>(func), std::move(defs), side_effect);
		}

		v8::Local<v8::FunctionTemplate> js_func = class_info_.js_function_template();
		js_func->PrototypeTemplate()->Set(v8_name, wrapped_fun, attr);
		if constexpr (!is_mem_fun)
		{
			js_func->Set(v8_name, wrapped_fun, attr);
		}
		return *this;
	}

	template<typename Attribute>
	static void member_get(v8::Local<v8::Name>,
		v8::PropertyCallbackInfo<v8::Value> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		try
		{
			auto self = unwrap_object(isolate, info.This());
			if (!self)
			{
				info.GetReturnValue().Set(throw_ex(isolate, "accessing member on non-existent C++ object"));
				return;
			}
			Attribute attr = detail::external_data::get<Attribute>(info.Data());
			info.GetReturnValue().Set(to_v8(isolate, (*self).*attr));
		}
		catch (std::exception const& ex)
		{
			info.GetReturnValue().Set(throw_ex(isolate, ex.what()));
		}
	}

	template<typename Attribute>
	static void member_set(v8::Local<v8::Name>, v8::Local<v8::Value> value,
		v8::PropertyCallbackInfo<void> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		try
		{
			auto self = unwrap_object(isolate, info.This());
			if (!self)
			{
				isolate->ThrowException(throw_ex(isolate, "setting member on non-existent C++ object"));
				return;
			}
			Attribute ptr = detail::external_data::get<Attribute>(info.Data());
			using attr_type = typename detail::function_traits<Attribute>::return_type;
			(*self).*ptr = v8pp::from_v8<attr_type>(isolate, value);
		}
		catch (std::exception const& ex)
		{
			if (info.ShouldThrowOnError())
			{
				isolate->ThrowException(throw_ex(isolate, ex.what()));
			}
			// TODO: info.GetReturnValue().Set(false);
		}
	}
};

/// Interface to access C++ classes bound to V8
/// Objects are stored in std::shared_ptr
template<typename T>
using shared_class = class_<T, shared_ptr_traits>;

void cleanup(v8::Isolate* isolate);

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/class.ipp"
#endif
