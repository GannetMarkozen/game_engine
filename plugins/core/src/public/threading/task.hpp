#pragma once

#include "core_include.hpp"
#include "thread_safe_types.hpp"
#include <algorithm>
#include <thread>
#include <condition_variable>

// @TODO: Create custom allocator for SharedPtr Tasks.

namespace task {
enum class Priority : u8 {
	HIGH, NORMAL, LOW, COUNT
};

enum class Thread : u8 {
	MAIN, ANY, COUNT
};

enum class TaskState : u8 {
	STANDBY, WAITING, EXECUTING, COMPLETE
};

struct Task {
	struct Exclusive {
		WeakPtr<Task> other;
		SharedPtr<Mutex> mutex;
	};

	FORCEINLINE Task(Fn<void(const SharedPtr<Task>&)> fn, const Priority priority, const Thread thread, MpscQueue<SharedPtr<Task>>&& subsequents = {})
		: fn{std::move(fn)}, priority{priority}, subsequents{std::move(subsequents)}, thread{thread} {}

	[[nodiscard]] FORCEINLINE static auto make(cpts::Invokable<const SharedPtr<Task>&> auto&& task, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
		MpscQueue<SharedPtr<Task>> subsequents = {}, MpscQueue<WeakPtr<Task>>&& exclusives = {}) -> SharedPtr<Task>
	{
		return std::make_shared<Task>(FORWARD_AUTO(task), priority, thread, std::move(subsequents));
	}

	[[nodiscard]] FORCEINLINE static auto make(cpts::Invokable auto&& task, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
		MpscQueue<SharedPtr<Task>> subsequents = {}, MpscQueue<WeakPtr<Task>>&& exclusives = {}) -> SharedPtr<Task>
	{
		return make([task = std::move(task)](const SharedPtr<Task>&) { task(); }, priority, thread, std::move(subsequents));
	}

	[[nodiscard]] FORCEINLINE auto has_completed() const -> bool {
		return state == TaskState::COMPLETE;
	}

	[[nodiscard]] FORCEINLINE auto has_prerequisites() const -> bool {
		return prerequisites_remaining.load(std::memory_order_relaxed) > 0;
	}

	// @NOTE: Not entirely thread-safe! Must guarantee that that the other task is not being executed during this!
	auto add_subsequent(SharedPtr<Task> other) -> bool {
		ASSERT(other);
		ASSERT(!other->has_completed());
		ASSERTF(!other->enqueued || other->has_prerequisites(), "Can not add subsequent to a task that has the potential to be enqueued! other->enqueued == {}. other->has_prerequisites() == {}",
			other->enqueued, other->has_prerequisites());

		ScopeSharedLock _{subsequents_mutex};

		if (has_completed()) {
			return false;
		}

		++other->prerequisites_remaining;
		subsequents.enqueue(std::move(other));

		return true;
	}

	Fn<void(const SharedPtr<Task>&)> fn;
	MpscQueue<SharedPtr<Task>> subsequents;
	SharedLock<MpscQueue<Exclusive>> exclusives;
	mutable SharedMutex subsequents_mutex;

