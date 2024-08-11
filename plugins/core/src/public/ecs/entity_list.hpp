#pragma once

#include "entity.hpp"
#include "ids.hpp"
#include "types.hpp"

namespace ecs {
struct EntityDesc {
	ArchetypeId archetype_id;
	usize index_within_archetype;
};

struct EntityList {
	struct EntityDescOrNext {
		constexpr EntityDescOrNext()
			: next{NULL_OPTIONAL} {}

		union {
			EntityDesc desc;
			Optional<usize> next;
		};
	};

	static_assert(std::is_trivially_destructible_v<EntityDesc>);
	static_assert(std::is_trivially_destructible_v<Optional<u32>>);

	[[nodiscard]] auto reserve_entity() -> Entity {
		ASSERT(entries.size() == versions.size());
		ASSERT(entries.size() <= initialized_mask.size() * 64);

		if (first_free_entry) [[likely]] {
			const usize index = *first_free_entry;
			ASSERTF(!is_initialized(index), "next_free_index points to initialized index {}!", index);
			ASSERTF(versions[index] < UINT32_MAX, "Entity at index {} has reached the max version!", index);

			// Point the first_free_entry to the next available entry (or NULL_OPTIONAL).
			first_free_entry = entries[*first_free_entry].next;

			return Entity{static_cast<u32>(index), ++versions[index]};
		} else {
			const usize index = entries.size();

			entries.emplace_back();
			versions.push_back(0);
			if (entries.size() > initialized_mask.size() * 64) [[unlikely]] {
				initialized_mask.push_back(0);
			}

			return Entity{static_cast<u32>(index), 0};
		}
	}

	FORCEINLINE auto initialize_entity(const Entity entity, EntityDesc description) -> EntityDesc& {
		ASSERTF(is_valid_index(entity.get_index()), "Entity at index {} is not valid for EntityList of size {}!", entity.get_index(), entries.size());
		ASSERTF(versions[entity.get_index()] == entity.get_version(), "Entity version {} does not match the current version {} at index {}!", entity.get_version(), versions[entity.get_index()], entity.get_index());
		ASSERTF(!is_initialized(entity.get_index()), "Attempted to initialize Entity at index {} when it has already been initialized!", entity.get_index());

		initialized_mask[entity.get_index() / 64] |= 1ull << entity.get_index() % 64;
		return *new(&entries[entity.get_index()]) EntityDesc{std::move(description)};
	}

	auto remove_entity(const Entity entity) -> void {
		ASSERTF(is_entity_valid(entity), "Attempted to remove invalid entity! index: {}, version: {}", entity.get_index(), entity.get_version());

		if (!first_free_entry || entity.get_index() < *first_free_entry) {
			new(&entries[entity.get_index()]) Optional<usize>{first_free_entry};
			first_free_entry = entity.get_index();
		} else {
			Optional<usize> next_free_entry = first_free_entry;
			while (true) {
				const usize previous_free_entry = *next_free_entry;
				next_free_entry = entries[*next_free_entry].next;

				if (!next_free_entry || entity.get_index() < *next_free_entry) {
					new(&entries[entity.get_index()]) Optional<usize>{next_free_entry};
					entries[previous_free_entry].next = entity.get_index();
					break;
				}
			}
		}

		initialized_mask[entity.get_index() / 64] &= ~(1ull << entity.get_index() % 64);

		// @TODO: Deallocate.
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE auto get_entity_desc(this Self&& self, const Entity entity) -> decltype(auto) {
		ASSERTF(self.is_valid_entity(entity), "Invalid entity {{}}!", entity);
		return (std::forward_like<Self>(self.entries[entity.get_index()].desc));
	}

	[[nodiscard]] FORCEINLINE auto is_valid_index(const std::integral auto index) const -> bool {
		return index >= 0 && index < entries.size();
	}

	[[nodiscard]] FORCEINLINE auto is_initialized(const std::integral auto index) const -> bool {
		ASSERTF(is_valid_index(index), "Invalid index {} of size {}!", index, entries.size());
		return initialized_mask[index / 64] & 1ull << index % 64;
	}

	[[nodiscard]] FORCEINLINE auto is_entity_valid(const Entity entity) const -> bool {
		return is_valid_index(entity.get_index()) && versions[entity.get_index()] == entity.get_version() && is_initialized(entity.get_index());
	}

	Array<EntityDescOrNext> entries;
	Array<u32> versions;
	Array<u64> initialized_mask;
	Optional<usize> first_free_entry;
};
}