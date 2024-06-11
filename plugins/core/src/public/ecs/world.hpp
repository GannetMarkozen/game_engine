#pragma once

#include "concurrency/thread_safe_types.hpp"
#include "types.hpp"
#include "entity.hpp"
#include "ids.hpp"
#include "component.hpp"
#include "archetype.hpp"

namespace core {
struct Task;
}

namespace core::ecs {
struct ComponentMask;
struct Archetype;
struct App;
struct SystemBase;

struct FreeEntityRecord {
	// No value if this is the last free entity in the list.
	Optional<u32> next_free_entity_index;
};

struct EntityDeferredCreateInfo {
	ComponentMask components;
};

struct EntityInfo {
	ArchetypeId archetype;
	u32 index_within_archetype;
};

// Stores entity archetype information and tracks unused entity indices for recycling.
struct EntityRecords {
private:
	// There is no entity for this index. All this serves to do is point to the next free entity index to make reserving entity indices fast.
	struct Uninitialized {
		Optional<u32> next_free_index;
	};

	// Entity has been reserved but not constructed yet.
	struct PendingConstruction {};

	struct Record {
		Variant<Uninitialized, PendingConstruction, EntityInfo> state{std::in_place_type<Uninitialized>};
		u32 version = 0;
	};

	static constexpr usize CHUNK_SIZE = 128 * 1024;
	static constexpr usize NUM_RECORDS_PER_CHUNK = CHUNK_SIZE / sizeof(Record);

	struct RecordsChunk {
		RecordsChunk() {// @NOTE: More expensive than necessary because it's double initializing each index but it's simpler this way.
			for (u32 i = 0; i < NUM_RECORDS_PER_CHUNK - 1; ++i) {
				records[i].state.get<Uninitialized>().next_free_index = i + 1;
			}
		}

		// Marked as union just so it doesn't get default-initialized.
		alignas(CACHE_LINE_SIZE) Record records[NUM_RECORDS_PER_CHUNK];
	};

public:
	[[nodiscard]]
	FORCEINLINE fn get_entity_record(const std::integral auto index) const -> Record& {
		return chunks[index / NUM_RECORDS_PER_CHUNK]->records[index % NUM_RECORDS_PER_CHUNK];
	}

	[[nodiscard]]
	fn is_entity_valid(const Entity entity) const -> bool {
		return entity.index < chunks.size() * NUM_RECORDS_PER_CHUNK && entity.version == get_entity_record(entity.index).version;
	}

	[[nodiscard]]
	fn is_entity_constructed(const Entity entity) const -> bool {
		ASSERT(is_entity_valid(entity));
		return get_entity_record(entity.index).state.is<EntityInfo>();
	}

	// Reserves an entity identifier.
	[[nodiscard]]
	fn reserve_entity() -> Entity {
		if (!next_record_free_index) [[unlikely]] {// If there is currently no available entity indices, allocate more records.
			const auto old_chunks_count = chunks.size();
			chunks.push_back(std::make_unique<RecordsChunk>());
			next_record_free_index = old_chunks_count * NUM_RECORDS_PER_CHUNK;// Point to the first element of the newly allocated chunk.
			ASSERT(next_record_free_index.has_value());
		}

		Record& record = get_entity_record(*next_record_free_index);

		const u32 entity_index = *next_record_free_index;
		next_record_free_index = record.state.get<Uninitialized>().next_free_index;

		// Entity should be pending construction now.
		record.state.emplace<PendingConstruction>();

		ASSERT(record.version != UINT32_MAX);
		return Entity{entity_index, ++record.version};
	}

	// Finally constructs the entity. The entity is invalid to use before this point.
	fn construct_entity(const Entity entity, EntityInfo info) const -> void {
		ASSERTF(is_entity_valid(entity), "Entity index == {}. Version == {}. Current version == {}.", entity.index, entity.version, get_entity_record(entity.index).version);
		ASSERT(!is_entity_constructed(entity));

		get_entity_record(entity.index).state.set(std::move(info));
	}

	// Frees an entity identifier.
	fn free_entity(const Entity entity) -> void {
		ASSERT(is_entity_valid(entity));
		ASSERTF(get_entity_record(entity.index).state.is<EntityInfo>(),
			"Entity must be fully constructed in order to free! Entity was in {} state!", get_entity_record(entity.index).state.match([]<typename T>() { return get_type_name<T>(); }));

		if (!next_record_free_index || *next_record_free_index > entity.index) {// Set new head.
			get_entity_record(entity.index).state.set(Uninitialized{
				.next_free_index = next_record_free_index,
			});

			next_record_free_index = entity.index;
		} else {// Insert into the list.
			u32 free_index = *next_record_free_index;
			while (true) {
				Optional<u32>& next_free_index = get_entity_record(free_index).state.get<Uninitialized>().next_free_index;
				if (!next_free_index || *next_free_index > entity.index) {
					get_entity_record(entity.index).state.set(Uninitialized{
						.next_free_index = next_free_index,
					});

					next_free_index = entity.index;
					break;
				}

				free_index = *next_free_index;
			}
		}
	}

