#pragma once

#include "core_include.hpp"
#include "fmt/format.h"
#include "thread_safe_types.hpp"
#include "utils.hpp"
#include <algorithm>
#include <thread>
#include <condition_variable>
#include <source_location>

// @TODO: Create custom allocator for SharedPtr Tasks.

#define TMP_ALLOW_DOUBLE_ENQUEUE 0

enum class Priority : u8 {
	HIGH,
	NORMAL,
	LOW,
	COUNT
};

enum class Thread : u8 {
	MAIN,// Only runs on the main thread.
	ANY,// Run on any thread (main / workers).
	COUNT
};

enum class TaskState : u8 {
	STANDBY,// Awaiting execution.
	EXECUTING,// Currently executing.
	COMPLETE,// Finished execution.
};

struct Task {
	struct Exclusive {
		WeakPtr<Task> other;// The other task that runs exclusively against this task.
		SharedPtr<Mutex> mutex;// The shared mutex for the two exclusive tasks (self and other). A mutex for each pair (to reduce contention).
	};

	FORCEINLINE Task(Fn<void(const SharedPtr<Task>&)> fn, const Priority priority, const Thread thread, MpscQueue<SharedPtr<Task>>&& subsequents = {}
#if ASSERTIONS_ENABLED
		, String name = default_name_from_source_location(std::source_location::current())
#endif
		)
		: fn{std::move(fn)}, priority{priority}, subsequents{std::move(subsequents)}, thread{thread}
#if ASSERTIONS_ENABLED
			, name{std::move(name)}
#endif
		 {}

	[[nodiscard]] FORCEINLINE static auto make(cpts::Invokable<const SharedPtr<Task>&> auto&& task, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
		MpscQueue<SharedPtr<Task>> subsequents = {}
#if ASSERTIONS_ENABLED
		, String name = default_name_from_source_location(std::source_location::current())
#endif
		) -> SharedPtr<Task>
	{
		return std::make_shared<Task>(FORWARD_AUTO(task), priority, thread, std::move(subsequents)
#if ASSERTIONS_ENABLED
			, std::move(name)
#endif
		);
	}

	[[nodiscard]] FORCEINLINE static auto make(cpts::Invokable auto&& task, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
		MpscQueue<SharedPtr<Task>> subsequents = {}
#if ASSERTIONS_ENABLED
		, String name = default_name_from_source_location(std::source_location::current())
#endif
		) -> SharedPtr<Task>
	{
		return make([task = std::move(task)](const SharedPtr<Task>&) { task(); }, priority, thread, std::move(subsequents)
#if ASSERTIONS_ENABLED
			, name
#endif
		);
	}

	[[nodiscard]] FORCEINLINE auto has_completed() const -> bool {
		return state == TaskState::COMPLETE;
	}

	[[nodiscard]] FORCEINLINE auto has_prerequisites() const -> bool {
		return prerequisites_remaining.load(std::memory_order_relaxed) > 0;
	}

#if 0
	// @NOTE: Not entirely thread-safe! Must guarantee that that the other task is not being executed during this!
	auto add_subsequent(SharedPtr<Task> other) -> bool {
		ASSERT(other);

		if (other->has_completed()) {
			return false;
		}

#if 0
		ASSERT(!other->has_completed());
		ASSERTF(!other->enqueued || other->has_prerequisites(), "Can not add subsequent to a task that has the potential to be enqueued! other->enqueued == {}. other->has_prerequisites() == {}",
			other->enqueued, other->has_prerequisites());
#endif

		ScopeSharedLock _{subsequents_mutex};

		if (has_completed()) {
			return false;
		}

		++other->prerequisites_remaining;
		subsequents.enqueue(std::move(other));

		return true;
	}
#endif

	enum class AddSubsequentResult : u8 {
		SUCCESS,
		SUBSEQUENT_ALREADY_EXECUTING,
		SUBSEQUENT_ALREADY_COMPLETED,
		PREREQUISITE_ALREADY_COMPLETED,
	};

	auto add_subsequent_assumes_locked(SharedPtr<Task> other) -> AddSubsequentResult {
		switch (other->state.load(std::memory_order_relaxed)) {
		case TaskState::EXECUTING: return AddSubsequentResult::SUBSEQUENT_ALREADY_EXECUTING;
		case TaskState::COMPLETE: return AddSubsequentResult::SUBSEQUENT_ALREADY_COMPLETED;
		default:
		}

		if (has_completed()) {
			return AddSubsequentResult::PREREQUISITE_ALREADY_COMPLETED;
		}

		ASSERTF(!other->subsequents.for_each_with_break([&](const SharedPtr<Task>& subsequent) {
			return subsequent.get() != this;
		}), "Attempted to enqueue subsequent task {} to {} that also has that task as a subsequent! Circular subsequent dependencies!", other->name, name);

		++other->prerequisites_remaining;
		subsequents.enqueue(std::move(other));

		return AddSubsequentResult::SUCCESS;
	}

