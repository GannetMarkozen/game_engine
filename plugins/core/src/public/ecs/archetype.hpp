#pragma once

#include "types.hpp"
#include "concepts.hpp"
#include "ids.hpp"
#include "entity.hpp"

struct Task;

struct ArchetypeDesc {
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const ArchetypeDesc& other) const -> bool {
		return comps == other.comps;
	}

	CompMask comps;
};

namespace std {
template <>
struct hash<ArchetypeDesc> {
	[[nodiscard]] FORCEINLINE constexpr auto operator()(const ArchetypeDesc& value) const -> usize {
		return std::hash<CompMask>{}(value.comps);
	}
};
}

struct EXPORT_API Archetype {
	struct Chunk;

	// @TODO: Find a good value for this. 1024 * 128 is what Mass Entity uses.
	static constexpr usize BYTES_PER_CHUNK = 1024 * 64 - sizeof(UniquePtr<Chunk>);

	struct CompInfo {
		usize offset_within_chunk;
		CompId id;
	};

	// @NOTE: May be optimal to make chunks grow exponentially as more are added (will make things a lot more complicated though).
	struct Chunk {
		constexpr Chunk() = default;
		NON_COPYABLE(Chunk);

		alignas(CACHE_LINE_SIZE) u8 data[BYTES_PER_CHUNK];
		UniquePtr<Chunk> next = null;
	};
	static_assert(sizeof(Chunk) == BYTES_PER_CHUNK + sizeof(UniquePtr<Chunk>));

	NON_COPYABLE(Archetype);

	explicit Archetype(const ArchetypeDesc& in_description);

	~Archetype();

	[[nodiscard]] FORCEINLINE auto get_chunk(std::integral auto chunk_index) [[clang::lifetimebound]] -> Chunk& {
		Chunk* current_chunk = &head_chunk;
		while (chunk_index-- > 0) {
			current_chunk = current_chunk->next.get();
		}
		return *current_chunk;
	}

	auto for_each_chunk(::cpts::Invokable<Chunk&, usize> auto&& fn) -> void {
		const usize num_chunks = math::divide_and_round_up(num_entities, num_entities_per_chunk);
		if (num_chunks == 0) {
			return;
		}

		Chunk* current_chunk = &head_chunk;
		for (usize i = 0; i < num_chunks - 1; ++i) {
			std::invoke(fn, *current_chunk, num_entities_per_chunk);
			current_chunk = current_chunk->next.get();
		}

		std::invoke(fn, *current_chunk, ((num_entities + num_entities_per_chunk - 1) % num_entities_per_chunk) + 1);
	}

	auto for_each_chunk_in_range(const usize start, const usize count, ::cpts::Invokable<Chunk&, usize, usize> auto&& fn) -> void {
		if (num_entities == 0) {
			return;
		}

		const usize start_chunk = start / num_entities_per_chunk;
		const usize end_chunk = (start + count - 1) / num_entities_per_chunk;

		if (start_chunk == end_chunk) {
			Chunk& chunk = get_chunk(start_chunk);

			const usize start_within_chunk = start % num_entities_per_chunk;

			std::invoke(fn, chunk, start_within_chunk, count);
		} else {
			Chunk* chunk = &get_chunk(start_chunk);

			{
				const usize start_within_chunk = start % num_entities_per_chunk;
				const usize count_within_chunk = num_entities_per_chunk - start_within_chunk;
				std::invoke(fn, *chunk, start_within_chunk, count_within_chunk);

				chunk = chunk->next.get();
			}

			for (usize i = start_chunk + 1; i < end_chunk; ++i) {
				std::invoke(fn, *chunk, 0, num_entities_per_chunk);
				chunk = chunk->next.get();
			}

			{
				ASSERT(chunk == &get_chunk(end_chunk));

				const usize count_within_chunk = ((start + count + num_entities_per_chunk - 1) % num_entities_per_chunk) + 1;
				std::invoke(fn, *chunk, 0, count_within_chunk);
			}
		}
	}

