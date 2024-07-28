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

	std::atomic<bool> flag = false;
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
	std::atomic<i32> value = 0;// Positive for readers count. -1 for writer.
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
	std::atomic<std::thread::id> exclusive_owner;
	std::atomic<u32> exclusive_recursion_count = 0;
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
	FORCEINLINE constexpr auto get() -> T& { return value; }
	FORCEINLINE constexpr auto get() const -> const T& { return value; }

	FORCEINLINE constexpr auto get_mutex() -> SharedMutexType& { return mutex; }
	FORCEINLINE constexpr auto get_mutex() const -> const SharedMutexType& { return mutex; }
	//~

private:
	T value;
	mutable SharedMutexType mutex;
};