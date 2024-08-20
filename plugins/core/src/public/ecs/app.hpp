#pragma once

#include "ids.hpp"
#include "system.hpp"
#include "world.hpp"
#include "utils.hpp"
#include "defaults.hpp"
#include "threading/task.hpp"

namespace ecs {
struct App;

namespace cpts {
template <typename T>
concept Plugin = requires (T t, App& app) {
	t.init(app);
};
}

struct Ordering {
	GroupId within = get_group_id<group::GameFrame>();
	GroupMask after;// Prerequisites.
	GroupMask before;// Subsequents.
};

struct SystemDesc {
	GroupId group = get_group_id<group::GameFrame>();
	EventId event = get_event_id<event::OnUpdate>();
	AccessRequirements access_requirements;
	Priority priority = Priority::NORMAL;
	Thread thread = Thread::ANY;
};

struct App {
	struct SystemInfo {
		Fn<UniquePtr<SystemBase>()> factory;
		SystemDesc desc;
	};

	App();

	[[nodiscard]] FORCEINLINE static auto build() -> App { return {}; }

	auto register_group(const GroupId group, Ordering ordering) -> App& {
#if ASSERTIONS_ENABLED
		ASSERTF(!registered_groups.has(group), "Double registered group {}!", TypeRegistry<GroupId>::get_type_info(group).name);
		ASSERTF(!ordering.within.is_valid() || registered_groups.has(ordering.within), "The group that {} is within {} must be registered first!", TypeRegistry<GroupId>::get_type_info(group).name, TypeRegistry<GroupId>::get_type_info(ordering.within).name);
		registered_groups.add(group);
#endif
		ASSERTF(!ordering.after.has(group) && !ordering.before.has(group), "Can not schedule group before or after self: {}!", TypeRegistry<GroupId>::get_type_info(group).name);
		ASSERTF(!(ordering.after & ordering.before).mask.has_any_set_bits(), "Can not schedule both before an after another group for group {}!", TypeRegistry<GroupId>::get_type_info(group).name);

#if 0
		if (ordering.within.is_valid()) {
			ordering.before |= group_subsequents[ordering.within];
			ordering.after |= group_prerequisites[ordering.within];
		}

		group_subsequents[group] |= ordering.before;
		ordering.before.for_each([&](const GroupId subsequent) {
			group_subsequents[subsequent].add(group);
		});

		group_prerequisites[group] |= ordering.after;
		ordering.after.for_each([&](const GroupId prerequisite) {
			group_subsequents[prerequisite].add(group);
		});

		return *this;
#endif

#if 0
		if (ordering.within.is_valid()) {
			ordering.before |= group_subsequents[ordering.within];
			ordering.after |= group_prerequisites[ordering.within];
		}

		group_subsequents[group] |= ordering.before;
		group_prerequisites[group] |= ordering.after;

		ordering.before.for_each([&](const GroupId id) {
			//group_subsequents[id].add(group);
			group_prerequisites[id].add(group);
		});

		ordering.after.for_each([&](const GroupId id) {
			//group_prerequisites[id].add(group);
			group_subsequents[id].add(group);
		});
#else

		group_subsequents[group] |= ordering.before;
		group_prerequisites[group] |= ordering.after;

		ordering.before.for_each([&](const GroupId id) {
			//group_subsequents[id].add(group);
			group_prerequisites[id].add(group);
		});

		ordering.after.for_each([&](const GroupId id) {
			//group_prerequisites[id].add(group);
			group_subsequents[id].add(group);
		});

		if (ordering.within.is_valid()) {
			nested_groups[ordering.within].add(group);
		}
#endif

		return *this;
	}

	template <typename T>
	auto register_group(Ordering ordering = {}) -> App& {
		return register_group(get_group_id<T>(), std::move(ordering));
	}

	template <cpts::System T, typename... Args> requires std::constructible_from<T, Args&&...>
	auto register_system(SystemDesc desc, Args&&... args) -> App& {
#if ASSERTIONS_ENABLED
		ASSERTF(!registered_systems.has<T>(), "Double registered system {}!", utils::get_type_name<T>());
		registered_systems.add<T>();
#endif

		group_systems[desc.group].add(get_system_id<T>());

		event_systems[desc.event].add(get_system_id<T>());

		struct System final : public SystemBase {// @NOTE: Could be better to just store a void* and an execute fn.
			constexpr System(const Args&... args)
				: system{args...} {}

			virtual auto execute(ExecContext& context) -> void override {
				system.execute(context);
			}

			T system;
		};

		desc.access_requirements |= T::get_access_requirements();

		system_create_infos[get_system_id<T>()] = SystemInfo{
			.factory = std::bind([](const Args&... args) -> UniquePtr<SystemBase> {
				return std::make_unique<System>(args...);
			}, std::forward<Args>(args)...),
			.desc = std::move(desc),
		};

		return *this;
	}

