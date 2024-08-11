#pragma once

#include <mutex>
#include <shared_mutex>
#include <concurrentqueue.h>
#include "defines.hpp"
#include "types.hpp"

DECLARE_INT_ALIAS(ThreadId, u16);

namespace thread {
namespace impl {
thread_local inline ThreadId thread_index{static_cast<u16>(0)};
}

// Maximum number of allowed concurrent threads. This exists for certain data structures to be decently performant.
constexpr usize MAX_THREADS = 32;

constexpr ThreadId MAIN_THREAD_ID{static_cast<u16>(0)};

[[nodiscard]] FORCEINLINE auto get_this_thread_id() -> ThreadId {
	return impl::thread_index;
}

[[nodiscard]] FORCEINLINE auto is_in_main_thread() -> bool {
	return get_this_thread_id() == MAIN_THREAD_ID;
}

[[nodiscard]] FORCEINLINE auto is_in_worker_thread() -> bool {
	return !is_in_main_thread();
}
}

template<typename T, typename Traits = moodycamel::ConcurrentQueueDefaultTraits>
using ConcurrentQueue = moodycamel::ConcurrentQueue<T, Traits>;

// Pads T to cache-line alignment.
template <typename T>
struct alignas(CACHE_LINE_SIZE) CacheLinePadded {
	FORCEINLINE constexpr CacheLinePadded() = default;
	FORCEINLINE constexpr CacheLinePadded(const CacheLinePadded&) = default;
	FORCEINLINE constexpr CacheLinePadded(CacheLinePadded&&) noexcept = default;
	FORCEINLINE constexpr auto operator=(const CacheLinePadded&) -> CacheLinePadded& = default;
	FORCEINLINE constexpr auto operator=(CacheLinePadded&&) noexcept -> CacheLinePadded& = default;

