#pragma once

#include "../defines.hpp"
#include "../assert.hpp"
#include "thread_safe_types.hpp"

#include <atomic>
#include <memory>
#include <chrono>
#include <thread>

#if 1

namespace core {
enum class ThreadType : u8 { MAIN, WORKER };
static constexpr usize THREAD_TYPE_COUNT = 2;
static constexpr usize NAMED_THREAD_COUNT = THREAD_TYPE_COUNT - 1;// Main.

namespace task {
enum class Priority : u8 { HIGH, NORMAL, LOW };
enum class Thread : u8 { MAIN, ANY };// Maybe add a render thread for pinning tasks to rendering etc?
static constexpr usize PRIORITY_COUNT = 3;
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
			while (!(task = try_dequeue_task<THREAD>())) {
				if (std::invoke(FORWARD_AUTO(request_exit)) || (should_terminate && num_tasks_in_flight.load() == 0)) [[unlikely]] {
					return;
				}

				std::mutex mutex;
				std::unique_lock lock{mutex};
				thread_conditions[static_cast<usize>(THREAD)].wait(lock);
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
				std::lock_guard lock{task->add_subsequents_mutex};

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
		while (num_tasks_enqueued[static_cast<usize>(THREAD)].load(std::memory_order_relaxed) > 0) {
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

			std::this_thread::yield();
		}

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
				num_prerequisites++;
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

		enqueue([&] {
			all_tasks_complete = true;
			wake_up_all_threads_of_type(get_thread_type());
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
			should_exit = true;
			wake_up_all_threads_of_type(this_thread_type);
		}, task::Priority::HIGH, task::Thread::ANY, tasks, false);

		wake_up_all_threads();

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
	std::condition_variable thread_conditions[THREAD_TYPE_COUNT];
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