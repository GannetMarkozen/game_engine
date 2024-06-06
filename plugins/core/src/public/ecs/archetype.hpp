#pragma once

#include "component.hpp"

namespace core::ecs {
struct Archetype {
	// This is the size that Mass uses.
	static constexpr usize CHUNK_SIZE = 128 * 1024;

	struct Chunk {
		alignas(CACHE_LINE_SIZE) u8 buffer[CHUNK_SIZE];
	};

	struct ComponentData {
		ComponentId id;
		usize offset_into_chunk;
	};

	ComponentMask component_mask;
	Array<UniquePtr<Chunk>> chunks;
};
}