	auto add_subsequent(SharedPtr<Task> other) -> AddSubsequentResult {
		ASSERT(other);

		// Acquire all locks. Ensure no deadlocking.
		// @TODO: Should make an equivelent to std::scoped_lock that accepts lock_shared so I don't have to do this.
		UniqueSharedLock<SharedMutex> l1{null}, l2{null}, l3{null};

		u32 retries = 0;
		while (true) {
			ASSERTF(retries < 1024, "Reached maximum number of retries!");

			l1 = UniqueSharedLock{subsequents_mutex};

			if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(other->subsequents_mutex)) {
				l2 = std::move(*result);
			} else {
				l1.unlock();

				thread::exponential_yield(retries);
				continue;
			}

			if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(other->exclusives.get_mutex())) {
				l3 = std::move(*result);
			} else {
				l1.unlock();
				l2.unlock();

				thread::exponential_yield(retries);
				continue;
			}

			break;
		}

		switch (other->state.load(std::memory_order_relaxed)) {
		case TaskState::EXECUTING: return AddSubsequentResult::SUBSEQUENT_ALREADY_EXECUTING;
		case TaskState::COMPLETE: return AddSubsequentResult::SUBSEQUENT_ALREADY_COMPLETED;
		default:
		}

		if (has_completed()) {
			return AddSubsequentResult::PREREQUISITE_ALREADY_COMPLETED;
		}

		ASSERTF(!subsequents.for_each_with_break([&](const SharedPtr<Task>& subsequent) {
			return subsequent != other;
		}), "Attempted to enqueue a subsequent {} for {} that is already a subsequent of this task!", other->name, name);

		ASSERTF(!other->subsequents.for_each_with_break([&](const SharedPtr<Task>& subsequent) {
			return subsequent.get() != this;
		}), "Attempted to enqueue subsequent task {} for {} that also has that task as a subsequent! Circular subsequent dependencies!", other->name, name);

		++other->prerequisites_remaining;
		subsequents.enqueue(std::move(other));

		return AddSubsequentResult::SUCCESS;
	}

	enum class TryAddSubsequentElsePrerequisiteResult : u8 {
		ADDED_SUBSEQUENT,
		ADDED_PREREQUISITE,
		FAIL,
	};

	static auto try_add_subsequent_else_add_prerequisite(const SharedPtr<Task>& try_prerequisite, const SharedPtr<Task>& try_subsequent) -> TryAddSubsequentElsePrerequisiteResult {
		// Acquire all locks. Ensure no deadlocking.
		// @TODO: Should make an equivelent to std::scoped_lock that accepts lock_shared so I don't have to do this.
		UniqueSharedLock<SharedMutex> l1{null}, l2{null}, l3{null}, l4{null};

		u32 retries = 0;
		while (true) {
			ASSERTF(retries < 4096, "Reached maximum number of retries!");

			l1 = UniqueSharedLock{try_prerequisite->subsequents_mutex};

			if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(try_prerequisite->exclusives.get_mutex())) {
				l2 = std::move(*result);
			} else {
				l1.unlock();

				thread::exponential_yield(retries);
				continue;
			}

			if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(try_subsequent->subsequents_mutex)) {
				l3 = std::move(*result);
			} else {
				l1.unlock();
				l2.unlock();

				thread::exponential_yield(retries);
				continue;
			}

			if (auto result = UniqueSharedLock<SharedMutex>::from_try_lock(try_subsequent->exclusives.get_mutex())) {
				l4 = std::move(*result);
			} else {
				l1.unlock();
				l2.unlock();
				l3.unlock();

				thread::exponential_yield(retries);
				continue;
			}

			break;
		}

		const auto subsequent_result = try_prerequisite->add_subsequent_assumes_locked(try_subsequent);
		if (subsequent_result != AddSubsequentResult::SUCCESS) {
			const auto prerequisite_result = try_subsequent->add_subsequent_assumes_locked(try_prerequisite);
			if (prerequisite_result == AddSubsequentResult::SUCCESS) {
				return TryAddSubsequentElsePrerequisiteResult::ADDED_PREREQUISITE;
			} else {
				return TryAddSubsequentElsePrerequisiteResult::FAIL;
			}
		} else {
			return TryAddSubsequentElsePrerequisiteResult::ADDED_SUBSEQUENT;
		}
	}

	static auto add_exclusive(const SharedPtr<Task>& a, const SharedPtr<Task>& b) -> void {
		ASSERT(a);
		ASSERT(b);
		#if 0
		ASSERT(!a->contains_exclusive(*b));
		ASSERT(!b->contains_exclusive(*a));
		#endif

		if (a->contains_exclusive(*b)) {// @TODO: Suboptimal. Extra work.
			return;
		}

		auto mutex = std::make_shared<Mutex>();

		a->exclusives.lock_shared()->enqueue(Exclusive{
			.other = b,
			.mutex = mutex,
		});

		b->exclusives.lock_shared()->enqueue(Exclusive{
			.other = a,
			.mutex = std::move(mutex),
		});
	}

	[[nodiscard]] auto contains_exclusive(const Task& other) const -> bool {
		return const_cast<Task*>(this)->exclusives.lock_exclusive()->for_each_with_break([&](const Exclusive& exclusive) {
			return exclusive.other.lock().get() == &other;
		});
	}

	[[nodiscard]] static constexpr auto default_name_from_source_location(const std::source_location& source_location = std::source_location::current()) -> String {
		return fmt::format("{}:{}:{}", utils::to_compact_file_name(source_location.file_name()), source_location.line(), source_location.column());
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
	String name;
	bool enqueued = false;
#endif
};

