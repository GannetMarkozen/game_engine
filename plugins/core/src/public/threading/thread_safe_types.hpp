#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <concurrentqueue.h>
#include "defines.hpp"
#include "types.hpp"
#include "assert.hpp"
#include "rtti.hpp"

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

namespace thread {
template <std::integral T, T MAX_RETRIES = 1048, T MAX_YIELD_EXPONENT = 4>
NOINLINE inline auto exponential_yield(T& retries) -> void {
	if (retries < MAX_RETRIES) {
		std::this_thread::yield();
	} else {
		const std::chrono::microseconds delay{1u << std::min(retries - MAX_RETRIES, MAX_YIELD_EXPONENT)};
		std::this_thread::sleep_for(delay);
	}
	++retries;
}
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

	[[nodiscard]] FORCEINLINE constexpr explicit ScopeLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		mutex.lock();
	}

	FORCEINLINE ~ScopeLock() {
		mutex.unlock();
	}

private:
	T& [[clang::lifetimebound]] mutex;
};

template <cpts::Mutex T>
struct UniqueLock {
	[[nodiscard]] FORCEINLINE constexpr explicit UniqueLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{&in_mutex} {
		mutex->lock();
	}

	[[nodiscard]] FORCEINLINE constexpr explicit UniqueLock(T* in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		lock();
	}

	constexpr explicit UniqueLock(NoInit) {}

	[[nodiscard]] FORCEINLINE constexpr UniqueLock(UniqueLock&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}
		mutex = other.mutex;
		other.mutex = null;
	}

	FORCEINLINE auto operator=(UniqueLock&& other) noexcept -> UniqueLock& {
		if (this == &other) [[unlikely]] {
			return *this;
		}
		unlock();

		mutex = other.mutex;
		other.mutex = null;

		return *this;
	}

	FORCEINLINE ~UniqueLock() {
		unlock();
	}

	// Attempts to construct a UniqueLock from try_lock(). Will return NULL_OPTIONAL on failure.
	[[nodiscard]] FORCEINLINE static constexpr auto from_try_lock(T& mutex [[clang::lifetimebound]]) -> Optional<UniqueLock> requires requires { { std::declval<T&>().try_lock() } -> std::same_as<bool>; } {
		if (mutex.try_lock()) {
			Optional<UniqueLock> out{NO_INIT};
			out->mutex = &mutex;
			return out;
		} else {
			return NULL_OPTIONAL;
		}
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
	T* [[clang::lifetimebound]] mutex;
};

template <cpts::SharedMutex T>
struct ScopeExclusiveLock {
	NON_COPYABLE(ScopeExclusiveLock);

	[[nodiscard]] FORCEINLINE constexpr explicit ScopeExclusiveLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		mutex.lock();
	}

	FORCEINLINE ~ScopeExclusiveLock() {
		mutex.unlock();
	}

private:
	T& [[clang::lifetimebound]] mutex;
};

template <cpts::SharedMutex T>
struct UniqueExclusiveLock {
	FORCEINLINE constexpr explicit UniqueExclusiveLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{&in_mutex} {
		mutex->lock();
	}

	FORCEINLINE constexpr explicit UniqueExclusiveLock(T* in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		lock();
	}

	FORCEINLINE constexpr explicit UniqueExclusiveLock(NoInit) {}

	FORCEINLINE constexpr UniqueExclusiveLock(UniqueExclusiveLock&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}
		mutex = other.mutex;
		other.mutex = null;
	}

	FORCEINLINE auto operator=(UniqueExclusiveLock&& other) noexcept -> UniqueExclusiveLock& {
		if (this == &other) [[unlikely]] {
			return *this;
		}
		unlock();

		mutex = other.mutex;
		other.mutex = null;

		return *this;
	}

	FORCEINLINE ~UniqueExclusiveLock() {
		unlock();
	}

	// Attempts to construct a UniqueLock from try_lock(). Will return NULL_OPTIONAL on failure.
	[[nodiscard]] FORCEINLINE static constexpr auto from_try_lock(T& mutex [[clang::lifetimebound]]) -> Optional<UniqueExclusiveLock> requires requires { { std::declval<T&>().try_lock() } -> std::same_as<bool>; } {
		if (mutex.try_lock()) {
			Optional<UniqueExclusiveLock> out{NO_INIT};
			out->mutex = &mutex;
			return out;
		} else {
			return NULL_OPTIONAL;
		}
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
	T* [[clang::lifetimebound]] mutex;
};