	FORCEINLINE auto for_each_chunk_from_start(const usize start, ::cpts::Invokable<Chunk&, usize, usize> auto&& fn) -> void {
		for_each_chunk_in_range(start, num_entities - start, FORWARD_AUTO(fn));
	}

	[[nodiscard]] FORCEINLINE auto get_comp_data(const CompInfo& comp, Chunk& chunk, const usize index) -> u8* {
		return &chunk.data[comp.offset_within_chunk + index * get_type_info(comp.id).size];
	}

	auto add_uninitialized_entities(const usize count = 1) -> usize;

	template <typename... Comps>
	auto add_entity(const Entity entity, Comps&&... comps) -> usize {
		const usize index = add_uninitialized_entities(1);

		Chunk& chunk = get_chunk(index / num_entities_per_chunk);
		const usize index_within_chunk = index % num_entities_per_chunk;

		std::construct_at(&reinterpret_cast<Entity*>(&chunk.data[entity_offset_within_chunk])[index_within_chunk], entity);

		([&] {
			using Comp = std::decay_t<Comps>;
			if constexpr (!std::is_empty_v<Comp>) {
				const auto it = std::ranges::find(this->comps, get_comp_id<Comp>(), &CompInfo::id);
				ASSERTF(it != this->comps.end(), "Component {} does not exist on Archetype!", utils::get_type_name<Comp>());

				const usize offset_within_chunk = it->offset_within_chunk;
				std::construct_at(&reinterpret_cast<Comp*>(&chunk.data[offset_within_chunk])[index], std::forward<Comps>(comps));
			}
		}(), ...);

		return index;
	}

	template <typename... Comps>
	auto add_entities(const Span<const Entity> entities, const Comps&... comps) -> usize {
		ASSERT(!entities.empty());

		const usize start = add_uninitialized_entities(entities.size());

		const usize offsets[] = {
			[&] {
				using Comp = std::decay_t<Comps>;
				if constexpr (std::is_empty_v<Comp>) {
					return std::numeric_limits<usize>::max();
				} else {
					const auto it = std::ranges::find(comps, &rtti::get_type_info<Comp>(), &CompInfo::id);
					ASSERTF(it != comps.end(), "Component {} does not exist on archetype!", rtti::get_type_info<Comp>().name);
					return it->offset_within_chunk;
				}
			}()...
		};

		usize num_constructed = 0;
		for_each_chunk_from_start(start, [&](Chunk& chunk, const usize index, const usize count) {
			Entity* dst = reinterpret_cast<Entity*>(&chunk.data[entity_offset_within_chunk + index * sizeof(Entity)]);
			const Entity* src = &entities[num_constructed];

			memcpy(dst, src, count * sizeof(Entity));

			([&] {
				using Comp = std::decay_t<Comps>;

				Comp* dst = reinterpret_cast<Comp*>(&chunk.data[offsets[utils::index_of_type<Comps, Comps...>()] + index * sizeof(Comp)]);
				auto* src = &comps[num_constructed];

				for (usize i = 0; i < count; ++i) {
					std::construct_at(&dst[i], src[i]);
				}
			}(), ...);

			num_constructed += count;
		});

		return start;
	}

