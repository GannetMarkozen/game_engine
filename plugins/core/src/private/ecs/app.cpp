#include "ecs/app.hpp"
#include "ecs/world.hpp"
#include "threading/task.hpp"

#define PRINT_SYSTEM_ORDERING 1

App::App() {
	group_subsequents.resize(get_num_groups());
	group_prerequisites.resize(get_num_groups());
	group_systems.resize(get_num_groups());

	comp_accessing_systems.resize(get_num_comps());

	system_create_infos.resize(get_num_systems());
	concurrent_conflicting_systems.resize(get_num_systems());

	event_systems.resize(get_num_events());
	event_system_prerequisites.resize(get_num_events());
	event_root_groups.resize(get_num_events());

	// Initialize GameFrame group by default.
	register_group<group::GameFrame>(Ordering{
		.within = GroupId::invalid_id(),
	});
}

auto App::run(const usize num_worker_threads) -> void {
	ASSERTF(!~registered_groups, "Groups {} are implicitly instantiated but are not registered!", ~registered_groups);
	ASSERTF(!~registered_systems, "Systems {} are implicitly instantiated but are not registered!", ~registered_systems);
	ASSERTF(!~registered_resources, "Resources {} are implicitly instantiated but not registered!", ~registered_resources);

	// Initialize nested group ordering as subsequents / prerequisites.
	for (GroupId id = 0; id < get_num_groups(); ++id) {
		nested_groups[id].for_each([&](const GroupId nested_id) {
			group_subsequents[nested_id] |= group_subsequents[id];
			group_prerequisites[nested_id] |= group_prerequisites[id];
		});
	}

	// Clear prerequisites first as they will be regenerated based off of subsequents.
	for (auto& prerequisites : group_prerequisites) {
		prerequisites.mask.clear();
	}

	// Find all groups that have no prerequisites / no subsequents. First groups to be dispatched.
	for (GroupId id = 0; id < get_num_groups(); ++id) {
		if (!group_subsequents[id].mask.has_any_set_bits()) {
			leaf_groups.add(id);
		}

		bool is_root_group = true;
		for (GroupId other_id = 0; other_id < get_num_groups(); ++other_id) {
			if (id == other_id) {
				continue;
			}

			if (group_subsequents[other_id].has(id)) {
				is_root_group = false;
				break;
			}
		}

		if (is_root_group) {
			root_groups.add(id);
		}
	}
	// @TODO: More in-depth error msg.
	ASSERTF(root_groups.mask.has_any_set_bits(), "No root groups!");

	// Debugging.
	const auto print_subsequents = [&] {
		root_groups.for_each([&](const GroupId id) {
			[&](this auto&& self, const GroupId id, const usize depth = 0) -> void {
				fmt::println("{}{}:", [&] {
					String out;
					for (usize i = 0; i < depth; ++i) {
						out += '-';
					}
					return out;
				}(), TypeRegistry<GroupId>::get_type_info(id).name);

				group_subsequents[id].for_each([&](const GroupId subsequent) {
					self(subsequent, depth + 1);
				});
			}(id);
		});
	};

	//fmt::println("INITIAL:");
	//print_subsequents();

	// @TODO: Would've been simpler to do this calculation based-off of group_prerequisites rather than group_subsequents.
	// Recursively prune extraneous dependencies and ensure there are no circular-dependencies.
	// @TODO: This is way too expensive and causes way too much fragmentation. With 512 groups this starts to crash from operator new.
	root_groups.for_each([&](const GroupId id) {
		[&](this auto&& self, const GroupId id, const Array<GroupId>& prerequisites = {}) -> void {
			const auto& subsequents = group_subsequents[id];
			if (subsequents.mask.has_any_set_bits()) {// More subsequents.
				subsequents.for_each([&](const GroupId subsequent) {
					auto subsequent_prerequisites = prerequisites;
					subsequent_prerequisites.push_back(id);

					// @TODO: More in-depth error msg.
					ASSERTF(!std::ranges::contains(subsequent_prerequisites, subsequent),
						"Circular dependency detected! Groups {} and {} are both subsequents of each other!",
						TypeRegistry<GroupId>::get_type_info(subsequent).name, TypeRegistry<GroupId>::get_type_info(id).name);

					self(subsequent, subsequent_prerequisites);
				});
			} else {// Final task. Now remove extraneous subsequents.
				GroupMask visited_subsequents;

				auto subsequent = id;
				for (isize i = static_cast<isize>(prerequisites.size()) - 1; i >= 0; --i) {
					const auto prerequisite = prerequisites[i];

					group_subsequents[prerequisite] &= ~visited_subsequents;

					visited_subsequents.add(subsequent);
					subsequent = prerequisite;
				}
			}
		}(id);
	});

	// Initialize group_prerequisites based off of group_subsequents.
	for (auto& prerequisites : group_prerequisites) {
		prerequisites = GroupMask{};
	}

	for (GroupId id = 0; id < get_num_groups(); ++id) {
		group_subsequents[id].for_each([&](const GroupId subsequent) {
			group_prerequisites[subsequent].add(id);
		});
	}

#if PRINT_SYSTEM_ORDERING
	print_subsequents();
#endif

	// Find prerequisite systems for event.
	for (EventId id = 0; id < get_num_events(); ++id) {
		GroupMask groups;
		event_systems[id].for_each([&](const SystemId id) {
			groups.add(system_create_infos[id].desc.group);
		});

		GroupMask root_groups, leaf_groups;
		groups.for_each([&, &event_root_groups = event_root_groups[id]](const GroupId id) {
			const bool has_any_prerequisites_within_event = group_prerequisites[id].for_each_with_break([&](const GroupId id) {
				return [&](this auto&& self, const GroupId id) -> bool {
					return groups.has(id) || group_prerequisites[id].for_each_with_break([&](const GroupId id) { return self(id); });
				}(id);
			});

			if (!has_any_prerequisites_within_event) {
				root_groups.add(id);
				event_root_groups.add(id);
			}

			const bool has_any_subsequents_within_event = group_subsequents[id].for_each_with_break([&](const GroupId id) {
				return [&](this auto&& self, const GroupId id) -> bool {
					return groups.has(id) || group_subsequents[id].for_each_with_break([&](const GroupId id) { return self(id); });
				}(id);
			});

			if (!has_any_subsequents_within_event) {
				leaf_groups.add(id);
			}
		});

		auto& prerequisites = event_system_prerequisites[id];
		root_groups.for_each([&](const GroupId id) {
			group_prerequisites[id].for_each([&](const GroupId id) {
				prerequisites |= group_systems[id];
			});
		});

		leaf_groups.for_each([&](const GroupId id) {
			prerequisites |= group_systems[id];
		});
	}

	// Find conflicting concurrent systems.
	#if 0
	for (SystemId id = 0; id < get_num_systems(); ++id) {
		for (SystemId other_id = 0; other_id < get_num_systems(); ++other_id) {
			if (id != other_id && !system_create_infos[id].desc.access_requirements.can_execute_concurrently_with(system_create_infos[other_id].desc.access_requirements)) {
				concurrent_conflicting_systems[id].add(other_id);
				concurrent_conflicting_systems[other_id].add(id);
				WARN("Systems {} and {} (groups {} and {}) have conflicting dependencies and no explicit ordering! Execution order will be non-deterministic!",
					TypeRegistry<SystemId>::get_type_info(id).name, TypeRegistry<SystemId>::get_type_info(other_id).name,
					TypeRegistry<GroupId>::get_type_info(system_create_infos[id].desc.group).name, TypeRegistry<GroupId>::get_type_info(system_create_infos[other_id].desc.group).name);
			}
		}
	}
	#endif

	const auto is_prerequisite_of = [&](this auto&& self, const GroupId a, const GroupId b) -> bool {
		return a == b || group_prerequisites[a].for_each_with_break([&](const GroupId prerequisite) {
			return !self(prerequisite, b);
		});
	};

	const auto assign_if_conflicting_systems = [&](const SystemId a, const SystemId b) {
		ASSERT(a != b);

		if (!system_create_infos[a].desc.access_requirements.can_execute_concurrently_with(system_create_infos[b].desc.access_requirements)) {
			concurrent_conflicting_systems[a].add(b);
			concurrent_conflicting_systems[b].add(a);

			#if 0
			WARN("Systems {} and {} (groups {} and {}) have conflicting dependencies and no explicit ordering! Execution order will be non-deterministic!",
				TypeRegistry<SystemId>::get_type_info(a).name, TypeRegistry<SystemId>::get_type_info(b).name,
				TypeRegistry<GroupId>::get_type_info(system_create_infos[a].desc.group).name, TypeRegistry<GroupId>::get_type_info(system_create_infos[b].desc.group).name);
			#endif
		}
	};

	for (const SystemMask& systems : group_systems) {
		systems.for_each([&](const SystemId id) {
			systems.for_each([&](const SystemId other_id) {
				if (id != other_id) {
					assign_if_conflicting_systems(id, other_id);
				}
			});
		});
	}

	for (GroupId id = 0; id < get_num_groups(); ++id) {
		for (GroupId other_id = 0; other_id < get_num_groups(); ++other_id) {
			if (id != other_id && !is_prerequisite_of(id, other_id) && !is_prerequisite_of(other_id, id)) {// No explicit ordering.
				group_systems[id].for_each([&](const SystemId id) {
					group_systems[other_id].for_each([&](const SystemId other_id) {
						assign_if_conflicting_systems(id, other_id);
					});
				});
			}
		}
	}

	// Initialize comp_accessing_systems.
	for (SystemId id = 0; id < get_num_systems(); ++id) {
		const AccessRequirements& requirements = system_create_infos[id].desc.access_requirements;
		requirements.get_accessing_comps().for_each([&](const CompId comp_id) {
			comp_accessing_systems[comp_id].add(id);
		});
	}

	task::init(num_worker_threads);

	// Construct the world.
	world.emplace(*this);
	world->run();

	task::deinit();
}