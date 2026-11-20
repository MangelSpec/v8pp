#pragma once

#include <algorithm>
#include <exception>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "v8pp/config.hpp"
#include "v8pp/function.hpp"
#include "v8pp/managed_wrappers.hpp"
#include "v8pp/native_dispatch.hpp"
#include "v8pp/overload.hpp"
#include "v8pp/property.hpp"
#include "v8pp/ptr_traits.hpp"
#include "v8pp/type_info.hpp"

#if V8_MAJOR_VERSION > 13 || (V8_MAJOR_VERSION == 13 && V8_MINOR_VERSION >= 3)
#include <v8-external-memory-accounter.h>
#endif
#include "v8pp/object.hpp"

#if V8PP_ENABLE_MANAGED_WRAPPERS
#include <cppgc/cross-thread-persistent.h>
#include <cppgc/garbage-collected.h>
#endif

namespace v8pp::detail {

class classes;

struct class_info : std::enable_shared_from_this<class_info>
{
	type_info const type;
	type_info const traits;
	bool retired = false;
	external_data::group callbacks;

	class_info(type_info const& type, type_info const& traits);
	virtual ~class_info() = default;
	virtual void retire() = 0;

	std::string class_name() const;
	static std::string class_name(type_info const& type, type_info const& traits);
};

struct wrapper_registration
{
	using owner_type = std::variant<void*, std::shared_ptr<void>>;
	class_info* registry = nullptr;
	owner_type owner;
};

#if V8PP_ENABLE_MANAGED_WRAPPERS
struct managed_wrapper_registration final : cppgc::GarbageCollected<managed_wrapper_registration>
{
	wrapper_registration registration;

	void Trace(cppgc::Visitor*) const {}
};
#endif

struct weak_wrapper
{
	explicit weak_wrapper(classes& state);
	virtual ~weak_wrapper() = default;
	virtual void collect() = 0;
	void make_weak(v8::Global<v8::Object>& handle);
	void reset_weak_handle();

	classes& state;
	v8::Global<v8::Object>* weak_handle = nullptr;
	weak_wrapper** previous = nullptr;
	weak_wrapper* next = nullptr;
	bool detached = false;
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

	object_registry(classes& state, v8::Isolate* isolate, type_info const& type, dtor_function&& dtor);

	object_registry(object_registry const&) = delete;
	object_registry(object_registry&&) = delete;

	object_registry& operator=(object_registry const&) = delete;
	object_registry& operator=(object_registry&&) = delete;

	~object_registry() override;
	void retire() override;

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

	void set_ctor(ctor_function&& ctor) { ctor_ = std::make_shared<ctor_function>(std::move(ctor)); }

	void add_base(object_registry& info, cast_function cast);
	bool cast(pointer_type& ptr, type_info const& actual_type) const;

	void remove_object(object_id const& obj);
	void remove_objects();

	v8::Local<v8::Object> find_v8_object(pointer_type const& ptr) const;
	v8::Local<v8::Object> find_v8_object_by_id(object_id const& id) const;

	v8::Local<v8::Object> wrap_this(v8::Local<v8::Object> obj, pointer_type const& object, size_t size);
	v8::Local<v8::Object> wrap_object(pointer_type const& object, size_t size);
	v8::Local<v8::Object> wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args);

	template<typename Create>
	v8::Local<v8::Object> create_wrapped_object(Create&& create)
	{
		native_dispatch_scope dispatch(isolate_);
		auto self = shared_from_this();
		factory_call factory(*this);
		auto object = std::invoke(std::forward<Create>(create));
		if (!object) return {};
		auto const size = Traits::object_size(object);
		try
		{
			auto wrapped = wrap_object(object, size);
			if (!wrapped.IsEmpty()) return wrapped;
		}
		catch (...)
		{
			rollback_factory_result(object, size);
			throw;
		}
		rollback_factory_result(object, size);
		return {};
	}

	pointer_type unwrap_object(v8::Local<v8::Value> value, v8::Local<v8::Object>* registered_wrapper = nullptr);

	std::unordered_map<std::string, std::function<v8::Local<v8::Value>(v8::Isolate*, pointer_type)>> const_properties;

private:
	struct wrapped_object final : weak_wrapper
	{
		wrapped_object(object_registry& registry, object_id id, size_t size);
		void collect() override;

