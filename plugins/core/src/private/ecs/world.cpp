#include "ecs/world.hpp"

namespace ecs {
World::World() {
	comp_archetypes_set.resize(TypeRegistry<CompId>::get_num_registered_types());
}
}