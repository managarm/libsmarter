#ifndef LIBSMARTER_SMARTER_HPP
#define LIBSMARTER_SMARTER_HPP

#include <atomic>
#include <new>
#include <utility>
#include <cstddef>

#include <frg/container_of.hpp>
#include <frg/manual_box.hpp>

// Allow the user to override __STDC_HOSTED__. This is useful when building mlibc.
#if __STDC_HOSTED__ && !defined(LIBSMARTER_FORCE_FREESTANDING)
	#define LIBSMARTER_HOSTED 1
#else
	#define LIBSMARTER_HOSTED 0
#endif

#if LIBSMARTER_HOSTED
	#include <cassert>
	#include <iostream>
#else
	#ifndef assert
		#define assert(c) do { (void)(c); } while(0)
	#endif
#endif

namespace smarter {

struct adopt_rc_t { };
//inline constexpr adopt_rc_t adopt_rc;
static constexpr adopt_rc_t adopt_rc;

struct counter {
public:
	counter()
	: _count{0} { }

	counter(adopt_rc_t, unsigned int initial_count)
	: _count{initial_count} { }

	counter(const counter &) = delete;

	counter &operator= (const counter &) = delete;

	~counter() {
		assert(!_count.load(std::memory_order_relaxed));
	}
	void setup(adopt_rc_t, unsigned int initial_count) {
		assert(!_count.load(std::memory_order_relaxed));
		_count.store(initial_count, std::memory_order_relaxed);
	}

	unsigned int check_count() {
		return _count.load(std::memory_order_relaxed);
	}

	void increment() {
		auto count = _count.fetch_add(1, std::memory_order_acq_rel);
		assert(count);
	}

	[[nodiscard]] bool increment_if_nonzero() {
		auto count = _count.load(std::memory_order_relaxed);
		do {
			if(!count)
				return false;
		} while(!_count.compare_exchange_strong(count, count + 1,
					std::memory_order_acq_rel));
		return true;
	}

	[[nodiscard]] bool decrement_and_check_if_zero() {
		auto count = _count.fetch_sub(1, std::memory_order_acq_rel);
		assert(count >= 1);
		return count == 1;
	}

private:
	std::atomic<unsigned int> _count;
};

struct default_deallocator {
	template<typename X>
	void operator() (X *p) {
		delete p;
	}
};

template<typename A>
struct allocator_deallocator {
	allocator_deallocator(A allocator)
	: allocator_{std::move(allocator)} { }

	template<typename X>
	void operator() (X *p) {
		A allocator = std::move(allocator_);
		p->~X();
		allocator.deallocate(p, sizeof(X));
	}

private:
	A allocator_;
};

struct meta_object_base {
	meta_object_base(void (*finalize)(meta_object_base *),
			void (*finalize_weak)(meta_object_base *))
	: _finalize{finalize}, _finalize_weak{finalize_weak} { }

	counter &ctr() { return _ctr; }
	counter &weak_ctr() { return _weak_ctr; }
	void finalize() { _finalize(this); }
	void finalize_weak() { _finalize_weak(this); }

	// Recover the meta_object_base from a pointer to its counter as returned by ctr().
	static meta_object_base *from_ctr(counter *ctr) {
		return frg::container_of(ctr, &meta_object_base::_ctr);
	}

