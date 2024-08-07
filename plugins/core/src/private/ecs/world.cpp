#include "ecs/world.hpp"
#include "ecs/app.hpp"
#include "threading/task.hpp"

namespace ecs {
World::World(const App& app)
	: app{app} {
	systems.reserve(get_num_systems());
	system_tasks.resize(get_num_systems());

	// Create systems.
	for (const auto& create_info : app.system_create_infos) {
		systems.push_back(create_info.factory());
	}
}

auto World::run() -> void {
	ASSERT(thread::is_in_main_thread());

	dispatch_event(get_event_id<event::OnInit>());

	task::do_work([&] { return is_pending_destruction; });
}

auto World::dispatch_event(const EventId event) -> void {
	const auto& event_systems = app.event_systems[event];

	Array<SharedPtr<task::Task>> tmp_tasks;// Temporary allocation to keep task references alive.
	tmp_tasks.reserve(event_systems.mask.count_set_bits());

	Array<SharedPtr<task::Task>> event_prerequisites;

	Array<Array<SharedPtr<task::Task>>> system_prerequisites;
	system_prerequisites.reserve(event_systems.mask.count_set_bits());

	ScopeLock lock{system_tasks_mutex};

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
		// make a different variant of this lambda for non-conflicting systems.
		SharedPtr<task::Task> task = task::Task::make([this, id] {
			const SystemMask& conflicting_systems = app.concurrent_conflicting_systems[id];
			const bool has_conflicting_systems = !!conflicting_systems;
			if (has_conflicting_systems) {
				while (true) {
					{
						ScopeSharedLock lock{conflicting_executing_systems_mutex};
						if (const SystemMask executing_conflicting_systems = conflicting_systems.mask & conflicting_executing_systems.mask.atomic_clone()) {
							// @NOTE: Having to construct this array may be sort of bad performance-wise. busy_wait_for_tasks_to_complete should potentially take in an iterator instead of Span.
							Array<SharedPtr<task::Task>> executing_conflicting_tasks;
							executing_conflicting_tasks.reserve(executing_conflicting_systems.mask.count_set_bits());

							executing_conflicting_systems.for_each([&, this_id = id](const SystemId id) {
								auto task = system_tasks[id].lock();
								if (task && !task->has_completed()) {
									executing_conflicting_tasks.push_back(std::move(task));
								}

								// @TMP: Just for debugging for now.
								WARN("{} is executing while {} attempted to execute. Waiting for completion",
									TypeRegistry<SystemId>::get_type_info(id).name, TypeRegistry<SystemId>::get_type_info(this_id).name);
							});

							task::busy_wait_for_tasks_to_complete(executing_conflicting_tasks);
						}
					}

					ScopeLock lock{conflicting_executing_systems_mutex};

					// Check again that no conflicting tasks are executing after acquiring the exclusive lock. It's possible
					// that another conflicting task began executing as the previous lock was released and before this lock was acquired.
					if (!(conflicting_systems.mask & conflicting_executing_systems.mask.atomic_clone())) [[likely]] {
						conflicting_executing_systems.mask[id.get_value()].atomic_set(true);
						break;
					}
				}
			}

			systems[id]->execute(*this);

			if (has_conflicting_systems) {
				// No need for a lock here. Clearing a system can't cause race-conditions.
				conflicting_executing_systems.mask[id.get_value()].atomic_set(false);
			}
		}, desc.priority, desc.thread);

		system_tasks[id] = task;
		tmp_tasks.push_back(std::move(task));// system_tasks will not keep these SharedPtrs alive since it uses WeakPtrs! Need this tmp allocation until referenced.
	});

	// Assign prerequisites / subsequents.
	event_systems.for_each([&](const SystemId id) {
		Array<SharedPtr<task::Task>> prerequisites;

		const GroupId group = app.system_create_infos[id].desc.group;
		app.group_prerequisites[group].for_each([&](const GroupId prerequisite_group) {
			app.group_systems[prerequisite_group].for_each([&](const SystemId prerequisite_system) {
				auto prerequisite_task = system_tasks[prerequisite_system].lock();
				if (prerequisite_task && !prerequisite_task->has_completed()) {
					prerequisites.push_back(std::move(prerequisite_task));
				}
			});
		});

		app.group_subsequents[group].for_each([&](const GroupId subsequent_group) {
			app.group_systems[subsequent_group].for_each([&](const SystemId subsequent_system) {
				auto subsequent_task = system_tasks[subsequent_system].lock();
				if (subsequent_task && !subsequent_task->has_completed()) {
					system_tasks[id].lock()->add_subsequent(std::move(subsequent_task));
				}
			});
		});

		if (app.event_root_groups[event].has(group)) {
			prerequisites.append_range(event_prerequisites);
		}

		system_prerequisites.push_back(std::move(prerequisites));
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

#if 0
auto World::dispatch_event(const EventId event) -> void {
	using namespace task;

	const auto& system_mask = app.event_systems[event];

	Array<SharedPtr<Task>> tmp_tasks;// Temporary allocation to keep task references alive.
	tmp_tasks.reserve(system_mask.mask.count_set_bits());

	// Final tasks within this group mask. Needed to enqueue new event after the current event (if there is one) finishes.
	SystemMask leaf_systems_in_group;
	app.leaf_groups.for_each([&](const GroupId group_id) {
		leaf_systems_in_group |= app.group_systems[group_id];
	});
	leaf_systems_in_group &= app.event_systems[event];

	// Acquire lock before touching tasks.
	// @NOTE: Possible to make this mutex more granular but not sure if that's required.
	ScopeLock lock{system_tasks_mutex};

	Array<SharedPtr<Task>> existing_leaf_tasks;
	leaf_systems_in_group.for_each([&](const SystemId id) {
		auto existing_task = system_tasks[id].lock();
		if (existing_task && !existing_task->has_completed()) {
			existing_leaf_tasks.push_back(std::move(existing_task));
		}
	});

	// Construct tasks.
	system_mask.for_each([&](const SystemId id) {
		const SystemDesc& desc = app.system_create_infos[id].desc;

		Array<SharedPtr<Task>> concurrent_conflicting_tasks;
		if (const usize count = app.concurrent_conflicting_systems[id].mask.count_set_bits()) {
			concurrent_conflicting_tasks.reserve(count);

			app.concurrent_conflicting_systems[id].for_each([&](const SystemId id) {
				auto task = system_tasks[id].lock();
				if (task && !task->has_completed()) {
					concurrent_conflicting_tasks.push_back(std::move(task));
				}
			});
		}

		SharedPtr<Task> task = Task::make([this, id, concurrent_conflicting_tasks = std::move(concurrent_conflicting_tasks)](const SharedPtr<Task>& this_task) {
			// @TODO: Make it so that conflicting systems can not run at the same time.

			systems[id]->execute(*this);
		}, desc.priority, desc.thread);

		system_tasks[id] = task;
		tmp_tasks.push_back(std::move(task));
	});

	// Assign subsequents and enqueue.
	system_mask.for_each([&](const SystemId id) {
		auto task = system_tasks[id].lock();
		ASSERT(task);

		Array<SharedPtr<Task>> prerequisites;

		// Assign prerequisites.
		const auto group = app.system_create_infos[id].desc.group;
		app.group_prerequisites[group].for_each([&](const GroupId prerequisite_group) {
			app.group_systems[prerequisite_group].for_each([&](const SystemId prerequisite_system) {
				auto prerequisite_task = system_tasks[prerequisite_system].lock();
				if (prerequisite_task && !prerequisite_task->has_completed()) {
					prerequisites.push_back(std::move(prerequisite_task));
				}
			});
		});

		// Assign subsequents. Skip systems registered to this event though, they will be handled through prerequisites above.
		// @NOTE: May be cheaper to assign subsequents rather than prerequisites for enqueueing systems.
		app.group_subsequents[group].for_each([&](const GroupId subsequent_group) {
			(app.group_systems[subsequent_group] & ~system_mask).for_each([&](const SystemId subsequent_system) {
				auto subsequent_task = system_tasks[subsequent_system].lock();
				if (subsequent_task && !subsequent_task->has_completed()) {
					task->add_subsequent(std::move(subsequent_task));
				}
			});
		});

		prerequisites.append_range(existing_leaf_tasks);

		// Enqueue task.
		const auto priority = task->priority;
		const auto thread = task->thread;
		enqueue(std::move(task), priority, thread, prerequisites);
	});
}
#endif
}

#if 0
#include "ecs/world.hpp"
#include "threading/task.hpp"

namespace ecs {
World::World() {
	comp_archetypes_set.resize(TypeRegistry<CompId>::get_num_registered_types());
}

auto World::enqueue_task(SharedPtr<task::Task> task, const DataRequirements& requirements) -> void {
	ASSERT(task);

	{
		ScopeSharedLock lock{archetypes_mutex};

		for_each_matching_archetype(requirements, [&](const ArchetypeId id) {
			ScopeLock lock{*archetypes[id].enqueued_accessing_tasks_mutex};

			archetypes[id].enqueued_accessing_tasks.push_back(task);
		});
	}

	
}
}
#endif