	Atomic<u32> prerequisites_remaining = 0;
	Atomic<TaskState> state = TaskState::STANDBY;
	Priority priority;
	Thread thread;
#if ASSERTIONS_ENABLED
	bool enqueued = false;
#endif
};

struct alignas(CACHE_LINE_SIZE) ThreadCondition {
	std::condition_variable_any condition;
	SharedMutex mutex;
	volatile bool is_asleep = false;
	volatile bool pending_shutdown = false;
};

inline CacheLinePadded<ConcurrentQueue<SharedPtr<Task>>> queues[utils::enum_count<Thread>()][utils::enum_count<Priority>()];
alignas(CACHE_LINE_SIZE) inline Array<UniquePtr<ThreadCondition>> thread_conditions;// Indexed via thread-id.
alignas(CACHE_LINE_SIZE) inline Array<std::thread> worker_threads;
alignas(CACHE_LINE_SIZE) inline Atomic<u32> num_tasks_in_flight = 0;
alignas(CACHE_LINE_SIZE) inline bool pending_shutdown = false;

namespace impl {
[[nodiscard]] FORCEINLINE auto get_this_thread_condition() -> ThreadCondition& {
	return *thread_conditions[thread::get_this_thread_id()];
}

// Impl for waking up a thread. Returns true if successfully woke up the thread.
inline auto wake_up(ThreadCondition& condition) -> bool {
	ScopeSharedLock lock{condition.mutex};

	// If thread is already awake do nothing.
	if (!condition.is_asleep) {
		return false;
	}

	condition.is_asleep = false;
	condition.condition.notify_one();

	return true;
}
}

[[nodiscard]] FORCEINLINE auto get_num_worker_threads() -> usize {
	return worker_threads.size();
}

[[nodiscard]] FORCEINLINE auto get_num_threads() -> usize {
	return get_num_worker_threads() + 1;
}

FORCEINLINE auto wake_up_thread(const ThreadId id) -> bool {
	return impl::wake_up(*thread_conditions[id]);
}

inline auto wake_up_threads(const Thread thread = Thread::ANY, const usize count = 1) -> usize {
	ASSERT(count > 0);

	usize num_awoken = 0;

	if (impl::wake_up(*thread_conditions[thread::MAIN_THREAD_ID]) && ++num_awoken >= count) {
		return num_awoken;
	}

	// Search for a worker thread to wake up.
	// @NOTE: Order is arbitrary. Potentially bad.
	if (thread == Thread::ANY) {
		for (usize i = thread::MAIN_THREAD_ID; i < thread_conditions.size(); ++i) {
			if (impl::wake_up(*thread_conditions[i]) && ++num_awoken >= count) {
				return num_awoken;
			}
		}
	}

	return num_awoken;
}

inline auto wake_up_all_threads() -> usize {
	usize num_awoken = 0;

	for (auto& thread_condition : thread_conditions) {
		num_awoken += impl::wake_up(*thread_condition);
	}

	return num_awoken;
}

template <bool IS_IN_MAIN_THREAD>
[[nodiscard]] FORCEINLINE auto try_dequeue_task() -> SharedPtr<Task> {
	SharedPtr<Task> out = null;

	if constexpr (IS_IN_MAIN_THREAD) {
		for (auto& queue : queues[static_cast<u8>(Thread::MAIN)]) {
			if (queue->try_dequeue(out)) {
				return out;
			}
		}
	}

	for (auto& queue : queues[static_cast<u8>(Thread::ANY)]) {
		if (queue->try_dequeue(out)) {
			return out;
		}
	}

	return out;
}

template <bool IS_IN_MAIN_THREAD>
NOINLINE inline auto do_work(cpts::InvokableReturns<bool> auto&& request_exit) -> void {
	while (!std::invoke(FORWARD_AUTO(request_exit))) {
		SharedPtr<Task> task = null;

		// Try dequeue an available task. Else sleep this thread.
		u64 count = 0;
		while (true) {
			while (!(task = try_dequeue_task<IS_IN_MAIN_THREAD>())) [[unlikely]] {
				auto& thread_condition = impl::get_this_thread_condition();

				UniqueExclusiveLock lock{thread_condition.mutex};

				if ((task = try_dequeue_task<IS_IN_MAIN_THREAD>())) [[likely]] {
					break;
				}

				// Try exiting again.
				if (std::invoke(FORWARD_AUTO(request_exit)) || (pending_shutdown && num_tasks_in_flight.load(std::memory_order_relaxed) == 0)) [[unlikely]] {
					return;
				}

				ASSERT(!thread_condition.is_asleep);
				thread_condition.is_asleep = true;

				// Only awake if is_asleep is false.
				// @TODO: Spin for a bit before sleeping.
				thread_condition.condition.wait(lock, [&] { return !thread_condition.is_asleep; });
			}

			auto exclusives_access = task->exclusives.lock_exclusive();
			ASSERT(!task->has_completed());

			// Skip exclusives logic if there are no exclusives.
			if (exclusives_access->is_empty()) {
				task->state = TaskState::EXECUTING;
				break;
			}

#if 0
			const auto acquire_all_required_locks = [&] {
				const bool result = exclusives_access->for_each_with_break([&](const WeakPtr<Task>& weak_exclusive_task) {
					if (const auto exclusive_task = weak_exclusive_task.lock()) {
						if (auto result = UniqueLock<Mutex>::from_try_lock(exclusive_task->exclusive_execution_mutex)) {
							execution_locks.emplace_back(std::move(*result));
						} else {
							return false;
						}

						if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(exclusive_task->subsequents_mutex)) {
							subsequent_locks.emplace_back(std::move(*result));
						} else {
							return false;
						}
					}

					return true;
				});
			};
#endif

#if 0
			u32 num_retries = 0;
			u32 num_currently_executing_exclusives = 0;

			Array<UniqueLock<Mutex>> execution_locks;
			Array<UniqueSharedLock<SharedMutex>> subsequent_locks;

			while (true) {
				UniqueLock lock{task->exclusive_execution_mutex};

				using Node = MpscQueue<WeakPtr<Task>>::Node;
				for (Node* node = exclusives_access->head.load(std::memory_order_relaxed); node; node = node->next) {
					for (i32 i = static_cast<i32>(node->count.load(std::memory_order_relaxed)) - 1; i >= 0; --i) {

						const auto exclusive = reinterpret_cast<WeakPtr<Task>*>(node->data)[i].lock();
						if (!exclusive || exclusive->has_completed()) {
							continue;
						}

						if (auto result = UniqueLock<Mutex>::from_try_lock(exclusive->exclusive_execution_mutex)) {
							execution_locks.emplace_back(std::move(*result));
						} else {
							goto RETRY_OR_CONTINUE;
						}

						if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(exclusive->subsequents_mutex)) {
							subsequent_locks.emplace_back(std::move(*result));
						} else {
							goto RETRY_OR_CONTINUE;
						}

						if (exclusive->state.load(std::memory_order_relaxed) == TaskState::EXECUTING) {
							exclusive->subsequents.enqueue(task);
							++num_currently_executing_exclusives;
						}
					}
				}

				task->state = TaskState::EXECUTING;
				goto EXECUTE;

				ASSERT_UNREACHABLE;

			RETRY_OR_CONTINUE:
				// Clear all locks.
				execution_locks.clear();
				subsequent_locks.clear();
				lock.unlock();

				if (num_currently_executing_exclusives == 0) {// Spin then retry.
					// Exponential backoff and try again.
					// @TODO: Should fallback to dequeueing tasks to not totally waste CPU-cycles.
					static constexpr u32 MAX_RETRIES = 8;
					if (num_retries++ < MAX_RETRIES) {
						std::this_thread::yield();
					} else {
						const std::chrono::microseconds delay{1u << std::min(num_retries - MAX_RETRIES, MAX_RETRIES)};
						std::this_thread::sleep_for(delay);
					}
				} else {
					task->prerequisites_remaining += num_currently_executing_exclusives;
					break;
				}
			}
#elif 0

			Array<UniqueLock<Mutex>> execution_locks;
			Array<UniqueSharedLock<SharedMutex>> subsequent_locks;

			UniqueLock lock{task->exclusive_execution_mutex};

			u32 num_retries = 0;
			while (exclusives_access->for_each_with_break([&](const WeakPtr<Task>& weak_exclusive_task) {
				if (const auto exclusive_task = weak_exclusive_task.lock()) {
					if (auto result = UniqueLock<Mutex>::from_try_lock(exclusive_task->exclusive_execution_mutex)) {
						execution_locks.emplace_back(std::move(*result));
					} else {
						return false;
					}

					if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(exclusive_task->subsequents_mutex)) {
						subsequent_locks.emplace_back(std::move(*result));
					} else {
						return false;
					}
				}

				return true;
			})) {
				// Clear all locks.
				lock.unlock();
				execution_locks.clear();
				subsequent_locks.clear();

				// Exponential backoff and try again.
				// @TODO: Should fallback to dequeueing tasks to not totally waste CPU-cycles.
				static constexpr u32 MAX_RETRIES = 8;
				if (num_retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					const std::chrono::microseconds delay{1u << std::min(num_retries - MAX_RETRIES, MAX_RETRIES)};
					std::this_thread::sleep_for(delay);
				}
			}