template <cpts::SharedMutex T>
struct UniqueSharedLock {
	FORCEINLINE constexpr explicit UniqueSharedLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{&in_mutex} {
		mutex->lock_shared();
	}

	FORCEINLINE constexpr explicit UniqueSharedLock(T* in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		lock();
	}

	FORCEINLINE constexpr explicit UniqueSharedLock(NoInit) {}

	FORCEINLINE constexpr UniqueSharedLock(UniqueSharedLock&& other) noexcept {
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

	// Attempts to construct a UniqueSharedLock from try_lock_shared(). Will return NULL_OPTIONAL on failure.
	[[nodiscard]] FORCEINLINE static constexpr auto from_try_lock(T& mutex [[clang::lifetimebound]]) -> Optional<UniqueSharedLock> requires requires { { std::declval<T&>().try_lock_shared() } -> std::same_as<bool>; } {
		if (mutex.try_lock_shared()) {
			Optional<UniqueSharedLock> out{NO_INIT};
			out->mutex = &mutex;
			return out;
		} else {
			return NULL_OPTIONAL;
		}
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
	T* [[clang::lifetimebound]] mutex;
};

// Locks an shared mutex for read access within the scope.
template <cpts::SharedMutex T>
struct ScopeSharedLock {
	NON_COPYABLE(ScopeSharedLock);

	[[nodiscard]] FORCEINLINE constexpr explicit ScopeSharedLock(T& in_mutex [[clang::lifetimebound]])
		: mutex{in_mutex} {
		mutex.lock_shared();
	}

	FORCEINLINE ~ScopeSharedLock() {
		mutex.unlock_shared();
	}

private:
	T& [[clang::lifetimebound]] mutex;
};

// Moveable scoped access to the owner. Locks on construction, unlocks on destruction.
// @NOTE: Internal-use only.
template <typename Owner, cpts::Invokable<Owner&> ValueFn, cpts::Invokable<Owner&> LockFn, cpts::Invokable<Owner&> UnlockFn>
struct UniqueAccess {
	static_assert(!std::same_as<void, decltype(ValueFn{}(std::declval<Owner&>()))>);
	static_assert(std::is_reference_v<decltype(ValueFn{}(std::declval<Owner&>()))>);

	constexpr explicit UniqueAccess(Owner& in_owner [[clang::lifetimebound]])
		: owner{&in_owner} {
		lock();
	}

	constexpr UniqueAccess(UniqueAccess&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		owner = other.owner;
		other.owner = null;
	}

	constexpr auto operator=(UniqueAccess&& other) noexcept -> UniqueAccess& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		owner = other.owner;
		other.owner = null;

		return *this;
	}

	constexpr ~UniqueAccess() {
		unlock();
	}

	[[nodiscard]] constexpr auto has_value() const -> bool {
		return !!owner;
	}

	[[nodiscard]] constexpr auto operator->() const -> auto* {
		ASSERT(has_value());
		return &**this;
	}

	[[nodiscard]] constexpr auto operator*() const -> decltype(auto) {
		ASSERT(has_value());
		return (ValueFn{}(*owner));
	}

	// Unlocks and invalidates access. Will not double-unlock on destruction.
	constexpr auto unlock() -> bool {
		if (owner) {
			UnlockFn{}(*owner);
			owner = null;
			return true;
		}
		return false;
	}

private:
	constexpr auto lock() -> void {
		ASSERT(owner);
		LockFn{}(*owner);
	}

	Owner* [[clang::lifetimebound]] owner;
};

template <typename T, cpts::Mutex MutexType = Mutex>
struct Lock {
	NON_COPYABLE(Lock);

	constexpr Lock() = default;

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr Lock(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template <typename Self>
	[[nodiscard]] constexpr auto lock(this Self&& self) {
		using UniqueAccess = UniqueAccess<
			std::remove_reference_t<Self>,
			decltype([](Self& self) -> auto& { return self.value; }),
			decltype([](Self& self) { self.mutex.lock(); }),
			decltype([](Self& self) { self.mutex.unlock(); })
		>;
		return UniqueAccess{self};
	}

	// Retrieves the value without locking. Unsafe!
	[[nodiscard]] constexpr auto get_unsafe(this auto&& self) -> auto& {
		return self.value;
	}

	[[nodiscard]] constexpr auto get_mutex() const -> MutexType& {
		return mutex;
	}

private:
	T value;
	mutable MutexType mutex;
};

template <typename T, cpts::SharedMutex SharedMutexType = SharedMutex>
struct SharedLock {
	NON_COPYABLE(SharedLock);