	// Recover the meta_object_base from a pointer to its counter as returned by weak_ctr().
	static meta_object_base *from_weak_ctr(counter *ctr) {
		return frg::container_of(ctr, &meta_object_base::_weak_ctr);
	}

private:
	counter _ctr;
	counter _weak_ctr;
	void (*_finalize)(meta_object_base *);
	void (*_finalize_weak)(meta_object_base *);
};

template<typename T, typename Deallocator>
struct meta_object final
: meta_object_base {
	template<typename... Args>
	meta_object(unsigned int initial_count, Deallocator d, Args &&... args)
	: meta_object_base{&finalize_, &finalize_weak_}, _d{std::move(d)} {
		ctr().setup(adopt_rc, initial_count);
		weak_ctr().setup(adopt_rc, 1);
		_bx.initialize(std::forward<Args>(args)...);
	}

	~meta_object() = default;

	T *get() {
		return _bx.get();
	}

private:
	static void finalize_(meta_object_base *base) {
		auto self = static_cast<meta_object *>(base);
		self->_bx.destruct();
		if(base->weak_ctr().decrement_and_check_if_zero())
			base->finalize_weak();
	}

	static void finalize_weak_(meta_object_base *base) {
		auto self = static_cast<meta_object *>(base);
		self->_d(self);
	}

	frg::manual_box<T> _bx;
	Deallocator _d;
};

template<typename P>
concept rc_policy = requires(const P &policy) {
	// Check whether the policy corresponds to a non-null object.
	{ static_cast<bool>(policy) };
	// Increment the refcount.
	// Precondition: the refcount is already non-zero.
	{ policy.increment() } -> std::same_as<void>;
	// Decrements the refcount.
	// Destructs (or otherwise performs finalization) the object if the refcount drops to zero.
	{ policy.decrement() } -> std::same_as<void>;
};

// Superset of rc_policy that also supports weak pointers.
template<typename P>
concept weak_rc_policy = rc_policy<P> && requires(const P &policy) {
	// Try to increment the refcount (not the weak refcount!).
	// Returns true if the refcount was already non-zero.
	{ policy.try_increment() } -> std::same_as<bool>;
	// Increment the weak refcount.
	// Precondition: the weak refcount is already non-zero.
	{ policy.increment_weak() } -> std::same_as<void>;
	// Decrements the weak refcount.
	// Deallocates (or otherwise performs weak finalization) the object if the weak refcount drops to zero.
	{ policy.decrement_weak() } -> std::same_as<void>;
};

template<typename C>
struct rc_policy_tag { };

template<typename P, typename C>
concept rc_policy_downcastable = rc_policy<P> && rc_policy<C> && requires(const P &policy) {
	// Converts a policy to another policy.
	// Does not change the refcount associated with either policy.
	// Invariant: if the refcount of P is non-zero, the refcount of downcast_policy() must also be non-zero
	//            (such that increment() can be called on the result of downcast_policy()).
	{ policy.downcast_policy(rc_policy_tag<C>{}) } -> std::same_as<C>;
};

struct default_rc_policy {
	default_rc_policy() = default;

	explicit default_rc_policy(meta_object_base *base)
	: _base{base} { }

	explicit operator bool() const {
		return _base != nullptr;
	}

	void increment() const {
		_base->ctr().increment();
	}

	void decrement() const {
		if(_base->ctr().decrement_and_check_if_zero())
			_base->finalize();
	}

	bool try_increment() const {
		return _base->ctr().increment_if_nonzero();
	}

	void increment_weak() const {
		_base->weak_ctr().increment();
	}

	void decrement_weak() const {
		if(_base->weak_ctr().decrement_and_check_if_zero())
			_base->finalize_weak();
	}

	meta_object_base *base() const {
		return _base;
	}

private:
	meta_object_base *_base{nullptr};
};
static_assert(weak_rc_policy<default_rc_policy>);

template<typename T, rc_policy P>
struct shared_ptr;

template<typename T, typename D>
struct ptr_access_crtp {
	T &operator* () const {
		auto d = static_cast<const D *>(this);
		return *d->get();
	}

	T *operator-> () const {
		auto d = static_cast<const D *>(this);
		return d->get();
	}
};

template<typename D>
struct ptr_access_crtp<void, D> {

};

template<typename T, rc_policy P = default_rc_policy>
struct shared_ptr : ptr_access_crtp<T, shared_ptr<T, P>> {
	template<typename T_, rc_policy P_>
	friend struct shared_ptr;

	friend struct ptr_access_crtp<T, shared_ptr>;

	friend void swap(shared_ptr &x, shared_ptr &y) {
		std::swap(x._object, y._object);
		std::swap(x._policy, y._policy);
	}

	shared_ptr()
	: _object{nullptr}, _policy{} { }

	shared_ptr(std::nullptr_t)
	: _object{nullptr}, _policy{} { }

	shared_ptr(adopt_rc_t, T *object, P policy)
	: _object{object}, _policy{std::move(policy)} { }

	shared_ptr(const shared_ptr &other)
	: _object{other._object}, _policy{other._policy} {
		if(_policy)
			_policy.increment();
	}

	shared_ptr(shared_ptr &&other)
	: shared_ptr{} {
		swap(*this, other);
	}

	template<typename X>
	shared_ptr(shared_ptr<X, P> other)
	requires std::convertible_to<X *, T *>
	: _object{std::exchange(other._object, nullptr)},
			_policy{std::exchange(other._policy, P{})} { }

	// Aliasing constructor.
	template<typename X>
	shared_ptr(const shared_ptr<X, P> &other, T *object)
	: _object{object}, _policy{other._policy} {
		if(_policy)
			_policy.increment();
	}

	~shared_ptr() {
		if(_policy)
			_policy.decrement();
	}