			u32 num_currently_executing_exclusive_tasks = 0;
			exclusives_access->for_each([&](const WeakPtr<Task>& weak_exclusive_task) {
				const auto exclusive_task = weak_exclusive_task.lock();
				if (exclusive_task && exclusive_task->state.load(std::memory_order_relaxed) == TaskState::EXECUTING) {
					++num_currently_executing_exclusive_tasks;
					exclusive_task->subsequents.enqueue(task);
				}
			});

			if (num_currently_executing_exclusive_tasks == 0) {
				task->state = TaskState::EXECUTING;
				break;
			} else {
				task->prerequisites_remaining += num_currently_executing_exclusive_tasks;
			}

#elif 0
			Array<UniqueLock<Mutex>> execution_locks;
			Array<UniqueSharedLock<SharedMutex>> subsequent_locks;
			u32 num_retries = 0;
			while (exclusives_access->for_each_with_break([&](const WeakPtr<Task>& weak_exclusive_task) {
				if (const auto exclusive_task = weak_exclusive_task.lock()) {
					if (auto result = UniqueLock<Mutex>::from_try_lock(exclusive_task->exclusive_execution_mutex)) {
						execution_locks.emplace_back(std::move(*result));
					} else {
						return false;
					}

					if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(exclusive_task->subsequents_mutex)) {
						subsequent_locks.emplace_back(std::move(*result));
					} else {
						return false;
					}
				}

				return true;
			})) {
				// Clear all locks.
				execution_locks.clear();
				subsequent_locks.clear();

				// Exponential backoff and try again.
				// @TODO: Should fallback to dequeueing tasks to not totally waste CPU-cycles.
				static constexpr u32 MAX_RETRIES = 8;
				if (num_retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					const std::chrono::microseconds delay{1u << std::min(num_retries - MAX_RETRIES, MAX_RETRIES)};
					std::this_thread::sleep_for(delay);
				}
			}