	constexpr SharedLock() = default;

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr SharedLock(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	template <typename Self>
	[[nodiscard]] constexpr auto lock_exclusive(this Self&& self) {
		using UniqueAccess = UniqueAccess<
			std::remove_reference_t<Self>,
			decltype([](Self& self) -> auto& { return self.value; }),
			decltype([](Self& self) { self.mutex.lock(); }),
			decltype([](Self& self) { self.mutex.unlock(); })
		>;
		return UniqueAccess{self};
	}

	template <typename Self>
	[[nodiscard]] constexpr auto lock_shared(this Self&& self) {
		using UniqueAccess = UniqueAccess<
			std::remove_reference_t<Self>,
			decltype([](Self& self) -> auto& { return self.value; }),
			decltype([](Self& self) { self.mutex.lock_shared(); }),
			decltype([](Self& self) { self.mutex.unlock_shared(); })
		>;
		return UniqueAccess{self};
	}

	// Retrieves the value without locking. Unsafe!
	[[nodiscard]] constexpr auto get_unsafe(this auto&& self) -> auto& {
		return self.value;
	}

	[[nodiscard]] constexpr auto get_mutex() const -> SharedMutexType& {
		return mutex;
	}

private:
	T value;
	mutable SharedMutexType mutex;
};

template <typename T, cpts::SharedMutex SharedMutexType = SharedMutex>
struct RwLock {
	NON_COPYABLE(RwLock);

	constexpr RwLock() = default;

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr RwLock(Args&&... args)
		: value{std::forward<Args>(args)...} {}

	// Acquires exclusive write-access.
	[[nodiscard]] constexpr auto write() {
		using UniqueAccess = UniqueAccess<
			RwLock,
			decltype([](RwLock& self) -> T& { return self.value; }),
			decltype([](RwLock& self) { self.mutex.lock(); }),
			decltype([](RwLock& self) { self.mutex.unlock(); })
		>;
		return UniqueAccess{*this};
	}

	// Acquires shared read-access.
	[[nodiscard]] constexpr auto read() const {
		using UniqueAccess = UniqueAccess<
			const RwLock,
			decltype([](const RwLock& self) -> const T& { return self.value; }),
			decltype([](const RwLock& self) { self.mutex.lock_shared(); }),
			decltype([](const RwLock& self) { self.mutex.unlock_shared(); })
		>;
		return UniqueAccess{*this};
	}

	// Retrieves the value without locking. Unsafe!
	[[nodiscard]] constexpr auto get_unsafe(this auto&& self) -> auto& {
		return self.value;
	}

