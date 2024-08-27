#include "ecs/app.hpp"
#include "gameplay_framework.hpp"
#include "default_plugins.hpp"

struct SomeSystem {
	[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
		return {
			.resources{
				.writes = ResMask::make<res::RequestExit>(),
			},
		};
	}

	auto execute(ExecContext& context) -> void {
		fmt::println("Executing {}", count);

		if (++count >= 1000) {
			context.get_mut_res<res::RequestExit>().value = true;
		}
	}

	usize count = 0;
};

auto main() -> int {
	App::build()
		.register_plugin(DefaultPlugins{})
		.register_system<SomeSystem>()
		.run();
}