#else
			Array<Task::Exclusive> exclusives;
			exclusives_access->for_each([&](const Task::Exclusive& exclusive) {
				if (auto exclusive_task = exclusive.other.lock()) {
					exclusives.push_back(exclusive);
				}
			});

			exclusives_access.unlock();

			// Try lock all mutexes that require it.
			Array<UniqueLock<Mutex>> exclusive_locks;
			Array<UniqueSharedLock<SharedMutex>> subsequent_locks;

			u32 num_retries = 0;
			while (true) {
				for (const Task::Exclusive& exclusive : exclusives) {
					if (const auto exclusive_task = exclusive.other.lock()) {
						if (auto result = UniqueLock<Mutex>::from_try_lock(*exclusive.mutex)) {
							exclusive_locks.push_back(std::move(*result));
						} else {
							goto RETRY;
						}

						if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(exclusive_task->subsequents_mutex)) {
							subsequent_locks.push_back(std::move(*result));
						} else {
							goto RETRY;
						}
					}
				}

				break;
				ASSERT_UNREACHABLE;

			RETRY:
				exclusive_locks.clear();
				subsequent_locks.clear();

				// Exponential backoff. Required for decent performance in highly contentious situations.
				static constexpr u32 MAX_RETRIES = 8;
				if (num_retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					const std::chrono::microseconds delay{1 << std::min(num_retries - MAX_RETRIES, 10u)};
					std::this_thread::sleep_for(delay);
				}
			}

			u32 num_exclusives_executing = 0;
			for (const Task::Exclusive& exclusive : exclusives) {
				const auto exclusive_task = exclusive.other.lock();
				if (exclusive_task && exclusive_task->state.load(std::memory_order_relaxed) == TaskState::EXECUTING) {
					exclusive_task->subsequents.enqueue(task);
					++num_exclusives_executing;
				}
			}

			if (num_exclusives_executing == 0) {
				task->state = TaskState::EXECUTING;
				break;
			} else {
				task->prerequisites_remaining += num_exclusives_executing;
#if ASSERTIONS_ENABLED
				task->enqueued = false;
#endif
			}