	[[nodiscard]] constexpr auto get_mutex() const -> SharedMutexType& {
		return mutex;
	}

private:
	T value;
	mutable SharedMutexType mutex;
};

// Acquires multiple locks at once with dead-lock avoidance algorithm. Must use a form of UniqueLock. Returns a tuple of UniqueLocks.]
// @TODO: Make this logic less cursed.
template <template <cpts::Mutex> typename... Locks, cpts::Mutex... Mutexes>
[[nodiscard]] auto lock_multi(Mutexes&... mutexes) -> Tuple<Locks<Mutexes>...> {
	Tuple<Optional<Locks<Mutexes>>...> locks;
	usize first_try_lock_index = 0;

	usize retries = 0;
	while (![&] -> bool {
		[&]<usize I>(this auto&& self) -> void {
			if constexpr (I < sizeof...(Locks)) {
				if (first_try_lock_index == I) {
					std::get<I>(locks).emplace(*std::get<I>(utils::make_tuple(&mutexes...)));
				} else {
					self.template operator()<I + 1>();
				}
			}
		}.template operator()<0>();

		for (usize i = 0; i < first_try_lock_index; ++i) {
			const bool result = [&]<usize I>(this auto&& self) -> bool {
				if constexpr (I < sizeof...(Locks)) {
					if (i == I) {
						if (auto result = utils::TypeAtIndex<I, Locks<Mutexes>...>::from_try_lock(*std::get<I>(utils::make_tuple(&mutexes...)))) {
							std::get<I>(locks).emplace(std::move(*result));
							return true;
						} else {
							first_try_lock_index = i;
							return false;
						}
					} else {
						return self.template operator()<I + 1>();
					}
				}

				ASSERT_UNREACHABLE;
			}.template operator()<0>();

			if (!result) {
				return false;
			}
		}

		for (usize i = first_try_lock_index + 1; i < sizeof...(Locks); ++i) {
			const bool result = [&]<usize I>(this auto&& self) -> bool {
				if constexpr (I < sizeof...(Locks)) {
					if (i == I) {
						if (auto result = utils::TypeAtIndex<I, Locks<Mutexes>...>::from_try_lock(*std::get<I>(utils::make_tuple(&mutexes...)))) {
							std::get<I>(locks).emplace(std::move(*result));
							return true;
						} else {
							first_try_lock_index = i;
							return false;
						}
					} else {
						return self.template operator()<I + 1>();
					}
				}

				ASSERT_UNREACHABLE;
			}.template operator()<0>();

			if (!result) {
				return false;
			}
		}

		return true;
	}()) {
		// Unlock all.
		utils::make_index_sequence_param_pack<sizeof...(Locks)>([&]<usize... Is>() {
			(std::get<Is>(locks).reset(), ...);
		});

		// Exponential yield.
		thread::exponential_yield(retries);
	}

	return utils::make_index_sequence_param_pack<sizeof...(Locks)>([&]<usize... Is>() {
		return utils::make_tuple(std::move(*std::get<Is>(locks))...);
	});
}

enum class AtomicAcquisitionResult : u8 {
	FOUND, ENQUEUED
};

template <typename T>
struct AtomicFindOrEnqueueResult {
	T& value;
	AtomicAcquisitionResult result;
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
	[[nodiscard]] constexpr auto find_or_enqueue(cpts::InvokableReturns<bool, const T&> auto&& predicate, Args&&... args) -> AtomicFindOrEnqueueResult<T> {
		Node* old_head = head.load(std::memory_order_relaxed);

		// First check if any current nodes have this value.
		for (Node* node = old_head; node; node = node->next) {
			if (std::invoke(FORWARD_AUTO(predicate), node->value)) {
				return {
					.value = node->value,
					.result = AtomicAcquisitionResult::FOUND,
				};
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
					return {
						.value = node->value,
						.result = AtomicAcquisitionResult::FOUND,
					};
				}
			}

			previous_next_node = new_head->next;

			std::this_thread::yield();
		}

		return {
			.value = new_head->value,
			.result = AtomicAcquisitionResult::ENQUEUED,
		};
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
template <typename T, usize N = 32>
struct MpscQueue {
	struct Node {
		Atomic<u32> count = 0;
		alignas(T) u8 data[N * sizeof(T)];
		Node* next = null;// Only useful for consumer so doesn't need to be atomic.
	};

	constexpr MpscQueue()
		: head{new Node{}} {}

	// Thread-safe move construct. (Thread-safe for other). Keeps the queue valid for more enqueues.
	constexpr MpscQueue(MpscQueue&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		Node* previous_head = other.head.exchange(new Node{}, std::memory_order_seq_cst);
		head.store(previous_head, std::memory_order_relaxed);
	}

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