	shared_ptr &operator= (shared_ptr other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () const {
		return _object;
	}

#if LIBSMARTER_HOSTED
	std::pair<T *, P> release() {
		return std::make_pair(std::exchange(_object, nullptr),
				std::exchange(_policy, P{}));
	}
#else
	void release() {
		_object = nullptr;
		_policy = P{};
	}
#endif

	T *get() const {
		return _object;
	}

	const P &policy() const {
		return _policy;
	}

private:
	T *_object;
	P _policy;
};

template<typename X, typename T, rc_policy P>
shared_ptr<X, P> static_pointer_cast(shared_ptr<T, P> other) {
	auto ptr = shared_ptr<X, P>{adopt_rc, static_cast<X *>(other.get()), other.policy()};
	other.release();
	return std::move(ptr);
}

template<rc_policy C, typename T, rc_policy P>
requires rc_policy_downcastable<P, C>
shared_ptr<T, C> rc_policy_downcast(const shared_ptr<T, P> &other) {
	auto new_policy = other.policy().downcast_policy(rc_policy_tag<C>{});
	new_policy.increment();
	return shared_ptr<T, C>{adopt_rc, other.get(), new_policy};
}

template<typename T, rc_policy P = default_rc_policy>
struct borrowed_ptr : ptr_access_crtp<T, borrowed_ptr<T, P>> {
	template<typename T_, rc_policy P_>
	friend struct borrowed_ptr;

	borrowed_ptr()
	: _object{nullptr}, _policy{} { }

	borrowed_ptr(std::nullptr_t)
	: _object{nullptr}, _policy{} { }

	borrowed_ptr(T *object, P policy)
	: _object{object}, _policy{std::move(policy)} { }

	template<typename X>
	requires std::convertible_to<X *, T *>
	borrowed_ptr(borrowed_ptr<X, P> other)
	: _object{other._object}, _policy{other._policy} { }

	// Construction from shared_ptr.
	template<typename X>
	requires std::convertible_to<X *, T *>
	borrowed_ptr(const shared_ptr<X, P> &other)
	: _object{other.get()}, _policy{other.policy()} { }

	T *get() const {
		return _object;
	}

	const P &policy() const {
		return _policy;
	}

	explicit operator bool () const {
		return _object;
	}

	shared_ptr<T, P> lock() const {
		if(!_policy)
			return shared_ptr<T, P>{};
		_policy.increment();
		return shared_ptr<T, P>{adopt_rc, _object, _policy};
	}

private:
	T *_object;
	P _policy;
};

template<typename X, typename T, rc_policy P>
borrowed_ptr<X, P> static_pointer_cast(borrowed_ptr<T, P> other) {
	return borrowed_ptr<X, P>{static_cast<X *>(other.get()), other.policy()};
}

template<typename T, weak_rc_policy P = default_rc_policy>
struct weak_ptr {
	friend void swap(weak_ptr &x, weak_ptr &y) {
		std::swap(x._object, y._object);
		std::swap(x._policy, y._policy);
	}

	weak_ptr()
	: _object{nullptr}, _policy{} { }

	weak_ptr(const shared_ptr<T, P> &ptr)
	: _object{ptr.get()}, _policy{ptr.policy()} {
		if (_policy)
			_policy.increment_weak();
	}

	template<typename X>
	weak_ptr(const shared_ptr<X, P> &ptr)
	requires std::convertible_to<X *, T *>
	: _object{ptr.get()}, _policy{ptr.policy()} {
		if (_policy)
			_policy.increment_weak();
	}

	weak_ptr(const weak_ptr &other)
	: _object{other._object}, _policy{other._policy} {
		if(_policy)
			_policy.increment_weak();
	}

	weak_ptr(weak_ptr &&other)
	: weak_ptr{} {
		swap(*this, other);
	}

	~weak_ptr() {
		if(_policy)
			_policy.decrement_weak();
	}

	weak_ptr &operator= (weak_ptr other) {
		swap(*this, other);
		return *this;
	}

	const P &policy() const {
		return _policy;
	}

	shared_ptr<T, P> lock() const {
		if(!_policy)
			return shared_ptr<T, P>{};

		if(!_policy.try_increment())
			return shared_ptr<T, P>{};

		return shared_ptr<T, P>{adopt_rc, _object, _policy};
	}

private:
	T *_object;
	P _policy;
};

template<typename T, typename... Args>
shared_ptr<T> make_shared(Args &&... args) {
	auto meta = new meta_object<T, default_deallocator>{1,
			default_deallocator{}, std::forward<Args>(args)...};
	return shared_ptr<T>{adopt_rc, meta->get(), default_rc_policy{meta}};
}

template<typename T, typename Allocator, typename... Args>
shared_ptr<T> allocate_shared(Allocator alloc, Args &&... args) {
	using meta_type = meta_object<T, allocator_deallocator<Allocator>>;
	auto memory = alloc.allocate(sizeof(meta_type));
	auto meta = new (memory) meta_type{1,
			allocator_deallocator<Allocator>{alloc}, std::forward<Args>(args)...};
	return shared_ptr<T>{adopt_rc, meta->get(), default_rc_policy{meta}};
}

} // namespace smarter

#endif // LIBSMARTER_SMARTER_HPP
