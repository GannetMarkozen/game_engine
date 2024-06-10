#pragma once

#include "../defines.hpp"
#include "../assert.hpp"
#include "thread_safe_types.hpp"
#include "thread_affinity.hpp"

#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <condition_variable>

#if 01

namespace core {
namespace task {
namespace Priority { enum Type : u8 { HIGH, NORMAL, LOW}; }
namespace Thread { enum Type : u8 { MAIN, ANY }; }
namespace ThreadPriority { enum Type : u8 { FOREGROUND, BACKGROUND }; }
static constexpr usize PRIORITY_COUNT = 3;
static constexpr usize THREAD_COUNT = 2;
static constexpr usize THREAD_PRIORITY_COUNT = 2;
static constexpr usize NAMED_THREAD_COUNT = THREAD_COUNT - 1;
}

// This enum should match up with task::Thread. It's just semantically different.
namespace ThreadType { enum Type : u8 { MAIN = task::Thread::MAIN, WORKER = task::Thread::ANY }; }

namespace impl {
inline thread_local ThreadType::Type thread_type;
}

FORCEINLINE fn get_thread_type() -> ThreadType::Type { return impl::thread_type; }
FORCEINLINE fn is_in_main_thread() -> bool { return get_thread_type() == ThreadType::MAIN; }

struct alignas(CACHE_LINE_SIZE) Task {
	NON_COPYABLE(Task);
	friend struct TaskGraph;

	[[nodiscard]]
	FORCEINLINE explicit Task(Fn<void(const SharedPtr<Task>&)> in_func, const task::Priority::Type in_priority, const task::Thread::Type in_thread, const task::ThreadPriority::Type in_thread_priority)
		: func{std::move(in_func)}, completed{false}, priority{in_priority}, thread{in_thread}, thread_priority{in_thread_priority} {}

	[[nodiscard]]
	FORCEINLINE fn has_completed() const -> bool { return completed; }

	[[nodiscard]]
	FORCEINLINE fn has_prerequisites() const -> bool { return num_prerequisites_remaining.load(std::memory_order_relaxed); }

private:
	const Fn<void(const SharedPtr<Task>&)> func;
	Array<SharedPtr<Task>> subsequents;
	std::atomic<u32> num_prerequisites_remaining = 0;
	SpinLock subsequents_mutex;
	u8 completed: 1;
	const task::Priority::Type priority: 2;
	const task::Thread::Type thread: 2;
	const task::ThreadPriority::Type thread_priority: 2;
};

struct alignas(CACHE_LINE_SIZE) TaskGraph {
private:
	struct alignas(CACHE_LINE_SIZE) ThreadCondition {
		std::condition_variable_any condition;
		Mutex condition_mutex;
		bool is_asleep = false;
	};

	struct WorkerThread {
		std::thread thread;
		SharedPtr<ThreadCondition> thread_condition;
	};

public:
	[[nodiscard]]
	inline static fn get() -> TaskGraph& {
		static TaskGraph singleton{};
		return singleton;
	}

	fn initialize(const usize num_workers) -> void {
		impl::thread_type = ThreadType::MAIN;

		for (auto& thread_condition : named_thread_conditions) {
			thread_condition = std::make_shared<ThreadCondition>();
		}

		worker_threads.write([&](auto& worker_threads) {
			worker_threads.reserve(num_workers);
			for (usize i = 0; i < num_workers; ++i) {
				const auto thread_condition = std::make_shared<ThreadCondition>();
				worker_threads.push_back(WorkerThread{
					.thread{[this, thread_condition] {
						impl::thread_type = ThreadType::WORKER;
						internal_do_work<ThreadType::WORKER>(thread_condition, [&] { return false; });
					}},
					.thread_condition = thread_condition,
				});

				// Worse performance in artificial benchmarks (presumably due to hyper-threading). Will have to test
				// this out again when cache-coherency and context-switching becomes an actual bottleneck.
				// utils::set_thread_affinity(worker_threads.back().thread, i);
			}
		});
	}

	fn deinitialize() -> void {
		shutting_down = true;
		wakeup_all_threads();

		worker_threads.write([&](auto& worker_threads) {
			for (auto& worker_thread : worker_threads) {
				worker_thread.thread.join();
			}

			worker_threads.clear();
		});
	}

	template<ThreadType::Type THREAD>
	fn do_work(InvokableReturns<bool> auto&& should_exit) -> void {
		const SharedPtr<ThreadCondition> thread_condition = [&] {
			if constexpr (THREAD == ThreadType::WORKER) {
				return worker_threads.read([&](const auto& worker_threads) {
					return std::find_if(worker_threads.begin(), worker_threads.end(), [](const WorkerThread& worker_thread) { return worker_thread.thread.get_id() == std::this_thread::get_id(); })->thread_condition;
				});
			} else {
				return named_thread_conditions[THREAD];
			}
		}();
		internal_do_work<THREAD>(thread_condition, FORWARD_AUTO(should_exit));
	}

	fn do_work(InvokableReturns<bool> auto&& should_exit) -> void {
		switch (get_thread_type()) {
		case ThreadType::MAIN: do_work<ThreadType::MAIN>(FORWARD_AUTO(should_exit)); break;
		case ThreadType::WORKER: do_work<ThreadType::WORKER>(FORWARD_AUTO(should_exit)); break;
		}
	}

	fn enqueue(Fn<void(const SharedPtr<Task>&)> func, const task::Priority::Type priority = task::Priority::NORMAL, const task::Thread::Type thread = task::Thread::ANY,
		const task::ThreadPriority::Type thread_priority = task::ThreadPriority::FOREGROUND, const Span<const SharedPtr<Task>>& prerequisites = {}, const bool wake_up_thread = true)
		-> SharedPtr<Task> {
		const auto task = std::make_shared<Task>(std::move(func), priority, thread, thread_priority);

		u32 num_prerequisites = 0;
		for (const auto& prerequisite : prerequisites) {
			prerequisite->subsequents_mutex.lock();// This lock must be maintained until num_prerequisites_remaining has been assigned.

			// Prerequisite has already completed so it doesn't count.
			if (prerequisite->has_completed()) {
				continue;
			}

			prerequisite->subsequents.push_back(task);
			++num_prerequisites;
		}

		task->num_prerequisites_remaining = num_prerequisites;

		for (const auto& prerequisite : prerequisites) {
			prerequisite->subsequents_mutex.unlock();
		}

		// Increment num_tasks_in_flight regardless of if this task is immediately enqueued or not.
		++num_tasks_in_flight;

		if (num_prerequisites == 0) {
			internal_enqueue_no_prerequisites(task, wake_up_thread);
		}

		return task;
	}

	FORCEINLINE fn enqueue(Invokable auto&& func, const task::Priority::Type priority = task::Priority::NORMAL, const task::Thread::Type thread = task::Thread::ANY,
		const task::ThreadPriority::Type thread_priority = task::ThreadPriority::FOREGROUND, const Span<const SharedPtr<Task>>& prerequisites = {}, const bool wake_up_thread = true)
		-> SharedPtr<Task> {
		return enqueue([func = FORWARD_AUTO(func)](const SharedPtr<Task>&) { func(); }, priority, thread, thread_priority, prerequisites, wake_up_thread);
	}

