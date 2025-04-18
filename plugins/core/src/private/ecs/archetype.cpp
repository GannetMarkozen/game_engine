#include "ecs/archetype.hpp"

Archetype::Archetype(const ArchetypeDesc& in_description)
	: description{in_description} {
	usize aggregate_size = 0;
	description.comps.for_each([&](const CompId id) {
		if (!!(get_type_info(id).flags & rtti::Flags::EMPTY)) {// Tag type. No storage for tags.
			return;
		}

		comps.push_back({
			.offset_within_chunk = 0,
			.id = id,
		});
		aggregate_size += get_type_info(id).size;
	});
	comps.shrink_to_fit();

	aggregate_size += sizeof(Entity);

	num_entities_per_chunk = BYTES_PER_CHUNK / aggregate_size;

	// First sort by alignment (high -> low) to reduce padding.
	std::sort(comps.begin(), comps.end(), [](const CompInfo& a, const CompInfo& b) {
		return get_type_info(a.id).alignment > get_type_info(b.id).alignment;
	});

	// Keep recalculating offsets until alignment / size requirements are met. Not necessarily optimal.
	do {
		usize aggregate_offset = 0;
		for (CompInfo& comp : comps) {
			comp.offset_within_chunk = aggregate_offset;
			aggregate_offset += get_type_info(comp.id).size * num_entities_per_chunk;
		}

		entity_offset_within_chunk = math::align(aggregate_offset, alignof(Entity));
	} while (entity_offset_within_chunk + num_entities_per_chunk * sizeof(Entity) > BYTES_PER_CHUNK);

	// Keep testing alignment constraints and shrinking the number of entities until met.
	#if 0
	while (true) {
		usize aggregate_alignment_padding = 0;
		for (usize i = 1; i < comps.size(); ++i) {
			const usize previous_offset = comps[i - 1].offset_within_chunk;
			const usize unaligned_offset = previous_offset + get_type_info(comps[i - 1].id).size * num_entities_per_chunk;
			const usize offset = math::align(unaligned_offset, get_type_info(comps[i].id).alignment);
			aggregate_alignment_padding += offset - unaligned_offset;

			comps[i].offset_within_chunk = offset;
		}

		const usize previous_offset = comps.back().offset_within_chunk;
		const usize unaligned_offset = previous_offset + get_type_info(comps.back().id).size * num_entities_per_chunk;
		entity_offset_within_chunk = math::align(unaligned_offset, alignof(Entity));
		aggregate_alignment_padding += entity_offset_within_chunk - unaligned_offset;

		const usize aligned_num_entities_per_chunk = (BYTES_PER_CHUNK - aggregate_alignment_padding) / aggregate_size;
		ASSERT(aligned_num_entities_per_chunk <= num_entities_per_chunk);

		if (aligned_num_entities_per_chunk == num_entities_per_chunk) {
			break;
		} else {
			--num_entities_per_chunk;
		}
	}
	#endif
}

Archetype::~Archetype() {
	for_each_chunk([&](Chunk& chunk, const usize count) {
		for (const auto& comp : comps) {
			const auto& type_info = get_type_info(comp.id);
			if (!!(type_info.flags & rtti::Flags::TRIVIAL)) {
				continue;
			}

			type_info.destruct(&chunk.data[comp.offset_within_chunk], count);
		}
	});
}

auto Archetype::add_uninitialized_entities(const usize count) -> usize {
	ASSERT(count > 0);

	const usize old_num_entities = num_entities;
	num_entities += count;

	const usize old_num_chunks = math::divide_and_round_up(old_num_entities, num_entities_per_chunk);
	const usize num_chunks = math::divide_and_round_up(num_entities, num_entities_per_chunk);
	const usize add_num_chunks = num_chunks - old_num_chunks;

	if (add_num_chunks > 0 && num_chunks > 1) [[unlikely]] {
		Chunk* chunk = &get_chunk(std::max<isize>(static_cast<isize>(old_num_chunks) - 1, 0));
		for (usize i = 0; i < add_num_chunks; ++i) {
			chunk->next = std::make_unique<Chunk>();
			chunk = chunk->next.get();
		}
	}

	return old_num_entities;
}