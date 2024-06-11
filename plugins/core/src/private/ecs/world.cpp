#include "ecs/world.hpp"
#include "ecs/app.hpp"

namespace core::ecs {
// @NOTE: This is insane.
// @TODO: Resolving system dependencies should ideally be handled by the App rather than
// per-world instance, but that potentially complicates instancing systems and stuff.
World::World(const App& app) {
	ASSERTF(app.systems.size() > 0, "No systems registered to app!");

	// Fill-out group_info_map.
	struct GroupInfo {
		Set<SystemGroup> subsequents;
		Array<i32> systems;// Indexes into App::systems.
	};

	Map<Optional<SystemGroup>, GroupInfo> group_info_map;

	for (const auto& ordering : app.group_ordering_info) {
		const auto group_num_entries = get_group_info(ordering.group).num_entries;
		for (i32 i = 0; i < group_num_entries; ++i) {
			const SystemGroup group{
				.group = ordering.group,
				.value = static_cast<u8>(i),
			};

			auto& subsequents = group_info_map[group].subsequents;

			if (i < group_num_entries - 1) {// Make subsequent group entries subsequents of this group (systems within a group always run in the enum declaration order).
				subsequents.insert(SystemGroup{
					.group = ordering.group,
					.value = static_cast<u8>(i + 1),
				});
			}

#if ASSERTIONS_ENABLED
			for (const auto& subsequent : ordering.ordering.subsequents) {
				ASSERTF(std::find_if(app.group_ordering_info.begin(), app.group_ordering_info.end(), [&](const auto& ordering) { return ordering.group == subsequent.group; }) != app.group_ordering_info.end(),
					"{} has {} as a subsequent group but that group has not been registered with the app!", get_group_info(group.group).name, get_group_info(subsequent.group).name)
			}
#endif

			subsequents.insert_range(ordering.ordering.subsequents);

			// Convert prerequisites into subsequents.
			for (const auto& prerequisite : ordering.ordering.prerequisites) {
				ASSERTF(std::find_if(app.group_ordering_info.begin(), app.group_ordering_info.end(), [&](const auto& ordering) { return ordering.group == prerequisite.group; }) != app.group_ordering_info.end(),
					"{} has {} as a prerequisite group but that group has not been registered with the app!", get_group_info(group.group).name, get_group_info(prerequisite.group).name);

				group_info_map[prerequisite].subsequents.insert(group);
			}
		}
	}

	// Add systems to groups.
	for (i32 i = 0; i < app.systems.size(); ++i) {
		const auto& system_group = app.systems[i].info.group;
		ASSERTF(!system_group || std::find_if(app.group_ordering_info.begin(), app.group_ordering_info.end(), [&](const auto& group) { return group.group == system_group->group; }) != app.group_ordering_info.end(),
			"System {} was set to run in group {} but that group was never registered with the app!", app.systems[i].info.name, get_group_info(system_group->group).name);

		group_info_map[system_group].systems.push_back(i);
	}

	// Filter groups to remove any without any valid systems and remove extraneous subsequent dependencies (will improve TaskGraph performance).
	const auto recursively_filter_extraneous_dependencies = [&](auto&& self, const SystemGroup group, const Array<SystemGroup>& previously_visited_groups) {
		const auto& subsequents = group_info_map[group].subsequents;
		if (subsequents.empty()) {
			return;
		}

		const Array<SystemGroup> new_previously_visited_groups = [&] {
			auto out = previously_visited_groups;
			out.push_back(group);
			return out;
		}();

		for (const auto& subsequent : group_info_map[group].subsequents) {
			// Ensure there are no circular ordering dependencies.
#if ASSERTIONS_ENABLED
			{
				const auto found_previously_visited_group = std::find(previously_visited_groups.begin(), previously_visited_groups.end(), subsequent);
				ASSERTF(found_previously_visited_group == previously_visited_groups.end(),
					"Detected group circular-dependency! {}[{}] and {}[{}] are both scheduled to run after the other!",
					get_group_info(found_previously_visited_group->group).name, found_previously_visited_group->value, get_group_info(group.group).name, group.value);
			}
#endif

			// Clear extraneous subsequent dependencies (only the dependency furthest down the tree matters).
			for (const auto& previously_visited_group : previously_visited_groups) {
				ASSERT(group != previously_visited_group);
				group_info_map[previously_visited_group].subsequents.erase(subsequent);
			}

			self(self, subsequent, new_previously_visited_groups);
		}
	};

	for (const auto& [group, _] : group_info_map) {
		if (group) {
			recursively_filter_extraneous_dependencies(recursively_filter_extraneous_dependencies, *group, {});
		}
	}

	const auto recursively_gather_used_subsequents = [&](auto&& self, const SystemGroup group, Set<SystemGroup>& out_subsequents) -> void {
		const auto& info = group_info_map[group];
		if (info.systems.empty()) {// Unused.
			for (const auto& subsequent : info.subsequents) {
				self(self, subsequent, out_subsequents);
			}
		} else {// Used.
			out_subsequents.insert(group);
		}
	};

	// For groups that have other groups as subsequents with no systems (unused), recursively search for used subsequents and replace the reference. This fixes unnecessary scheduling / contention for unused groups.
	for (const auto& [group, info] : group_info_map) {
		if (!group || !info.systems.empty()) {
			continue;
		}

		Set<SystemGroup> used_subsequents;
		recursively_gather_used_subsequents(recursively_gather_used_subsequents, *group, used_subsequents);

		for (auto& [other_group, other_info] : group_info_map) {
			if (!other_group || *other_group == *group || other_info.systems.empty() || !other_info.subsequents.contains(*group)) {
				continue;
			}

			other_info.subsequents.erase(*group);
			other_info.subsequents.insert_range(used_subsequents);
		}
	}

	// @TMP
	for (const auto& [group, info] : group_info_map) {
		if (!group) {
			continue;
		}
		fmt::println("{}[{}]({}) has subsequents [{}]", get_group_info(group->group).name, info.systems.empty() ? "UNUSED" : "used", group->value,
			[&] {
				String list;
				for (const auto& subsequent : info.subsequents) {
					list += fmt::format("{}[{}], ", get_group_info(subsequent.group).name, subsequent.value);
				}
				return list;
			}());
	}

	// Create our systems instances and their requirements.
	systems.reserve(app.systems.size());

	Array<Requirements> system_requirements;
	system_requirements.reserve(app.systems.size());

	Map<Optional<SystemGroup>, Array<u32>> group_to_system_indices_map;

	// Create systems and map groups to system indices.
	for (const auto& system_create_info : app.systems) {
		Requirements requirements = system_create_info.info.requirements;

		const auto index = systems.size();
		group_to_system_indices_map[system_create_info.info.group].push_back(index);

		systems.push_back(System{
			.system = system_create_info.factory(requirements),
		});

		system_requirements.push_back(std::move(requirements));
	}

	// Set subsequents.
	for (const auto& [group, system_indices] : group_to_system_indices_map) {
		const auto& info = group_info_map[group];
		for (const auto system_index : system_indices) {
			for (const auto subsequent_group : info.subsequents) {
				systems[system_index].subsequents.append_range(group_to_system_indices_map[subsequent_group]);
			}
		}
	}

	const auto recursive_is_subsequent_of = [&](auto&& self, const u32 system_index, const u32 query_system_index) -> bool {
		if (system_index == query_system_index) {
			return true;
		}

		for (const auto subsequent : systems[system_index].subsequents) {
			if (self(self, subsequent, query_system_index)) {
				return true;
			}
		}

		return false;
	};

	// Set contentious systems.
	for (u32 system_index = 0; system_index < systems.size(); ++system_index) {
		for (u32 other_system_index = 0; other_system_index < systems.size(); ++other_system_index) {
			if (system_index == other_system_index) {
				continue;
			}

			// Check to see if the systems can run concurrently with each other. If not check to see if the existing scheduling will make it so that the 2 systems will never run at the same time anyways. Else assign as a contentious system.
			// @TODO: This could be optimized to not be quite O log(n) complexity instead of O(n)^2 (since we compare the same 2 systems against each other twice in this double for-loop).
			if (!system_requirements[system_index].can_execute_concurrently_with(system_requirements[other_system_index]) &&
				!recursive_is_subsequent_of(recursive_is_subsequent_of, system_index, other_system_index) &&
				!recursive_is_subsequent_of(recursive_is_subsequent_of, other_system_index, system_index)) {
				systems[system_index].contentious_systems.push_back(other_system_index);
				fmt::println("System {} and {} are contentious!", app.systems[system_index].info.name, app.systems[other_system_index].info.name);
			}
		}
	}


#if 0
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
				const SystemGroup next_group_value{
					.group = system_group.group,
					.value = static_cast<u8>(system_group.value + 1),
				};

				// Only add as a subsequent if there's any systems using this group.
				if (groups.contains(next_group_value)) {
					group.subsequents.insert(next_group_value);
				}
			}

