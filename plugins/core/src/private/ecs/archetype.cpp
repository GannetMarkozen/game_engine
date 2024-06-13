#include "ecs/archetype.hpp"
#include "ecs/component.hpp"

namespace ecs {
Archetype::Archetype(ComponentMask in_component_mask)
	: component_mask{std::move(in_component_mask)} {
	Array<ComponentId> ids;
	component_mask.for_each_component([&](const ComponentId id) {
		// Don't include tag-components. They don't have any storage.
		if (!get_component_info(id).is_tag) {
			ids.push_back(id);
		}
	});

	// Sort components by alignment to reduce padding.
	std::sort(ids.begin(), ids.end(), [](const auto& a, const auto& b) {
		return get_component_info(a).alignment > get_component_info(b).alignment;
	});

	usize total_padding = 0;
	usize total_structure_size = 0;
	Array<usize> component_padding;

	for (usize i = 0; i < ids.size(); ++i) {
		const auto& id = ids[i];
		const auto& info = get_component_info(id);
		ASSERT(info.alignment <= CACHE_LINE_SIZE);

		const usize current_alignment = i == 0 ? CACHE_LINE_SIZE : get_component_info(ids[i - 1]).alignment;
		const usize padding = math::align(info.alignment, current_alignment) - info.alignment;
		total_padding += padding;
		component_padding.push_back(padding);

		total_structure_size += info.size;
	}

	const usize entity_padding = math::align(alignof(u32), get_component_info(ids.back()).alignment);
	total_padding += entity_padding;
	total_structure_size += sizeof(u32);

	num_entities_per_chunk = (CHUNK_SIZE - total_padding) / total_structure_size;
	ASSERT(num_entities_per_chunk > 0);

	usize current_offset = 0;
	components_info.reserve(ids.size());
	for (usize i = 0; i < ids.size(); ++i) {
		components_info.push_back(ComponentInfo{
			.id = ids[i],
			.offset_into_chunk = current_offset,
		});
		current_offset += get_component_info(ids[i]).size * num_entities_per_chunk + component_padding[i];
	}

	entities_offset_into_chunk = current_offset;
	current_offset += entity_padding + num_entities_per_chunk * sizeof(u32);

	ASSERTF(current_offset <= CHUNK_SIZE, "Offset == {}. CHUNK_SIZE == {}.", current_offset, CHUNK_SIZE);
}

fn Archetype::add_uninitialized_entities(const Span<const Entity> entities) -> u32 {
	ASSERT(entities.size() > 0);

	const u32 old_num_entities = num_entities;
	num_entities += entities.size();

	const u32 new_num_chunks = math::divide_and_round_up(num_entities, num_entities_per_chunk);
	if (new_num_chunks > chunks.size()) [[unlikely]] {
		chunks.resize(new_num_chunks);
	}

	// Initialize the entity values in the buffer.
	u32 previous_count = 0;
	for_each_entity_chunk_in_range(old_num_entities, entities.size(), [&](u32* entity_index, const u32 count) {
		for (u32 i = 0; i < count; ++i) {
			entity_index[i] = entities[i + previous_count].index;
		}
		previous_count += count;
	});

	return old_num_entities;
}
}