
#include <../third_party/taskflow-master/taskflow/taskflow.hpp>

#include "core/src/public/concurrency/task_graph.hpp"
#include "core/src/public/defines.hpp"

#include <chrono>
#include <thread>

std::atomic<u32> count = 0;
volatile bool request_exit = false;

#include "core/src/public/ecs/system.hpp"
#include "core/src/public/math.hpp"
#include "core/src/public/ecs/component.hpp"
#include "core/src/public/ecs/archetype.hpp"
#include "core/src/public/concurrency/threading.hpp"

fn main_loop(const SharedPtr<core::Task>& this_task) -> void {
	using namespace core;

	const auto start = std::chrono::high_resolution_clock::now();

	for (i32 i = 0; i < 10; ++i) {
		TaskGraph::get().parallel_for(1000, [&](const i32 i) {
			++count;
			std::this_thread::sleep_for(std::chrono::microseconds{1});
		});
	}

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("Average time == {}\ncount == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), count.load());

	request_exit = true;
}

enum class SomeGroup : u8 {
	START, MIDDLE, END
};

enum class SomeOtherGroup : u8 {
	START, END
};

struct Name {
	String value = "Bob";
};

struct Position {
	f32 x, y, z;
};

struct Rotation {
	f32 pitch, yaw, roll;
};

struct Attributes {
	f32 health;
	f32 shields;
	u32 ammo;
};

struct Transform {
	Position position;
	Rotation rotation;
};

struct AttachParent {
	Entity parent;
	Transform relative_transform;
};

#include <flecs.h>
#include "core/src/public/ecs/app.hpp"
#include "core/src/public/ecs/query.hpp"

fn main(const i32 args_count, const char* args[]) -> i32 {
	using namespace core;
	using namespace core::ecs;

#if 01
#if 0
	query.for_each_matching_archetype(world, [&](Archetype& archetype) {
		fmt::println("Matched archetype");
		archetype.for_each_view<const Name>([&](const u32 count, const Name* names) {
			fmt::println("count == {}", count);

			for (u32 i = 0; i < count; ++i) {
				fmt::println("Name[{}] == {}", i, names[i].value);
			}
		});
	});
#endif
#endif



#if 0
	TaskGraph::get().initialize(32);

	TaskGraph::get().enqueue(main_loop, task::Priority::NORMAL, task::Thread::MAIN);

	TaskGraph::get().do_work<ThreadType::MAIN>([&] { return request_exit; });

	// Shut down task graph (stalls for all previous tasks to finish first).
	TaskGraph::get().deinitialize();
#endif

	return EXIT_SUCCESS;
}