#pragma once

#include "concurrentqueue.h"

#include "../defines.hpp"

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
		} while (flag.load());
	}

	std::atomic<bool> flag = false;
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

		while (old_head && !head.compare_exchange_weak(old_head, old_head->next.load(std::memory_order_acq_rel))) {}

		if (old_head) {
			out = std::move(old_head->value);
			delete old_head;
			return true;
		}

		return false;
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