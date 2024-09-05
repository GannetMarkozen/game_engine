#include "core_include.hpp"
#include "default_plugins.hpp"
#include "vulkan/vulkan_core.h"

#if 0
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
#endif

using res::RequestExit;

auto some_system(ExecContext& context, usize& count, i32& something, Query<const u64>& query, const Res<RequestExit> request_exit) -> void {
#if 0
	if (++count >= 10000) {
		request_exit->value = true;
	}
#endif
}

auto main() -> int {
	App::build()
		.register_plugin(DefaultPlugins{})
		.register_system<some_system>()
		.run();

	using Members = App::FnSystem<some_system>::Members;
	static constexpr auto THING = std::tuple_size_v<Members>;

	fmt::println("EXITING!");

	std::this_thread::sleep_for(std::chrono::seconds{1});
}