	fn busy_wait_for_tasks_to_complete(const Span<const SharedPtr<Task>>& tasks) -> void {
		volatile bool tasks_completed = false;

		enqueue([&, this_thread_type = get_thread_type(), this_thread_id = std::this_thread::get_id()] {
			tasks_completed = true;
			try_wakeup_specific_thread(this_thread_type, this_thread_id);
		}, task::Priority::HIGH, task::Thread::ANY, task::ThreadPriority::FOREGROUND, tasks);

		do_work([&] { return tasks_completed; });
	}

	template<std::integral Int>
	fn parallel_for(const Int count, Invokable<Int> auto&& func) -> void {
		if (count <= 1) {
			for (Int i = 0; i < count; ++i) {
				std::invoke(func, i);
			}
			return;
		}

		// @TODO: Implement balanced load optimizations (if necessary).
		Array<SharedPtr<Task>> tasks;
		tasks.reserve(static_cast<usize>(count));
		for (Int i = 0; i < count; ++i) {
			tasks.push_back(enqueue(std::bind(func, i), task::Priority::HIGH, task::Thread::ANY, task::ThreadPriority::FOREGROUND, {}, false));
		}

		volatile bool tasks_completed = false;

		enqueue([this, &tasks_completed, this_thread_type = get_thread_type(), this_thread = std::this_thread::get_id()] {
			tasks_completed = true;
			try_wakeup_specific_thread(this_thread_type, this_thread);
		}, task::Priority::HIGH, task::Thread::ANY, task::ThreadPriority::FOREGROUND, tasks, false);

		// Try to wake up as many threads as there are tasks (-1 since this thread will also do work).
		try_wakeup_threads<task::Thread::ANY>(static_cast<usize>(count - 1));

		do_work([&] { return tasks_completed; });
	}

private:
	fn internal_enqueue_no_prerequisites(SharedPtr<Task> task, const bool wake_up_thread) -> void {
		const task::Thread::Type thread = task->thread;

		tasks_queues[task->thread][task->priority].enqueue(std::move(task));

		if (wake_up_thread) {
			try_wakeup_thread(thread);
		}
	}

	// @TODO: Implement thread priorities and thread affinity.
	template<ThreadType::Type THREAD>
	NOINLINE fn internal_do_work(const SharedPtr<ThreadCondition>& thread_condition, InvokableReturns<bool> auto&& should_exit) -> void {
		while (!std::invoke(should_exit)) [[likely]] {
			SharedPtr<Task> task = nullptr;

			// Try dequeue any available task else sleep loop.
			while (!(task = try_dequeue_task<THREAD>())) [[unlikely]] {
				UniqueLock lock{thread_condition->condition_mutex};

				if ((task = try_dequeue_task<THREAD>())) [[likely]] {
					break;
				}

				if (std::invoke(should_exit) || (shutting_down && num_tasks_in_flight.load(std::memory_order_relaxed) == 0)) [[unlikely]] {
					return;
				}

				// @TODO: Implement worker-thread priority.
				thread_condition->is_asleep = true;
				sleeping_threads_queues[THREAD].enqueue(thread_condition);

				thread_condition->condition.wait(lock, [&] { return !thread_condition->is_asleep; });
			}

			task->func(task);

			const auto current_num_tasks_in_flight = --num_tasks_in_flight;
			if (shutting_down && current_num_tasks_in_flight == 0) [[unlikely]] {
				// Wake-up all threads so they can exit as well.
				wakeup_all_threads();
				return;
			}

			Array<SharedPtr<Task>> subsequents = [&] {
				ScopeLock lock{task->subsequents_mutex};

				task->completed = true;

				return std::move(task->subsequents);
			}();

			for (auto& subsequent : subsequents) {
				if (--subsequent->num_prerequisites_remaining == 0) {
					internal_enqueue_no_prerequisites(std::move(subsequent), true);
				}
			}
		}
	}

	// Returned task may be NULL.
	template<ThreadType::Type THREAD>
	fn try_dequeue_task() -> SharedPtr<Task> {
		SharedPtr<Task> task = nullptr;

		if constexpr (THREAD != ThreadType::WORKER) {// Named thread.
			for (auto& queue : tasks_queues[THREAD]) {
				if (queue.try_dequeue(task)) {
					return task;
				}
			}
		}

		// Attempt to dequeue an any-thread task.
		for (auto& queue : tasks_queues[task::Thread::ANY]) {
			if (queue.try_dequeue(task)) {
				return task;
			}
		}

		return task;
	}

	template<task::Thread::Type THREAD>
	fn try_wakeup_thread() -> bool {
		const auto try_wakeup = [&](const SharedPtr<ThreadCondition>& thread_condition) -> bool {
			// There can only be contention over this mutex if a thread is about to go to sleep
			// and we try to wake up that thread before it's asleep so it's a tiny window.
			ScopeLock lock{thread_condition->condition_mutex};

			// This can return false if the thread is still awake. This will only really happen if you wake up
			// a specific thread not using the queue so we need to try and dequeue again.
			if (!thread_condition->is_asleep) [[unlikely]] {
				return false;
			}

			// Must set this flag to allow the thread to be woken up.
			thread_condition->is_asleep = false;

			// Only one thread will wait on this.
			thread_condition->condition.notify_one();

			return true;
		};

		SharedPtr<ThreadCondition> thread_condition = nullptr;

		if constexpr (THREAD == task::Thread::ANY) {// Try wake-up named-thread only.
			for (auto& sleeping_thread_queue : sleeping_threads_queues) {
				if (sleeping_thread_queue.try_dequeue(thread_condition) && try_wakeup(thread_condition)) {
					return true;
				}
			}
		} else {// Try wake-up named-thread then worker thread.
			if (sleeping_threads_queues[THREAD].try_dequeue(thread_condition) && try_wakeup(thread_condition)) {
				return true;
			}
		}

		return false;
	}

	template<task::Thread::Type THREAD>
	fn try_wakeup_threads(const usize num_threads) -> usize {
		ASSERT(num_threads > 0);

		const auto try_wakeup = [&](ConcurrentQueue<SharedPtr<ThreadCondition>>& queue) -> bool {
			SharedPtr<ThreadCondition> thread_condition = nullptr;

			while (true) {
				if (!queue.try_dequeue(thread_condition)) {
					return false;
				}

				// There can only be contention over this mutex if a thread is about to go to sleep
				// and we try to wake up that thread before it's asleep so it's a tiny window.
				ScopeLock lock{thread_condition->condition_mutex};

				// Try again. This thread is already awake. This can happen if a specific thread was woken up so it
				// was not dequeued.
				if (!thread_condition->is_asleep) [[unlikely]] {
					continue;
				}

				// Must set this flag to allow the thread to be woken up.
				thread_condition->is_asleep = false;

				// Only one thread will wait on this.
				thread_condition->condition.notify_one();

				return true;
			}
		};

		usize num_awoken = 0;

		if constexpr (THREAD == task::Thread::ANY) {// Try wake-up named-thread then worker thread.
			for (auto& sleeping_thread_queue : sleeping_threads_queues) {
				while (try_wakeup(sleeping_thread_queue) && ++num_awoken < num_threads) {}
			}
		} else {// Try wake-up named-thread only.
			while (try_wakeup(sleeping_threads_queues[THREAD]) && ++num_awoken < num_threads) {}
		}

		return num_awoken;
	}