				if (current_head->count.compare_exchange_weak(previous_count, previous_count + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) [[likely]] {
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
				if (!head.compare_exchange_strong(current_head, new_head, std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
					// Failed to change the head (another thread did it before us).
					delete new_head;
				}

				continue;
			}

			T& data = reinterpret_cast<T*>(current_head->data)[previous_count];
			std::construct_at(&data, std::forward<Args>(args)...);

			return data;
		}

		UNREACHABLE;
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

	// NOT thread-safe.
	constexpr auto for_each(cpts::Invokable<T&> auto&& fn) -> void {
		for (Node* node = head.load(std::memory_order_relaxed); node; node = node->next) {
			const u32 count = node->count.load(std::memory_order_relaxed);
			for (u32 i = 0; i < count; ++i) {
				std::invoke(FORWARD_AUTO(fn), reinterpret_cast<T*>(node->data)[i]);
			}
		}
	}

	constexpr auto for_each_with_break(cpts::InvokableReturns<bool, T&> auto&& fn) -> bool {
		for (Node* node = head.load(std::memory_order_relaxed); node; node = node->next) {
			const u32 count = node->count.load(std::memory_order_relaxed);
			for (u32 i = 0; i < count; ++i) {
				if (!std::invoke(FORWARD_AUTO(fn), reinterpret_cast<T*>(node->data)[i])) {
					return true;
				}
			}
		}
		return false;
	}

	// Thread-safe but requires locking for accuracy.
	[[nodiscard]] constexpr auto is_empty(this const MpscQueue& self) -> bool {
		return self.head.load(std::memory_order_relaxed)->count.load(std::memory_order_relaxed) == 0;
	}

	// NOT thread-safe. Does not maintain order. Implements swap.
	constexpr auto remove_at(this MpscQueue& self, Node* node, const u32 index) -> bool {
		ASSERT(node);
		ASSERT(node->count.load(std::memory_order_relaxed) > index);

		Node* head = self.head.load(std::memory_order_relaxed);
		ASSERT(head);

		const u32 swap_index = --head->count;
		if constexpr (!std::is_trivially_destructible_v<T>) {
			std::destroy_at(&reinterpret_cast<T*>(head->data)[index]);
		}

		if (node != head || index != swap_index) {
			T& dst = reinterpret_cast<T*>(node->data)[index];
			T& src = reinterpret_cast<T*>(head->data)[swap_index];
			if constexpr (cpts::TriviallyRelocatable<T>) {
				memcpy(&src, &dst, sizeof(T));
			} else {
				static_assert(std::is_move_constructible_v<T>);

				std::construct_at(&dst, std::move(src));
				std::destroy_at(&src);
			}
		}

		if (swap_index == 0) {// Removed last element in head. Swap out with next.
			self.head.store(head->next, std::memory_order_relaxed);
			delete head;

			return node == head;
		}

		return false;
	}

	Atomic<Node*> head;
};

// A closeable multi producer single consumer set.
template <typename T, usize N = 32, typename Hasher = std::hash<T>, typename EqualOp = std::equal_to<T>>
struct MpscSet {
	[[nodiscard]] constexpr auto find_or_enqueue(T value) -> AtomicFindOrEnqueueResult<T> {
		return slots[Hasher{}(value) % N].find_or_enqueue([&](const T& other) { return EqualOp{}(value, other); }, std::move(value));
	}

	constexpr auto enqueue_unique(T value) -> bool {
		return find_or_enqueue(std::move(value)).result == AtomicAcquisitionResult::ENQUEUED;
	}

	constexpr auto enqueue(T value) -> const T& {
		const usize index = Hasher{}(value) % N;
		ASSERT(!slots[index].find([&](const T& other) { return EqualOp{}(value, other); }));

		return slots[index].enqueue(std::move(value));
	}

	[[nodiscard]] constexpr auto find_or_add(T value) -> T& {
		return find_or_enqueue(std::move(value)).value;
	}

	[[nodiscard]] constexpr auto find(const T& value) -> T* {
		return slots[Hasher{}(value) % N].find([&](const T& other) { return EqualOp{}(value, other); });
	}

	[[nodiscard]] constexpr auto find_and_dequeue(const T& value) -> Optional<T> {
		return slots[Hasher{}(value) % N].find_and_dequeue([&](const T& other) { return EqualOp{}(value, other); });
	}

	// NOT thread-safe.
	constexpr auto for_each(cpts::Invokable<T&> auto&& fn) -> void {
		for (usize i = 0; i < N; ++i) {
			using Node = MpscList<T>::Node;
			for (Node* node = slots[i].head.load(std::memory_order_relaxed); node; node = node->next) {
				std::invoke(FORWARD_AUTO(fn), node->value);
			}
		}
	}

	// NOT thread-safe.
	constexpr auto for_each_with_break(cpts::InvokableReturns<bool, T&> auto&& fn) -> bool {
		for (usize i = 0; i < N; ++i) {
			using Node = MpscList<T>::Node;
			for (Node* node = slots[i].head.load(std::memory_order_relaxed); node; node = node->next) {
				if (!std::invoke(FORWARD_AUTO(fn), node->value)) {
					return true;
				}
			}
		}
		return false;
	}

	// NOT thread-safe.
	[[nodiscard]] constexpr auto is_empty() const -> bool {
		for (usize i = 0; i < N; ++i) {
			if (!!slots[i].head.load(std::memory_order_relaxed)) {
				return false;
			}
		}
		return true;
	}

