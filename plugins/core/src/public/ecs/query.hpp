#pragma once

#include "world.hpp"

namespace ecs {
struct Query {
	template <typename... Comps>
	FORCEINLINE auto for_each_view(const World& world, cpts::Invokable<usize, const Entity*, Comps*...> auto&& fn) -> void {
		ASSERTF((read_comp_mask | write_comp_mask) == CompMask::make<std::decay_t<Comps>...>(),
			"Query iteration does not include all read and write components! Potential bug. Use includes / excludes if this was intentional.");

		{
			// @TODO: Make checking archetypes count some sort of atomic operation so we don't need to grab a lock every time a query is ran.
			// (Although shared mutex locks are pretty cheap).
			ScopeSharedLock lock{world.archetypes_mutex};

			if (previous_num_archetypes != world.archetypes.size()) [[unlikely]] {
				ASSERT(previous_num_archetypes < world.archetypes.size());

				update_matching_archetypes(world);
			}
		}

		for (Archetype* archetype : matching_archetypes) {
			archetype->for_each_view<Comps...>(FORWARD_AUTO(fn));
		}
	}

	template <typename... Comps>
	FORCEINLINE auto for_each(const World& world, cpts::Invokable<usize, const Entity&, Comps&...> auto&& fn) -> void {
		for_each_view<Comps...>(world, [&](const usize count, const Entity* entities, Comps*... comps) {
			for (usize i = 0; i < count; ++i) {
				std::invoke(FORWARD_AUTO(fn), entities[i], comps[i]...);
			}
		});
	}

	NOINLINE auto update_matching_archetypes(const World& world) -> void {
		matching_archetypes.clear();

		ArchetypeMask matching_archetypes_mask;
		(read_comp_mask | write_comp_mask | include_comp_mask).for_each([&](const CompId id) {
			matching_archetypes_mask |= world.comp_archetypes_set[id];
		});

		exclude_comp_mask.for_each([&](const CompId id) {
			matching_archetypes_mask &= ~world.comp_archetypes_set[id];
		});
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<Comps> && ...))
	FORCEINLINE auto reads() -> Query& {
		(read_comp_mask.add<Comps>(), ...);
		return *this;
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<Comps> && ...))
	FORCEINLINE auto writes() -> Query& {
		(write_comp_mask.add<Comps>(), ...);
		return *this;
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0)
	FORCEINLINE auto includes() -> Query& {
		(include_comp_mask.add<Comps>(), ...);
		return *this;
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0)
	FORCEINLINE auto excludes() -> Query& {
		(exclude_comp_mask.add<Comps>(), ...);
		return *this;
	}

	u32 previous_num_archetypes = 0;
	Array<Archetype*> matching_archetypes;

	CompMask read_comp_mask, write_comp_mask, include_comp_mask, exclude_comp_mask;
};
}