	[[nodiscard]]
	fn operator[](const std::integral auto index) const -> EntityInfo& {
		ASSERTF(index < chunks.size() * NUM_RECORDS_PER_CHUNK, "Attempted to access index {} from array of size {}!", index, chunks.size() * NUM_RECORDS_PER_CHUNK);
		return get_entity_record(index).info;
	}

private:
	Array<UniquePtr<RecordsChunk>> chunks;
	Optional<u32> next_record_free_index;
};

struct World {
	NON_COPYABLE(World);

	// Constructs systems and determines system ordering.
	explicit World(const App& app);

	struct System {
		SharedPtr<Task> task;
		UniquePtr<SystemBase> system;
		Array<u32> subsequents;// System indices that are enqueued once this system has completed executing.
		Array<u32> contentious_systems;// System indices not allowed to run in-parallel with this one.
	};

	// Thread-safe.
	// Reserves an entity identifier but doesn't do any of the construction. Okay to be called from systems.
	[[nodiscard]]
	fn reserve_entity() -> Entity {
		return entity_records.write([&](EntityRecords& entity_records) {
			return entity_records.reserve_entity();
		});
	}

	// Thread-safe but blocks while the archetype is in-use so not ideal (and can deadlock if spawned on the same thread the archetype is targeted for is active).
	template<typename... Components>
	requires (sizeof...(Components) > 0)
	fn construct_entity(const Entity entity, Components&&... components) -> void {
		const ArchetypeId archetype = find_or_add_archetype(ComponentMask::make<std::decay_t<Components>...>());

		const u32 index_within_archetype = get_archetype(archetype).write([&](Archetype& archetype) {
			return archetype.emplace_entity(entity, std::forward<Components>(components)...);
		});

		entity_records.read([&](const EntityRecords& records) {
			// Technically not thread-safe but the same entity should never be
			// constructed from multiple threads so this should be a non-issue.
			records.construct_entity(entity, EntityInfo{
				.archetype = archetype,
				.index_within_archetype = index_within_archetype,
			});
		});
	}

	// Thread-safe but blocks while the archetype is in-use so not ideal (and can deadlock if spawned on the same thread the archetype is targeted for is active).
	template<typename... Components>
	requires (sizeof...(Components) > 0)
	fn spawn_entity_immediate(Components&&... components) -> Entity {
		const Entity entity = reserve_entity();
		construct_entity(entity, std::forward<Components>(components)...);
		return entity;
	}

	// Thread-safe.
	[[nodiscard]]
	FORCEINLINE fn get_archetype(const ArchetypeId archetype) const -> RWLocked<Archetype>& {
		return archetypes.read([&](const auto& archetypes) -> RWLocked<Archetype>& {
			return *archetypes[archetype];
		});
	}

	// Thread-safe.
	[[nodiscard]]
	fn find_or_add_archetype(const ComponentMask& mask) -> ArchetypeId {
		const auto try_find_archetype = [&](const Array<UniquePtr<RWLocked<Archetype>>>& archetypes) -> Optional<ArchetypeId> {
			for (usize i = 0; i < archetypes.size(); ++i) {
				if (archetypes[i]->read([&](const Archetype& archetype) { return archetype.component_mask == mask; })) {
					return ArchetypeId{static_cast<u16>(i)};
				}
			}

			return {};
		};

		// First try finding the archetype through shared read access (ideal).
		Optional<ArchetypeId> archetype = archetypes.read([&](const auto& archetypes) {
			return try_find_archetype(archetypes);
		});

		if (!archetype) [[unlikely]] {
			// Create the archetype if one doesn't already exist through an exclusive write lock (should only happen once per archetype).
			archetype = archetypes.write([&](auto& archetypes) -> ArchetypeId {
				// Try find an existing archetype again since it could've gotten created right before this lock.
				if (const auto archetype = try_find_archetype(archetypes)) {
					return *archetype;
				}

				ASSERT(archetypes.size() < UINT16_MAX);
				archetypes.push_back(std::make_unique<RWLocked<Archetype>>(mask));
				return ArchetypeId{static_cast<u16>(archetypes.size() - 1)};
			});
		}

		return *archetype;
	}

	fn get_entity_archetype(const Entity entity) const -> ArchetypeId {
		return entity_records.read([&](const EntityRecords& records) { return records.get_entity_record(entity.index).state.get<EntityInfo>().archetype; });
	}

	RWLocked<EntityRecords> entity_records;
	RWLocked<Array<UniquePtr<RWLocked<Archetype>>>> archetypes;

	// Commands with world-access that will be run at sync-points.
	ThreadLocal<Array<Fn<void(World&)>>> deferred_commands;

	Array<System> systems;

	// Whether or not systems are currently active.
	bool is_processing = false;
};
}