namespace task {
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
			u32 try_dequeue_retry_count = 0;
			while (!(task = try_dequeue_task<IS_IN_MAIN_THREAD>())) [[unlikely]] {
				// Spin for a bit before going to sleep.
				static constexpr u32 MAX_RETRY_COUNT = 64;// Arbitrary.
				if (try_dequeue_retry_count++ < MAX_RETRY_COUNT) {
					std::this_thread::yield();
					continue;
				}

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

#if 1
			// @NOTE: Race-condition here. While a this task is enqueued, another task can make this task a subsequent of it and complete execution before this task is dequeued,
			// thus enqueueing it twice. This happens very rarely so it's okay to just have the two clones of the same task race to begin executing but just use atomic exchange
			// for starting execution.
			if (task->state.load(std::memory_order_relaxed) >= TaskState::EXECUTING) {
				continue;
			}

			auto exclusives_access = task->exclusives.lock_exclusive();

			// Prerequisites were added after enqueueing. Do not execute, will be enqueued at a later time.
			if (task->has_prerequisites()) [[unlikely]] {
#if ASSERTIONS_ENABLED
				task->enqueued = false;
#endif
				continue;
			}
			#endif

			// Skip exclusives logic if there are no exclusives.
			if (exclusives_access->is_empty()) {
				#if TMP_ALLOW_DOUBLE_ENQUEUE
				task->state = TaskState::EXECUTING;
				#else
				TaskState expected = TaskState::STANDBY;
				if (!task->state.compare_exchange_strong(expected, TaskState::EXECUTING, std::memory_order_seq_cst, std::memory_order_relaxed)) {
					continue;
				}
				#endif

				goto EXECUTE;
			}

			Array<Task::Exclusive> exclusives;
			exclusives_access->for_each([&](const Task::Exclusive& exclusive) {
				if (auto exclusive_task = exclusive.other.lock()) {
					exclusives.push_back(exclusive);
				}
			});

			//exclusives_access.unlock();

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
				// @TODO: Should fallback to busy waiting after a duration.
#if 01
				static constexpr u32 MAX_RETRIES = 512;
				if (num_retries++ < MAX_RETRIES) {
					std::this_thread::yield();
				} else {
					const std::chrono::microseconds delay{1 << std::min(num_retries - MAX_RETRIES, 4u)};
					std::this_thread::sleep_for(delay);
				}
#else
				std::this_thread::yield();
#endif
			}

			u32 num_exclusives_executing = 0;
			for (const Task::Exclusive& exclusive : exclusives) {
				const auto exclusive_task = exclusive.other.lock();
				if (exclusive_task && exclusive_task->state.load(std::memory_order_relaxed) == TaskState::EXECUTING) {
					exclusive_task->subsequents.enqueue(task);// @NOTE: Could potentially decrease this task's priority.
					++num_exclusives_executing;
				}
			}

			if (num_exclusives_executing == 0) {
				#if TMP_ALLOW_DOUBLE_ENQUEUE
				task->state = TaskState::EXECUTING;
				#else
				TaskState expected = TaskState::STANDBY;
				if (!task->state.compare_exchange_strong(expected, TaskState::EXECUTING, std::memory_order_seq_cst, std::memory_order_relaxed)) {
					continue;
				}
				#endif

				goto EXECUTE;
			} else {
				task->prerequisites_remaining += num_exclusives_executing;
#if ASSERTIONS_ENABLED
				task->enqueued = false;
#endif
			}
		}
	EXECUTE:
		ASSERT(task->state.load(std::memory_order_relaxed) == TaskState::EXECUTING);

		// Execute the task.
		task->fn(task);

		// Dequeued the final task. Wake up all other threads and exit.
		// @TODO: Change this to num_threads_active to reduce contention and bug-avoidance.
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
	//ASSERT(!std::ranges::contains(exclusives, task));

