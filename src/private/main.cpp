
#include <../third_party/taskflow-master/taskflow/taskflow.hpp>

#include "core/src/public/concurrency/task_graph.hpp"
#include "core/src/public/defines.hpp"

#include <chrono>
#include <thread>

std::atomic<u32> count = 0;
volatile bool request_exit = false;

#if 0
fn main() -> i32 {
	tf::Executor executor{32};

	tf::Taskflow taskflow;

	static constexpr auto COUNT = 1000;

#if 0
	Array<tf::Task> previous_tasks;
	for (i32 i = 0; i < COUNT; ++i) {
		for (i32 i = 0; i < 10000; ++i) {
			tf::Task task = taskflow.emplace([&] { ++count; });
			for (auto& previous_task : previous_tasks) {
				task.succeed(previous_task);
			}
		}
	}
#endif

	for (i32 i = 0; i < 10000 * COUNT; ++i) {
		taskflow.emplace([&] { ++count; });
	}

	const auto start = std::chrono::high_resolution_clock::now();

	executor.run(taskflow).wait();

	const auto end = std::chrono::high_resolution_clock::now();


	fmt::println("Time == {}", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

	return EXIT_SUCCESS;
}

#else

fn main_loop(const SharedPtr<core::Task>& this_task) -> void {
	using namespace core;

	const auto start = std::chrono::high_resolution_clock::now();

	constexpr auto COUNT = 1000;
	//for (i32 i = 0; i < COUNT; ++i) {
	TaskGraph::get().parallel_for(10000 * COUNT, [&](const i32 i) -> void { ++count; });
	//}

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("Average time == {}\ncount == {}", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), count.load());

	request_exit = true;
}

fn main(const i32 args_count, const char* args[]) -> i32 {
	using namespace core;

	TaskGraph::get().initialize(32);

	TaskGraph::get().enqueue(main_loop, task::Priority::NORMAL, task::Thread::MAIN);
	TaskGraph::get().do_work<ThreadType::MAIN>([&] { return request_exit; });

	// Shut down task graph (stalls for all previous tasks to finish first).
	TaskGraph::get().deinitialize();

	//'fmt::println("Duration == {}.\nCount == {}.", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), count.load(std::memory_order_relaxed));

	fmt::println("Completed!");

	return EXIT_SUCCESS;
}
#endif