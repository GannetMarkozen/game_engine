#pragma once

#include <shared_mutex>

#include "concurrentqueue.h"

#include "defines.hpp"
#include "threading.hpp"

template<typename T>
concept MutexConcept = requires(T t) {
	{ t.lock() };
	{ t.unlock() };
};

template<typename T>
concept SharedMutexConcept = requires(T t) {
	{ t.lock() };
	{ t.unlock() };
	{ t.lock_shared() };
	{ t.unlock_shared() };
};

struct SpinLock {
	[[nodiscard]] FORCEINLINE constexpr SpinLock() = default;
	NON_COPYABLE(SpinLock);

	FORCEINLINE fn lock() -> void {
		while (flag.exchange(true)) [[unlikely]] {
			read_only_spin();
		}
	}

	FORCEINLINE fn unlock() -> void {
		flag.store(false);
	}

private:
	// Read-only as to not mess with the cache-line and cause bad performance for other threads.
	NOINLINE fn read_only_spin() const -> void {
		static constexpr i32 MAX_RETRIES = 8;
		i32 retries = 0;
		do [[unlikely]] {
			if (retries++ < MAX_RETRIES) [[likely]] {
				std::this_thread::yield();
			} else {
				// Exponential backoff. Allow max sleep time of 255 microseconds. Really if you're hitting this you should be using a mutex.
				const std::chrono::microseconds delay{1 << std::min(retries - MAX_RETRIES, MAX_RETRIES)};
				std::this_thread::sleep_for(delay);
			}
		} while (flag.load(std::memory_order_relaxed));
	}

	std::atomic<bool> flag = false;
};

struct SharedSpinLock {
	[[nodiscard]] FORCEINLINE constexpr SharedSpinLock() = default;
	NON_COPYABLE(SharedSpinLock);

	fn lock_shared() -> void {
		i32 expected = value.load(std::memory_order_relaxed);
		do {
			static constexpr i32 MAX_RETRIES = 8;
			i32 retries = 0;
			while (expected < 0) {
				if (retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					// Exponential backoff. Allow max sleep time of 255 microseconds. Really if you're hitting this you should be using a mutex.
					const std::chrono::microseconds delay{1 << std::min(retries - MAX_RETRIES, MAX_RETRIES)};
					std::this_thread::sleep_for(delay);
				}

				expected = value.load(std::memory_order_relaxed);
			}
		} while (!value.compare_exchange_weak(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed));
	}

	fn unlock_shared() -> void {
		value.fetch_sub(1, std::memory_order_release);
	}

	fn lock() -> void {
		i32 expected = 0;
		while (!value.compare_exchange_weak(expected, -1, std::memory_order_acquire, std::memory_order_relaxed)) {
			static constexpr i32 MAX_RETRIES = 8;
			i32 retries = 0;
			do {
				if (retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					// Exponential backoff. Allow max sleep time of 255 microseconds. Really if you're hitting this you should be using a mutex.
					const std::chrono::microseconds delay{1 << std::min(retries - MAX_RETRIES, MAX_RETRIES)};
					std::this_thread::sleep_for(delay);
				}

				expected = value.load(std::memory_order_relaxed);
			} while (expected != 0);
		}
	}

	fn unlock() -> void {
		value.store(0, std::memory_order_release);
	}

private:
	std::atomic<i32> value = 0;// Positive for readers count. -1 for writer.
};

using Mutex = std::mutex;

using SharedMutex = std::shared_mutex;

// RAII mutex scope guard.
template<MutexConcept T>
struct ScopeLock {
	NON_COPYABLE(ScopeLock);

	[[nodiscard]] FORCEINLINE constexpr explicit ScopeLock(T& in_mutex)
		: mutex{in_mutex} {
		mutex.lock();
	}

	FORCEINLINE ~ScopeLock() {
		mutex.unlock();
	}

private:
	T& mutex;
};

template<MutexConcept T>
struct UniqueLock {
	[[nodiscard]] FORCEINLINE constexpr explicit UniqueLock(T& in_mutex)
		: mutex{&in_mutex} {
		mutex->lock();
	}

	[[nodiscard]] FORCEINLINE constexpr UniqueLock(UniqueLock&& other) noexcept {
		if (this == &other) [[unlikely]] return;
		mutex = other.mutex;
		other.mutex = nullptr;
	}

	FORCEINLINE fn operator=(UniqueLock&& other) noexcept -> UniqueLock& {
		if (this == &other) [[unlikely]] return *this;
		unlock();
		mutex = other.mutex;
		other.mutex = nullptr;
	}

	FORCEINLINE ~UniqueLock() {
		if (mutex) {
			mutex->unlock();
		}
	}

	FORCEINLINE fn lock() -> void {
		if (mutex) {
			mutex->lock();
		}
	}

