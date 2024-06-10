#pragma once

#include "component.hpp"
#include "entity.hpp"

namespace core::ecs {
struct alignas(CACHE_LINE_SIZE) Archetype {
	NON_COPYABLE(Archetype);

	EXPORT_API explicit Archetype(ComponentMask in_component_mask);

	~Archetype() {
		for_each_component_chunk_in_range(0, num_entities, [&](const ComponentId id, u8* data, const u32 count) {
			get_component_info(id).destruct(data, count);
		});
	}

	// This is the size that Mass uses (probably overkill as we will likely have a lot of archetypes).
	static constexpr usize CHUNK_SIZE = 128 * 1024;

	struct ChunkRef {
		struct Chunk {
			alignas(CACHE_LINE_SIZE) u8 buffer[CHUNK_SIZE];
		};

		ChunkRef() : buffer{std::make_unique<Chunk>()} {
			ASSERTF(buffer.get() != nullptr, "Allocation failure for chunk occurred!");
		}

		[[nodiscard]] FORCEINLINE fn operator[](const std::integral auto index) -> u8& {
			ASSERTF(index >= 0 && index < CHUNK_SIZE, "Attempted to get index {} from buffer of size {}!", index, CHUNK_SIZE);
			return buffer->buffer[index];
		}
		[[nodiscard]] FORCEINLINE fn operator[](const std::integral auto index) const -> const u8& {
			ASSERTF(index >= 0 && index < CHUNK_SIZE, "Attempted to get index {} from buffer of size {}!", index, CHUNK_SIZE);
			return buffer->buffer[index];
		}

		UniquePtr<Chunk> buffer;
	};

	struct ComponentInfo {
		ComponentId id;
		usize offset_into_chunk;
	};

	[[nodiscard]]
	fn matches_requirements(const ComponentMask& includes, const ComponentMask& excludes) const -> bool {
		return includes.has_all_matching_set_bits(component_mask) && !excludes.has_any_matching_set_bits(component_mask);
	}

	fn add_uninitialized_entities(const Span<const Entity> entities) -> u32;

	fn add_defaulted_entities(const Span<const Entity> entities) -> u32 {
		const u32 start = add_uninitialized_entities(entities);

		u32 chunk_index = 0;
		for_each_component_chunk_in_range(start, entities.size(), [&](const ComponentId id, u8* data, const u32 count) {
			get_component_info(id).construct(data, count);
			++chunk_index;
		});

		return start;
	}

	// Returns the entity index within the archetype.
	template<typename... Components>
	requires (sizeof...(Components) > 0)
	fn emplace_entity(const Entity entity, Components&&... components) -> u32 {
		const u32 index = add_uninitialized_entities({&entity, 1});
		([&] {
			using Type = std::decay_t<Components>;
			if constexpr (!std::is_empty_v<Type>) {// Skip tag types.
				new(&get<Type>(index)) Type{std::forward<Components>(components)};
			}
		}(), ...);
		return index;
	}

	fn for_each_entity_chunk_in_range(const u32 start_index, const u32 count, Invokable<u32*, u32> auto&& func) -> void {
		ASSERTF(start_index + count <= num_entities, "Attempted to index into {} of size {}!", start_index + count, num_entities);

		const u32 start_chunk_index = start_index / num_entities_per_chunk;
		const u32 end_chunk_index = (start_index + count - 1) / num_entities_per_chunk;
		if (start_chunk_index == end_chunk_index) {// No need to traverse chunks.
			const u32 index_within_chunk = start_index % num_entities_per_chunk;
			std::invoke(func, reinterpret_cast<u32*>(&chunks[start_chunk_index][entities_offset_into_chunk + index_within_chunk * sizeof(u32)]), count);
		} else {// Traverse chunks.
			const u32 start_index_within_chunk = start_index % num_entities_per_chunk;
			const u32 end_num_entities = (start_index + count) - (num_entities_per_chunk * end_chunk_index);
			std::invoke(func, reinterpret_cast<u32*>(&chunks[start_chunk_index][entities_offset_into_chunk + start_index_within_chunk * sizeof(u32)]), num_entities_per_chunk - start_index_within_chunk);

			for (u32 i = start_chunk_index + 1; i < end_chunk_index; ++i) {
				std::invoke(func, reinterpret_cast<u32*>(&chunks[i][entities_offset_into_chunk]), num_entities_per_chunk);
			}

			std::invoke(func, reinterpret_cast<u32*>(&chunks[end_chunk_index][entities_offset_into_chunk]), end_num_entities);
		}
	}