		object_registry& registry;
		object_id id;
		size_t size; // 0 for referenced objects
		wrapper_registration* registration = nullptr;
		std::unique_ptr<wrapper_registration> portable_registration;
		v8::Global<v8::Object> portable_cell;
#if V8PP_ENABLE_MANAGED_WRAPPERS
		// Isolates can transfer their Locker to another thread between
		// publication and collection. Keep the cell alive for the second pass.
		cppgc::subtle::CrossThreadPersistent<managed_wrapper_registration> managed_registration;
#endif
	};

	void detach_object(object_id id);
	void reset_object(std::unique_ptr<wrapped_object> wrapped);
	bool apply_const_properties(v8::Local<v8::Object> obj, pointer_type const& ext);
	bool owns_new_factory_result(object_id id) const;
	void rollback_factory_result(pointer_type const& object, size_t size);

	struct factory_call
	{
		explicit factory_call(object_registry& registry);
		~factory_call();

		object_registry& registry;
		factory_call* const previous;
		std::vector<object_id> retired_objects;
	};

	struct base_class_info
	{
		object_registry* info;
		cast_function cast;

		base_class_info(object_registry& info, cast_function cast)
			: info(&info)
			, cast(cast)
		{
		}
	};

	struct object_entry
	{
		explicit object_entry(std::unique_ptr<wrapped_object> value)
			: receipt(std::move(value))
		{
		}

		object_entry(object_entry&&) = delete;
		object_entry& operator=(object_entry&&) = delete;

		v8::Global<v8::Object> wrapper;
		std::unique_ptr<wrapped_object> receipt;
	};

	std::vector<base_class_info> bases_;
	std::vector<object_registry*> derivatives_;
	std::unordered_map<object_id, object_entry> objects_;

	classes& state_;
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

	std::shared_ptr<ctor_function> ctor_;
	std::shared_ptr<dtor_function> dtor_;
	factory_call* factory_call_ = nullptr;
	bool auto_wrap_objects_ = false;
	bool const managed_wrappers_;
};

