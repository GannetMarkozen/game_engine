#include "threading/task.hpp"
#include "threading/thread_safe_types.hpp"
#include <chrono>
#include <mutex>

auto main() -> int {
	using Task = task::Task;

	Atomic<bool> any_task_executing = false;
	Atomic<u32> count = 0;

	task::init(32);

	task::enqueue([&] {
#if 0
		#if 0
		Array<SharedPtr<Task>> tasks;
		for (usize i = 0; i < 500; ++i) {
			tasks.push_back(Task::make([&] {
				const bool previous_value = any_task_executing.exchange(true);
				//ASSERTF(!previous_value, "Some task was executing alongside others!");
				if (previous_value) {
					WARN("Overlapping execution!");
				}

				const auto value = ++count;
				fmt::println("Executing {} on {}", value, std::hash<std::thread::id>{}(std::this_thread::get_id()));

				std::this_thread::sleep_for(std::chrono::milliseconds{5});

				any_task_executing.store(false);
			}));
		}

		for (auto task : tasks) {
			const auto priority = task->priority;
			const auto thread = task->thread;

			Array<SharedPtr<Task>> exclusives;
			for (auto other_task : tasks) {
				if (task != other_task) {
					exclusives.push_back(std::move(other_task));
				}
			}

			task::enqueue(std::move(task), priority, thread, {}, exclusives);
		}
		#endif

		const auto start = std::chrono::high_resolution_clock::now();

		task::parallel_for(100000, [&](const usize i) {
			//fmt::println("{}", i);
		});

		const auto end = std::chrono::high_resolution_clock::now();

		fmt::println("Duration == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
#endif

		Atomic<u32> some_count = 0;
		Atomic<ThreadId> are_any_tasks_executing = ThreadId::invalid_id();

		Array<SharedPtr<Task>> tasks;
		for (usize i = 0; i < 1000; ++i) {
			tasks.push_back(Task::make([&some_count, &are_any_tasks_executing] {
				const ThreadId currently_executing = are_any_tasks_executing.exchange(thread::get_this_thread_id());
				ASSERTF(!currently_executing.is_valid(), "{} was executing while {} was executing!", currently_executing.get_value(), thread::get_this_thread_id().get_value());

				//std::this_thread::sleep_for(std::chrono::milliseconds{2});
				const auto value = ++some_count;

				fmt::println("Executed {}", value);

				are_any_tasks_executing.store(ThreadId::invalid_id());
			}));
		}

#if 01
		for (usize i = 0; i < tasks.size(); ++i) {
			auto clone = tasks;
			clone.erase(clone.begin() + i);

			task::enqueue(tasks[i], task::Priority::NORMAL, task::Thread::ANY, {}, i == 0 ? Span<const SharedPtr<Task>>{} : Span<const SharedPtr<Task>>{{tasks[i - 1]}});
		}
#else
		task::enqueue(tasks[0]);
		for (usize i = 1; i < tasks.size(); ++i) {
			task::enqueue(tasks[i], task::Priority::NORMAL, task::Thread::ANY, Span<const SharedPtr<Task>>{{tasks[i - 1]}});
		}
#endif

		task::busy_wait_for_tasks_to_complete(tasks);

		fmt::println("count == {}", some_count.load(std::memory_order_relaxed));
	}, task::Priority::HIGH, task::Thread::MAIN);

	task::do_work_until_all_tasks_complete();

	task::deinit();
}