	FORCEINLINE fn wakeup_all_threads() -> bool {
		return try_wakeup_threads<task::Thread::ANY>(static_cast<usize>(UINT64_MAX)) > 0;
	}

	fn try_wakeup_thread(const task::Thread::Type thread) -> bool {
		switch (thread) {
		case task::Thread::MAIN: return try_wakeup_thread<task::Thread::MAIN>();
		case task::Thread::ANY: return try_wakeup_thread<task::Thread::ANY>();
		}
	}

	fn try_wakeup_specific_thread(const ThreadType::Type thread, const std::thread::id thread_id) -> bool {
		const SharedPtr<ThreadCondition> thread_condition = [&] {
			if (thread == ThreadType::WORKER) {
				return worker_threads.read([&](const auto& worker_threads) {
					return std::find_if(worker_threads.begin(), worker_threads.end(), [&](const auto& worker_thread) { return worker_thread.thread.get_id() == thread_id; })->thread_condition;
				});
			} else {
				return named_thread_conditions[thread];
			}
		}();

		// Could potentially use a shared lock here in-case multiple threads attempt to wake up this thread would lead to stalling.
		ScopeLock lock{thread_condition->condition_mutex};

		// Already awake.
		if (!thread_condition->is_asleep) {
			return false;
		}

		// Must set this flag otherwise the thread won't wake up.
		thread_condition->is_asleep = false;

		// Wake-up.
		thread_condition->condition.notify_one();

		return true;
	}

	ConcurrentQueue<SharedPtr<Task>> tasks_queues[task::THREAD_COUNT][task::PRIORITY_COUNT];
	ConcurrentQueue<SharedPtr<ThreadCondition>> sleeping_threads_queues[task::THREAD_COUNT];// @NOTE: There's only ever possibly 1 in each named-thread queue so could optimize that.
	SharedPtr<ThreadCondition> named_thread_conditions[task::NAMED_THREAD_COUNT];
	RWLocked<Array<WorkerThread>> worker_threads;
	std::atomic<u32> num_tasks_in_flight = 0;
	volatile bool shutting_down = false;
};
}

#elif 01

namespace core {
enum class ThreadType : u8 { MAIN, WORKER };
static constexpr usize THREAD_TYPE_COUNT = 2;
static constexpr usize NAMED_THREAD_COUNT = THREAD_TYPE_COUNT - 1;// Main.

namespace task {
enum class Priority : u8 { HIGH, NORMAL, LOW };
enum class Thread : u8 { MAIN, ANY };// Maybe add a render thread for pinning tasks to rendering etc?
static constexpr usize PRIORITY_COUNT = 3;
static constexpr usize THREAD_COUNT = 2;
}

namespace impl {
thread_local inline ThreadType thread_type;
}

[[nodiscard]] FORCEINLINE fn get_thread_type() { return impl::thread_type; }
[[nodiscard]] FORCEINLINE fn is_in_main_thread() { return get_thread_type() == ThreadType::MAIN; }
[[nodiscard]] FORCEINLINE fn is_in_worker_thread() { return get_thread_type() == ThreadType::WORKER; }

// Thread-safe arbitrary task that can be enqueued from any thread.
struct alignas(CACHE_LINE_SIZE) Task {
	friend struct TaskGraph;

	[[nodiscard]] Task() = default;
	[[nodiscard]] Task(Fn<void(const SharedPtr<Task>&)> in_functor, const task::Priority in_priority, const task::Thread in_thread)
		: functor{std::move(in_functor)}, priority{in_priority}, thread{in_thread} {}

	NON_COPYABLE(Task);

	[[nodiscard]]
	FORCEINLINE fn has_completed() const -> bool { return completed; }

	[[nodiscard]]
	FORCEINLINE fn has_prerequisites() const -> bool { return prerequisites_remaining.load() != 0; }

private:
	Fn<void(const SharedPtr<Task>&)> functor;
	Array<SharedPtr<Task>> subsequent_tasks;
	std::atomic<u32> prerequisites_remaining = 0;
	SpinLock add_subsequents_mutex;
	task::Priority priority = task::Priority::NORMAL;
	task::Thread thread = task::Thread::ANY;
	volatile bool completed = false;
};

#if 0


struct alignas(CACHE_LINE_SIZE) TaskGraph {
private:
	struct ThreadCondition {
		std::condition_variable_any condition;
		SharedMutex mutex;
		std::atomic<bool> is_asleep = false;
		bool is_stale = false;// The thread has been destroyed already.
	};

	struct WorkerThread {
		std::thread thread;
		SharedPtr<ThreadCondition> condition;
	};

	template<ThreadType THREAD>
	fn try_dequeue_task() -> SharedPtr<Task> {
		SharedPtr<Task> task = nullptr;
		if constexpr (THREAD != ThreadType::WORKER) {
			for (usize i = 0; i < task::PRIORITY_COUNT; ++i) {
				if (enqueued_tasks[static_cast<usize>(THREAD)][i].try_dequeue(task)) {
					return task;
				}
			}
		}

		for (usize i = 0; i < task::PRIORITY_COUNT; ++i) {
			if (enqueued_tasks[static_cast<usize>(task::Thread::ANY)][i].try_dequeue(task)) {
				return task;
			}
		}

		return task;
	}

public:
	[[nodiscard]]
	FORCEINLINE static fn get() -> TaskGraph&;

	fn initialize(const usize num_workers) -> void {
		ASSERT(num_workers > 0);
		worker_threads.write([&](Array<WorkerThread>& worker_threads) {
			worker_threads.reserve(num_workers);
			for (usize i = 0; i < num_workers; ++i) {
				worker_threads.push_back(WorkerThread{
					.thread{[this] {
						do_work<ThreadType::WORKER>([&] { return false; });
					}},
					.condition = std::make_shared<ThreadCondition>(),
				});
			}
		});
	}

	template<ThreadType THREAD>
	fn do_work(InvokableReturns<bool> auto&& should_exit) -> void {
		auto& thread_condition = [&]() -> ThreadCondition& {
			if constexpr (THREAD != ThreadType::WORKER) {
				return named_thread_conditions[static_cast<usize>(THREAD)];
			} else {
				// Unsafe but should be okay in this case (since ThreadCondition should have the same lifespan as the thread it's being used in).
				return worker_threads.read([](const Array<WorkerThread>& worker_threads) -> ThreadCondition& {
					return *std::find_if(worker_threads.begin(), worker_threads.end(), [&](const WorkerThread& thread) { return thread.thread.get_id() == std::this_thread::get_id(); })->condition;
				});
			}
		}();
		do_work_internal<THREAD>(thread_condition, FORWARD_AUTO(should_exit));
	}