class classes final : public registration_lifetime
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
	friend void dispose_native_registrations(v8::Isolate*, std::exception_ptr*);
	friend struct weak_wrapper;
	template<typename> friend class object_registry;

	using classes_info = std::unordered_map<uintptr_t, std::shared_ptr<class_info>>;
	classes_info classes_;
	v8::Global<v8::Private> portable_key_;
	v8::Global<v8::ObjectTemplate> portable_template_;
	weak_wrapper* pending_weak_ = nullptr;

	enum class operation
	{
		get,
		add
	};
	static classes* instance(operation op, v8::Isolate* isolate);
	explicit classes(v8::Isolate* isolate) : isolate_(isolate) {}
	v8::Local<v8::Private> portable_key();
	v8::Local<v8::Object> create_portable_cell(v8::Local<v8::Context> context);
	bool idle() const noexcept override { return classes_.empty() && !pending_weak_; }
	void release_if_idle();
	v8::Isolate* isolate_;
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
	};

	static void iterator_callback(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();
		v8::HandleScope scope(isolate);
		v8::TryCatch try_catch(isolate);

		try
		{
			native_dispatch_scope dispatch(isolate);
			external_data::pin callback(args.Data());
			auto& registry = classes::find<Traits>(isolate, type_id<T>());
			auto registry_pin = registry.shared_from_this();
			v8::Local<v8::Object> container;
			auto self = Traits::template static_pointer_cast<T>(registry.unwrap_object(args.This(), &container));
			if (!self)
			{
				throw std::runtime_error("calling [Symbol.iterator] on null instance");
			}

			auto& factory = callback.get<iterator_factory>();

			state iter_state{
				std::invoke(factory.begin_fn, std::as_const(*self)),
				std::invoke(factory.end_fn, std::as_const(*self)) };
			if (!callback.live()) throw std::runtime_error("native iterator binding has been retired");

			v8::Local<v8::Context> context = isolate->GetCurrentContext();
			v8::Local<v8::Object> iter_obj = v8::Object::New(isolate);

			v8::Local<v8::Value> state_ext = external_data::set(isolate, std::move(iter_state), &registry.callbacks);
			v8::Local<v8::Function> next_fn;
			if (!v8::Function::New(context, &next_callback, state_ext).ToLocal(&next_fn))
			{
				throw std::runtime_error("failed to create iterator next()");
			}

			// Retain the exact registration across later prototype changes on a delegate.
			if (!state_ext.As<v8::Object>()->SetPrivate(context,
							v8::Private::ForApi(isolate, v8pp::to_v8_name(isolate, "v8pp.iterator.container")), container)
					.FromMaybe(false) ||
				!iter_obj->CreateDataProperty(context, v8pp::to_v8_name(isolate, "next"), next_fn).FromMaybe(false))
			{
				throw std::runtime_error("failed to initialize iterator");
			}

			args.GetReturnValue().Set(iter_obj);
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

	static void next_callback(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();
		v8::HandleScope scope(isolate);
		v8::TryCatch try_catch(isolate);

		try
		{
			native_dispatch_scope dispatch(isolate);
			external_data::pin callback(args.Data());
			v8::Local<v8::Context> context = isolate->GetCurrentContext();
			v8::Local<v8::Value> container;
			if (!args.Data().As<v8::Object>()->GetPrivate(context,
					v8::Private::ForApi(isolate, v8pp::to_v8_name(isolate, "v8pp.iterator.container"))).ToLocal(&container))
				throw std::runtime_error("could not read native iterator container");
			auto self = class_<T, Traits>::unwrap_object(isolate, container);
			if (!self) throw std::runtime_error("native iterator container has been retired");
			auto* iter_state = &callback.get<state>();
			v8::Local<v8::Object> result = v8::Object::New(isolate);

			if (iter_state->current == iter_state->end)
			{
				if (!result->CreateDataProperty(context,
							   v8pp::to_v8_name(isolate, "value"),
							   v8::Undefined(isolate))
						.FromMaybe(false) ||
					!result->CreateDataProperty(context,
							   v8pp::to_v8_name(isolate, "done"),
							   v8::Boolean::New(isolate, true))
						.FromMaybe(false))
				{
					throw std::runtime_error("failed to create iterator result");
				}
			}
			else
			{
				external_data::result_scope conversion(callback);
				if (!result->CreateDataProperty(context,
							   v8pp::to_v8_name(isolate, "value"),
							   v8pp::to_v8(isolate, *iter_state->current))
						.FromMaybe(false) ||
					!result->CreateDataProperty(context,
							   v8pp::to_v8_name(isolate, "done"),
							   v8::Boolean::New(isolate, false))
						.FromMaybe(false))
				{
					throw std::runtime_error("failed to create iterator result");
				}
				++iter_state->current;
			}

			args.GetReturnValue().Set(result);
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
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
		v8::TryCatch try_catch(isolate);

		try
		{
			native_dispatch_scope dispatch(isolate);
			external_data::pin callback_data(args.Data());
			auto self = class_<T, Traits>::unwrap_object(isolate, args.This());
			if (!self)
			{
				throw std::runtime_error("calling [Symbol.toPrimitive] on null instance");
			}

			auto& invoker = callback_data.get<to_primitive_invoker>();

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
				decltype(auto) result = std::invoke(invoker.func, std::as_const(*self), std::string_view(hint));
				if (!try_catch.HasCaught())
				{
					external_data::result_scope conversion(callback_data);
					args.GetReturnValue().Set(to_v8(isolate, result));
				}
			}
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
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
			[func = F(std::forward<Function>(func)), defs = std::move(defs)](v8::FunctionCallbackInfo<v8::Value> const& args) mutable
			{
				auto object = detail::call_from_v8<Traits>(std::move(func), args, defs);
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
				 detail::make_overload_entry(std::forward<Fs>(fs))...) }](v8::FunctionCallbackInfo<v8::Value> const& args) mutable
			{
				v8::Isolate* isolate = args.GetIsolate();
				v8::TryCatch try_catch(isolate);
				try
				{
					size_t const arg_count = args.Length();
					bool matched = false;
					bool js_exception = false;
					object_pointer_type result{};
					std::string errors;

					std::apply([&](auto&... entries)
						{ ((matched || js_exception || [&]
							   {
								   using Entry = std::decay_t<decltype(entries)>;
								   using F = typename detail::entry_func_type<Entry>::type;

								   constexpr size_t min = detail::overload_arg_range<Entry>::min_args;
								   constexpr size_t max = detail::overload_arg_range<Entry>::max_args;
								   if (arg_count < min || arg_count > max)
									   return false;

								   if (arg_count > 0 && !detail::overload_types_match<F, Traits, detail::overload_arg_range<Entry>::num_defaults>(isolate, args, arg_count))
									   return false;

								   try
								   {
									   result = object_pointer_type(detail::call_ctor_entry<Traits>(entries, args));
									   matched = true;
									   return true;
								   }
								   catch (detail::argument_conversion_error const& ex)
								   {
									   if (try_catch.HasCaught())
									   {
										   js_exception = true;
										   return true;
									   }
									   if (!errors.empty()) errors += "; ";
									   errors += ex.what();
									   return false;
								   }
							   }()),
							  ...); }, set.entries);

					if (js_exception)
					{
						try_catch.ReThrow();
						return std::make_pair(pointer_type{}, size_t{});
					}
					if (!matched)
					{
						std::string msg = "No matching constructor overload for " + std::to_string(arg_count) + " argument(s)";
						if (!errors.empty())
						{
							msg += ". Tried: " + errors;
						}
						throw std::runtime_error(msg);
					}

					if (try_catch.HasCaught())
					{
						try_catch.ReThrow();
					}
					return std::make_pair(pointer_type(result), Traits::object_size(result));
				}
				catch (...)
				{
					if (try_catch.HasCaught())
					{
						try_catch.ReThrow();
						return std::make_pair(pointer_type{}, size_t{});
					}
					throw;
				}
			});
		return *this;
	}

	/// Inhert from C++ class U
	template<typename U>
	class_& inherit()
	{
		static_assert(std::derived_from<T, U>, "Class U should be base for class T");
		v8::HandleScope scope(isolate());
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
			fast_function<FuncPtr>{}, side_effect, &class_info_.callbacks);

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
		v8::Local<v8::Data> wrapped_fun = detail::wrap_class_overload_template<Traits>(isolate(), &class_info_.callbacks,
			std::forward<F1>(f1), std::forward<F2>(f2), std::forward<Fs>(fs)...);

		v8::Local<v8::FunctionTemplate> js_func = class_info_.js_function_template();
		js_func->PrototypeTemplate()->Set(v8_name, wrapped_fun);
		// Also on constructor for static access
		js_func->Set(v8_name, wrapped_fun);
		return *this;
	}

	/// Set class member variable
	template<typename Attribute>
	class_& var(std::string_view name, Attribute attribute,
		v8::SideEffectType getter_effect = v8::SideEffectType::kHasSideEffect)
	{
		static_assert(std::is_member_object_pointer_v<Attribute>, "Attribute must be pointer to member data");

		v8::HandleScope scope(isolate());

		using attribute_type = typename detail::function_traits<Attribute>::template pointer_type<T>;
		attribute_type attr = attribute;

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		v8::Local<v8::Value> data = detail::external_data::set(isolate(), std::forward<attribute_type>(attr), &class_info_.callbacks);
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
		// V8 12.9+ removed ObjectTemplate::SetAccessor (crbug.com/336325111).
		// Use SetAccessorProperty on PrototypeTemplate with FunctionTemplate-based
		// getter/setter. This creates a proper JS accessor property on the prototype,
		// preserving correct prototype chain semantics (properties can be overridden
		// via Object.defineProperty on derived prototypes).
		// See: https://www.mail-archive.com/v8-dev@googlegroups.com/msg162125.html
		auto getter_template = v8::FunctionTemplate::New(isolate(),
			&member_get_fn<attribute_type>, data,
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kThrow,
			getter_effect);
		auto setter_template = v8::FunctionTemplate::New(isolate(),
			&member_set_fn<attribute_type>, data,
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kThrow,
			v8::SideEffectType::kHasSideEffect);
		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template, setter_template,
			v8::PropertyAttribute(v8::DontDelete));
