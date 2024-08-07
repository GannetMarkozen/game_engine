#pragma once

#include "archetype.hpp"
#include "threading/thread_safe_types.hpp"

namespace task { struct Task; }

namespace ecs {
struct App;
struct World;

struct SystemBase {
	virtual ~SystemBase() = default;
	virtual auto execute(World& world) -> void = 0;
};

enum class SystemState : u8 {
	STANDBY,
	WAITING,
	EXECUTING,
	COMPLETE,
};

struct World {
	explicit World(const App& app);

	auto run() -> void;

	auto dispatch_event(const EventId event) -> void;

	template <typename T>
	FORCEINLINE auto dispatch_event() -> void {
		dispatch_event(get_event_id<T>());
	}

	const App& app;

	Array<UniquePtr<SystemBase>> systems;// Indexed via SystemId.

	mutable RecursiveSharedMutex system_tasks_mutex;
	Array<WeakPtr<task::Task>> system_tasks;// Indexed via SystemId. May be NULL or completed.

	mutable RecursiveSharedMutex conflicting_executing_systems_mutex;
	SystemMask conflicting_executing_systems;// Mask of systems currently running. Only potentially set for systems with other conflicting systems.

	volatile bool is_pending_destruction = false;
};
}

#if 0
#pragma once

#include "archetype.hpp"
#include "bitmask.hpp"
#include "threading/thread_safe_types.hpp"
#include "bitmask.hpp"
#include "system.hpp"

namespace task {
struct Task;
}

namespace ecs {
struct World {
	struct ArchetypeInfo {
		UniquePtr<Archetype> archetype;
		Array<WeakPtr<task::Task>> enqueued_accessing_tasks;// All currently enqueued tasks that require access to this archetype.
		UniquePtr<RecursiveSharedMutex> enqueued_accessing_tasks_mutex;// Not relocatable so requires indirection.
	};

	World();


	// Thread-safe.
	auto get_matching_archetypes_mask(const DataRequirements& requirements, ArchetypeMask& out_mask, const bool requires_locking = true) const -> void {
		out_mask.mask.reset();

		UniqueSharedLock lock{requires_locking ? &archetypes_mutex : null};

		// Bitwise AND all component archetype masks to find the archetypes that have ALL the components we
		// are looking for, then iterate over the mask which should correspond to it's ArchetypeId.
		requirements.components.for_each([&](const CompId comp) {
			if (out_mask.mask.is_empty()) {
				out_mask = comp_archetypes_set[comp];
			} else {
				out_mask &= comp_archetypes_set[comp];
			}
		});
	}

	// Thread-safe.
	auto for_each_matching_archetype(const DataRequirements& requirements, cpts::Invokable<ArchetypeId> auto&& fn) -> void {
		static thread_local constinit ArchetypeMask matching_archetypes_mask;
		get_matching_archetypes_mask(requirements, matching_archetypes_mask);

		matching_archetypes_mask.for_each([&](const ArchetypeId id) {
			std::invoke(fn, id);
		});
	}

	// Thread-safe.
	[[nodiscard]] auto find_archetype_id(const ArchetypeDesc& desc, const bool requires_locking = true) const -> Optional<ArchetypeId> {
		UniqueSharedLock lock{requires_locking ? &archetypes_mutex : null};

		const auto it = archetype_desc_to_id_map.find(desc);
		if (it != archetype_desc_to_id_map.end()) {
			return {it->second};
		} else {
			return {};
		}
	}

	// Thread-safe.
	[[nodiscard]] auto find_or_create_archetype(const ArchetypeDesc& desc) -> ArchetypeId {
		if (const auto id = find_archetype_id(desc)) [[likely]] {
			return *id;
		} else {// @TODO: Make this branch NOINLINE.
			ScopeLock lock{archetypes_mutex};

			// Check again. Another thread could have potentially created the archetype when the previous shared lock was released.
			if (const auto id = find_archetype_id(desc, false)) {
				return *id;
			}

			const usize index = archetypes.size();
			ASSERTF(index <= ArchetypeId::max(), "Archetype index {} exceeds max value of ArchetypeId {}!", index, ArchetypeId::max().get_value());

			archetypes.push_back(ArchetypeInfo{
				.archetype = std::make_unique<Archetype>(desc),
				.enqueued_accessing_tasks_mutex = std::make_unique<RecursiveSharedMutex>(),
			});

			ArchetypeId out{static_cast<u16>(index)};

			archetype_desc_to_id_map[desc] = out;

			// Set component masks.
			desc.comps.for_each([&](const CompId id) {
				comp_archetypes_set[id].add(out);
			});

			return out;
		}
	}

	auto enqueue_task(SharedPtr<task::Task> task, const DataRequirements& requirements) -> void;

	mutable SharedMutex archetypes_mutex;
	Array<ArchetypeMask> comp_archetypes_set;// Indexed via CompId. Map of mask of all archetypes storing this component.
	Array<ArchetypeInfo> archetypes;// Indexed via ArchetypeId. All archetypes. Indexes are stable. Write locked for adding new archetypes.
	Map<ArchetypeDesc, ArchetypeId> archetype_desc_to_id_map;
};
}
#endif