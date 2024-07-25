#pragma once

#include "archetype.hpp"
#include "entity_list.hpp"
#include "threading/thread_safe_types.hpp"

namespace ecs {
using ArchetypeSet = Set<UniquePtr<Archetype>,
	decltype([](const UniquePtr<Archetype>& value) { return std::hash<ArchetypeDesc>{}(value->description); }),
	decltype([](const UniquePtr<Archetype>& a, const UniquePtr<Archetype>& b) { return a->description == b->description; })>;

struct World {

	RWLocked<ArchetypeSet> archetypes;
};
}