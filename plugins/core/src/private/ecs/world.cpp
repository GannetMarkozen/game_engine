#include "ecs/world.hpp"
#include "ecs/app.hpp"
#include "threading/task.hpp"

World::World(const App& app)
	: app{app} {
	systems.reserve(get_num_systems());
	system_tasks.resize(get_num_systems());

	comp_archetypes_mask.resize(get_num_comps());

	// Create resources.
	for (ResId id = 0; id < get_num_resources(); ++id) {
		resources[id] = app.resource_factories[id]();
	}

	// Create systems.
	for (const auto& create_info : app.system_create_infos) {
		systems.push_back(create_info.factory());
	}
}

World::~World() {
	// Destroy resources.
	for (ResId id = 0; id < get_num_resources(); ++id) {
		const auto& type_info = get_type_info(id);
		type_info.destruct(resources[id], 1);
		mem::dealloc(resources[id], type_info.alignment);
	}
}

auto World::run() -> void {
	ASSERT(thread::is_in_main_thread());

	dispatch_event<event::OnInit>();

	//task::do_work_until_all_tasks_complete([&] { return is_pending_destruction; });

	// @TMP: This will not work with multiple worlds. Need to make the task system automatically exit when all threads have no work to do.
	task::do_work_until_all_tasks_complete();

	task::pending_shutdown = true;
}

auto World::dispatch_event(const EventId event) -> void {
	const auto& event_systems = app.event_systems[event];

	Array<SharedPtr<Task>> tmp_tasks;// Temporary allocation to keep task references alive.
	tmp_tasks.reserve(event_systems.mask.count_set_bits());

	Array<SharedPtr<Task>> event_prerequisites;

	Array<Array<SharedPtr<Task>>> system_prerequisites;
	system_prerequisites.reserve(event_systems.mask.count_set_bits());

	ScopeLock _{system_tasks_mutex};

	app.event_system_prerequisites[event].for_each([&](const SystemId id) {
		auto task = system_tasks[id].lock();
		if (task && !task->has_completed()) {
			event_prerequisites.push_back(std::move(task));
		}
	});

	// Create systems.
	event_systems.for_each([&](const SystemId id) {
		const auto& desc = app.system_create_infos[id].desc;

		// @NOTE: Conflicting tasks logic is potentially too much. Stalling from this should be extremely rare though. Could
		// make a different variant of this lambda for non-conflicting
		SharedPtr<Task> task = Task::make([this, id](const SharedPtr<Task>& this_task) -> void {
#if 0
			const SystemMask& conflicting_systems = app.concurrent_conflicting_systems[id];
			const bool has_conflicting_systems = !!conflicting_systems;
			if (has_conflicting_systems) {
				while (true) {
					Array<SharedPtr<Task>> executing_conflicting_tasks;
					{
						ScopeSharedLock lock{conflicting_executing_systems_mutex};

						if (const SystemMask executing_conflicting_systems = conflicting_systems.mask & executing_systems.mask.atomic_clone()) {
							// @NOTE: Having to construct this array may be sort of bad performance-wise. busy_wait_for_tasks_to_complete should potentially take in an iterator instead of Span.
							executing_conflicting_tasks.reserve(executing_conflicting_systems.mask.count_set_bits());

							executing_conflicting_systems.for_each([&, this_id = id](const SystemId id) {
								auto task = system_tasks[id].lock();
								if (task && !task->has_completed()) {
									executing_conflicting_tasks.push_back(std::move(task));
								}

								// @TMP: Just for debugging for now.
								#if 0
								WARN("{} is executing while {} attempted to execute. Waiting for completion",
									TypeRegistry<SystemId>::get_type_info(id).name, TypeRegistry<SystemId>::get_type_info(this_id).name);
									#endif
							});
						}
					}

					// Re-enqueue this task and wait for the currently executing conflicting tasks to complete.
					if (!executing_conflicting_tasks.empty()) {
#if 0
						task::busy_wait_for_tasks_to_complete(executing_conflicting_tasks);
#else
						MpscQueue<SharedPtr<Task>> subsequents = [&] {
							ScopeExclusiveLock lock{this_task->subsequents_mutex};
							return std::move(this_task->subsequents);
						}();

						// Re-enqueue self.
						SharedPtr<Task> new_task = Task::make(std::move(this_task->fn), this_task->priority, this_task->thread, std::move(subsequents));

						{
							ScopeExclusiveLock lock{system_tasks_mutex};
							system_tasks[id] = new_task;
						}

						task::enqueue(std::move(new_task), this_task->priority, this_task->thread, executing_conflicting_tasks);
						return;
#endif
					}

					ScopeLock lock{conflicting_executing_systems_mutex};

					// Check again that no conflicting tasks are executing after acquiring the exclusive lock. It's possible
					// that another conflicting task began executing as the previous lock was released and before this lock was acquired.
					if (!(conflicting_systems.mask & executing_systems.mask.atomic_clone())) [[likely]] {
						// Needs to be atomic because this value will be reset without a lock.
						executing_systems.mask[id.get_value()].atomic_set(true);
						break;
					}
				}
			} else {
				executing_systems.mask[id.get_value()].atomic_set(true);
			}
#endif

			ExecContext context{
				.world = *this,
				.delta_time = 0.f,// @TODO: Do something with this.
				.currently_executing_system = id,
				.currently_executing_system_thread = std::this_thread::get_id(),
			};

			systems[id]->execute(context);

			// No need for a lock here. Clearing a system can't cause race-conditions.
			executing_systems.mask[id.get_value()].atomic_set(false);
		}, desc.priority, desc.thread, {}
#if ASSERTIONS_ENABLED
			, fmt::format("\"{}:{}:{}: {}\"", __FILE_NAME__, __LINE__, __builtin_COLUMN(), get_type_info(id).name)
#endif
		);

		system_tasks[id] = task;
		tmp_tasks.push_back(std::move(task));// system_tasks will not keep these SharedPtrs alive since it uses WeakPtrs! Need this tmp allocation until referenced.
	});

	// Assign prerequisites / subsequents / exclusives.
	event_systems.for_each([&](const SystemId id) {
		Array<SharedPtr<Task>> prerequisites;

		const GroupId group = app.system_create_infos[id].desc.group;
		app.group_subsequents[group].for_each([&](const GroupId subsequent_group) {
			app.group_systems[subsequent_group].for_each([&](const SystemId subsequent_system) {
				auto subsequent_task = system_tasks[subsequent_system].lock();
				if (subsequent_task && !subsequent_task->has_completed()) {
					const auto result = system_tasks[id].lock()->add_subsequent(std::move(subsequent_task));

					ASSERTF(app.system_create_infos[subsequent_system].desc.event != event || result == Task::AddSubsequentResult::SUCCESS, "Failed to add subsequent of task {} to task {}! Result == {}!",
						subsequent_system, id, [&] {
							switch (result) {
							case Task::AddSubsequentResult::SUCCESS: return "SUCCESS";
							case Task::AddSubsequentResult::SUBSEQUENT_ALREADY_COMPLETED: return "SUBSEQUENT_ALREADY_COMPLETED";
							case Task::AddSubsequentResult::SUBSEQUENT_ALREADY_EXECUTING: return "SUBSEQUENT_ALREADY_EXECUTING";
							case Task::AddSubsequentResult::PREREQUISITE_ALREADY_COMPLETED: return "PREREQUISITE_ALREADY_COMPLETED";
							}
						}());
				}
			});
		});

		if (app.event_root_groups[event].has(group)) {
			prerequisites.append_range(event_prerequisites);
		}

		system_prerequisites.push_back(std::move(prerequisites));

		// Schedule against exclusives.
		app.concurrent_conflicting_systems[id].for_each([&](const SystemId conflicting_id) {
			if (const auto conflicting_task = system_tasks[conflicting_id].lock()) {
				Task::add_exclusive(system_tasks[id].lock(), conflicting_task);
			}
		});
	});

	// Enqueue.
	usize count = 0;
	event_systems.for_each([&](const SystemId id) {
		auto task = system_tasks[id].lock();
		const auto priority = task->priority;
		const auto thread = task->thread;
		task::enqueue(std::move(task), priority, thread, system_prerequisites[count++]);
	});
}

