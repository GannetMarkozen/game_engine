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
		info.name = get_type_name<T>();
		systems.push_back(CreateSystemInfo{
			.info = std::move(info),
			.factory = [](Requirements& out_requirements) -> UniquePtr<SystemBase> {
				return std::make_unique<T>(out_requirements);
			}
		});
		return *this;
	}

	template<concepts::Group T>
	fn add_group(GroupOrdering ordering = {}) -> App& {
		ASSERTF(std::find_if(group_ordering_info.begin(), group_ordering_info.end(), [&](const auto& ordering) { return ordering.group == get_group_id<T>(); }) == group_ordering_info.end(),
			"Attempted to register group {} which has already been registered.", get_type_name<T>());

		ASSERTF(std::find_if(ordering.prerequisites.begin(), ordering.prerequisites.end(), [](const auto& group) { return group.group == get_group_id<T>(); }) == ordering.prerequisites.end(),
			"Can not schedule a prerequisite against the same group {}!", get_type_name<T>());

		ASSERTF(std::find_if(ordering.subsequents.begin(), ordering.subsequents.end(), [](const auto& group) { return group.group == get_group_id<T>(); }) == ordering.subsequents.end(),
			"Can not schedule a subsequent against the same group {}!", get_type_name<T>());

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