#if ASSERTIONS_ENABLED
	task->enqueued = true;
#endif

	// Add exclusives.
	if (!exclusives.empty()) {
		for (const auto& exclusive : exclusives) {
			ASSERT(exclusive);

			struct Accessor final : public WeakPtr<Task> {
				[[nodiscard]] static constexpr auto get_data(const WeakPtr<Task>& value) -> Task* {
					return static_cast<const Accessor&>(value).get();
				}
			};

			// Ensure the task isn't already enqueued as an exclusive.
			if (exclusive != task && !task->exclusives.get_unsafe().for_each_with_break([&](const Task::Exclusive& exclusive_info) {
				return Accessor::get_data(exclusive_info.other) != exclusive.get();
			})) {
				auto mutex = std::make_shared<Mutex>();

				task->exclusives.get_unsafe().enqueue(Task::Exclusive{
					.other = exclusive,
					.mutex = mutex,
				});

				exclusive->exclusives.lock_shared()->enqueue(Task::Exclusive{
					.other = task,
					.mutex = std::move(mutex),
				});
			}
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

	num_prerequisites = task->prerequisites_remaining += num_prerequisites;

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

	if (num_prerequisites == 0) {
		queues[static_cast<u8>(thread)][static_cast<u8>(priority)]->enqueue(std::move(task));

		if (should_wake_up_thread) {
			wake_up_threads(thread);
		}
	}
}

inline auto enqueue(Fn<void(const SharedPtr<Task>&)> fn, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
	const Span<const SharedPtr<Task>> prerequisites = {}, const Span<const SharedPtr<Task>> exclusives = {}, const bool should_wake_up_thread = true
#if ASSERTIONS_ENABLED
	, String task_name = Task::default_name_from_source_location(std::source_location::current())
#endif
	) -> SharedPtr<Task>
{
	auto task = Task::make(std::move(fn), priority, thread, {}
#if ASSERTIONS_ENABLED
		, std::move(task_name)
#endif
	);

	enqueue(task, priority, thread, prerequisites, exclusives, should_wake_up_thread);

	return task;
}

FORCEINLINE auto enqueue(cpts::Invokable auto&& fn, const Priority priority = Priority::NORMAL, const Thread thread = Thread::ANY,
	const Span<const SharedPtr<Task>> prerequisites = {}, const Span<const SharedPtr<Task>> exclusives = {}, const bool should_wake_up_thread = true
#if ASSERTIONS_ENABLED
	, String task_name = Task::default_name_from_source_location(std::source_location::current())
#endif
	) -> SharedPtr<Task>
{
	return enqueue([fn = FORWARD_AUTO(fn)](const SharedPtr<Task>&) { std::invoke(fn); }, priority, thread, prerequisites, exclusives, should_wake_up_thread
#if ASSERTIONS_ENABLED
		, std::move(task_name)
#endif
	);
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
inline auto parallel_for_unbalanced(const usize count, cpts::Invokable<usize> auto&& fn, const std::source_location& source_location = std::source_location::current()) -> void {
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
		}, Priority::HIGH, Thread::ANY, {}, {}, false
#if ASSERTIONS_ENABLED
			, fmt::format("\"{}: task::parallel_for[{}]\"", utils::to_compact_file_name(source_location.file_name()), source_location.line(), source_location.column(), i)
#endif
		);
	}

	wake_up_threads(Thread::ANY, count);

	do_work([&] { return !num_tasks_remaining.load(std::memory_order_relaxed); });
#endif
}

// Creates a task per-thread for minimal task overhead.
inline auto parallel_for_balanced(const usize count, cpts::Invokable<usize> auto&& fn, const usize num_tasks = get_num_threads(), const std::source_location& source_location = std::source_location::current()) -> void {
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
	}, source_location);
}

template <bool BALANCED = false>
FORCEINLINE auto parallel_for(const usize count, cpts::Invokable<usize> auto&& fn, const std::source_location& source_location = std::source_location::current()) -> void {
	if constexpr (BALANCED) {
		parallel_for_balanced(count, FORWARD_AUTO(fn), source_location);
	} else {
		parallel_for_unbalanced(count, FORWARD_AUTO(fn), source_location);
	}
}
}