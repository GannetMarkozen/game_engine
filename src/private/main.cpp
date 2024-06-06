
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

#elif 01


fn main_loop(const SharedPtr<core::Task>& this_task) -> void {
	using namespace core;

	const auto start = std::chrono::high_resolution_clock::now();

	for (i32 i = 0; i < 1000; ++i) {
		TaskGraph::get().parallel_for(1000, [&](const i32 i) {});
		fmt::println("Executed loop {}", i);
	}

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("Average time == {}\ncount == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), count.load());

	request_exit = true;

	//std::this_thread::sleep_for(std::chrono::seconds{5});
}

#include "core/src/public/ecs/system.hpp"
#include "core/src/public/math.hpp"
#include "core/src/public/ecs/component.hpp"

enum class SomeGroup : u8 {
	START, MIDDLE, END
};

enum class SomeOtherGroup : u8 {
	START, END
};

struct FirstComp {
	FirstComp() {
		fmt::println("The constructor was called!");
	};

	f64 value = 10.0;
};

struct SecondComp {
	f32 something = 0.f;
};

struct ThirdComp {
	f32 something;
	f32 something_else;
};

fn main(const i32 args_count, const char* args[]) -> i32 {
	using namespace core;
	using namespace core::ecs;

	const StringView string = get_type_name<SomeGroup>();
	fmt::println("{}", string);

	return EXIT_SUCCESS;


#if 01

	TaskGraph::get().initialize(32);

	TaskGraph::get().enqueue(main_loop, task::Priority::NORMAL, task::Thread::MAIN);
	TaskGraph::get().do_work<ThreadType::MAIN>([&] { return request_exit; });

	// Shut down task graph (stalls for all previous tasks to finish first).
	TaskGraph::get().deinitialize();

	//'fmt::println("Duration == {}.\nCount == {}.", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), count.load(std::memory_order_relaxed));

	

	fmt::println("Completed!");
#endif

	return EXIT_SUCCESS;
}
#else

fn main() -> i32 {

	fmt::println("number == {}", 10000);

	RWLocked<i32> number{0};

	Array<std::thread> threads;

	std::atomic<u32> readers_count = 0;
	for (i32 i = 0; i < 2; ++i) {
		threads.push_back(std::thread{[&] {
			for (i32 j = 0; j < 100000; ++j) {
				number.read([&](const i32& in_number) {
					++readers_count;
					--readers_count;
				});
			}
		}});
	}

	for (i32 i = 0; i < 2; ++i) {
		threads.push_back(std::thread{[&] {
			for (i32 j = 0; j < 100000; ++j) {
				number.write([](i32& in_number) {
					++in_number;
				});
			}
		}});
	}

	const auto start = std::chrono::high_resolution_clock::now();

	for (auto& thread : threads) {
		thread.join();
	}

	const auto end = std::chrono::high_resolution_clock::now();

	threads.clear();


	fmt::println("Duration == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

	fmt::println("number == {}", number.read([](const i32& in_number) { return in_number; }));

	return EXIT_SUCCESS;
}

#endif