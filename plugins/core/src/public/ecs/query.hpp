#pragma once

#include "world.hpp"

namespace ecs {
// An efficient mechanism for iterating for entities / components matching a composition. Caches matching archetypes for maximal efficiency.
template <typename... Comps> requires (!std::is_empty_v<Comps> && ...)
struct Query {
	auto for_each_view(const ExecContext& context, ::cpts::Invokable<usize, const Entity*, Comps*...> auto&& fn) -> void {
		assert_has_valid_access_requirements(context);

		if (previous_archetype_count != context.world.archetypes.size()) [[unlikely]] {
			ScopeSharedLock _{context.world.archetypes_mutex};

			ASSERT(previous_archetype_count != context.world.archetypes.size());
			update_matching_archetypes_assumes_locked(context.world);
		}

		for (Archetype* archetype : matching_archetypes) {
			archetype->for_each_view<std::decay_t<Comps>...>(FORWARD_AUTO(fn));
		}
	}

	FORCEINLINE auto for_each(const ExecContext& context, ::cpts::Invokable<const Entity&, Comps&...> auto&& fn) -> void {
		for_each_view(context, [&](const usize count, const Entity* entities, Comps*... comps) {
			for (usize i = 0; i < count; ++i) {
				std::invoke(FORWARD_AUTO(fn), entities[i], comps[i]...);
			}
		});
	}

	NOINLINE inline auto update_matching_archetypes_assumes_locked(const World& world) -> void {
		ASSERT(previous_archetype_count == std::numeric_limits<usize>::max() || previous_archetype_count < world.archetypes.size());

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
			// Must resize the mask to max size before flipping bits in this case.
			// @TODO: operator~ should somehow just handle this.
			ArchetypeMask mask = world.comp_archetypes_mask[id];
			mask.mask.resize_to_fit(world.archetypes.size());
			mask.flip_bits();
			mask.mask.words.back() &= std::numeric_limits<u64>::max() >> (world.archetypes.size() % 64);

			matching_archetypes_mask &= mask;
		});

		matching_archetypes_mask.for_each([&](const ArchetypeId id) {
			matching_archetypes.push_back(world.archetypes[id].get());
		});

		previous_archetype_count = world.archetypes.size();
	}

	static auto assert_has_valid_access_requirements(const ExecContext& context) -> void {
#if ASSERTIONS_ENABLED
		CompMask reads, writes;
		([&] {
			reads.add<std::decay_t<Comps>>();
			if constexpr (!std::is_const_v<Comps>) {
				writes.add<std::decay_t<Comps>>();
			}
		}(), ...);

		const AccessRequirements& access_requirements = context.world.get_system_access_requirements(context.currently_executing_system);

		ASSERTF(((access_requirements.comps.writes | access_requirements.comps.concurrent_writes) & writes) == writes,
				"{} does not have proper access requirements to access {} mutably!", context.currently_executing_system, ~(access_requirements.comps.writes | access_requirements.comps.concurrent_writes) & writes);
		ASSERTF((access_requirements.get_accessing_comps() & reads) == reads,
			"{} does not have the proper access requirements to access {} immutably!", context.currently_executing_system, ~access_requirements.get_accessing_comps() & reads);
#endif
	}


	const CompMask includes;
	const CompMask excludes;
	Array<Archetype*> matching_archetypes;
	usize previous_archetype_count = std::numeric_limits<usize>::max();
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