	template<ThreadType THREAD>
	fn do_work_internal(ThreadCondition& thread_condition, InvokableReturns<bool> auto&& should_exit) -> void {
		static constexpr auto THREAD_INDEX = static_cast<usize>(THREAD);

		while (!std::invoke(should_exit)) {
			SharedPtr<Task> task = nullptr;

			// Try find a task or sleep loop.
			while (!(task = try_dequeue_task<THREAD>())) [[unlikely]] {
				if (std::invoke(should_exit) || (should_terminate && num_tasks_in_flight.load(std::memory_order_relaxed) == 0)) [[unlikely]] {
					return;
				}

				// .wait unlocks this mutex after the thread has been put to sleep.
				UniqueLock lock{thread_condition.mutex};

				// Try dequeueing one last time during the lock in-case a crazy race-condition occurred where a task
				// was enqueued in-between the previous dequeue and the lock so this thread won't be woken up. Performance
				// doesn't matter here since this thread is going to go to sleep anyways otherwise.
				if ((task = try_dequeue_task<THREAD>())) break;

				thread_condition.is_asleep = true;

				// We don't care about spurious wake-ups in this case since nothing bad will happen.
				thread_condition.condition.wait(lock);
			}

			task->functor(task);

			// Decrement number of tasks in flight after the task since the task can enqueue tasks and don't want this to prematurely hit 0.
			if (--num_tasks_in_flight == 0 && should_terminate) [[unlikely]] {
				// Wake up all threads.
				return;
			}

			Array<SharedPtr<Task>> subsequents = [&] {
				ScopeLock lock{task->add_subsequents_mutex};

				task->completed = true;

				return std::move(task->subsequent_tasks);
			}();

			for (auto& subsequent : subsequents) {
				if (--subsequent->prerequisites_remaining == 0) {
					enqueue_no_prerequisites(std::move(subsequent));
				}
			}
		}
	}

	fn enqueue(Fn<void(const SharedPtr<Task>&)> functor, const task::Priority priority = task::Priority::NORMAL, const task::Thread thread = task::Thread::ANY, const Span<const SharedPtr<Task>>& prerequisites = {}) -> SharedPtr<Task> {
		const auto task = std::make_shared<Task>(priority, thread);

		u32 num_prerequisites = 0;
		if (prerequisites.size() > 0) {
			for (auto& prerequisite : prerequisites) {
				prerequisite->add_subsequents_mutex.lock();

				if (!prerequisite->has_completed()) {
					++num_prerequisites;
				}
			}

			task->prerequisites_remaining = num_prerequisites;

			for (auto& prerequisite : prerequisites) {
				prerequisite->add_subsequents_mutex.unlock();
			}
		}

		if (num_prerequisites > 0) {
			enqueue_no_prerequisites(task);
		}

		return task;
	}

	fn enqueue_no_prerequisites(SharedPtr<Task> task) -> void {

	}


private:
	ConcurrentQueue<SharedPtr<Task>> enqueued_tasks[task::THREAD_COUNT][task::PRIORITY_COUNT];
	ThreadCondition named_thread_conditions[NAMED_THREAD_COUNT];
	RWLocked<Array<WorkerThread>> worker_threads;
	std::atomic<u32> num_tasks_in_flight = 0;
	volatile bool should_terminate = false;
};

#else

struct alignas(CACHE_LINE_SIZE) TaskGraph {
	[[nodiscard]] FORCEINLINE static fn get() -> TaskGraph&;

	// Must be called from the main thread.
	fn initialize(const usize num_workers) -> void {
		ASSERT(num_workers > 0);

		impl::thread_type = ThreadType::MAIN;

		worker_threads.reserve(num_workers);
		for (usize i = 0; i < num_workers; ++i) {
			worker_threads.push_back(std::thread{[this] {
				impl::thread_type = ThreadType::WORKER;
				do_work<ThreadType::WORKER>([&] { return false; });
			}});
		}
	}

	// Must be called outside of the do_work loop on the main thread.
	fn deinitialize() -> void {
		ASSERT(is_in_main_thread());

		should_terminate = true;

		// Wake up all threads so that they can exit their loops.
		wake_up_all_threads_of_type(ThreadType::WORKER);
		for (auto& worker : worker_threads) {
			worker.join();
		}
		worker_threads.clear();
	}

	template<ThreadType THREAD>
	NOINLINE fn do_work(InvokableReturns<bool> auto&& request_exit) -> void {
		ASSERTF(THREAD == get_thread_type(), "Thread type == {} for thread {}!",
			static_cast<usize>(THREAD), std::hash<std::thread::id>{}(std::this_thread::get_id()));

		static constexpr auto THREAD_INDEX = static_cast<usize>(THREAD);

		while (!std::invoke(FORWARD_AUTO(request_exit))) [[likely]] {
			// Try dequeue a task.
			SharedPtr<Task> task = nullptr;
			while (!(task = try_dequeue_task<THREAD>())) [[unlikely]] {
				UniqueLock lock{thread_condition_mutexes[THREAD_INDEX]};

				if ((task = try_dequeue_task<THREAD>())) [[likely]] {
					break;
				}

				if (std::invoke(FORWARD_AUTO(request_exit)) || (should_terminate && num_tasks_in_flight.load(std::memory_order_relaxed) == 0)) [[unlikely]] {
					return;
				}

				thread_conditions[THREAD_INDEX].wait(lock);
			}

			// Execute task.
			task->functor(task);

			// Decrement after invoking the task in-case the task enqueues other tasks.
			// Don't want this number to prematurely hit 0. If this does hit 0 and we want to exit.
			// Wake up all other threads to let them know.
			const auto current_num_tasks_in_flight = --num_tasks_in_flight;
			if (should_terminate && current_num_tasks_in_flight == 0) [[unlikely]] {
				wake_up_all_threads();
				return;
			}

			// Take the subsequent tasks. This lock should be as short as possible.
			// @TODO: This subsequents array destructor is the slowest thing in this function.
			Array<SharedPtr<Task>> subsequents = [&] {
				ScopeLock lock{task->add_subsequents_mutex};

				task->completed = true;

				return std::move(task->subsequent_tasks);
			}();

			for (auto& subsequent : subsequents) {
				if (--subsequent->prerequisites_remaining == 0) {
					enqueue_task_no_prerequisites(std::move(subsequent), true);
				}
			}
		}
	}

	FORCEINLINE fn do_work(InvokableReturns<bool> auto&& request_exit) -> void {
		switch (get_thread_type()) {
		case ThreadType::MAIN:		do_work<ThreadType::MAIN>(FORWARD_AUTO(request_exit)); break;
		case ThreadType::WORKER: 	do_work<ThreadType::WORKER>(FORWARD_AUTO(request_exit)); break;
		}
	}

	template<ThreadType THREAD>
	NOINLINE fn try_dequeue_task() -> SharedPtr<Task> {
		static constexpr auto THREAD_INDEX = static_cast<usize>(THREAD);

		SharedPtr<Task> task = nullptr;

		// The queue can occasionally fail if there's too much thread-contention even if it's not empty. Keep attempting to
		// dequeue a task as long as there are tasks in the queue.
		//while (num_tasks_enqueued[static_cast<usize>(THREAD)].load(std::memory_order_relaxed) > 0) {
			// First try to dequeue named-thread tasks if this is running on a named-thread in order of priority (high -> low).
			if constexpr (THREAD != ThreadType::WORKER) {
				for (auto& queue : named_thread_queues[static_cast<usize>(THREAD)]) {
					if (queue.size() != 0) {
						task = std::move(queue.back());
						queue.pop_back();

						--num_tasks_enqueued[THREAD_INDEX];
						return task;
					}
				}
			}

			// Try dequeue worker-thread tasks in order of priority (high -> low).
			for (auto& queue : priority_queues) {
				if (queue.try_dequeue(task)) {
					for (auto& num : num_tasks_enqueued) {
						--num;
					}

					return task;
				}
			}

		//	std::this_thread::yield();
		//}

		return task;
	}