	template <bool MOVE_CONSTRUCT = false, typename... Comps>
	auto add_entities(const usize count, const Entity* entities, Comps*... comps) -> usize {
		ASSERT(entities);
		ASSERT(comps && ...);

		const usize start = add_uninitialized_entities(count);

		const usize offsets[] = {
			[&] {
				using Comp = std::decay_t<Comps>;
				if constexpr (std::is_empty_v<Comp>) {
					return std::numeric_limits<usize>::max();
				} else {
					const auto it = std::ranges::find(comps, &rtti::get_type_info<Comp>(), &CompInfo::id);
					ASSERTF(it != comps.end(), "Component {} does not exist on archetype!", rtti::get_type_info<Comp>().name);
					return it->offset_within_chunk;
				}
			}()...
		};

		usize num_constructed = 0;
		for_each_chunk_from_start(start, [&](Chunk& chunk, const usize index, const usize count) {
			Entity* dst = reinterpret_cast<Entity*>(&chunk.data[entity_offset_within_chunk + index * sizeof(Entity)]);
			const Entity* src = &entities[num_constructed];

			memcpy(dst, src, count * sizeof(Entity));

			([&] {
				using Comp = std::decay_t<Comps>;
				if constexpr (!std::is_empty_v<Comp>) {
					static_assert(std::is_trivially_copy_constructible_v<Comp> == std::is_trivially_move_constructible_v<Comp>);

					Comp* dst = reinterpret_cast<Comp*>(&chunk.data[offsets[utils::index_of_type<Comps, Comps...>()] + index * sizeof(Comp)]);
					auto* src = &comps[num_constructed];

					if constexpr (std::is_trivially_copy_constructible_v<Comp>) {
						memcpy(dst, src, count * sizeof(Comp));
					} else if constexpr (MOVE_CONSTRUCT && std::is_move_constructible_v<Comp>) {
						static_assert(!std::is_const_v<std::remove_reference_t<Comps>>);

						for (usize i = 0; i < count; ++i) {
							std::construct_at(&dst[i], std::move(src[i]));
						}
					} else {
						for (usize i = 0; i < count; ++i) {
							std::construct_at(&dst[i], src[i]);
						}
					}
				}
			}(), ...);

			num_constructed += count;
		});

		return start;
	}

	[[nodiscard]] auto get_chunk_index(const Chunk& chunk) const -> usize {
		const Chunk* current_chunk = &head_chunk;
		usize index = 0;
		while (true) {
			if (current_chunk == &chunk) {
				return index;
			}

			++index;
			current_chunk = current_chunk->next.get();
		}

		ASSERT_UNREACHABLE;
	}

	auto add_defaulted_entities(const ::cpts::Range<Entity> auto& entities) -> usize {
		auto it = std::ranges::begin(entities);
		const usize count = std::ranges::end(entities) - it;
		ASSERT(count > 0);

		const usize start = add_uninitialized_entities(count);
		for_each_chunk_from_start(start, [&](Chunk& chunk, const usize start, const usize count) {
			for (const auto& comp : comps) {
				const auto& type_info = get_type_info(comp.id);
				type_info.construct(&chunk.data[comp.offset_within_chunk + start * type_info.size], count);
			}

			for (usize i = 0; i < count; ++i) {
				reinterpret_cast<Entity*>(&chunk.data[entity_offset_within_chunk])[i] = *it++;
			}
		});

		return start;
	}

	auto remove_from_end(const usize count = 1) -> usize {
		ASSERTF(count <= num_entities, "Attempted to remove {} entities from size of {}!", count, num_entities);

		for_each_chunk_from_start(num_entities - count, [&](Chunk& chunk, const usize start, const usize count) {
			for (const auto& comp : comps) {
				const auto& type_info = get_type_info(comp.id);
				if (!!(type_info.flags & rtti::Flags::TRIVIAL)) {
					continue;
				}

				type_info.destruct(&chunk.data[comp.offset_within_chunk + start * type_info.size], count);
			}
		});

		return num_entities -= count;
	}

	// Implements swap. Will not retain order (don't think we need that).
	auto remove_at(const usize index, usize count = 1) -> usize {
		ASSERTF(count <= num_entities, "Attempted to remove {} entities from size of {} for archetype {}!", count, num_entities, description.comps);
		ASSERTF(index < num_entities, "Attempted to remove index {} from size {} for archetype {}!", index, num_entities, description.comps);
		ASSERTF((index + count) <= num_entities, "Attempted to remove entities {} through {} of size {} for archetype {}!", index, index + count, num_entities, description.comps);

		const usize num_from_end = num_entities - (index + count);
		const usize no_swap_count = count - std::min(count, num_from_end);

		count -= no_swap_count;

		if (count) {// Relocate.
			usize num_needs_removal = count;
			for_each_chunk_in_range(index, count, [&](Chunk& swapee_chunk, const usize swapee_start, const usize swapee_count) {
				usize num_removed_within_chunk = 0;

				for_each_chunk_in_range(num_entities - num_needs_removal - no_swap_count, swapee_count, [&](Chunk& swap_chunk, const usize swap_start, const usize swap_count) {
					for (const auto& comp : comps) {
						const auto& type_info = get_type_info(comp.id);
						type_info.relocate_occupied(&swapee_chunk.data[comp.offset_within_chunk + (swapee_start + num_removed_within_chunk) * type_info.size], &swap_chunk.data[comp.offset_within_chunk + swap_start * type_info.size], swap_count);
					}

					num_removed_within_chunk += swap_count;
				});

				num_needs_removal -= num_removed_within_chunk;
			});
		}

		if (no_swap_count) {// Destruct count (end elements).
			remove_from_end(no_swap_count);
		}

		return num_entities -= count;
	}