	FORCEINLINE fn unlock() -> void {
		if (mutex) {
			mutex->unlock();
			mutex = nullptr;
		}
	}

private:
	T* mutex;
};

// Locks an shared mutex for read access within the scope.
template<SharedMutexConcept T>
struct ScopeSharedLock {
	NON_COPYABLE(ScopeSharedLock);

	[[nodiscard]]
	FORCEINLINE constexpr explicit ScopeSharedLock(T& in_mutex)
		: mutex{in_mutex} {
		mutex.lock_shared();
	}

	FORCEINLINE ~ScopeSharedLock() {
		mutex.unlock_shared();
	}

private:
	T& mutex;
};

// Stores T and Mutex. Enforces that you lock the mutex before accessing the object to prevent screw-ups.
template<typename T, MutexConcept MutexType = Mutex>
struct Locked {
	NON_COPYABLE(Locked);

	template<typename... Args>
	FORCEINLINE constexpr explicit Locked(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template<typename... Args>
	FORCEINLINE fn lock(Invokable<Args...> auto&& func, Args&&... args) -> decltype(func(std::forward<Args>(args)...)) {
		using Return = decltype(func(std::forward<Args>(args)...));

		ScopeLock lock{mutex};

		if constexpr (std::is_same_v<Return, void>) {
			std::invoke(FORWARD_AUTO(func), std::forward<Args>(args)...);
		} else {
			return std::invoke(FORWARD_AUTO(func), std::forward<Args>(args)...);
		}
	}

	//~
	// Unsafe.
	FORCEINLINE constexpr fn get() -> T& { return value; }
	FORCEINLINE constexpr fn get() const -> const T& { return value; }

	FORCEINLINE constexpr fn get_mutex() -> MutexType& { return mutex; }
	FORCEINLINE constexpr fn get_mutex() const -> const MutexType& { return mutex; }
	//~

private:
	T value;
	MutexType mutex;
};

// Stores T and Mutex. Enforces that you lock the mutex before accessing the object to prevent screw-ups.
template<typename T, SharedMutexConcept SharedMutexType = SharedMutex>
struct RWLocked {
	NON_COPYABLE(RWLocked);

	[[nodiscard]]
	FORCEINLINE constexpr RWLocked() = default;

	template<typename... Args>
	[[nodiscard]]
	FORCEINLINE constexpr explicit RWLocked(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template<typename... Args>
	FORCEINLINE fn read(Invokable<const T&, Args...> auto&& func, Args&&... args) const -> decltype(func(std::declval<const T&>(), std::forward<Args>(args)...)) {
		using Return = decltype(func(value, std::forward<Args>(args)...));

		ScopeSharedLock lock{mutex};

		if constexpr (std::is_same_v<Return, void>) {
			std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...);
		} else {
			return std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...);
		}
	}

	template<typename... Args>
	FORCEINLINE fn write(Invokable<T&, Args...> auto&& func, Args&&... args) -> decltype(func(std::declval<T&>(), std::forward<Args>(args)...)) {
		using Return = decltype(func(value, std::forward<Args>(args)...));

		ScopeLock lock{mutex};

		if constexpr (std::is_same_v<Return, void>) {
			std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...);
		} else {
			return std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...);
		}
	}

	//~
	// Unsafe.
	FORCEINLINE constexpr fn get() -> T& { return value; }
	FORCEINLINE constexpr fn get() const -> const T& { return value; }

	FORCEINLINE constexpr fn get_mutex() -> SharedMutexType& { return mutex; }
	FORCEINLINE constexpr fn get_mutex() const -> const SharedMutexType& { return mutex; }
	//~

private:
	T value;
	mutable SharedMutexType mutex;
};

#if 0
// @NOTE: This assumes threads aren't being constantly created and destroyed.
// Creates an instance of T for every thread so there is no contention.
template<typename T>
struct ThreadLocal {
private:
	struct alignas(CACHE_LINE_SIZE) CacheLinePadded {
		T value;
	};

public:
	ThreadLocal() {
		values.resize(core::get_num_threads());
	}

	[[nodiscard]] FORCEINLINE constexpr fn get() -> T& {
		return values[core::get_thread_index()].value;
	}

	[[nodiscard]] FORCEINLINE constexpr fn get() const -> const T& {
		return values[core::get_thread_index()].value;
	}

	Array<CacheLinePadded> values;
};
#endif

