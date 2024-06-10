#include "ecs/world.hpp"
#include "ecs/app.hpp"

namespace core::ecs {
World::World(const App& app) {
	ASSERTF(app.systems.size() > 0, "No systems registered to app!");

#if 0
	struct SystemWithInfo {
		SystemInfo info;
		UniquePtr<SystemBase> system;
	};

	Array<SystemWithInfo> system_with_info_array;
	system_with_info_array.reserve(app.systems.size());

	for (const auto& create_system_info : app.systems) {
		SystemInfo info = create_system_info.info;
		UniquePtr<SystemBase> system = create_system_info.factory(info.requirements);

		system_with_info_array.push_back(SystemWithInfo{
			.info = std::move(info),
			.system = std::move(system),
		});
	}

	// DAG of groups with systems.
	struct SystemGroupWithSubsequents {
		SystemGroup group;
		Array<UniquePtr<SystemBase>> systems;

		Array<SystemGroupWithSubsequents> subsequent_groups;
	};
#endif

	// Resolve system ordering and dependencies.
	struct SystemWithInfo {
		SystemInfo info;
		UniquePtr<SystemBase> system;
	};

	struct GroupInfo {
		Array<SystemWithInfo> systems;
		Set<SystemGroup> subsequents;// Set so we don't allow duplicates.
	};

	// Match systems with their group.
	Map<Optional<SystemGroup>, GroupInfo> groups;

	// Add map entries for each present group with atleast one system in it.
	for (const auto& create_system_info : app.systems) {
		SystemInfo info = create_system_info.info;
		UniquePtr<SystemBase> system = create_system_info.factory(info.requirements);

		groups[info.group].systems.push_back(SystemWithInfo{
			.info = std::move(info),
			.system = std::move(system),
		});
	}

	// Initialize the "subsequents" member of GroupInfo.
	for (const auto& ordering : app.group_ordering_info) {
		if (ordering.ordering.prerequisites.empty() && ordering.ordering.subsequents.empty()) {
			continue;
		}

		const auto num_entries_in_group = get_group_info(ordering.group).num_entries;
		for (u32 i = 0; i < num_entries_in_group; ++i) {
			const SystemGroup system_group{
				.group = ordering.group,
				.value = static_cast<u8>(i),
			};
			auto group_it = groups.find(Optional<SystemGroup>{system_group});

			// No systems in group. Skip.
			if (group_it == groups.end()) {
				continue;
			}

			auto& group = group_it->second;

			// Automatically add the next enum value within the group as a subsequent dependency.
			if (i < num_entries_in_group - 1) {
				group.subsequents.insert(SystemGroup{
					.group = system_group.group,
					.value = static_cast<u8>(system_group.value + 1),
				});
			}

			// Add subsequents to this list.
			for (const auto& subsequent : ordering.ordering.subsequents) {
				if (groups.contains(subsequent)) {// Only add the group as a subsequent if there's any systems associated with it.
					group.subsequents.insert(subsequent);
				}
			}

			// Add this to the subsequent list of prerequisite groups.
			for (const auto& prerequisite : ordering.ordering.prerequisites) {
				auto found = groups.find(prerequisite);
				if (found != groups.end()) {
					found->second.subsequents.insert(prerequisite);
				}
			}
		}
	}

	
}
}