	auto register_plugin(cpts::Plugin auto&& plugin) -> App& {
		plugin.init(*this);
		return *this;
	}

	auto run(const usize num_worker_threads = std::thread::hardware_concurrency() - 1) -> void;

	Array<GroupMask> group_subsequents;// Indexed via GroupId.
	Array<GroupMask> group_prerequisites;// Indexed via GroupId.
	GroupArray<GroupMask> nested_groups;
	Array<SystemMask> group_systems;// Indexed via GroupId.

	Array<SystemMask> comp_accessing_systems;// Indexed via CompId. All systems requiring access to this comp.

	Array<SystemInfo> system_create_infos;// Indexed via SystemId.
	Array<SystemMask> concurrent_conflicting_systems;// Indexed via SystemId.

	Array<SystemMask> event_systems;// Indexed via EventId. All systems registered to a given event.
	Array<SystemMask> event_system_prerequisites;// Indexed via EventId. These are the systems that need to complete execution before dispatching a new event.
	Array<GroupMask> event_root_groups;// Indexed via EventId.

	GroupMask root_groups;// Groups with no prerequisites.
	GroupMask leaf_groups;// Groups with no subsequents.

#if ASSERTIONS_ENABLED
	GroupMask registered_groups;
	SystemMask registered_systems;
#endif

	Optional<World> world;// Constructed in run().
};
}

#if 0
#pragma once

#include "types.hpp"
#include "ids.hpp"
#include "system.hpp"

namespace ecs {
struct App;
struct World;

namespace cpts {
template <typename T>
concept Plugin = requires (const T t, App& app) {
	t.init(app);
};
}

struct GroupOrdering {
	GroupMask after;
	GroupMask before;
};

struct SystemCreateInfo {
	Optional<GroupId> group;
};

struct SystemBase {
	virtual ~SystemBase() = default;
	virtual auto execute(World& world) -> void = 0;
};

struct App {
	struct GroupConstructionInfo {
		GroupMask prerequisites;
		GroupMask subsequents;
		SystemMask systems;
	};

	struct SystemConstructionInfo {
		Fn<UniquePtr<SystemBase>()> ctor;
		Optional<GroupId> group;
		AccessRequirements access_requirements;
	};

	App();

	template <::cpts::EnumWithCount auto GROUP>
	auto add_group(GroupOrdering ordering) -> App& {
		utils::make_index_sequence_param_pack<utils::enum_count<decltype(GROUP)>>([&]<usize... Is>() {
			([&] {
				if constexpr (Is < static_cast<usize>(GROUP)) {
					ordering.after.add(get_group_id<static_cast<decltype(GROUP)>(Is)>());
				} else if constexpr (Is > static_cast<usize>(GROUP)) {
					ordering.before.add(get_group_id<static_cast<decltype(GROUP)>(Is)>());
				}
			}(), ...);
		}); 

		GroupConstructionInfo& out = group_construction_info[get_group_id<GROUP>()];
		out.prerequisites = std::move(ordering.after);
		out.subsequents = std::move(ordering.before);

		return *this;
	}

	template <cpts::System T, typename... Args> requires std::constructible_from<T, Args&&...>
	auto add_system(SystemCreateInfo create_info, Args&&... args) -> App& {
		struct System final : public SystemBase {
			explicit System(Args&&... args)
				: system{std::forward<Args>(args)...} {}

			virtual auto execute(World& world) -> void {
				system.execute(world);
			}

			T system;
		};

		system_construction_info.push_back(SystemConstructionInfo{
			.ctor = std::bind([](const auto&... args) -> UniquePtr<SystemBase> {
				return std::make_unique<System>(args...);
			}, std::forward<Args>(args)...),
			.group = create_info.group,
		});

		if constexpr (cpts::SystemWithAccessRequirements<T>) {
			system_construction_info.back().access_requirements = T::get_requirements();
		}

		return *this;
	}

	auto run(const usize num_worker_threads = std::thread::hardware_concurrency() - 1) -> void;

	Array<GroupConstructionInfo> group_construction_info;// Indexed via GroupId.
	Array<SystemConstructionInfo> system_construction_info;// Indexed via SystemId.

	UniquePtr<World> world;
};
}
#endif