#endif
		}
	EXECUTE:

		// Execute the task.
		task->fn(task);

		// Dequeued the final task. Wake up all other threads and exit.
		if (--num_tasks_in_flight == 0 && pending_shutdown) [[unlikely]] {
			wake_up_all_threads();
			return;
		}

		// Take the subsequents.
		MpscQueue<SharedPtr<Task>> subsequents = [&] {
			ScopeExclusiveLock _{task->subsequents_mutex};

			task->state = TaskState::COMPLETE;

			return std::move(task->subsequents);
		}();

		u32 num_subsequents_enqueued = 0;
		[[maybe_unused]] bool any_subsequents_for_main_thread = false;

		// Enqueue subsequent if there's no more remaining prerequisites.
		Optional<SharedPtr<Task>> result;
		while ((result = subsequents.dequeue())) {
			auto& subsequent = *result;
			if (--subsequent->prerequisites_remaining == 0) {
				const auto thread = subsequent->thread;
				const auto priority = subsequent->priority;
				// Directly enqueue task.
				queues[static_cast<u8>(thread)][static_cast<u8>(priority)]->enqueue(std::move(subsequent));

				++num_subsequents_enqueued;
				if constexpr (!IS_IN_MAIN_THREAD) {
					any_subsequents_for_main_thread |= thread == Thread::MAIN;
				}
			}
		}

		// Only should wake up more threads if this task enqueued more than 1 subsequent. 1 subsequent can
		// be handled by this current awake thread when it loops over.
		if (num_subsequents_enqueued > 1) {
			wake_up_threads(Thread::ANY, num_subsequents_enqueued - 1);
		} else if constexpr (!IS_IN_MAIN_THREAD) {
			if (any_subsequents_for_main_thread) {// Wake up main thread if a single task was enqueued that targets the main thread.
				wake_up_thread(thread::MAIN_THREAD_ID);
			}
		}
	}
}

inline auto do_work(cpts::InvokableReturns<bool> auto&& request_exit) -> void {
	if (thread::is_in_main_thread()) {
		do_work<true>(FORWARD_AUTO(request_exit));
	} else {
		do_work<false>(FORWARD_AUTO(request_exit));
	}
}

inline auto do_work_until_all_tasks_complete(cpts::InvokableReturns<bool> auto&&... additional_conditions) -> void {
	do_work([&] {
		return num_tasks_in_flight.load(std::memory_order_relaxed) == 0 && (std::invoke(FORWARD_AUTO(additional_conditions)) && ...);
	});
}

inline auto init(const usize num_workers = std::thread::hardware_concurrency() - 1) -> bool {
	ASSERT(thread::is_in_main_thread());

	thread_conditions.reserve(num_workers + 1);// 1 is reserved for the Main thread.

	for (usize i = 0; i < num_workers + 1; ++i) {
		thread_conditions.push_back(std::make_unique<ThreadCondition>());
	}

	worker_threads.reserve(num_workers);

	for (usize i = 0; i < num_workers; ++i) {
		worker_threads.emplace_back([i] {
			thread::impl::thread_index = ThreadId{static_cast<u16>(i + 1)};

			do_work([] {
				return thread_conditions[thread::get_this_thread_id()]->pending_shutdown;
			});
		});
	}

	return true;
}