	MpscList<T> slots[N];
};

template <typename Key, typename T, usize N = 32, typename Hasher = std::hash<Key>, typename EqualOp = std::equal_to<Key>>
struct MpscMap {
	struct KeyValue {
		constexpr KeyValue(Key key)
			: key{std::move(key)}, value{} {}

		Key key;
		T value;
	};

	[[nodiscard]] constexpr auto find_or_enqueue(Key key) -> AtomicFindOrEnqueueResult<T> {
		const usize index = Hasher{}(key) % N;

		const AtomicFindOrEnqueueResult<KeyValue> result = slots[index].find_or_enqueue([&](const KeyValue& other) { return EqualOp{}(key, other.key); }, std::move(key));
		return AtomicFindOrEnqueueResult<T>{
			.value = result.value.value,
			.result = result.result,
		};
	}

	[[nodiscard]] constexpr auto find_or_add(Key key) -> T& {
		return find_or_enqueue(std::move(key)).value;
	}

	[[nodiscard]] constexpr auto find(const Key& key) -> T* {
		const usize index = Hasher{}(key) % N;
		KeyValue* found = slots[index].find([&](const KeyValue& other) { return EqualOp{}(key, other.key); });
		return found ? &found->value : null;
	}

	[[nodiscard]] constexpr auto find_and_dequeue(const Key& key) -> Optional<T> {
		const usize index = Hasher{}(key) % N;
		if (auto found = slots[index].find_and_dequeue([&](const KeyValue& other) { return EqualOp{}(key, other.key); })) {
			return {std::move(found->value)};
		} else {
			return NULL_OPTIONAL;
		}
	}

	MpscList<KeyValue> slots[N];
};

enum class RcMode {
	FAST, THREAD_SAFE,
};

template <typename T, RcMode MODE = RcMode::THREAD_SAFE>
struct ControlBlock {
	using RefCount = std::conditional_t<(MODE == RcMode::THREAD_SAFE), Atomic<u32>, u32>;

	auto decrement_refcount() -> bool {
		if (--refcount == 0) {
			delete this;
			return true;
		}

		return false;
	}

	auto increment_refcount() -> void {
		++refcount;
	}

	auto add_refcount(const std::integral auto count) {
		return refcount += count;
	}

	T value;
	RefCount refcount = 1;
};

template <typename T, RcMode MODE = RcMode::THREAD_SAFE>
struct RefCountPtr {
	using ControlBlock = ControlBlock<T, MODE>;

	constexpr RefCountPtr() : cb{null} {}

	constexpr RefCountPtr(std::nullptr_t) : cb{null} {}

	constexpr explicit RefCountPtr(ControlBlock* cb)
		: cb{cb} {}

	constexpr RefCountPtr(const RefCountPtr& other) {
		cb = other.cb;
		increment_refcount();
	}

	constexpr RefCountPtr(RefCountPtr&& other) noexcept {
		if (this == &other) [[unlikely]] {
			return;
		}

		cb = other.cb;
		other.cb = null;
	}

	constexpr auto operator=(const RefCountPtr& other) -> RefCountPtr& {
		decrement_refcount();
		cb = other.cb;
		increment_refcount();
	}

	constexpr auto operator=(RefCountPtr&& other) noexcept -> RefCountPtr& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		decrement_refcount();
		cb = other.cb;
		other.cb = null;

		return *this;
	}

	constexpr ~RefCountPtr() {
		decrement_refcount();
	}

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	[[nodiscard]] static constexpr auto make(Args&&... args) -> RefCountPtr {
		return RefCountPtr{new ControlBlock{
				.value{std::forward<Args>(args)...},
				.refcount = 1,
			}
		};
	}

	constexpr auto increment_refcount() -> void {
		if (cb) {
			cb->increment_refcount();
		}
	}

	constexpr auto decrement_refcount() -> bool {
		if (cb) {
			cb->decrement_refcount();
		}

		return false;
	}

	[[nodiscard]] constexpr auto operator*() const -> T& {
		ASSERT(cb);
		return cb->value;
	}

	[[nodiscard]] constexpr auto operator->() const -> T* {
		return &(**this);
	}

	[[nodiscard]] constexpr auto get() const -> T* {
		return cb ? &cb->value : null;
	}

	[[nodiscard]] constexpr auto is_valid() const -> bool {
		return !!cb;
	}

	[[nodiscard]] constexpr operator bool() const {
		return is_valid();
	}

