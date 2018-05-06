#ifndef LIBSMARTER_SMARTER_HPP
#define LIBSMARTER_SMARTER_HPP

#include <atomic>
#include <cassert>
#include <iostream>
#include <new>
#include <utility>

namespace smarter {

struct adopt_rc_t { };
//inline constexpr adopt_rc_t adopt_rc;
static constexpr adopt_rc_t adopt_rc;

// Each counter has a 'holder'. The holder is either null or
// another counter that controls the lifetime of the counter itself.
struct counter {
public:
	counter()
	: _holder{nullptr}, _count{0} { }

	// TODO: Do not take a pointer to a counter but a shared_ptr here.
	counter(adopt_rc_t, counter *holder, unsigned int initial_count)
	: _holder{holder}, _count{initial_count} { }

	counter(const counter &) = delete;

	counter &operator= (const counter &) = delete;

	~counter() {
		assert(!_count.load(std::memory_order_relaxed));
	}

protected:
	virtual void dispose() = 0;

public:
	void setup(adopt_rc_t, counter *holder, unsigned int initial_count) {
		assert(!_count.load(std::memory_order_relaxed));
		_holder = holder;
		_count.store(initial_count, std::memory_order_relaxed);
	}

	counter *holder() {
		return _holder;
	}

	unsigned int check_count() {
		return _count.load(std::memory_order_relaxed);
	}

	void increment() {
		auto count = _count.fetch_add(1, std::memory_order_acq_rel);
		assert(count);
	}

	bool increment_if_nonzero() {
		auto count = _count.load(std::memory_order_relaxed);
		do {
			if(!count)
				return false;
		} while(!_count.compare_exchange_strong(count, count + 1,
					std::memory_order_acq_rel));
		return true;
	}

	void decrement() {
		auto count = _count.fetch_sub(1, std::memory_order_acq_rel);
		if(count > 1)
			return;
		assert(count == 1);

		// dispose() is allowed to destruct the counter itself;
		// therefore we load _holder before calling it.
		auto h = _holder;

		dispose();

		// We expect that this recursion is too shallow to be a problem.
		if(h)
			h->decrement();
	}

private:
	counter *_holder;
	std::atomic<unsigned int> _count;
};

template<typename D, typename A = void>
struct crtp_counter : counter {
	crtp_counter() = default;

	crtp_counter(adopt_rc_t, counter *holder, unsigned int initial_count)
	: counter{adopt_rc, holder, initial_count} { }

	void dispose() override {
		auto d = static_cast<D *>(this);
		d->dispose(A{});
	}
};

struct dispose_memory { };
struct dispose_object { };

template<typename T>
struct box {
	// TODO: Write constructor that makes _stor appear initialized to GCC (e.g. via inline asm)?

	template<typename... Args>
	void construct(Args &&... args) {
		new (&_stor) T{std::forward<Args>(args)...};
	}

	T *get() {
		return reinterpret_cast<T *>(&_stor);
	}

	void destruct() {
		reinterpret_cast<T *>(&_stor)->~T();
	}

private:
	std::aligned_storage_t<sizeof(T), alignof(T)> _stor;
};

template<typename T>
struct meta_object
: crtp_counter<meta_object<T>, dispose_memory>,
		crtp_counter<meta_object<T>, dispose_object> {
	friend struct crtp_counter<meta_object, dispose_memory>;
	friend struct crtp_counter<meta_object, dispose_object>;

	template<typename... Args>
	meta_object(unsigned int initial_count, Args &&... args)
	: crtp_counter<meta_object, dispose_memory>{adopt_rc, nullptr, 1},
			crtp_counter<meta_object, dispose_object>{adopt_rc,
				static_cast<crtp_counter<meta_object, dispose_memory> *>(this),
				initial_count} {
		_bx.construct(std::forward<Args>(args)...);
	}

	T *get() {
		return _bx.get();
	}
	
	counter *memory_ctr() {
		return static_cast<crtp_counter<meta_object, dispose_memory> *>(this);
	}
	
	counter *object_ctr() {
		return static_cast<crtp_counter<meta_object, dispose_object> *>(this);
	}

private:
	void dispose(dispose_object) {
		_bx.destruct();
	}

	void dispose(dispose_memory) {
		delete this;
	}

	box<T> _bx;
};

template<typename T, typename H = void, typename = void>
struct borrowed_ptr {

private:
	T *_object;
	counter *_ctr;
};

template<typename T, typename H>
struct shared_ptr;

template<typename T, typename H>
struct shared_ptr_access {
	T &operator* () const {
		auto d = static_cast<const shared_ptr<T, H> *>(this);
		return *d->_object;
	}