	fn enqueue(Fn<void(const SharedPtr<Task>&)> functor, const task::Priority priority = task::Priority::NORMAL, const task::Thread thread = task::Thread::ANY, const Span<const SharedPtr<Task>>& prerequisites = {}, const bool wake_up_thread = true) -> SharedPtr<Task> {
		// @TODO: Overload new for tasks and pool these or use an allocator.
		const auto task = std::make_shared<Task>(std::move(functor), priority, thread);

		u32 num_prerequisites = 0;
		if (prerequisites.size() > 0) {
			for (const auto& prerequisite : prerequisites) {
				// Lock must persist until this task's prerequisites_remaining has been set otherwise the value could potentially be decremented before fully initialized.
				prerequisite->add_subsequents_mutex.lock();

				// Prerequisite has already completed so it's not a valid prerequisite.
				if (prerequisite->has_completed()) {
					continue;
				}

				prerequisite->subsequent_tasks.push_back(task);
				++num_prerequisites;
			}

			task->prerequisites_remaining = num_prerequisites;

			// Unlock all.
			for (const auto& prerequisite : prerequisites) {
				prerequisite->add_subsequents_mutex.unlock();
			}
		}

		// Increment even for subsequent tasks.
		++num_tasks_in_flight;

		// No prerequisites; enqueue immediately - otherwise will be automatically enqueued when prerequisites complete.
		if (num_prerequisites == 0) {
			enqueue_task_no_prerequisites(task, wake_up_thread);
		}

		return task;
	}

	fn enqueue(Invokable auto&& functor, const task::Priority priority = task::Priority::NORMAL, const task::Thread thread = task::Thread::ANY, const Span<const SharedPtr<Task>>& prerequisites = {}, const bool wake_up_thread = true) -> SharedPtr<Task> {
		return enqueue([functor = FORWARD_AUTO(functor)](const SharedPtr<Task>&) mutable {
			std::invoke(FORWARD_AUTO(functor));
		}, priority, thread, prerequisites, wake_up_thread);
	}

	fn enqueue_task_no_prerequisites(SharedPtr<Task> task, const bool wake_up_thread) -> void {
		ASSERT(!task->has_prerequisites());
		ASSERT(!task->has_completed());

		const auto priority = task->priority;
		const auto thread = task->thread;
		const auto priority_index = static_cast<usize>(priority);
		const auto thread_index = static_cast<usize>(thread);

		// This will only ever block if a thread is about to go to sleep while
		// a task on another thread is being enqueued.
		//std::shared_lock lock{thread_condition_mutexes[thread_index]};
		if (wake_up_thread) {
			thread_condition_mutexes[thread_index].lock_shared();
		}

		if (thread == task::Thread::ANY) {
			for (auto& num : num_tasks_enqueued) {
				++num;
			}

			priority_queues[priority_index].enqueue(std::move(task));
		} else {
			++num_tasks_enqueued[thread_index];

			named_thread_queues[thread_index][priority_index].push_back(std::move(task));
		}

		if (wake_up_thread) {
			wake_up_one_thread_of_type([&] {
				switch (thread) {
				case task::Thread::MAIN: 	return ThreadType::MAIN;
				case task::Thread::ANY: 	return ThreadType::WORKER;
				}
			}());

			thread_condition_mutexes[thread_index].unlock_shared();
		}
	}

	// Stall this thread until the tasks are completed.
	NOINLINE fn synchronous_wait_for_tasks_to_complete(const Span<const SharedPtr<Task>>& tasks) -> bool {
		// Check to see if all the tasks are already complete and early return. Optimize for this case since otherwise we stall anyways.
		if (std::find_if(tasks.begin(), tasks.end(), [](const auto& task) { return !task->has_completed(); }) == tasks.end()) [[likely]] {
			return false;
		}

		volatile bool all_tasks_complete = false;
		std::condition_variable condition;

		enqueue([&] {
			all_tasks_complete = true;
			condition.notify_one();
		}, task::Priority::HIGH, task::Thread::ANY, tasks);

		std::mutex mutex;
		std::unique_lock lock{mutex};
		condition.wait(lock, [&] { return all_tasks_complete; });

		return true;
	}

	// Wait for the tasks to complete before continuing, but do other work in the background while waiting.
	fn busy_wait_for_tasks_to_complete(const Span<const SharedPtr<Task>>& tasks) -> void {
		volatile bool all_tasks_complete = false;

		enqueue([&, this_thread_type = get_thread_type()] {
			all_tasks_complete = true;
			wake_up_all_threads_of_type(this_thread_type);
		}, task::Priority::HIGH, task::Thread::ANY, tasks);

		do_work([&] { return all_tasks_complete; });
	}

	template<std::integral Int>
	fn parallel_for(const Int count, Invokable<Int> auto&& functor) -> void {
		if (count <= 1) {
			for (Int i = 0; i < count; ++i) {
				std::invoke(FORWARD_AUTO(functor), i);
			}
			return;
		}

		// Don't try to wake up a thread for every task, takes too much time on this thread.
		// Enqueue all the tasks then wake up all threads once (may be wasteful to wake up all of them though).
		Array<SharedPtr<Task>> tasks;
		tasks.reserve(static_cast<usize>(count));
		for (Int i = 0; i < count; ++i) {
			tasks.push_back(enqueue([&functor, i] {
				std::invoke(FORWARD_AUTO(functor), i);
			}, task::Priority::HIGH, task::Thread::ANY, {}, false));
		}

		volatile bool should_exit = false;

		enqueue([&, this_thread_type = get_thread_type()] {
			ScopeSharedLock lock{thread_condition_mutexes[static_cast<usize>(this_thread_type)]};

			should_exit = true;
			wake_up_all_threads_of_type(this_thread_type);
		}, task::Priority::HIGH, task::Thread::ANY, tasks, false);


		{
			for (auto& mutex : thread_condition_mutexes) {
				mutex.lock_shared();
			}

			wake_up_all_threads();

			for (auto& mutex : thread_condition_mutexes) {
				mutex.unlock_shared();
			}
		}

		do_work([&] { return should_exit; });
	}

	// Expensive.
	fn wake_up_all_threads() -> void {
		for (usize i = 0; i < NAMED_THREAD_COUNT; ++i) {
			thread_conditions[i].notify_one();// One of each named-thread.
		}
		thread_conditions[static_cast<usize>(ThreadType::WORKER)].notify_all();
	}

	fn wake_up_one_thread_of_type(const ThreadType thread) -> void {
		thread_conditions[static_cast<usize>(thread)].notify_one();
	}

	fn wake_up_all_threads_of_type(const ThreadType thread) -> void {
		if (thread == ThreadType::WORKER) {
			ASSERT(worker_threads.size() > 0);
			thread_conditions[static_cast<usize>(thread)].notify_all();
		} else {
			thread_conditions[static_cast<usize>(thread)].notify_one();// Never more than 1 named-thread instance.
		}
	}

private:
	Array<std::thread> worker_threads;
	std::condition_variable_any thread_conditions[THREAD_TYPE_COUNT];
	SharedMutex thread_condition_mutexes[THREAD_TYPE_COUNT];
	std::atomic<u32> num_tasks_enqueued[THREAD_TYPE_COUNT]{};
	std::atomic<u32> num_tasks_in_flight = 0;// All tasks in-flight including subsequents.
	volatile bool should_terminate = false;
	Array<SharedPtr<Task>> named_thread_queues[NAMED_THREAD_COUNT][task::PRIORITY_COUNT];// Doesn't need to be thread-safe. Only used per-thread.
	ConcurrentQueue<SharedPtr<Task>> priority_queues[task::PRIORITY_COUNT];// High -> Low.
};

namespace impl {
EXPORT_API inline TaskGraph task_graph_singleton{};
}

FORCEINLINE fn TaskGraph::get() -> TaskGraph& {
	return impl::task_graph_singleton;
}
#endif
}

