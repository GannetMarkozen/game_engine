#include "ecs/app.hpp"

namespace core::ecs {
fn App::run(const usize num_worlds) -> void {
	ASSERT(num_worlds > 0);

	worlds.reserve(num_worlds);
	for (usize i = 0; i < num_worlds; ++i) {
		worlds.push_back(std::make_unique<World>(*this));
	}

}

}