	T *operator-> () const {
		auto d = static_cast<const shared_ptr<T, H> *>(this);
		return d->_object;
	}
};

template<typename H>
struct shared_ptr_access<void, H> {
	
};

template<typename T, typename H = void>
struct shared_ptr : shared_ptr_access<T, H> {
	template<typename T_, typename H_>
	friend struct shared_ptr;
	
	friend struct shared_ptr_access<T, H>;

	friend void swap(shared_ptr &x, shared_ptr &y) {
		std::swap(x._object, y._object);
		std::swap(x._ctr, y._ctr);
	}
	
	template<typename L>
	friend shared_ptr handle_cast(shared_ptr<T, L> other) {
		shared_ptr p;
		std::swap(p._object, other._object);
		std::swap(p._ctr, other._ctr);
		return std::move(p);
	}
	
	shared_ptr()
	: _object{nullptr}, _ctr{nullptr} { }
	
	shared_ptr(std::nullptr_t)
	: _object{nullptr}, _ctr{nullptr} { }

	shared_ptr(adopt_rc_t, T *object, counter *ctr)
	: _object{object}, _ctr{ctr} { }

	shared_ptr(const shared_ptr &other)
	: _object{other._object}, _ctr{other._ctr} {
		if(_ctr)
			_ctr->increment();
	}

	shared_ptr(shared_ptr &&other)
	: shared_ptr{} {
		swap(*this, other);
	}
	
	template<typename X>//, typename = std::enable_if_t<std::is_base_of_v<X, T>>>
	shared_ptr(shared_ptr<X, H> other)
	: _object{std::exchange(other._object, nullptr)},
			_ctr{std::exchange(other._ctr, nullptr)} { }
	
	~shared_ptr() {
		if(_ctr)
			_ctr->decrement();
	}

	shared_ptr &operator= (shared_ptr other) {
		swap(*this, other);
		return *this;
	}

	operator bool () const {
		return _object;
	}

	std::pair<T *, counter *> release() {
		return std::make_pair(std::exchange(_object, nullptr),
				std::exchange(_ctr, nullptr));
	}

	T *get() const {
		return _object;
	}

	counter *ctr() const {
		return _ctr;
	}

private:
	T *_object;
	counter *_ctr;
};

template<typename X, typename T, typename H>
shared_ptr<X, H> static_pointer_cast(shared_ptr<T, H> other) {
	auto [object, ctr] = other.release();
	return shared_ptr<X, H>{adopt_rc, static_cast<X *>(object), ctr};
}


template<typename T, typename H = void>
struct weak_ptr {
	friend void swap(weak_ptr &x, weak_ptr &y) {
		std::swap(x._object, y._object);
		std::swap(x._ctr, y._ctr);
	}

	weak_ptr()
	: _object{nullptr}, _ctr{nullptr} { }

	weak_ptr(const shared_ptr<T, H> &ptr)
	: _object{ptr.get()}, _ctr{ptr.ctr()} {
		assert(_ctr->holder());
		_ctr->holder()->increment();
	}

	template<typename X>
	weak_ptr(const shared_ptr<X, H> &ptr)
	: _object{ptr.get()}, _ctr{ptr.ctr()} {
		assert(_ctr->holder());
		_ctr->holder()->increment();
	}

	weak_ptr(const weak_ptr &other)
	: _object{other.object}, _ctr{other.ctr} {
		if(_ctr) {
			assert(_ctr->holder());
			_ctr->holder()->increment();
		}
	}

	weak_ptr(weak_ptr &&other)
	: weak_ptr{} {
		swap(*this, other);
	}

	~weak_ptr() {
		if(_ctr) {
			assert(_ctr->holder());
			_ctr->holder()->decrement();
		}
	}

	weak_ptr &operator= (weak_ptr other) {
		swap(*this, other);
		return *this;
	}

	shared_ptr<T, H> lock() const {
		if(!_ctr)
			return shared_ptr<T, H>{};

		if(!_ctr->increment_if_nonzero())
			return shared_ptr<T, H>{};
		
		return shared_ptr<T, H>{adopt_rc, _object, _ctr};
	}

private:
	T *_object;
	counter *_ctr;
};

template<typename T, typename... Args>
shared_ptr<T> make_shared(Args &&... args) {
	auto meta = new meta_object<T>{1, std::forward<Args>(args)...};
	return shared_ptr<T>{adopt_rc, meta->get(), meta->object_ctr()};
}

} // namespace smarter

#endif // LIBSMARTER_SMARTER_HPP