#elif 1
namespace core {
	// Thread-safe arbitrary task scheduler which can be enqueued from any thread and optionally with prerequisite tasks.
	struct alignas(CACHE_LINE_SIZE) Task {
		Task() = default;
		NON_COPYABLE(Task);

		enum class Priority : u8 { LOW, NORMAL, HIGH };
		enum class Thread : u8 { MAIN, RENDER, ANY };

		static constexpr usize PRIORITY_COUNT = 3;
		static constexpr usize THREAD_COUNT = 3;

		struct CreateInfo {
			Priority priority = Priority::NORMAL;
			Thread thread = Thread::ANY;
			Span<const SharedPtr<Task>> prerequisites;
		};

		[[nodiscard]]
		FORCEINLINE fn has_completed() const -> bool { return completed.load(); }

		[[nodiscard]]
		FORCEINLINE fn has_prerequisites() const -> bool { return prerequisites_remaining.load() != 0; }

		fn add_subsequent(SharedPtr<Task> subsequent) -> bool {
			ASSERT(!subsequent->has_completed());

			// Adding a subsequent can occur on multiple threads.
			std::lock_guard lock{subsequent_tasks_lock};

			if (has_completed()) {
				return false;
			}

			ASSERTF(std::find(subsequent_tasks.begin(), subsequent_tasks.end(), subsequent) == subsequent_tasks.end(),
				"Subsequent task already exists!");

			const u32 subsequent_num = subsequent->prerequisites_remaining++;
			subsequent_tasks.push_back(std::move(subsequent));

			return true;
		}

		friend struct TaskGraph;

	private:
		Fn<void()> functor;
		Array<SharedPtr<Task>> subsequent_tasks;
		std::atomic<u32> prerequisites_remaining = 0;
		std::atomic<bool> completed = false;
		SpinLock subsequent_tasks_lock;// Locks before modifying subsequent_tasks.
		Task::Priority priority = Task::Priority::NORMAL;
		Task::Thread thread = Task::Thread::ANY;
	};

	struct alignas(CACHE_LINE_SIZE) TaskGraph {
		[[nodiscard]]
		FORCEINLINE static fn get() -> TaskGraph&;

		~TaskGraph() {
			deinitialize();
		}

		fn initialize(const usize num_workers) -> void {
			ASSERT(num_workers > 0);
			ASSERT(worker_threads.size() == 0);

			worker_threads.reserve(num_workers);
			for (usize i = 0; i < num_workers; ++i) {
				worker_threads.push_back(std::thread{[&] {
					do_work([&] { return false; });
				}});
			}
		}

		fn deinitialize() -> void {
			should_terminate = true;

			worker_condition.notify_all();

			for (auto& worker : worker_threads) {
				worker.join();
			}

			worker_threads.clear();
		}

		EXPORT_API static inline std::atomic<u32> failed_to_find_task_count = 0;

		fn do_work(InvokableReturns<bool> auto&& request_exit_invokable) -> void {
			while (!request_exit_invokable()) {
				SharedPtr<Task> task;
				while (!queue.try_dequeue(task)) {
					++failed_to_find_task_count;
					if (should_terminate || request_exit_invokable()) [[unlikely]] {
						return;
					}

					std::mutex mutex;
					std::unique_lock lock{mutex};
					worker_condition.wait(lock);
				}

				task->functor();
			}
		}

		template<std::integral Int>
		fn parallel_for(const Int count, Invokable<Int> auto&& functor) -> void {
			std::condition_variable completion_condition;

			std::atomic<Int> tasks_completed = 0;
			for (Int i = 0; i < count; ++i) {
				auto task = std::make_shared<Task>();
				task->functor = [&, i] {
					functor(i);
					if (++tasks_completed == count) {
						completion_condition.notify_one();
					}
				};
				queue.enqueue(std::move(task));
			}

			worker_condition.notify_all();

			std::mutex mutex;
			std::unique_lock lock{mutex};
			completion_condition.wait(lock);
		}

	private:
		Array<std::thread> worker_threads;
		std::condition_variable worker_condition;
		ConcurrentQueue<SharedPtr<Task>> queue;
		volatile bool should_terminate = false;
	};

	namespace impl {
		EXPORT_API inline TaskGraph instance{};
	}

	fn TaskGraph::get() -> TaskGraph& { return impl::instance; }
}

#else

namespace core {
	struct TaskGraph;

	enum class NamedThread : u8 { MAIN, RENDER };
	enum class ThreadType : u8 { MAIN, RENDER, WORKER };

	inline constexpr usize NAMED_THREAD_COUNT = 2;
	inline constexpr usize THREAD_TYPE_COUNT = 3;

	namespace impl {
		EXPORT_API inline std::thread::id named_thread_ids[NAMED_THREAD_COUNT];
	}

	[[nodiscard]]
	FORCEINLINE fn is_in_main_thread() -> bool { return impl::named_thread_ids[static_cast<usize>(NamedThread::MAIN)] == std::this_thread::get_id(); }

	[[nodiscard]]
	FORCEINLINE fn is_in_render_thread() -> bool { return impl::named_thread_ids[static_cast<usize>(NamedThread::RENDER)] == std::this_thread::get_id(); }

	[[nodiscard]]
	FORCEINLINE fn get_thread_type() -> ThreadType {
		static thread_local ThreadType thread_type = [] {
			if (is_in_main_thread()) {
				return ThreadType::MAIN;
			} else if (is_in_render_thread()) {
				return ThreadType::RENDER;
			} else {
				return ThreadType::WORKER;
			}
		}();

		return thread_type;
	}

	// Thread-safe arbitrary task scheduler which can be enqueued from any thread and optionally with prerequisite tasks.
	struct alignas(CACHE_LINE_SIZE) Task {
		Task() = default;
		NON_COPYABLE(Task);

		enum class Priority : u8 { LOW, NORMAL, HIGH };
		enum class Thread : u8 { MAIN, RENDER, ANY };

		static constexpr usize PRIORITY_COUNT = 3;
		static constexpr usize THREAD_COUNT = 3;

		struct CreateInfo {
			Priority priority = Priority::NORMAL;
			Thread thread = Thread::ANY;
			Span<const SharedPtr<Task>> prerequisites;
		};

		[[nodiscard]]
		FORCEINLINE fn has_completed() const -> bool { return completed.load(); }

		[[nodiscard]]
		FORCEINLINE fn has_prerequisites() const -> bool { return prerequisites_remaining.load() != 0; }

		fn add_subsequent(SharedPtr<Task> subsequent) -> bool {
			ASSERT(!subsequent->has_completed());

			// Adding a subsequent can occur on multiple threads.
			std::lock_guard lock{subsequent_tasks_lock};

			if (has_completed()) {
				return false;
			}

			ASSERTF(std::find(subsequent_tasks.begin(), subsequent_tasks.end(), subsequent) == subsequent_tasks.end(),
				"Subsequent task already exists!");

			const u32 subsequent_num = subsequent->prerequisites_remaining++;
			subsequent_tasks.push_back(std::move(subsequent));

			return true;
		}

