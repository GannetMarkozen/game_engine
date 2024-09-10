#pragma once

#include "world.hpp"

// An efficient mechanism for iterating for entities / components matching a composition. Caches matching archetypes for maximal efficiency.
template <typename... Comps>
struct Query {
	using Types = Tuple<Comps...>;

	[[nodiscard]] auto get_single(const ExecContext& context) -> Tuple<Entity, Comps&...> {
		if (previous_archetype_count != context.world.archetypes.size()) [[unlikely]] {
			ScopeSharedLock _{context.world.archetypes_mutex};
			update_matching_archetypes_assumes_locked(context.world);

			ASSERTF(!matching_archetypes.empty(), "No matching archetypes for query {} !{}!", includes, excludes)
		}

		const auto it = std::ranges::find_if(matching_archetypes, [&](const Archetype* archetype) { return archetype->num_entities > 0; });
		ASSERTF(it != matching_archetypes.end(), "No matching archetypes for query {} !{}!", includes, excludes);

		Archetype& archetype = **it;
		return {archetype.get_entity(0), archetype.get<std::decay_t<Comps>>(archetype.head_chunk, 0)...};
	}

	[[nodiscard]] auto try_get_single(const ExecContext& context) -> Optional<Tuple<Entity, Comps&...>> {
		if (previous_archetype_count != context.world.archetypes.size()) {
			ScopeSharedLock _{context.world.archetypes_mutex};
			update_matching_archetypes_assumes_locked(context.world);
		}

		const auto it = std::ranges::find_if(matching_archetypes, [&](const Archetype* archetype) { return archetype->num_entities > 0; });
		if (it == matching_archetypes.end()) {
			return NULL_OPTIONAL;
		}

		Archetype& archetype = **it;
		return Tuple<Entity, Comps&...>{archetype.get_entity(0), archetype.get<std::decay_t<Comps>>(archetype.head_chunk, 0)...};
	}

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