#pragma once

#include "archetype.hpp"
#include "bitmask.hpp"
#include "entity_list.hpp"
#include "threading/thread_safe_types.hpp"
#include "bitmask.hpp"

namespace task {
struct Task;
}

namespace ecs {
struct World {
	struct ArchetypeInfo {
		UniquePtr<Archetype> archetype;
		Array<SharedPtr<task::Task>> enqueued_accessing_tasks;// All currently enqueued tasks that require access to this archetype.
		UniquePtr<RecursiveSharedMutex> enqueued_accessing_tasks_mutex;// Not relocatable so requires indirection.
	};

	World();

	auto for_each_matching_archetype(const CompMask& comps, cpts::Invokable<ArchetypeId> auto&& fn) -> void {
		// Bitwise AND all component archetype masks to find the archetypes that have ALL the components we
		// are looking for, then iterate over the mask which should correspond to it's ArchetypeId.
		BitMask<> matching_archetypes_mask;
		comps.for_each([&](const CompId comp) {
			if (matching_archetypes_mask.is_empty()) {
				matching_archetypes_mask = comp_archetypes_set[comp];
			} else {
				matching_archetypes_mask &= comp_archetypes_set[comp];
			}
		});

		matching_archetypes_mask.for_each_set_bit([&](const usize i) {
			std::invoke(fn, ArchetypeId{static_cast<u16>(i)});
		});
	}

	// Thread-safe.
	[[nodiscard]] auto find_archetype_id(const ArchetypeDesc& desc) const -> Optional<ArchetypeId> {
		ScopeSharedLock lock{archetypes_mutex};

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
			if (const auto id = find_archetype_id(desc)) {
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

			return out;
		}
	}

	Array<BitMask<>> comp_archetypes_set;// Indexed via CompId. Map of mask of all archetypes storing this component.

	mutable SharedMutex archetypes_mutex;
	Array<ArchetypeInfo> archetypes;// Indexed via ArchetypeId. All archetypes. Indexes are stable. Write locked for adding new archetypes.
	Map<ArchetypeDesc, ArchetypeId> archetype_desc_to_id_map;
};
}