		friend TaskGraph;

	private:
		Fn<void()> functor;
		Array<SharedPtr<Task>> subsequent_tasks;
		std::atomic<u32> prerequisites_remaining = 0;
		std::atomic<bool> completed = false;
		SpinLock subsequent_tasks_lock;// Locks before modifying subsequent_tasks.
		Task::Priority priority = Task::Priority::NORMAL;
		Task::Thread thread = Task::Thread::ANY;
	};

	static constexpr Task::CreateInfo TASK_DEFAULT_CREATE_INFO{
		.priority = Task::Priority::NORMAL,
		.thread = Task::Thread::ANY,
		.prerequisites{},
	};

	struct alignas(CACHE_LINE_SIZE) TaskGraph {
	private:
		struct alignas(CACHE_LINE_SIZE) ThreadTermination {
			volatile bool should_terminate = false;
		};

		struct DestructibleThread {
			FORCEINLINE fn terminate_and_join() -> void {
				termination_info->should_terminate = true;
				thread.join();
			}

			std::thread thread;
			UniquePtr<ThreadTermination> termination_info;
		};

		template<ThreadType THREAD>
		fn create_thread() -> DestructibleThread {
			static_assert(THREAD != ThreadType::MAIN);

			auto termination_info = std::make_unique<ThreadTermination>();
			std::thread thread{[this, &should_exit = termination_info->should_terminate] {
				do_work<THREAD>([&should_exit] { return should_exit; });
			}};

			return DestructibleThread{
				.thread = std::move(thread),
				.termination_info = std::move(termination_info),
			};
		};

	public:
		~TaskGraph() {
			deinitialize();
		}

		[[nodiscard]]
		static fn get() -> TaskGraph&;

		fn initialize(const u32 num_worker_threads) -> void {
			impl::named_thread_ids[static_cast<usize>(NamedThread::MAIN)] = std::this_thread::get_id();
			ASSERT(is_in_main_thread());

			set_num_workers(num_worker_threads);
		}

		fn set_num_workers(const u32 num_worker_threads) -> void {
			if (num_worker_threads == worker_threads.size()) return;

			if (num_worker_threads > worker_threads.size()) {
				const u32 add_num_threads = num_worker_threads - worker_threads.size();
				for (u32 i = 0; i < add_num_threads; ++i) {
					worker_threads.push_back(create_thread<ThreadType::WORKER>());
				}
			} else {
				for (i32 i = worker_threads.size() - 1; i >= num_worker_threads; --i) {
					worker_threads[i].terminate_and_join();
					worker_threads.pop_back();
				}
			}
		}

		fn deinitialize() -> void {
			ASSERT(is_in_main_thread());

			should_terminate = true;

			for (auto& condition : conditions) {
				condition.notify_all();
			}

			for (auto& thread : worker_threads) {
				thread.thread.join();
			}

			worker_threads.clear();
		}

		EXPORT_API static inline std::atomic<u32> failed_to_find_task_count = 0;

		template<ThreadType THREAD>
		fn do_work(InvokableReturns<bool> auto&& request_exit_invokable) -> void {
			while (true) {
				static constexpr auto THREAD_INDEX = static_cast<usize>(THREAD);

				SharedPtr<Task> task;

				// Try to dequeue a task (if there is one available).
				while (!(task = try_dequeue_task<THREAD>())) [[unlikely]] {
					if (num_tasks_in_flight[static_cast<usize>(ThreadType::WORKER)].load() != 0) {
						++failed_to_find_task_count;
					}
					// Check if this particular thread wants to exit.
					if (std::invoke(FORWARD_AUTO(request_exit_invokable))) {
						return;
					}

					// Named threads can potentially steal work from worker-threads.
					const auto any_tasks_in_flight = [&] { return num_tasks_in_flight[THREAD_INDEX].load() != 0 || (THREAD != ThreadType::WORKER && num_tasks_in_flight[static_cast<usize>(ThreadType::WORKER)].load() != 0); };

					// Check if task-graph is being terminated. Shut down all threads once tasks have been completed.
					// Once completed, wake up other threads so they can shut down
					if (should_terminate.load() && !any_tasks_in_flight()) {
						if constexpr (THREAD == ThreadType::WORKER) {
							conditions[THREAD_INDEX].notify_all();
						}
						return;
					}

					// No tasks to dequeue right now and failed to exit. Stall until
					// notified that a new task has been enqueued. Still attempt to dequeue even if not signalled in-case of potential race-condition.

					std::mutex mutex;
					std::unique_lock lock{mutex};
					conditions[THREAD_INDEX].wait_for(lock, std::chrono::seconds{5});// Checking for spurious wakeups here doesn't matter.
				}

				// Perform the task.
				task->functor();

				// Acquire a very short lock to move all the subsequent tasks out to then potentially enqueue.
				{
					Array<SharedPtr<Task>> subsequent_tasks;
					{
						std::lock_guard lock{task->subsequent_tasks_lock};

						task->completed = true;

						subsequent_tasks = std::move(task->subsequent_tasks);
						ASSERT(task->subsequent_tasks.size() == 0);
					}

					for (auto& subsequent : subsequent_tasks) {
						if (--subsequent->prerequisites_remaining == 0) {
							internal_enqueue_task<true>(std::move(subsequent));
						}
					}
				}

				// Decrement the number of in-flight tasks. Worker-threads can only dequeue worker thread tasks
				// so this can be optimized slightly with an if constexpr.
				u32 value;
				if constexpr (THREAD == ThreadType::WORKER) {
					value = --num_tasks_in_flight[THREAD_INDEX];
				} else {
					value = --num_tasks_in_flight[static_cast<usize>(task->thread)];
				}
				ASSERT(value < UINT32_MAX);
			}
		}

		fn do_work(InvokableReturns<bool> auto&& request_exit_invokable) -> void {
			switch (get_thread_type()) {
			case ThreadType::MAIN:		do_work<ThreadType::MAIN>(FORWARD_AUTO(request_exit_invokable)); break;
			case ThreadType::RENDER:	do_work<ThreadType::RENDER>(FORWARD_AUTO(request_exit_invokable)); break;
			case ThreadType::WORKER:	do_work<ThreadType::WORKER>(FORWARD_AUTO(request_exit_invokable)); break;
			}
		}

		// Will return NULL on queue empty.
		template<ThreadType THREAD>
		fn try_dequeue_task() -> SharedPtr<Task> {
			SharedPtr<Task> out = nullptr;
			if constexpr (THREAD != ThreadType::WORKER) {
				if (tasks[Task::PRIORITY_COUNT + static_cast<usize>(THREAD)].try_dequeue(out)) {
					return out;
				}
			}

			for (usize i = 0; i < Task::PRIORITY_COUNT; ++i) {
				if (tasks[i].try_dequeue(out)) {
					return out;
				}
			}

			return out;
		}

		FORCEINLINE fn enqueue_task_no_prerequisites(SharedPtr<Task> task) -> void {
			internal_enqueue_task<false>(std::move(task));
		}

