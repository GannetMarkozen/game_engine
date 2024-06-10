#pragma once

#include "defines.hpp"
#include "system.hpp"
#include "scheduler.hpp"

namespace core::ecs {
struct App;
struct World;

struct CreateSystemInfo {
	SystemInfo info;
	UniquePtr<SystemBase>(*factory)(Requirements&);
};

namespace concepts {
template<typename T>
concept Plugin = requires(T t, App app) {
	{ t.init(app) };
};
}

struct App {
	friend World;

	fn add_plugin(concepts::Plugin auto&& plugin) -> App& {
		plugin.init(*this);
		return *this;
	}

	template<concepts::System T>
	fn add_system(SystemInfo info) -> App& {
		systems.push_back(CreateSystemInfo{
			.info = std::move(info),
			.system_factory = [](Requirements& out_requirements) -> UniquePtr<SystemBase> {
				return std::make_unique<T>(out_requirements);
			}
		});
		return *this;
	}

	template<::concepts::Enum T>
	fn add_group(GroupOrdering ordering = {}) -> App& {
		group_ordering_info.push_back(SystemGroupOrderingInfo{
			.group = get_group_id<T>(),
			.ordering = std::move(ordering),
		});

		return *this;
	}

	EXPORT_API fn run(const usize num_worlds = 1) -> void;

private:
	struct SystemGroupOrderingInfo {
		SystemGroupId group;
		GroupOrdering ordering;
	};

	Array<UniquePtr<World>> worlds;
	Array<CreateSystemInfo> systems;// Systems are local to their world. This is for instancing systems per-world.
	Array<SystemGroupOrderingInfo> group_ordering_info;
	Scheduler scheduler;
};
}