auto World::find_archetype_id_assumes_locked(const ArchetypeDesc& desc) const -> Optional<ArchetypeId> {
	const auto it = archetype_desc_to_id.find(desc);
	if (it != archetype_desc_to_id.end()) {
		return it->second;
	} else {
		return NULL_OPTIONAL;
	}
}

auto World::find_archetype_id(const ArchetypeDesc& desc) const -> Optional<ArchetypeId> {
	ScopeSharedLock lock{archetypes_mutex};
	return find_archetype_id_assumes_locked(desc);
}

auto World::find_or_create_archetype_id(const ArchetypeDesc& desc) -> ArchetypeId {
	return find_or_create_archetype(desc).second;
}

auto World::find_archetype_assumes_locked(const ArchetypeDesc& desc) const -> Optional<Pair<Archetype&, ArchetypeId>> {
	if (const auto id = find_archetype_id_assumes_locked(desc)) {
		return {{*archetypes[*id], *id}};
	} else {
		return NULL_OPTIONAL;
	}
}

auto World::find_archetype(const ArchetypeDesc& desc, const bool assumes_locked) const -> Optional<Pair<Archetype&, ArchetypeId>> {
	UniqueSharedLock _{assumes_locked ? null : &archetypes_mutex};
	return find_archetype_assumes_locked(desc);
}

// @NOTE: Copied, so bad.
auto World::find_or_create_archetype(const ArchetypeDesc& desc, const bool assumes_write_locked) -> Pair<Archetype&, ArchetypeId> {
	if (auto pair = find_archetype(desc, assumes_write_locked)) {
		return *pair;
	}

	// @NOTE: This branch should be NOINLINE as it should rarely happen.
	UniqueExclusiveLock _{assumes_write_locked ? null : &archetypes_mutex};

	// Check again. The archetype could have potentially been created before the exclusive lock was acquired.
	if (auto pair = find_archetype_assumes_locked(desc)) {
		return *pair;
	}

	ASSERTF(archetypes.size() <= ArchetypeId::max(), "Attempted to create {} archetypes but ArchetypeId::max() == {}!",
		archetypes.size() + 1, ArchetypeId::max().get_value());

	const ArchetypeId id = archetypes.size();
	auto* archetype = new Archetype{desc};
	ASSERT(archetype);

	archetypes.emplace_back(archetype);

	archetype_desc_to_id[desc] = id;

	desc.comps.for_each([&](const CompId comp_id) {
		comp_archetypes_mask[comp_id].add(id);
	});

	return {*archetype, id};
}

auto World::get_accessing_systems(const ArchetypeDesc& desc) const -> SystemMask {
	SystemMask out;
	desc.comps.for_each([&](const CompId id) {
		out |= app.comp_accessing_systems[id];
	});
	return out;
}

auto World::get_system_access_requirements(const SystemId id) const -> const AccessRequirements& {
	return app.system_create_infos[id].desc.access_requirements;
}