	FORCEINLINE auto remove(const Entity entity) -> usize {
		return remove_at(entity.get_index());
	}

	template <typename... Ts> requires (!std::is_empty_v<Ts> && ...)
	FORCEINLINE auto for_each_view(::cpts::Invokable<usize, const Entity*, Ts*...> auto&& fn) -> void {
#if ASSERTIONS_ENABLED
		([&] {
			ASSERTF(description.comps.has<std::decay_t<Ts>>(), "Archetype does not have component {}!", utils::get_type_name<std::decay_t<Ts>>());
		}(), ...);
#endif

		const usize offsets[] = {
			std::ranges::find_if(comps, [](const CompInfo& comp) { return comp.id == get_comp_id<std::decay_t<Ts>>(); })->offset_within_chunk...
		};

		for_each_chunk([&](Chunk& chunk, const usize count) {
			std::invoke(fn, count, reinterpret_cast<const Entity*>(&chunk.data[entity_offset_within_chunk]), reinterpret_cast<Ts*>(&chunk.data[offsets[utils::index_of_type<Ts, Ts...>()]])...);
		});
	}

	template <typename... Ts> requires (!std::is_empty_v<Ts> && ...)
	FORCEINLINE auto for_each(::cpts::Invokable<const Entity&, Ts&...> auto&& fn) -> void {
		for_each_view<Ts...>([&](const usize count, const Entity* entities, Ts*... ts) {
			for (usize i = 0; i < count; ++i) {
				std::invoke(fn, entities[i], ts[i]...);
			}
		});
	}

	template <typename T> requires (!std::is_empty_v<T>)
	[[nodiscard]] auto get(Chunk& chunk, const usize index_within_chunk) -> T& {
		const auto it = std::ranges::find(comps, get_comp_id<T>(), &CompInfo::id);
		ASSERTF(it != comps.end(), "Component {} does not exist on archetype with composition {}!",
			utils::get_type_name<T>(), [&] {
				String out;
				description.comps.for_each([&](const CompId id) {
					out += fmt::format("{}, ", TypeRegistry<CompId>::get_type_info(id).name);
				});
				return out;
			}());

		return *reinterpret_cast<T*>(&chunk.data[it->offset_within_chunk + index_within_chunk * sizeof(T)]);
	}

	template <>
	[[nodiscard]] auto get<Entity>(Chunk& chunk, const usize index_within_chunk) -> Entity& {
		return *reinterpret_cast<Entity*>(&chunk.data[entity_offset_within_chunk + index_within_chunk * sizeof(Entity)]);
	}

	template <typename... Ts> requires (sizeof...(Ts) > 0)
	[[nodiscard]] auto get(const usize index) -> Tuple<Ts&...> {
		Chunk& chunk = get_chunk(index / num_entities_per_chunk);
		const usize index_within_chunk = index % num_entities_per_chunk;
		return {get<Ts>(chunk, index_within_chunk)...};
	}

	[[nodiscard]] auto get_entity(const usize index) const -> Entity {
		return reinterpret_cast<const Entity*>(&const_cast<Archetype*>(this)->get_chunk(index / num_entities_per_chunk).data[entity_offset_within_chunk])[index % num_entities_per_chunk];
	}

	usize num_entities = 0;
	usize num_entities_per_chunk;
	usize entity_offset_within_chunk;
	Array<CompInfo> comps;
	ArchetypeDesc description;
	Chunk head_chunk;
};