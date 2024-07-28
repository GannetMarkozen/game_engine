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