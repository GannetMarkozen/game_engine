
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

fn main_loop(const SharedPtr<task::Task>& this_task) -> void {
	const auto start = std::chrono::high_resolution_clock::now();

	for (i32 i = 0; i < 10; ++i) {
		task::TaskGraph::get().parallel_for(1000, [&](const i32 i) {
			++count;
			std::this_thread::sleep_for(std::chrono::microseconds{1});
		});
	}

	const auto end = std::chrono::high_resolution_clock::now();

	fmt::println("Average time == {}\ncount == {}", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), count.load());

	request_exit = true;
}

enum class SomeOtherGroup : u8 {
	START, END, COUNT
};

enum class GameFrameGroup : u8 {
	FRAME_END, COUNT
};

enum class RenderFrameGroup : u8 {
	FRAME_END, COUNT
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

#include "core/src/public/ecs/query.hpp"
#include "core/src/public/ecs/world.hpp"

struct SomeOtherSystem final : public ecs::SystemBase {
	explicit SomeOtherSystem(ecs::Requirements& out_requirements) {
		out_requirements.writes<Name>();
		out_requirements.writes<AttachParent>();
	}

	virtual fn execute(ecs::World& world) -> void override {
		fmt::println("Executed");
	}
};

struct OnFrameEndSystem final : public ecs::SystemBase {
	explicit OnFrameEndSystem(ecs::Requirements& out_requirements) {
		out_requirements.writes<Name>();
	}

	virtual fn execute(ecs::World& world) -> void override {
		fmt::println("The frame has ended");
	}
};

#include "core/src/public/ecs/app.hpp"
#include "core/src/public/ecs/query.hpp"


enum class SomeGroup : u8 {
	START, MIDDLE, END, COUNT
};

struct SomeSystem final : public ecs::SystemBase {
	explicit SomeSystem(ecs::Requirements& out_requirements) {
		query.reads<Name>();

		out_requirements |= query;
	}

	virtual fn execute(ecs::World& world) -> void override {
		query.for_each_matching_archetype(world, [&](ecs::Archetype& archetype) {
			archetype.for_each_view<const Name>([&](const u32 count, const Name* names) {
				for (u32 i = 0; i < count; ++i) {
					fmt::println("Name[{}] == {}", i, names[i].value);
				}
			});
		});
	}

	ecs::Query query;
};

struct SomePlugin {
	fn init(ecs::App& app) -> void {
		using namespace ecs;

		app
			.add_group<GameFrameGroup>()
			.add_group<SomeGroup>(GroupOrdering{
				.subsequents = { get_group(GameFrameGroup::FRAME_END) },
			})
			.add_group<SomeOtherGroup>(GroupOrdering{
				.subsequents = { get_group(SomeGroup::START), get_group(GameFrameGroup::FRAME_END) },
			})
			.add_system<SomeSystem>(SystemInfo{
				.group = get_group(SomeGroup::START),
			})
			.add_system<SomeOtherSystem>(SystemInfo{
				.group = get_group(SomeOtherGroup::START),
			})
			.add_system<OnFrameEndSystem>(SystemInfo{
				.group = get_group(GameFrameGroup::FRAME_END),
			});
	}
};

fn main(const i32 args_count, const char* args[]) -> i32 {
	using namespace ecs;

	App{}
		.add_plugin(SomePlugin{})
		.run();

#if 0
	App{}
		.add_group<GameFrameGroup>()
		.add_group<SomeGroup>(GroupOrdering{
			.subsequents = { get_group(GameFrameGroup::FRAME_END) },
		})
		.add_group<SomeOtherGroup>(GroupOrdering{
			//.prerequisites = { get_group(SomeGroup::END) },
			.subsequents = { get_group(GameFrameGroup::FRAME_END) },
		})
		.add_system<SomeSystem>(SystemInfo{
			.group = get_group(SomeGroup::START),
		})
		.add_system<SomeOtherSystem>(SystemInfo{
			.group = get_group(SomeOtherGroup::START),
		})
		.add_system<OnFrameEndSystem>(SystemInfo{
			.group = get_group(GameFrameGroup::FRAME_END),
		})
		.run();
#endif

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