#else
		v8::AccessorNameGetterCallback getter = &member_get<attribute_type>;
		v8::AccessorNameSetterCallback setter = &member_set<attribute_type>;
		class_info_.js_function_template()
			->PrototypeTemplate()
			->SetAccessor(v8_name, getter, setter, data,
#if V8_MAJOR_VERSION < 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION < 1)
				v8::DEFAULT,
#endif
				v8::PropertyAttribute(v8::DontDelete),
				getter_effect,
				v8::SideEffectType::kHasSideEffect);
#endif
		return *this;
	}

	/// Set read/write class property with getter and setter
	template<typename GetFunction, typename SetFunction = detail::none>
	requires(!is_fast_function<std::decay_t<GetFunction>>::value &&
		!std::same_as<std::decay_t<SetFunction>, v8::SideEffectType>)
	class_& property(std::string_view name, GetFunction&& get, SetFunction&& set = {},
		v8::SideEffectType getter_effect = v8::SideEffectType::kHasSideEffect)
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

		v8::Local<v8::String> v8_name = v8pp::to_v8_name(isolate(), name);
#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
		// V8 12.9+ removed ObjectTemplate::SetAccessor (crbug.com/336325111).
		// Use SetAccessorProperty on PrototypeTemplate with FunctionTemplate-based
		// getter/setter — the same approach as fast_fn properties.
		// See: https://www.mail-archive.com/v8-dev@googlegroups.com/msg162125.html

		// Direct PropertyCallbackInfo getters/setters cannot work with SetAccessorProperty
		// because it provides FunctionCallbackInfo, not PropertyCallbackInfo.
		{
			constexpr size_t get_offset = std::same_as<GetClass, detail::none> ? 0 : (std::is_member_function_pointer_v<Getter> ? 0 : 1);
			static_assert(!detail::is_direct_getter<Getter, get_offset>,
				"Direct PropertyCallbackInfo getters are not supported on V8 12.9+ "
				"(ObjectTemplate::SetAccessor was removed). Use a regular getter instead.");
		}

		v8::Local<v8::Value> data = detail::external_data::set(isolate(), property_type(std::move(get), std::move(set)), &class_info_.callbacks);

		auto getter_template = v8::FunctionTemplate::New(isolate(),
			&property_get_fn<property_type, GetClass>, data,
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kThrow,
			getter_effect);

		v8::Local<v8::FunctionTemplate> setter_template;
		if constexpr (!property_type::is_readonly)
		{
			constexpr size_t set_offset = std::same_as<SetClass, detail::none> ? 0 : (std::is_member_function_pointer_v<Setter> ? 0 : 1);
			static_assert(!detail::is_direct_setter<Setter, set_offset>,
				"Direct PropertyCallbackInfo setters are not supported on V8 12.9+ "
				"(ObjectTemplate::SetAccessor was removed). Use a regular setter instead.");

			setter_template = v8::FunctionTemplate::New(isolate(),
				&property_set_fn<property_type, Setter, SetClass>, data,
				v8::Local<v8::Signature>(), 0,
				v8::ConstructorBehavior::kThrow,
				v8::SideEffectType::kHasSideEffect);
		}

		v8::PropertyAttribute attrs = property_type::is_readonly ? v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete) : v8::PropertyAttribute(v8::DontDelete);
		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template, setter_template, attrs);