inline auto deinit() -> void {
	ASSERT(thread::is_in_main_thread());

	pending_shutdown = true;

	// Must wake up the thread so it can exit.
	wake_up_all_threads();

	for (auto& worker : worker_threads) {
		worker.join();
	}

	worker_threads.clear();
	thread_conditions.clear();
}

inline auto enqueue(SharedPtr<Task> task, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
	const Span<const SharedPtr<Task>> prerequisites = {}, const Span<const SharedPtr<Task>> exclusives = {}, const bool should_wake_up_thread = true) -> void
{
	ASSERT(task);
	ASSERT(!task->has_completed());
	ASSERT(!task->enqueued);
	ASSERT(!std::ranges::contains(prerequisites, task));
	ASSERT(!std::ranges::contains(exclusives, task));

#if ASSERTIONS_ENABLED
	task->enqueued = true;
#endif

	// Add exclusives.
	if (!exclusives.empty()) {
		for (const auto& exclusive : exclusives) {
			ASSERT(exclusive);

#if 0
			exclusive->exclusives.lock_shared()->enqueue(task);
			task->exclusives.get_unsafe().enqueue(exclusive);// Shouldn't be enqueued so should be safe to not lock.
#else
			auto mutex = std::make_shared<Mutex>();

			task->exclusives.get_unsafe().enqueue(Task::Exclusive{
				.other = exclusive,
				.mutex = mutex,
			});

			exclusive->exclusives.lock_shared()->enqueue(Task::Exclusive{
				.other = task,
				.mutex = std::move(mutex),
			});
#endif
		}
	}

	// Add prerequisites.
	u32 num_prerequisites = 0;

	// Need to lock subsequents of prerequisites since we don't want them dispatching on another thread while we are modifying it.
	for (const auto& prerequisite : prerequisites) {
		prerequisite->subsequents_mutex.lock_shared();

		if (!prerequisite->has_completed()) {
			++num_prerequisites;
			prerequisite->subsequents.enqueue(task);
		}
	}

	task->prerequisites_remaining += num_prerequisites;

	for (const auto& prerequisite : prerequisites) {
		prerequisite->subsequents_mutex.unlock_shared();
	}

	++num_tasks_in_flight;

	if (num_prerequisites == 0) {// Immediately enqueue. Otherwise a prerequisite task will enqueue at a later stage.
		queues[static_cast<u8>(thread)][static_cast<u8>(priority)]->enqueue(std::move(task));

		if (should_wake_up_thread) {
			wake_up_threads(thread);
		}
	}
}

inline auto enqueue(Fn<void(const SharedPtr<Task>&)> fn, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
	const Span<const SharedPtr<Task>> prerequisites = {}, const Span<const SharedPtr<Task>> exclusives = {}, const bool should_wake_up_thread = true) -> SharedPtr<Task>
{
	auto task = std::make_shared<Task>(std::move(fn), priority, thread);

	enqueue(task, priority, thread, prerequisites, exclusives, should_wake_up_thread);

	return task;
}

FORCEINLINE auto enqueue(cpts::Invokable auto&& fn, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
	const Span<const SharedPtr<Task>> prerequisites = {}, const Span<const SharedPtr<Task>> exclusives = {}, const bool should_wake_up_thread = true) -> SharedPtr<Task>
{
	return enqueue([fn = FORWARD_AUTO(fn)](const SharedPtr<Task>&) { std::invoke(fn); }, priority, thread, prerequisites, exclusives, should_wake_up_thread);
}

inline auto busy_wait_for_tasks_to_complete(const Span<const SharedPtr<Task>> tasks) -> void {
	volatile bool tasks_completed = false;

	enqueue([&tasks_completed, this_thread_id = thread::get_this_thread_id()] {
		tasks_completed = true;

		// Ensure the waiting thread is awoken to exit the do_work loop.
		wake_up_thread(this_thread_id);
	}, Priority::HIGH, Thread::ANY, tasks);

	do_work([&] { return tasks_completed; });
}

