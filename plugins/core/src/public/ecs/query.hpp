#pragma once

#include "defines.hpp"
#include "types.hpp"
#include "component.hpp"
#include "world.hpp"

namespace ecs {

struct Requirements {
	template<typename... Ts>
	requires ((!std::is_empty_v<Ts> && ...) && sizeof...(Ts) > 0)
	FORCEINLINE fn reads() -> Requirements& {
		read_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires ((!std::is_empty_v<Ts> && ...) && sizeof...(Ts) > 0)
	FORCEINLINE fn writes() -> Requirements& {
		write_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn includes() -> Requirements& {
		include_mask.add<Ts...>();
		return *this;
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0)
	FORCEINLINE fn excludes() -> Requirements& {
		exclude_mask.add<Ts...>();
		return *this;
	}

	fn operator&=(const Requirements& other) -> Requirements& {
		read_mask &= other.read_mask;
		write_mask &= other.write_mask;
		include_mask &= other.include_mask;
		exclude_mask &= other.exclude_mask;
		return *this;
	}

	fn operator|=(const Requirements& other) -> Requirements& {
		read_mask |= other.read_mask;
		write_mask |= other.write_mask;
		include_mask |= other.include_mask;
		exclude_mask |= other.exclude_mask;
		return *this;
	}

	[[nodiscard]]
	FORCEINLINE fn operator&(const Requirements& other) const -> Requirements {
		Requirements copy = *this;
		return copy &= other;
	}

	[[nodiscard]]
	FORCEINLINE fn operator|(const Requirements& other) const -> Requirements {
		Requirements copy = *this;
		return copy |= other;
	}

	[[nodiscard]]
	FORCEINLINE fn operator&(const Requirements& other) && -> Requirements {
		Requirements copy = std::move(*this);
		return copy &= other;
	}

	[[nodiscard]]
	FORCEINLINE fn operator|(const Requirements& other) && -> Requirements {
		Requirements copy = std::move(*this);
		return copy |= other;
	}

	[[nodiscard]]
	fn can_execute_concurrently_with(const Requirements& other) const -> bool {
		return !write_mask.has_any_matching_set_bits(other.write_mask) && !write_mask.has_any_matching_set_bits(other.read_mask) && !other.write_mask.has_any_matching_set_bits(read_mask);
	}

	ComponentMask read_mask;
	ComponentMask write_mask;
	ComponentMask include_mask;
	ComponentMask exclude_mask;
};

struct Query : public Requirements {
	using Requirements::Requirements;

	// @TODO: Cache matched archetypes (otherwise this will probably be prohibitively slow).
	fn for_each_matching_archetype(const World& world, Invokable<Archetype&> auto&& func) const -> void {
		world.archetypes.read([&](const auto& archetypes) {
			for (const auto& archetype : archetypes) {
				archetype->write([&](Archetype& archetype) {
					const auto& mask = archetype.component_mask;
					if (read_mask.has_all_matching_set_bits(mask) && write_mask.has_all_matching_set_bits(mask) && include_mask.has_all_matching_set_bits(mask) && !exclude_mask.has_any_matching_set_bits(mask)) {
						std::invoke(func, archetype);
					}
				});
			}
		});
	}

	template<typename... Components>
	requires (sizeof...(Components) > 0 && (!std::is_empty_v<std::decay_t<Components>> && ...))
	fn for_each_view(const World& world, Invokable<u32, Components*...> auto&& func) const -> void {
		ASSERT(((read_mask.has<std::decay_t<Components>>() || write_mask.has<std::decay_t<Components>>()) && ...));

		for_each_matching_archetype(world, [&](Archetype& archetype) {
			archetype.for_each_view<Components...>(FORWARD_AUTO(func));
		});
	}
};
}