	[[nodiscard]] constexpr auto operator!() const -> bool {
		return !cb;
	}

	ControlBlock* cb;
};

#if 0
// Atomic shared ptr using 2-layer reference-counting.
template <typename T>
struct AtomicRefCountPtr {
	static_assert(sizeof(T*) == sizeof(u64), "AtomicRefCountPtr only works on 64-bit architecture!");
	static_assert(alignof(T*) == alignof(u64));

	static constexpr usize REFCOUNT_OFFSET = 48;
	static constexpr u64 REFCOUNT_MASK = UINT64_MAX >> (64 - REFCOUNT_OFFSET);
	static constexpr u64 ADDRESS_MASK = ~REFCOUNT_MASK;

	struct ControlBlock {
		union {
			T value;
		};
		Atomic<u32> refcount = 1;
	};

	

	Atomic<u64> cb;// First 48 bits are pointer. Second 16 bits are local ref-count.
};
#endif

template <typename T>
struct std::atomic<RefCountPtr<T>> {
	using ControlBlock = typename RefCountPtr<T>::ControlBlock;

	static_assert(sizeof(T*) == sizeof(u64), "Atomic<RefCountPtr> only works on 64-bit architecture!");
	static_assert(alignof(T*) == alignof(u64));

	static constexpr usize REFCOUNT_OFFSET = 48;
	static constexpr u64 REFCOUNT_MASK = UINT64_MAX >> (64 - REFCOUNT_OFFSET);
	static constexpr u64 ADDRESS_MASK = ~REFCOUNT_MASK;

	NON_COPYABLE(atomic<RefCountPtr<T>>);

	constexpr atomic<RefCountPtr<T>>()
		: cb{NULL} {}

	constexpr atomic<RefCountPtr<T>>(std::nullptr_t)
		: cb{NULL} {}

	constexpr atomic<RefCountPtr<T>>(RefCountPtr<T> value)
		: cb{value.cb} {
		if (value.cb) {
			value.cb->increment_refcount();
		}
	}

	~atomic<RefCountPtr<T>>() {
		store(null, std::memory_order_relaxed);
	}

	auto increment_local_refcount() -> u64 {
		u64 old_cb = cb.load(std::memory_order_relaxed);
		u64 new_cb;
		while (true) {
			new_cb = (old_cb & ADDRESS_MASK) | ((old_cb >> REFCOUNT_OFFSET) + 1) << REFCOUNT_OFFSET;
			if (cb.compare_exchange_weak(old_cb, new_cb, std::memory_order_seq_cst, std::memory_order_relaxed)) {
				break;
			}

			std::this_thread::yield();
		}

		return new_cb;
	}

	auto decrement_local_refcount(const u64 previous_cb) -> u64 {
		u64 old_cb = previous_cb;
		while (true) {
			u64 new_cb = (old_cb & ADDRESS_MASK) | ((old_cb >> REFCOUNT_OFFSET) - 1) << REFCOUNT_OFFSET;
			if (cb.compare_exchange_weak(old_cb, new_cb)) {
				return new_cb;
			}

			// The pointer has changed due to a store operation. The local refcount has been moved onto the global refcount on our behalf so decrement it here.
			if ((old_cb & ADDRESS_MASK) != (previous_cb & ADDRESS_MASK)) {
				reinterpret_cast<ControlBlock*>(previous_cb & ADDRESS_MASK)->decrement_refcount();
				return old_cb;
			}
		}

		UNREACHABLE;
	}

	auto load(const std::memory_order memory_order) -> RefCountPtr<T> {
		auto current_cb_value = increment_local_refcount();
		auto* current_cb = reinterpret_cast<ControlBlock*>(current_cb_value & ADDRESS_MASK);
		current_cb->increment_refcount();
		RefCountPtr<T> result{current_cb};
		decrement_local_refcount(current_cb_value);
		return result;
	}

	auto store(const RefCountPtr<T>& desired, const std::memory_order memory_order) -> RefCountPtr<T> {
		const u64 new_cb_value = (reinterpret_cast<u64>(desired.cb) & ADDRESS_MASK) | 0ull << REFCOUNT_OFFSET;
		const u64 old_cb_value = cb.exchange(new_cb_value, memory_order);

		auto* old_cb = reinterpret_cast<ControlBlock*>(old_cb_value & ADDRESS_MASK);
		const auto old_cb_local_refcount = old_cb_value >> REFCOUNT_OFFSET;

		old_cb->add_refcount(old_cb_local_refcount);
		const bool destructed = old_cb->decrement_refcount();

		if (destructed) {
			return null;
		} else {
			return RefCountPtr<T>{old_cb};
		}
	}

