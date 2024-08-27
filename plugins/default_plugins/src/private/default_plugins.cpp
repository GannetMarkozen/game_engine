#include "default_plugins.hpp"

#include "gameplay_framework.hpp"
#include "vk_plugin.hpp"

#include "ecs/app.hpp"

auto DefaultPlugins::init(App& app) const -> void {
	app
		.register_plugin(GameplayFrameworkPlugin{})
		.register_plugin(VkPlugin{});
}