// Creates an instance of T for each thread so access is inherently thread-safe without contention.
template<typename T, SharedMutexConcept SharedMutexType = SharedMutex>
struct ThreadLocal {
private:
	struct alignas(CACHE_LINE_SIZE) CacheLinePadded {
		T value;
	};
public:
	[[nodiscard]]
	FORCEINLINE fn get() -> T& {
		T* value = thread_map.read([&](const auto& map) {
			const auto it = map.find(std::this_thread::get_id());
			return it != map.end() ? &(*it)->value : nullptr;
		});

		if (!value) [[unlikely]] {
			value = thread_map.write([&](auto& map) {
				//return map[std::this_thread::get_id()].value.get();
				return map.insert(std::this_thread::get_id(), std::make_unique<CacheLinePadded>()).get();
			});
			ASSERT(value != nullptr);
		}

		return *value;
	}

	[[nodiscard]]
	FORCEINLINE fn get() const -> const T& {
		return const_cast<ThreadLocal*>(this)->get();
	}

	// Exclusive blocking / write-access to each thread value.
	FORCEINLINE fn for_each_blocking(Invokable<T&> auto&& func) -> void {
		thread_map.write([&](auto& map) {
			for (auto& pair : map) {
				std::invoke(func, pair.second);
			}
		});
	}

	[[nodiscard]] FORCEINLINE fn operator*() -> T& { return get(); }
	[[nodiscard]] FORCEINLINE fn operator*() const -> const T& { return get(); }
	[[nodiscard]] FORCEINLINE fn operator->() -> T* { return &get(); }
	[[nodiscard]] FORCEINLINE fn operator->() const -> const T* { return &get(); }

	mutable RWLocked<Map<std::thread::id, UniquePtr<CacheLinePadded>>, SharedMutexType> thread_map;
};

#if 01
template<typename T, typename Traits = moodycamel::ConcurrentQueueDefaultTraits>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T, Traits>;

#elif 0



template<typename T>
struct ConcurrentQueue {
	FORCEINLINE fn enqueue(T value) -> bool {
		std::lock_guard lock{mutex};
		queue.push_back(std::move(value));
		return true;
	}

	FORCEINLINE fn try_dequeue(T& out) -> bool {
		std::lock_guard lock{mutex};

		if (queue.size() == 0) return false;

		out = std::move(queue.back());
		queue.pop_back();

		return true;
	}


private:
	Array<T> queue;
	SpinLock mutex;
};
#elif 01


template<typename T>
struct alignas(CACHE_LINE_SIZE) ConcurrentQueue {
private:
	struct Node {
		T value;
		std::atomic<Node*> next;
		u64 version;
	};

	struct TaggedNodePtr {
		Node* node;
		u64 counter;
	};

public:
	// Not thread-safe.
	~ConcurrentQueue() {
		for (Node* node = head.load(std::memory_order_relaxed); node; node = node->next.load(std::memory_order_relaxed)) {
			delete node;
		}
	}

	NOINLINE fn enqueue(T value) -> bool {
		auto* new_head = new Node{
			.value = std::move(value),
			.next = head.load(std::memory_order_relaxed),
		};

		static_assert(sizeof(Node*) == sizeof(std::atomic<Node*>));
		while (!head.compare_exchange_weak(reinterpret_cast<Node*&>(new_head->next), new_head)) {
			std::this_thread::yield();
		}

		return true;
	}

	NOINLINE fn try_dequeue(T& out) -> bool {
		Node* old_head = head.load(std::memory_order_relaxed);

		while (old_head && !head.compare_exchange_weak(old_head, old_head->next.load(std::memory_order_relaxed))) {}

		if (old_head) {
			out = std::move(old_head->value);
			delete old_head;
		}

		return !!old_head;
	}

private:
	std::atomic<Node*> head = nullptr;
};

#else

template<typename T>
struct alignas(CACHE_LINE_SIZE) ConcurrentQueue {
private:
	struct Node {
			T value;
			std::atomic<Node*> next;
			Node(T val) : value(std::move(val)), next(nullptr) {}
	};

public:
	~ConcurrentQueue() {
			Node* node = head.load(std::memory_order_relaxed);
			while (node) {
					Node* next = node->next.load(std::memory_order_relaxed);
					delete node;
					node = next;
			}
	}

	void enqueue(T value) {
			Node* new_node = new Node(std::move(value));
			Node* old_tail = tail.exchange(new_node, std::memory_order_acq_rel);
			old_tail->next.store(new_node, std::memory_order_release);
	}

	bool try_dequeue(T& out) {
			Node* old_head = head.load(std::memory_order_acquire);
			if (old_head == tail.load(std::memory_order_acquire)) {
					return false;  // Queue is empty
			}

			Node* next = old_head->next.load(std::memory_order_acquire);
			if (head.compare_exchange_strong(old_head, next, std::memory_order_acq_rel)) {
					out = std::move(next->value);
					delete old_head;
					return true;
			}

			return false;
	}

private:
	std::atomic<Node*> head = new Node(T{});  // Dummy node
	std::atomic<Node*> tail = head.load();
};


#endif