	auto compare_exchange_weak(RefCountPtr<T>& expected, const RefCountPtr<T>& desired, const std::memory_order success, const std::memory_order failure) -> bool {
		#if 0
		u64 old_cb_value = cb.load(std::memory_order_relaxed);
		const u64 desired_cb_value = (reinterpret_cast<u64>(desired.cb) & ADDRESS_MASK) | ((old_cb_value >> REFCOUNT_OFFSET) + 1) << REFCOUNT_OFFSET;
		if (cb.compare_exchange_weak(old_cb_value, desired_cb_value, success, failure)) {
			auto* old_cb = reinterpret_cast<ControlBlock*>(old_cb_value & ADDRESS_MASK);
			const auto old_cb_local_refcount = old_cb_value >> REFCOUNT_OFFSET;

			old_cb->add_refcount(old_cb_local_refcount);
			old_cb->decrement_refcount();
			return true;
		} else {
			//decrement_local_refcount(old_cb_value);
			return false;
		}
		#endif

		u64 old_cb_value = increment_local_refcount();
		const u64 desired_cb_value = reinterpret_cast<u64>(desired.cb) & ADDRESS_MASK;
		const bool result = cb.compare_exchange_weak(old_cb_value, desired_cb_value, success, failure);
		auto* old_cb = reinterpret_cast<ControlBlock*>(old_cb_value & ADDRESS_MASK);

		if (result) {
			const auto old_cb_local_refcount = old_cb_value >> REFCOUNT_OFFSET;

			old_cb->add_refcount(old_cb_local_refcount);
			old_cb->decrement_refcount();
			return true;
		} else {
			expected = RefCountPtr<T>{old_cb};
			decrement_local_refcount(old_cb_value);
		}

		return false;
	}

	Atomic<u64> cb;// First 48 bits are pointer. Second 16 bits are local ref-count.
};

#if 01
template <typename T>
struct AtomicList {
	struct Node {
		template <typename... Args> requires std::constructible_from<T, Args&&...>
		constexpr Node(SharedPtr<Node> next, Args&&... args)
			: value{std::forward<Args>(args)...}, next{std::move(next)} {}

		T value;
		Atomic<SharedPtr<Node>> next = null;
	};

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr auto enqueue(Args&&... args) -> void {
		auto new_head = std::make_shared<Node>(head.load(std::memory_order_relaxed), std::forward<Args>(args)...);
		while (!head.compare_exchange_weak(new_head->next, new_head, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			std::this_thread::yield();
		}
	}

	[[nodiscard]] constexpr auto dequeue() -> Optional<T> {
		auto old_head = head.load(std::memory_order_relaxed);
		while (old_head && !head.compare_exchange_weak(old_head, old_head->next, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			std::this_thread::yield();
		}

		if (old_head) {
			return {std::move(old_head->value)};
		} else {
			return NULL_OPTIONAL;
		}
	}

	Atomic<SharedPtr<Node>> head = null;
};
#else
template <typename T>
struct AtomicList {
	struct Node {
		template <typename... Args> requires std::constructible_from<T, Args&&...>
		constexpr Node(RefCountPtr<Node> next, Args&&... args)
			: value{std::forward<Args>(args)...}, next{std::move(next)} {}

		T value;
		RefCountPtr<Node> next = null;
	};

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	constexpr auto enqueue(Args&&... args) -> void {
		auto new_head = RefCountPtr<Node>::make(head.load(std::memory_order_relaxed), std::forward<Args>(args)...);
		while (!head.compare_exchange_weak(new_head->next, new_head, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			std::this_thread::yield();
		}
	}

	constexpr auto dequeue() -> Optional<T> {
		auto old_head = head.load(std::memory_order_relaxed);
		while (old_head && !head.compare_exchange_weak(old_head, old_head->next, std::memory_order_seq_cst, std::memory_order_relaxed)) {
			std::this_thread::yield();
		}

		if (old_head) {
			return {std::move(old_head->value)};
		} else {
			return NULL_OPTIONAL;
		}
	}

	Atomic<RefCountPtr<Node>> head = null;
};
#endif