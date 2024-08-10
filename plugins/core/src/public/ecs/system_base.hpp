#pragma once

namespace ecs {
struct World;

struct SystemBase {
	virtual ~SystemBase() = default;
	virtual auto execute(World& world) -> void = 0;
};

}