NOINLINE inline auto wait_for_tasks_to_complete(const Span<const SharedPtr<Task>> tasks) -> bool {
	// Early return if all tasks are already complete.
	if ([&] {
		for (const auto& task : tasks) {
			if (!task->has_completed()) {
				return false;
			}
		}
		return true;
	}()) return false;

	volatile bool tasks_completed = false;
	std::condition_variable_any condition;
	Mutex mutex;

	UniqueLock lock{mutex};

	enqueue([&tasks_completed, &condition, &mutex] {
		ScopeLock _{mutex};

		tasks_completed = true;
		condition.notify_one();
	}, Priority::HIGH, Thread::ANY, tasks, {}, true);

	condition.wait(lock, [&] { return tasks_completed; });

	return true;
}

// Creates a task for each index for best thread utilization but potentially high overhead.
inline auto parallel_for_unbalanced(const usize count, cpts::Invokable<usize> auto&& fn) -> void {
	if (count <= 1) {
		for (usize i = 0; i < count; ++i) {
			std::invoke(fn, i);
		}
	}

#if 0
	num_tasks_in_flight += count + 1;

	volatile bool tasks_complete = false;

	SharedPtr<Task> subsequent = Task::make([&, this_thread_id = thread::get_this_thread_id()](const SharedPtr<Task>&) {
		tasks_complete = true;
		wake_up_thread(this_thread_id);
	}, Priority::HIGH, Thread::ANY);

	subsequent->prerequisites_remaining = count;

	for (usize i = 0; i < count; ++i) {
		SharedPtr<Task> task = Task::make([&, i] {
			std::invoke(FORWARD_AUTO(fn), i);
		}, Priority::HIGH, Thread::ANY);
		task->subsequents.enqueue(subsequent);

		queues[static_cast<u8>(Thread::ANY)][static_cast<u8>(Priority::HIGH)]->enqueue(std::move(task));
	}

	// Wake up threads (-1 since we are including this thread).
	wake_up_threads(Thread::ANY, count - 1);

	do_work([&] { return tasks_complete; });
#else

	Atomic<usize> num_tasks_remaining = count;

	for (usize i = 0; i < count; ++i) {
		enqueue([&fn, &num_tasks_remaining, i, this_thread = thread::get_this_thread_id()] {
			std::invoke(FORWARD_AUTO(fn), i);

			if (!--num_tasks_remaining) {
				wake_up_thread(this_thread);
			}
		}, Priority::HIGH, Thread::ANY, {}, {}, false);
	}

	wake_up_threads(Thread::ANY, count);

	do_work([&] { return !num_tasks_remaining.load(std::memory_order_relaxed); });
#endif
}

// Creates a task per-thread for minimal task overhead.
inline auto parallel_for_balanced(const usize count, cpts::Invokable<usize> auto&& fn, const usize num_tasks = get_num_threads()) -> void {
	ASSERT(num_tasks > 0);

	if (count <= 1) {
		for (usize i = 0; i < count; ++i) {
			std::invoke(fn, i);
		}
	}

	const usize num_per_task = count / num_tasks;
	const usize leftover = count % num_tasks;

	parallel_for_unbalanced(std::min(count, num_tasks), [&](const usize i) {
		usize start = i * num_per_task;
		usize end = start + num_per_task;
		if (i < leftover) {
			start += i;
			end += i + 1;
		}

		for (usize j = start; j < end; ++j) {
			std::invoke(fn, j);
		}
	});
}

template <bool BALANCED = false>
FORCEINLINE auto parallel_for(const usize count, cpts::Invokable<usize> auto&& fn) -> void {
	if constexpr (BALANCED) {
		parallel_for_balanced(count, FORWARD_AUTO(fn));
	} else {
		parallel_for_unbalanced(count, FORWARD_AUTO(fn));
	}
}
}