			// Add subsequents to this list.
			for (const auto& subsequent : ordering.ordering.subsequents) {
				if (subsequent != system_group && groups.contains(subsequent)) {// Only add the group as a subsequent if there's any systems associated with it.
					group.subsequents.insert(subsequent);
				}
			}

			// Add this to the subsequent list of prerequisite groups.
			for (const auto& prerequisite : ordering.ordering.prerequisites) {
				auto found = groups.find(prerequisite);
				if (found != groups.end() && *found->first != prerequisite) {
					found->second.subsequents.insert(prerequisite);
				}
			}
		}
	}

	// Probably prohibitively slow (if there are a lot of groups). Atleast this only happens once. Allocators would probably help a lot.
	FnRef<void(SystemGroup, const Array<SystemGroup>&)> self;
	const auto recursively_clean_up_dependencies = [&](const SystemGroup group, const Array<SystemGroup>& previously_visited_groups) {
		const auto subsequents = groups[group].subsequents;// Take a copy of the set since we will be removing elements.
		for (const SystemGroup& subsequent : subsequents) {
			// Ensure there are no circular ordering dependencies.
#if ASSERTIONS_ENABLED
			{
				const auto found_previously_visited_group = std::find(previously_visited_groups.begin(), previously_visited_groups.end(), subsequent);
				ASSERTF(found_previously_visited_group == previously_visited_groups.end(),
					"Detected group circular-dependency! {}[{}] and {}[{}] are both scheduled to run after the other!",
					get_group_info(found_previously_visited_group->group).name, found_previously_visited_group->value, get_group_info(group.group).name, group.value);
			}
#endif

			for (const SystemGroup& previously_visited_group : previously_visited_groups) {
				const auto num_removed = groups[previously_visited_group].subsequents.erase(subsequent);
				if (num_removed > 0) {
					fmt::println("Removed subsequent dependency {} from group {}!", get_group_info(subsequent.group).name, get_group_info(group.group).name);
				}
			}

			auto new_previously_visited_groups = previously_visited_groups;
			new_previously_visited_groups.push_back(group);

			self(subsequent, new_previously_visited_groups);
		}
	};

	self = recursively_clean_up_dependencies;

#if 01
	for (const auto& [group, _] : groups) {
		if (group) {
			recursively_clean_up_dependencies(*group, {});
		}
	}
#endif

	for (const auto& [group, info] : groups) {
		if (!group) { 
			continue;
		}
		fmt::println("{}[{}] has subsequents {}", get_group_info(group->group).name, group->value,
			[&] {
				String list;
				for (const auto& subsequent : info.subsequents) {
					list += fmt::format("{}[{}], ", get_group_info(subsequent.group).name, subsequent.value);
				}
				return list;
			}());
	}
#endif
}
}