	fn for_each_component_chunk_in_range(const u32 start_index, const u32 count, Invokable<ComponentId, u8*, u32> auto&& func) -> void {
		ASSERTF(start_index + count <= num_entities, "Attempted to index into {} of size {}!", start_index + count, num_entities);

		const u32 start_chunk_index = start_index / num_entities_per_chunk;
		const u32 end_chunk_index = (start_index + count - 1) / num_entities_per_chunk;
		if (start_chunk_index == end_chunk_index) {// No need to traverse chunks.
			const u32 index_within_chunk = start_index % num_entities_per_chunk;
			for (const auto& info : components_info) {
				std::invoke(func, info.id, &chunks[start_chunk_index][info.offset_into_chunk + index_within_chunk * get_component_info(info.id).size], count);
			}
		} else {// Traverse chunks.
			const u32 start_index_within_chunk = start_index % num_entities_per_chunk;
			const u32 end_num_entities = (start_index + count) - (num_entities_per_chunk * end_chunk_index);
			for (const auto& info : components_info) {
				std::invoke(func, info.id, &chunks[start_chunk_index][info.offset_into_chunk + start_index_within_chunk * get_component_info(info.id).size], num_entities_per_chunk - start_index_within_chunk);
			}

			for (u32 i = start_chunk_index + 1; i < end_chunk_index; ++i) {
				for (const auto& info : components_info) {
					std::invoke(func, info.id, &chunks[i][info.offset_into_chunk], num_entities_per_chunk);
				}
			}

			for (const auto& info : components_info) {
				std::invoke(func, info.id, &chunks[end_chunk_index][info.offset_into_chunk], end_num_entities);
			}
		}
	}

	fn for_each_component_chunk_in_range(const u32 start_index, const u32 count, Invokable<ComponentId, const void*, u32> auto&& func) const -> void {
		const_cast<Archetype*>(this)->for_each_component_chunk_in_range(start_index, count, FORWARD_AUTO(func));
	}

	fn for_each_chunk_span(const Span<const ComponentId>& components, Invokable<const Span<u8*>&, u32> auto&& func) -> void {
		const u32 chunk_count = math::divide_and_round_up(num_entities, num_entities_per_chunk);

		u8** chunk_spans = static_cast<u8**>(_alloca(components.size() * sizeof(u8*)));
		u32* component_info_indices = static_cast<u32*>(_alloca(components.size() * sizeof(u32)));

		// Initialize component_info_indices. This is so we don't have to find associated component info for every chunk iteration.
		for (usize i = 0; i < components.size(); ++i) {
			const auto it = std::find_if(components_info.begin(), components_info.end(), [&](const ComponentInfo& info) { return info.id == components[i]; });
			ASSERTF(it != components_info.end(), "Could not find component {} in archetype!", get_component_info(components[i]).name);

			component_info_indices[i] = it - components_info.begin();
		}

		for (u32 chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
			const u32 count = std::min(num_entities - (chunk_index * num_entities_per_chunk), num_entities_per_chunk);
			for (usize component_index = 0; component_index < components.size(); ++component_index) {
				chunk_spans[component_index] = &chunks[chunk_index][components_info[component_info_indices[component_index]].offset_into_chunk];
			}

			std::invoke(func, Span<u8*>{chunk_spans, components.size()}, count);
		}
	}

	template<typename... Ts>
	requires (sizeof...(Ts) > 0 && (!std::is_empty_v<std::decay_t<Ts>> && ...))
	fn for_each_view(Invokable<u32, Ts*...> auto&& func) -> void {
		const ComponentId ids[] = { get_component_id<std::decay_t<Ts>>()... };
		const u32 component_indices[] = { get_component_index<std::decay_t<Ts>>()... };

		for_each_chunk_span(ids, [&](const Span<u8*>& chunk_span, const u32 count) {
			std::invoke(func, count, reinterpret_cast<Ts*>(chunk_span[component_indices[::utils::index_of_type<Ts, Ts...>()]])...);
		});
	}

	template<typename... Components>
	requires (sizeof...(Components) > 0 && (!std::is_empty_v<std::decay_t<Components>> && ...))
	fn for_each(Invokable<Components&...> auto&& func) -> void {
		for_each_view<Components...>([&](const u32 count, Components*... components) {
			for (u32 i = 0; i < count; ++i) {
				std::invoke(func, components[i]...);
			}
		});
	}

	template<typename T>
	requires (!std::is_empty_v<T>)
	fn get(const u32 index) -> T& {
		return reinterpret_cast<T&>(chunks[index / num_entities_per_chunk][components_info[get_component_index<T>()].offset_into_chunk + (index % num_entities_per_chunk) * sizeof(T)]);
	}

	template<typename T>
	requires (!std::is_empty_v<T>)
	fn get_component_index() const -> u32 {
		// @TODO: Consider using a constant-time lookup with a sparse-set.
		const auto found = std::find_if(components_info.begin(), components_info.end(), [&](const ComponentInfo& info) { return info.id == get_component_id<T>(); });
		ASSERT(found != components_info.end());

		return found - components_info.begin();
	}

	ComponentMask component_mask;
	usize entities_offset_into_chunk;

	Array<ComponentInfo> components_info;
	Array<ChunkRef> chunks;

	u32 num_entities_per_chunk;
	u32 num_entities = 0;
};
}