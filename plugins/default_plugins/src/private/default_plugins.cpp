#include "default_plugins.hpp"

#include "ecs/app.hpp"

auto DefaultPlugins::init(App& app) -> void {
	app
		.register_plugin(gameplay_framework)
		.register_plugin(vk);
}