		// Initializes the task. Only call if you know what you're doing. Doesn't register the task.
		template<typename Functor, typename... Args>
		FORCEINLINE static fn initialize_task(const SharedPtr<Task>& allocated_task, const Task::CreateInfo& create_info, Functor&& functor, Args&&... args) -> void {
			allocated_task->functor = std::bind(std::forward<Functor>(functor), std::forward<Args>(args)...);
			allocated_task->priority = create_info.priority;
			allocated_task->thread = create_info.thread;

			for (const auto& prerequisite : create_info.prerequisites) {
				prerequisite->add_subsequent(allocated_task);
			}
		}

		// Constructs a task without enqueuing it.
		template<typename Functor, typename... Args>
		FORCEINLINE static fn construct_task(const Task::CreateInfo& create_info, Functor&& functor, Args&&... args) -> SharedPtr<Task> {
			const auto task = std::make_shared<Task>();
			initialize_task(task, create_info, std::forward<Functor>(functor), std::forward<Args>(args)...);
			return task;
		}

		template<typename Functor, typename... Args>
		FORCEINLINE fn enqueue(const Task::CreateInfo& create_info, Functor&& functor, Args&&... args) -> SharedPtr<Task> {
			const SharedPtr<Task> task = construct_task(create_info, std::forward<Functor>(functor), std::forward<Args>(args)...);

			// Only enqueue if there's no prerequisites. Otherwise the last prerequisite will enqueue upon completion.
			if (!task->has_prerequisites()) {
				enqueue_task_no_prerequisites(task);
			} else {
				// Increment the num tasks in flight anyways since this task will be enqueued at a later time.
				++num_tasks_in_flight[static_cast<usize>(task->thread)];
			}

			return task;
		}

		// Wait for the task to complete on the given thread. Will return false if all tasks are already complete.
		NOINLINE fn wait_for_tasks_to_complete(const Span<const SharedPtr<Task>>& tasks) -> bool {
			// Early-return if all tasks are complete.
			if (std::find_if(tasks.begin(), tasks.end(), [](const auto& task) { return !task->has_completed(); }) == tasks.end()) [[likely]] {
				return false;
			}

			std::mutex mutex;
			std::condition_variable condition;
			std::atomic<bool> tasks_completed = false;

			// Enqueue a task for another thread to notify this thread when all the tasks have completed.
			enqueue(Task::CreateInfo{
				.priority = Task::Priority::NORMAL,
				.thread = Task::Thread::ANY,
				.prerequisites = tasks,
			}, [&] {
				// @NOTE: This is probably technically unsafe. It's possible this task can run before the lock occurs and the other thread stalls forever. Although
				// Unreal's TaskGraph does the same thing in this situation so maybe it's fine.
				tasks_completed = true;
				condition.notify_one();
			});

			std::unique_lock lock{mutex};

			// Using wait_for in-case of the possible race-condition stated above. Likely not necessary.
			while (!tasks_completed.load()) [[unlikely]] {
				condition.wait_for(lock, std::chrono::milliseconds{100}, [&] { return tasks_completed.load(); });
			}

			ASSERT(std::find_if(tasks.begin(), tasks.end(), [](const auto& task) { return task->has_completed(); }) == tasks.end());

			return true;
		}

		// Wait for the task to complete on the given thread. Will return false if all tasks are already complete.
		fn busy_wait_for_tasks_to_complete(const Span<const SharedPtr<Task>>& tasks) -> void {
			volatile bool all_tasks_complete = false;

			// Enqueue a task for another thread to notify this thread when all the tasks have completed.
			enqueue(Task::CreateInfo{
				.priority = Task::Priority::HIGH,
				.thread = Task::Thread::ANY,
				.prerequisites = tasks,
			}, [this, &all_tasks_complete] {
				all_tasks_complete = true;

				const ThreadType thread_type = get_thread_type();
				if (thread_type == ThreadType::WORKER) {
					conditions[static_cast<usize>(ThreadType::WORKER)].notify_all();
				} else {
					conditions[static_cast<usize>(thread_type)].notify_one();
				}
			});

			// Keep doing work on this thread while waiting for the above tasks to complete.
			do_work([&] { return all_tasks_complete; });
		}

		[[nodiscard]]
		FORCEINLINE fn has_tasks_to_process(const ThreadType thread = ThreadType::WORKER) const -> bool {
			return num_tasks_in_flight[static_cast<usize>(thread)].load();
		}

		template<std::integral Int>
		fn parallel_for(const Int count, Invokable<Int> auto&& invokable) -> void {
			if (count <= 1) {
				for (Int i = 0; i < count; ++i) {
					std::invoke(FORWARD_AUTO(invokable), i);
				}
				return;
			}

			// Enqueue a task for each loop iteration.
			// @TODO: Implement balanced-workload optimizations to not have to enqueue a task for each iteration to avoid enqueueing overhead.
			Array<SharedPtr<Task>> tasks;
			tasks.reserve(static_cast<usize>(count));
			for (Int i = 0; i < count; ++i) {
				tasks.push_back(enqueue(Task::CreateInfo{
					.priority = Task::Priority::HIGH,
					.thread = Task::Thread::ANY,
					.prerequisites = tasks,
				}, [&invokable, i] {
					std::invoke(FORWARD_AUTO(invokable), i);
				}));
			}

			// This is potentially dangerous as another task could stall waiting on this task to complete but just don't do that.
			busy_wait_for_tasks_to_complete(tasks);
		}

	//private:
		template<bool IS_PREREQUISITE>
		fn internal_enqueue_task(SharedPtr<Task> task) -> void {
			ASSERTF(!should_terminate.load(), "Called during TaskGraph termination!");
			ASSERTF(!task->has_completed(), "Attempted to enqueue task that has already completed!");
			ASSERT(!task->has_prerequisites());

			const Task::Priority priority = task->priority;
			const Task::Thread thread = task->thread;

			const auto queue_index = thread == Task::Thread::ANY ?
					static_cast<usize>(priority) : static_cast<usize>(thread) + Task::PRIORITY_COUNT;

			if constexpr (!IS_PREREQUISITE) {
				// Increment the number of tasks in flight (exclude prerequisites since they have already incremented this).
				++num_tasks_in_flight[static_cast<usize>(task->thread)];
			}

			const bool success = tasks[queue_index].enqueue(std::move(task));
			ASSERT(success);

			if (thread != Task::Thread::ANY) {
				conditions[static_cast<usize>(thread)].notify_one();
			} else {
				for (usize i = 0; i < Task::THREAD_COUNT; ++i) {
					conditions[i].notify_one();// @NOTE: This call is fairly expensive. Maybe could optimize away calling named-thread conditions.
				}
			}
		}

		Array<DestructibleThread> worker_threads;
		ConcurrentQueue<SharedPtr<Task>> tasks[Task::PRIORITY_COUNT + NAMED_THREAD_COUNT];// @TODO: Named threads don't need concurrent queues.
		std::condition_variable conditions[THREAD_TYPE_COUNT];
		std::atomic<u32> num_tasks_in_flight[THREAD_TYPE_COUNT]{};// All tasks in-flight per-named-thread (including ones with prerequisites).
		std::atomic<bool> should_terminate = false;
	};

	namespace impl {
		EXPORT_API inline TaskGraph singleton;
	};

	FORCEINLINE fn TaskGraph::get() -> TaskGraph& {
		return impl::singleton;
	}
}
#endif