	template <typename... Args> requires (std::is_constructible_v<T, Args&&...>)
	FORCEINLINE constexpr CacheLinePadded(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template <typename Other> requires (std::is_assignable_v<T, Other&&>)
	FORCEINLINE constexpr auto operator=(Other&& other) noexcept -> CacheLinePadded& {
		value = std::forward<Other>(other);
		return *this;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto operator*(this Self&& self) -> decltype(auto) {
		return (std::forward_like<Self>(self.value));
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto operator->(this Self&& self) -> decltype(auto) {
		return &(*std::forward<Self>(self));
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto get(this Self&& self) -> decltype(auto) {
		return (*std::forward<Self>(self));
	}

	[[nodiscard]] FORCEINLINE constexpr operator T&() {
		return value;
	}

	[[nodiscard]] FORCEINLINE constexpr operator const T&() const {
		return value;
	}

private:
	T value;
};

template <typename T>
using Atomic = std::atomic<T>;

namespace cpts {
template<typename T>
concept Mutex = requires(T t) {
	{ t.lock() };
	{ t.unlock() };
};

template<typename T>
concept SharedMutex = requires(T t) {
	{ t.lock() };
	{ t.unlock() };
	{ t.lock_shared() };
	{ t.unlock_shared() };
};
}

struct SpinLock {
	[[nodiscard]] FORCEINLINE constexpr SpinLock() = default;
	NON_COPYABLE(SpinLock);

	FORCEINLINE auto lock() -> void {
		while (flag.exchange(true)) [[unlikely]] {
			read_only_spin();
		}
	}

	FORCEINLINE auto unlock() -> void {
		flag.store(false);
	}

private:
	// Read-only as to not mess with the cache-line and cause bad performance for other threads.
	NOINLINE auto read_only_spin() const -> void {
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

	Atomic<bool> flag = false;
};

// @NOTE: SharedMutex is generally faster.
struct SharedSpinLock {
	[[nodiscard]] FORCEINLINE constexpr SharedSpinLock() = default;
	NON_COPYABLE(SharedSpinLock);

	auto lock_shared() -> void {
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

	auto unlock_shared() -> void {
		value.fetch_sub(1, std::memory_order_release);
	}

	auto lock() -> void {
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

	auto unlock() -> void {
		value.store(0, std::memory_order_release);
	}

private:
	Atomic<i32> value = 0;// Positive for readers count. -1 for writer.
};

using Mutex = std::mutex;

using SharedMutex = std::shared_mutex;

using RecursiveMutex = std::recursive_mutex;

// Can recursively call lock() / lock_shared() within the same thread without deadlocking. Will still deadlock if you first
// acquire a shared lock then an exclusive lock - can not upgrade locks.
struct RecursiveSharedMutex {
	FORCEINLINE auto lock() -> void {
		if (exclusive_owner != std::this_thread::get_id()) {// @NOTE: Does this comparison have to be atomic?
			mutex.lock();
			exclusive_owner = std::this_thread::get_id();
		}
		++exclusive_recursion_count;
	}

	FORCEINLINE auto unlock() -> void {
		if (--exclusive_recursion_count == 0) {
			exclusive_owner = std::thread::id{};
			mutex.unlock();
		}
	}

	FORCEINLINE auto lock_shared() -> void {
		if (exclusive_owner != std::this_thread::get_id()) {
			mutex.lock_shared();
		}
	}

	FORCEINLINE auto unlock_shared() -> void {
		if (exclusive_owner != std::this_thread::get_id()) {
			mutex.unlock_shared();
		}
	}

private:
	SharedMutex mutex;
	Atomic<std::thread::id> exclusive_owner;
	Atomic<u32> exclusive_recursion_count = 0;
};

// RAII mutex scope guard.
template <cpts::Mutex T>
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

template <cpts::Mutex T>
struct UniqueLock {
	[[nodiscard]] FORCEINLINE constexpr explicit UniqueLock(T& in_mutex)
		: mutex{&in_mutex} {
		mutex->lock();
	}

	[[nodiscard]] FORCEINLINE constexpr explicit UniqueLock(T* in_mutex)
		: mutex{in_mutex} {
		lock();
	}

	[[nodiscard]] FORCEINLINE constexpr UniqueLock(UniqueLock&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}
		mutex = other.mutex;
		other.mutex = null;
	}

	FORCEINLINE auto operator=(UniqueLock&& other) noexcept -> UniqueLock& {
		if (this == &other) [[unlikely]] {
			return;
		}
		unlock();

		mutex = other.mutex;
		other.mutex = null;
	}

	FORCEINLINE ~UniqueLock() {
		unlock();
	}

	FORCEINLINE auto lock() -> void {
		if (mutex) {
			mutex->lock();
		}
	}

	FORCEINLINE auto unlock() -> void {
		if (mutex) {
			mutex->unlock();
			mutex = null;
		}
	}

private:
	T* mutex;
};

template <cpts::SharedMutex T>
struct UniqueSharedLock {
	[[nodiscard]] FORCEINLINE constexpr explicit UniqueSharedLock(T& in_mutex)
		: mutex{&in_mutex} {
		mutex->lock_shared();
	}

	[[nodiscard]] FORCEINLINE constexpr explicit UniqueSharedLock(T* in_mutex)
		: mutex{in_mutex} {
		lock();
	}

	[[nodiscard]] FORCEINLINE constexpr UniqueSharedLock(UniqueSharedLock&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}
		mutex = other.mutex;
		other.mutex = null;
	}

	FORCEINLINE constexpr auto operator=(UniqueSharedLock&& other) noexcept -> UniqueSharedLock& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		unlock();

		mutex = other.mutex;
		other.mutex = null;

		return *this;
	}

	FORCEINLINE ~UniqueSharedLock() {
		unlock();
	}

	FORCEINLINE auto lock() -> void {
		if (mutex) {
			mutex->lock_shared();
		}
	}

	FORCEINLINE auto unlock() -> void {
		if (mutex) {
			mutex->unlock_shared();
			mutex = null;
		}
	}

private:
	T* mutex;
};

// Locks an shared mutex for read access within the scope.
template <cpts::SharedMutex T>
struct ScopeSharedLock {
	NON_COPYABLE(ScopeSharedLock);

	[[nodiscard]] FORCEINLINE constexpr explicit ScopeSharedLock(T& in_mutex)
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
template <typename T, cpts::Mutex MutexType = Mutex>
struct Locked {
	NON_COPYABLE(Locked);

	template<typename... Args> requires std::constructible_from<T, Args&&...>
	FORCEINLINE constexpr explicit Locked(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template<typename... Args>
	FORCEINLINE auto lock(cpts::Invokable<T&, Args&&...> auto&& func, Args&&... args) -> decltype(auto) {
		ScopeLock lock{mutex};
		return (std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...));
	}

	//~
	// Unsafe.
	[[nodiscard]] FORCEINLINE constexpr auto get() -> T& { return value; }
	[[nodiscard]] FORCEINLINE constexpr auto get() const -> const T& { return value; }

	[[nodiscard]] FORCEINLINE constexpr auto get_mutex() -> MutexType& { return mutex; }
	[[nodiscard]] FORCEINLINE constexpr auto get_mutex() const -> const MutexType& { return mutex; }
	//~

private:
	T value;
	MutexType mutex;
};

// Stores T and Mutex. Enforces that you lock the mutex before accessing the object to prevent screw-ups.
template<typename T, cpts::SharedMutex SharedMutexType = SharedMutex>
struct RWLocked {
	NON_COPYABLE(RWLocked);

	FORCEINLINE constexpr RWLocked() = default;

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	FORCEINLINE constexpr explicit RWLocked(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template <typename... Args>
	FORCEINLINE auto read(cpts::Invokable<const T&, Args...> auto&& func, Args&&... args) const -> decltype(auto) {
		ScopeSharedLock lock{mutex};
		return (std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...));
	}

	template <typename... Args>
	FORCEINLINE auto write(cpts::Invokable<T&, Args&&...> auto&& func, Args&&... args) -> decltype(auto) {
		ScopeLock lock{mutex};
		return (std::invoke(FORWARD_AUTO(func), value, std::forward<Args>(args)...));
	}

	struct ScopedReadAccess {
		NON_COPYABLE(ScopedReadAccess);

		explicit ScopedReadAccess(const RWLocked& owner [[clang::lifetimebound]])
			: owner{owner} {
			owner.mutex.lock_shared();
		}

		FORCEINLINE ~ScopedReadAccess() {
			owner.mutex.unlock_shared();
		}

		[[nodiscard]] FORCEINLINE auto operator*() const -> const T& {
			return owner.value;
		}

		[[nodiscard]] FORCEINLINE auto operator->() const -> const T* {
			return &owner.value;
		}

		[[nodiscard]] FORCEINLINE auto get() const -> const T& {
			return **this;
		}

	private:
		const RWLocked& owner;
	};

	struct ScopedWriteAccess {
		NON_COPYABLE(ScopedWriteAccess);

		explicit ScopedWriteAccess(RWLocked& owner [[clang::lifetimebound]])
			: owner{owner} {
			owner.mutex.lock();
		}

		FORCEINLINE ~ScopedWriteAccess() {
			owner.mutex.unlock();
		}

		[[nodiscard]] FORCEINLINE auto operator*() const -> T& {
			return owner.value;
		}

		[[nodiscard]] FORCEINLINE auto operator->() const -> T* {
			return &owner.value;
		}

		[[nodiscard]] FORCEINLINE auto get() const -> T& {
			return **this;
		}

	private:
		RWLocked& owner;
	};

	[[nodiscard]] FORCEINLINE auto get_scoped_read_access() const {
		return ScopedReadAccess{*this};
	}

	[[nodiscard]] FORCEINLINE auto get_scoped_write_access() {
		return ScopedWriteAccess{*this};
	}

	//~
	// Unsafe.
	[[nodiscard]] FORCEINLINE constexpr auto get() -> T& { return value; }
	[[nodiscard]] FORCEINLINE constexpr auto get() const -> const T& { return value; }

	[[nodiscard]] FORCEINLINE constexpr auto get_mutex() -> SharedMutexType& { return mutex; }
	[[nodiscard]] FORCEINLINE constexpr auto get_mutex() const -> const SharedMutexType& { return mutex; }
	//~

private:
	T value;
	mutable SharedMutexType mutex;
};

// Atomic multiple producer single consume linked-list.
template <typename T>
struct MpscList {
	struct Node {
		T value;
		Node* next = null;
	};

	// NOT thread-safe.
	constexpr ~MpscList() {
		Node* node = head.load(std::memory_order_relaxed);
		while (node) {
			Node* old_node = node;
			node = node->next;

			delete old_node;
		}
	}

	// Thread-safe.
	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr auto enqueue(Args&&... args) -> T& {
		Node* new_head = new Node{
			.value{std::forward<Args>(args)...},
			.next = head.load(std::memory_order_relaxed),
		};

		while (!head.compare_exchange_weak(new_head->next, new_head, std::memory_order_relaxed, std::memory_order_relaxed)) {
			std::this_thread::yield();
		}

		return new_head->value;
	}

	// Thread-safe.
	[[nodiscard]] constexpr auto find(cpts::InvokableReturns<bool, const T&> auto&& predicate) -> T* {
		Node* node = head.load(std::memory_order_relaxed);
		do {
			if (!node) {
				break;
			}

			if (std::invoke(FORWARD_AUTO(predicate), node->value)) {
				return &node->value;
			}
		} while ((node = node->next));

		return null;
	}

	// NOT thread-safe.
	[[nodiscard]] constexpr auto find_and_dequeue(cpts::InvokableReturns<bool, const T&> auto&& predicate) -> Optional<T> {
		Node* node = head.load(std::memory_order_relaxed);
		Node* parent = null;
		do {
			if (!node) {
				break;
			}

			if (std::invoke(FORWARD_AUTO(predicate), node->value)) {
				if (parent) {
					parent->next = node->next;
				} else {
					head = node->next;
				}

				Optional<T> out{std::move(node->value)};
				delete node;

				return out;
			}
			parent = node;
		} while ((node = node->next));

		return NULL_OPTIONAL;
	}

	// Thread-safe.
	template <typename... Args> requires std::constructible_from<T, Args&&...>
	[[nodiscard]] constexpr auto find_or_enqueue(cpts::InvokableReturns<bool, const T&> auto&& predicate, Args&&... args) -> T& {
		Node* old_head = head.load(std::memory_order_relaxed);

		// First check if any current nodes have this value.
		for (Node* node = old_head; node; node = node->next) {
			if (std::invoke(FORWARD_AUTO(predicate), node->value)) {
				return node->value;
			}
		}

		Node* new_head = new Node{
			.value{std::forward<Args>(args)...},
			.next = old_head,
		};

		const Node* previous_next_node = new_head->next;
		while (!head.compare_exchange_weak(new_head->next, new_head, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			for (Node* node = new_head->next; node && node != previous_next_node; node = node->next) {
				if (std::invoke(FORWARD_AUTO(predicate), node->value)) {
					delete new_head;
					return node->value;
				}
			}

			previous_next_node = new_head->next;

			std::this_thread::yield();
		}

		return new_head->value;
	}

	// NOT thread-safe.
	[[nodiscard]] constexpr auto dequeue() -> Optional<T> {
		if (Node* old_head = head.load(std::memory_order_relaxed)) {
			head.store(old_head->next, std::memory_order_relaxed);
			Optional<T> out{std::move(old_head->value)};

			delete old_head;

			return out;
		} else {
			return NULL_OPTIONAL;
		}
	}

	Atomic<Node*> head = null;
};

// Atomic multiple producer single consumer queue with chunked contiguous allocation.
template <typename T, usize N = 64>
struct MpscQueue {
	struct Node {
		Atomic<u32> count = 0;
		alignas(T) u8 data[N * sizeof(T)];
		Node* next = null;// Only useful for consumer so doesn't need to be atomic.
	};

	// NOT thread-safe.
	constexpr ~MpscQueue() {
		Node* node = head.load(std::memory_order_relaxed);
		do {
			if constexpr (!std::is_trivially_destructible_v<T>) {
				for (u32 i = 0; i < node->count.load(std::memory_order_relaxed); ++i) {
					std::destroy_at(&reinterpret_cast<T*>(node->data)[i]);
				}
			}

			Node* old_node = node;
			node = node->next;

			delete old_node;
		} while (node);
	}

	// Thread-safe.
	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr auto enqueue(Args&&... args) -> T& {
		Node* current_head = head.load(std::memory_order_relaxed);

		while (true) {
			// Whether this node is full. Means we need to create a new node.
			bool is_node_at_max_capacity = false;
			u32 previous_count = current_head->count.load(std::memory_order_relaxed);
			while (true) {
				if (previous_count == N) {
					is_node_at_max_capacity = true;
					break;
				}

				if (current_head->count.compare_exchange_weak(previous_count, previous_count + 1, std::memory_order_seq_cst, std::memory_order_seq_cst)) [[likely]] {
					break;
				}

				std::this_thread::yield();
			}

			// Construct a new node that will become the head. Then loop around again and try to enqueue.
			if (is_node_at_max_capacity) {
				Node* new_head = new Node{
					.next = current_head,
				};

				// Attempt to change the head.
				if (!head.compare_exchange_strong(current_head, new_head, std::memory_order_seq_cst, std::memory_order_seq_cst)) [[unlikely]] {
					// Failed to change the head (another thread did it before us).
					delete new_head;
				}

				continue;
			}

			T& data = reinterpret_cast<T*>(current_head->data)[previous_count];
			std::construct_at(&data, std::forward<Args>(args)...);

			return data;
		}
	}

	// NOT thread-safe.
	[[nodiscard]] constexpr auto dequeue() -> Optional<T> {
		Node* node = head.load(std::memory_order_relaxed);

		if (const u32 count = node->count.load(std::memory_order_relaxed)) {
			const auto index = node->count.fetch_sub(1, std::memory_order_relaxed) - 1;
			T& data = reinterpret_cast<T*>(node->data)[index];

			Optional<T> out{std::move(data)};
			std::destroy_at(&data);

			if (count == 1 && node->next) {// Just dequeued the final element in this node.
				head = node->next;
				delete node;
			}

			return out;
		} else {
			return NULL_OPTIONAL;
		}
	}

	Atomic<Node*> head = new Node{};
};

template <typename Key, typename T, usize N = 64, typename Hasher = std::hash<Key>, typename EqualOp = std::equal_to<Key>>
struct MpscMap {
	struct KeyValue {
		constexpr KeyValue(Key key)
			: key{std::move(key)}, value{} {}

		Key key;
		T value;
	};

	[[nodiscard]] constexpr auto find_or_add(Key key) -> T& {
		const usize index = Hasher{}(key) % N;

		return slots[index].find_or_enqueue([&](const KeyValue& other) { return EqualOp{}(key, other.key); }, std::move(key)).value;
	}

	[[nodiscard]] constexpr auto find(const Key& key) -> T* {
		const usize index = Hasher{}(key) % N;
		KeyValue* found = slots[index].find([&](const KeyValue& other) { return EqualOp{}(key, other.key); });
		return found ? &found->value : null;
	}

	MpscList<KeyValue> slots[N];
};