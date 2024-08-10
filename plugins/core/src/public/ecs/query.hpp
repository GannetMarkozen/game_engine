#pragma once

#include "world.hpp"

namespace ecs {
template <typename... Comps> requires (!std::is_empty_v<Comps> && ...)
struct Query {
	FORCEINLINE auto for_each_view(const World& world, ::cpts::Invokable<usize, const Entity*, Comps*...> auto&& fn) -> void {
		{
			ScopeSharedLock lock{world.archetypes_mutex};

			if (previous_archetype_count != world.archetypes.size()) [[unlikely]] {
				update_matching_archetypes_assumes_locked(world);
			}
		}

		for (Archetype* archetype : matching_archetypes) {
			archetype->for_each_view<std::decay_t<Comps>...>(FORWARD_AUTO(fn));
		}
	}

	FORCEINLINE auto for_each(const World& world, ::cpts::Invokable<const Entity&, Comps&...> auto&& fn) -> void {
		for_each_view(world, [&](const usize count, const Entity* entities, Comps*... comps) {
			for (usize i = 0; i < count; ++i) {
				std::invoke(FORWARD_AUTO(fn), entities[i], comps[i]...);
			}
		});
	}

	NOINLINE inline auto update_matching_archetypes_assumes_locked(const World& world) -> void {
		ASSERT(previous_archetype_count < world.archetypes.size());

		matching_archetypes.clear();

		ArchetypeMask matching_archetypes_mask;
		bool is_matching_archetypes_mask_initialized = false;
		const auto aggregate_includes = CompMask::make<std::decay_t<Comps>...>() | includes;

		aggregate_includes.for_each([&](const CompId id) {
			if (matching_archetypes_mask.mask.is_empty()) {
				matching_archetypes_mask = world.comp_archetypes_mask[id];
			} else {
				matching_archetypes_mask &= world.comp_archetypes_mask[id];
			}
		});

		excludes.for_each([&](const CompId id) {
			matching_archetypes_mask &= ~world.comp_archetypes_mask[id];
		});

		matching_archetypes_mask.for_each([&](const ArchetypeId id) {
			matching_archetypes.push_back(world.archetypes[id].get());
		});

		previous_archetype_count = world.archetypes.size();
	}

	[[nodiscard]] auto get_access_requirements() const -> AccessRequirements {
		AccessRequirements out{
			.reads = includes,
		};

		([&] {
			if constexpr (std::is_const_v<Comps>) {
				out.reads.add<std::decay_t<Comps>>();
			} else {
				out.writes.add<std::decay_t<Comps>>();
			}
		}(), ...);

		return out;
	}

	const CompMask includes;
	const CompMask excludes;
	Array<Archetype*> matching_archetypes;
	usize previous_archetype_count = 0;
};
}

#if 0
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
#endif