#else
		v8::AccessorNameGetterCallback getter = property_type::template get<Traits>;
		v8::AccessorNameSetterCallback setter = property_type::is_readonly ? nullptr : property_type::template set<Traits>;
		v8::Local<v8::Value> data = detail::external_data::set(isolate(), property_type(std::move(get), std::move(set)), &class_info_.callbacks);
		class_info_.js_function_template()->PrototypeTemplate()->SetAccessor(v8_name, getter, setter, data,
#if V8_MAJOR_VERSION < 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION < 1)
			v8::DEFAULT,
#endif
			v8::PropertyAttribute(v8::DontDelete),
			getter_effect, v8::SideEffectType::kHasSideEffect);
#endif
		return *this;
	}

	template<typename GetFunction>
	requires(!is_fast_function<std::decay_t<GetFunction>>::value)
	class_& property(std::string_view name, GetFunction&& get, v8::SideEffectType getter_effect)
	{
		return property(name, std::forward<GetFunction>(get), detail::none{}, getter_effect);
	}

	/// Set read-only class property with V8 Fast API getter
	template<auto GetPtr>
	class_& property(std::string_view name, fast_function<GetPtr>,
		v8::SideEffectType getter_effect = v8::SideEffectType::kHasSideEffect)
	{
		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto getter_template = wrap_function_template<GetPtr, Traits>(
			isolate(), fast_function<GetPtr>{}, getter_effect, &class_info_.callbacks);

		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template,
			v8::Local<v8::FunctionTemplate>(),
			v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete));

		return *this;
	}

	/// Set read/write class property with V8 Fast API getter and slow setter
	template<auto GetPtr, typename SetFunction>
	requires(!is_fast_function<std::decay_t<SetFunction>>::value &&
		!std::same_as<std::decay_t<SetFunction>, v8::SideEffectType>)
	class_& property(std::string_view name, fast_function<GetPtr>, SetFunction&& set,
		v8::SideEffectType getter_effect = v8::SideEffectType::kHasSideEffect)
	{
		using Setter = std::decay_t<SetFunction>;
		static_assert(std::is_member_function_pointer_v<SetFunction> || detail::is_callable<Setter>::value, "SetFunction must be callable");

		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto getter_template = wrap_function_template<GetPtr, Traits>(
			isolate(), fast_function<GetPtr>{}, getter_effect, &class_info_.callbacks);
		auto setter_template = v8::FunctionTemplate::New(isolate(),
			&detail::forward_function<Traits, Setter>,
			detail::external_data::set(isolate(), Setter(std::forward<SetFunction>(set)), &class_info_.callbacks),
			v8::Local<v8::Signature>(), 0,
			v8::ConstructorBehavior::kThrow,
			v8::SideEffectType::kHasSideEffect);

		class_info_.js_function_template()->PrototypeTemplate()->SetAccessorProperty(
			v8_name, getter_template, setter_template,
			v8::PropertyAttribute(v8::DontDelete));

		return *this;
	}

	/// Set read/write class property with V8 Fast API getter and setter
	template<auto GetPtr, auto SetPtr>
	class_& property(std::string_view name, fast_function<GetPtr>, fast_function<SetPtr>,
		v8::SideEffectType getter_effect = v8::SideEffectType::kHasSideEffect)
	{
		v8::HandleScope scope(isolate());

		v8::Local<v8::Name> v8_name = v8pp::to_v8_name(isolate(), name);
		auto getter_template = wrap_function_template<GetPtr, Traits>(
			isolate(), fast_function<GetPtr>{}, getter_effect, &class_info_.callbacks);
		auto setter_template = wrap_function_template<SetPtr, Traits>(
			isolate(), fast_function<SetPtr>{}, v8::SideEffectType::kHasSideEffect, &class_info_.callbacks);

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
				v8::SideEffectType::kHasNoSideEffect, &class_info_.callbacks);
		}
		else
		{
			using Invoker = detail::to_primitive_invoker<T, Traits, std::decay_t<Function>>;
			v8::Local<v8::Value> data = detail::external_data::set(iso,
				Invoker{ std::decay_t<Function>(std::forward<Function>(func)) }, &class_info_.callbacks);
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
				std::decay_t<EndFn>(std::forward<EndFn>(end_fn)) }, &class_info_.callbacks);

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

	/// Template for native wrapper construction. Requires an active HandleScope.
	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return class_info_.class_function_template();
	}

	/// Template for the JavaScript constructor. Requires an active HandleScope.
	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return class_info_.js_function_template();
	}

	/// Create JavaScript object which references externally created C++ class.
	/// It will not take ownership of the C++ pointer.
	static v8::Local<v8::Object> reference_external(v8::Isolate* isolate, object_pointer_type const& ext)
	{
		detail::external_data::result_scope::check(isolate);
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
		detail::external_data::result_scope::check(isolate);
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).wrap_object(ext, Traits::object_size(ext));
	}

	/// Get wrapped object from V8 value, may return nullptr on fail.
	static object_pointer_type unwrap_object(v8::Isolate* isolate, v8::Local<v8::Value> value)
	{
		detail::count_dispatch(&detail::dispatch_counts::expected_class_lookups);
		return Traits::template static_pointer_cast<T>(
			detail::classes::find<Traits>(isolate, detail::type_id<T>()).unwrap_object(value));
	}

	/// Create a wrapped C++ object and import it into JavaScript
	template<typename... Args>
	static v8::Local<v8::Object> create_object(v8::Isolate* isolate, Args&&... args)
	{
		detail::external_data::result_scope::check(isolate);
		auto& registry = detail::classes::find<Traits>(isolate, detail::type_id<T>());
		return registry.create_wrapped_object([&] { return object_create(std::forward<Args>(args)...); });
	}

	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail.
	static v8::Local<v8::Object> find_object(v8::Isolate* isolate, object_const_pointer_type const& obj)
	{
		if constexpr (std::same_as<Traits, shared_ptr_traits>)
		{
			return find_object_by_id(isolate, const_cast<T*>(obj.get()));
		}
		else
		{
			detail::external_data::result_scope::check(isolate);
			return detail::classes::find<Traits>(isolate, detail::type_id<T>()).find_v8_object(Traits::const_pointer_cast(obj));
		}
	}

	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail
	/// or wrap a copy of the obj if class_.auto_wrap_objects()
	static v8::Local<v8::Object> find_object(v8::Isolate* isolate, T const& obj)
	{
		detail::external_data::result_scope::check(isolate);
		auto& class_info = detail::classes::find<Traits>(isolate, detail::type_id<T>());
		v8::Local<v8::Object> wrapped_object = class_info.find_v8_object(Traits::key(const_cast<T*>(&obj)));
		if (wrapped_object.IsEmpty() && class_info.auto_wrap_objects())
		{
			wrapped_object = class_info.create_wrapped_object([&] { return Traits::clone(obj); });
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
	template<typename, typename> friend struct convert;

	static v8::Local<v8::Object> find_object_by_id(v8::Isolate* isolate, object_id const& id)
	{
		detail::external_data::result_scope::check(isolate);
		return detail::classes::find<Traits>(isolate, detail::type_id<T>()).find_v8_object_by_id(id);
	}

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
				mem_func_type(std::forward<Function>(func)), side_effect, &class_info_.callbacks);
		}
		else
		{
			wrapped_fun = wrap_function_template<Function, Traits>(isolate(),
				std::forward<Function>(func), side_effect, &class_info_.callbacks);
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
				mem_func_type(std::forward<Function>(func)), std::move(defs), side_effect, &class_info_.callbacks);
		}
		else
		{
			wrapped_fun = wrap_function_template<Function, Traits>(isolate(),
				std::forward<Function>(func), std::move(defs), side_effect, &class_info_.callbacks);
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

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(info.Data());
			auto self = unwrap_object(isolate, detail::property_receiver(info));
			if (!self)
			{
				throw std::runtime_error("accessing member on non-existent C++ object");
			}
			Attribute attr = callback.get<Attribute>();
			detail::external_data::result_scope conversion(callback);
			info.GetReturnValue().Set(to_v8(isolate, (*self).*attr));
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

	template<typename Attribute>
	static void member_set(v8::Local<v8::Name>, v8::Local<v8::Value> value,
		v8::PropertyCallbackInfo<void> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(info.Data());
			auto self = unwrap_object(isolate, detail::property_receiver(info));
			if (!self)
			{
				throw std::runtime_error("setting member on non-existent C++ object");
			}
			Attribute ptr = callback.get<Attribute>();
			using attr_type = typename detail::function_traits<Attribute>::return_type;
			(*self).*ptr = v8pp::from_v8<attr_type>(isolate, value);
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
			// TODO: info.GetReturnValue().Set(false);
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

#if V8_MAJOR_VERSION > 12 || (V8_MAJOR_VERSION == 12 && V8_MINOR_VERSION >= 9)
	/// FunctionCallback-signature adapter for member variable getter.
	/// Used with SetAccessorProperty (V8 12.9+) which requires FunctionTemplate.
	template<typename Attribute>
	static void member_get_fn(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(args.Data());
			auto self = unwrap_object(isolate, args.This());
			if (!self)
			{
				throw std::runtime_error("accessing member on non-existent C++ object");
			}
			Attribute attr = callback.get<Attribute>();
			detail::external_data::result_scope conversion(callback);
			args.GetReturnValue().Set(to_v8(isolate, (*self).*attr));
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

	/// FunctionCallback-signature adapter for member variable setter.
	/// Used with SetAccessorProperty (V8 12.9+) which requires FunctionTemplate.
	template<typename Attribute>
	static void member_set_fn(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(args.Data());
			auto self = unwrap_object(isolate, args.This());
			if (!self)
			{
				throw std::runtime_error("setting member on non-existent C++ object");
			}
			Attribute ptr = callback.get<Attribute>();
			using attr_type = typename detail::function_traits<Attribute>::return_type;
			(*self).*ptr = v8pp::from_v8<attr_type>(isolate, args[0]);
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

	/// FunctionCallback adapter for property getter (V8 12.9+).
	/// Handles all getter types: member functions, free functions with object,
	/// isolate-aware getters, and direct V8 callback getters.
	template<typename Property, typename GetClass>
	static void property_get_fn(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(args.Data());
			auto&& prop = callback.get<Property>();
			using Get = decltype(prop.getter);
			auto set_result = [&](auto&& result)
			{
				if (!try_catch.HasCaught())
				{
					detail::external_data::result_scope conversion(callback);
					args.GetReturnValue().Set(to_v8(isolate, result));
				}
			};

			if constexpr (std::same_as<GetClass, detail::none>)
			{
				constexpr size_t offset = 0;
				if constexpr (detail::is_isolate_getter<Get, offset>)
				{
					set_result(std::invoke(prop.getter, isolate));
				}
				else if constexpr (detail::is_getter<Get, offset>)
				{
					set_result(std::invoke(prop.getter));
				}
			}
			else
			{
				auto obj = class_<GetClass, Traits>::unwrap_object(isolate, args.This());
				if (!obj)
				{
					throw std::runtime_error("accessing property on non-existent C++ object");
				}
				constexpr size_t offset = std::is_member_function_pointer_v<Get> ? 0 : 1;
				if constexpr (detail::is_isolate_getter<Get, offset>)
				{
					set_result(std::invoke(prop.getter, *obj, isolate));
				}
				else if constexpr (detail::is_getter<Get, offset>)
				{
					set_result(std::invoke(prop.getter, *obj));
				}
			}
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}

	/// FunctionCallback adapter for property setter (V8 12.9+).
	/// Handles all setter types: member functions, free functions with object,
	/// and isolate-aware setters.
	template<typename Property, typename Set, typename SetClass>
	static void property_set_fn(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		v8::Isolate* isolate = args.GetIsolate();

		v8::TryCatch try_catch(isolate);
		try
		{
			detail::native_dispatch_scope dispatch(isolate);
			detail::external_data::pin callback(args.Data());
			auto&& prop = callback.get<Property>();
			v8::Local<v8::Value> value = args[0];

			if constexpr (std::same_as<SetClass, detail::none>)
			{
				constexpr size_t offset = 0;
				if constexpr (detail::is_isolate_setter<Set, offset>)
				{
					using value_type = typename detail::call_from_v8_traits<Set>::template arg_type<1 + offset>;
					std::invoke(prop.setter, isolate, v8pp::from_v8<value_type>(isolate, value));
				}
				else if constexpr (detail::is_setter<Set, offset>)
				{
					using value_type = typename detail::call_from_v8_traits<Set>::template arg_type<0 + offset>;
					std::invoke(prop.setter, v8pp::from_v8<value_type>(isolate, value));
				}
			}
			else
			{
				auto obj = class_<SetClass, Traits>::unwrap_object(isolate, args.This());
				if (!obj)
				{
					throw std::runtime_error("setting property on non-existent C++ object");
				}
				constexpr size_t offset = std::is_member_function_pointer_v<Set> ? 0 : 1;
				if constexpr (detail::is_isolate_setter<Set, offset>)
				{
					using value_type = typename detail::call_from_v8_traits<Set>::template arg_type<1 + offset>;
					std::invoke(prop.setter, *obj, isolate, v8pp::from_v8<value_type>(isolate, value));
				}
				else if constexpr (detail::is_setter<Set, offset>)
				{
					using value_type = typename detail::call_from_v8_traits<Set>::template arg_type<0 + offset>;
					std::invoke(prop.setter, *obj, v8pp::from_v8<value_type>(isolate, value));
				}
			}
		}
		catch (std::exception const& ex)
		{
			if (!try_catch.HasCaught()) throw_ex(isolate, ex.what());
		}
		if (try_catch.HasCaught()) try_catch.ReThrow();
	}
#endif
};

/// Interface to access C++ classes bound to V8
/// Objects are stored in std::shared_ptr
template<typename T>
using shared_class = class_<T, shared_ptr_traits>;

void cleanup(v8::Isolate* isolate);

/// Release bindings in an entered context before disposing the isolate.
/// Keep the returned plugin images through disposal of the isolate and its C++ heap.
std::vector<std::shared_ptr<void>> dispose_bindings(v8::Isolate* isolate,
	std::exception_ptr* deferred_error = nullptr);

} // namespace v8pp

#if V8PP_HEADER_ONLY
#include "v8pp/class.ipp"
#endif
