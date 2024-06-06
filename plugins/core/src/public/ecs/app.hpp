#pragma once

#include "../defines.hpp"
#include "system.hpp"
#include "scheduler.hpp"

namespace core::ecs {
struct App;
struct World;

template<typename T>
concept Plugin = requires(T t, App app) {
	{ t.setup(app) };
};

struct App {
	fn add_plugin(Plugin auto&& plugin) -> App& {
		plugin.setup(*this);
		return *this;
	}

	fn add_system(SystemInfo system) -> App& {
		scheduler.schedule_system(std::move(system));
		return *this;
	}

private:
	Array<UniquePtr<World>> worlds;
	Scheduler scheduler;
};
}