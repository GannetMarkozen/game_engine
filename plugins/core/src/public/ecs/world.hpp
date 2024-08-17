#pragma once

#include "system.hpp"
#include "archetype.hpp"
#include "entity_list.hpp"
#include "threading/thread_safe_types.hpp"
#include "threading/task.hpp"

namespace task { struct Task; }

namespace ecs {
struct App;
struct World;
}

namespace ecs {
struct ExecContext {
	using DeferredFn = Fn<void(const ExecContext&)>;

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entity(Comps&&... comps) -> Entity;

	World& [[clang::lifetimebound]] world;
	const f32 delta_time;
	const SystemId currently_executing_system;
	const std::thread::id currently_executing_system_thread;
	MpscQueue<DeferredFn> deferred_actions;
};

// @TODO: Archetype construction, deferred entity initialization, observers, resources, system "activation".
struct World {
	using Task = task::Task;

	NON_COPYABLE(World);

	explicit World(const App& app [[clang::lifetimebound]]);

	auto run() -> void;

	// Enqueue all systems bound to a particular event.
	auto dispatch_event(const EventId event) -> void;

	// Enqueue all systems bound to a particular event.
	template <typename T>
	FORCEINLINE auto dispatch_event() -> void {
		dispatch_event(get_event_id<T>());
	}

	[[nodiscard]] auto find_archetype_id_assumes_locked(const ArchetypeDesc& desc) const -> Optional<ArchetypeId>;
	[[nodiscard]] auto find_archetype_id(const ArchetypeDesc& desc) const -> Optional<ArchetypeId>;
	[[nodiscard]] auto find_or_create_archetype_id(const ArchetypeDesc& desc) -> ArchetypeId;// Thread-safe. Locks.

	[[nodiscard]] auto find_archetype_assumes_locked(const ArchetypeDesc& desc) const -> Optional<Pair<Archetype&, ArchetypeId>>;
	[[nodiscard]] auto find_archetype(const ArchetypeDesc& desc) const -> Optional<Pair<Archetype&, ArchetypeId>>;
	[[nodiscard]] auto find_or_create_archetype(const ArchetypeDesc& desc) -> Pair<Archetype&, ArchetypeId>;

#if 0
	auto internal_spawn_entities(const ArchetypeDesc& desc, const usize count, ::cpts::Invokable<const Array<Entity>&, Archetype&, usize> auto&& on_construction_fn, Array<Entity>* optional_out_entities = null,
		const task::Priority construction_task_priority = task::Priority::HIGH, const task::Thread construction_task_thread = task::Thread::ANY) -> void {
		ASSERT(count > 0);

		const auto [archetype, id] = find_or_create_archetype(desc);

		Array<Entity> entities;
		entities.reserve(count);
		{
			ScopeLock lock{entities_mutex};
			entities.push_back(this->entities.reserve_entity());
		}

		if (optional_out_entities) {
			*optional_out_entities = entities;
		}

		EntityCtorNode* old_head;
		EntityCtorNode* new_head;
		{
			ScopeSharedLock lock{archetypes_mutex};

			auto& head = archetype_entity_ctors[id].head;

			new_head = new EntityCtorNode{
				.ctor = [this, count, entities = std::move(entities), &archetype, id, on_construction_fn = FORWARD_AUTO(on_construction_fn)] mutable {
					const usize index = archetype.add_uninitialized_entities(count);

					{
						ScopeLock lock{entities_mutex};

						for (usize i = 0; i < count; ++i) {
							this->entities.initialize_entity(entities[i], EntityDesc{
								.archetype_id = id,
								.index_within_archetype = index + i,
							});
						}
					}

					on_construction_fn(entities, archetype, index);
				},
			};

			// Enqueue the new head.
			old_head = archetype_entity_ctors[id].head.load(std::memory_order_relaxed);
			while (!head.compare_exchange_weak(old_head, new_head)) [[unlikely]] {
				std::this_thread::yield();
			}
		}

		new_head->next = old_head;

		// If the old head was NULL (meaning we enqueued the first entity to construct), create the deferred task
		// for the actual construction of the entity.
		if (!old_head) [[unlikely]] {
			SharedPtr<task::Task> construct_entities_task = task::Task::make([this, &archetype, id](const SharedPtr<task::Task>& this_task) {
				ScopeSharedLock lock{archetypes_mutex};

				ASSERTF(!archetype_entity_ctors[id].is_dequeueing.exchange(true),
					"Already dequeueing entity ctors during the entity construction task!");

				EntityCtorNode* node = archetype_entity_ctors[id].head.load(std::memory_order_relaxed);
				ASSERTF(node, "Head node of archetype entity ctors is NULL in construction task!");

				do {
					node->ctor();

					auto* old_node = node;
					node = node->next.load(std::memory_order_relaxed);

					delete old_node;
				} while (node);

				archetype_entity_ctors[id].head = null;

#if ASSERTIONS_ENABLED
				archetype_entity_ctors[id].is_dequeueing = false;
#endif
			}, task::Priority::HIGH);

			const SystemMask accessing_systems = get_accessing_systems(desc);

			const SystemMask currently_executing_systems = executing_systems.mask.atomic_clone();

			// Any systems that require access to the archetype that aren't running will become subsequents of the construction task.
			const SystemMask subsequent_systems = accessing_systems & ~currently_executing_systems;

			// If a system is currently running that requires access to the archetype, run the construction task after.
			const SystemMask prerequisite_systems = accessing_systems & currently_executing_systems;

			Array<SharedPtr<task::Task>> prerequisites;
			{
				ScopeSharedLock lock{system_tasks_mutex};

				subsequent_systems.for_each([&](const SystemId id) {
					if (auto subsequent = system_tasks[id].lock()) {
						construct_entities_task->add_subsequent(std::move(subsequent));
					}
				});

				prerequisite_systems.for_each([&](const SystemId id) {
					if (auto prerequisite = system_tasks[id].lock()) {
						prerequisites.push_back(std::move(prerequisite));
					}
				});
			}

			task::enqueue(std::move(construct_entities_task), construction_task_priority, construction_task_thread, prerequisites);
		}
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entity(Comps&&... comps) -> Entity {
		static const ArchetypeDesc DESC{
			.comps = CompMask::make<std::decay_t<Comps>...>(),
		};

		Array<Entity> out_entities;
		internal_spawn_entities(DESC, 1, [comps = utils::make_tuple(std::forward<Comps>(comps)...)](const Array<Entity>& entities, Archetype& archetype, const usize index) mutable {
			// @NOTE: This code should be within Archetype somehow.
			Archetype::Chunk& chunk = archetype.get_chunk(index / archetype.num_entities_per_chunk);
			const usize index_within_chunk = index % archetype.num_entities_per_chunk;

			utils::visit(comps, [&](auto& comp) {
				static_assert(!std::is_const_v<std::remove_reference_t<decltype(comp)>>);

				using Comp = std::decay_t<decltype(comp)>;
				if constexpr (!std::is_empty_v<Comp>) {
					std::construct_at(&archetype.get<Comp>(chunk, index_within_chunk), std::move(comp));
				}
			});

			std::construct_at(&archetype.get<Entity>(chunk, index_within_chunk), entities[0]);
		}, &out_entities);

		return out_entities[0];
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entities(usize count, Comps&&... comps) -> Array<Entity> {
		static const ArchetypeDesc DESC{
			.comps = CompMask::make<std::decay_t<Comps>...>(),
		};

		Array<Entity> out_entities;
		internal_spawn_entities(DESC, count, [comps = utils::make_tuple(std::forward<Comps>(comps)...), count](const Array<Entity>& entities, Archetype& archetype, const usize index) {
			static constexpr usize NUM_NON_TAG_TYPES = [] {
				usize count = 0;
				((count += !std::is_empty_v<std::decay_t<Comps>>), ...);
				return count;
			}();

			static constexpr auto GET_OFFSET_INDEX = []<typename T>() constexpr -> usize {
				static_assert(utils::contains_type<T, Comps...>());

				usize index = 0;
				utils::make_index_sequence_param_pack<utils::index_of_type<T, Comps...>()>([&]<usize... Is>() {
					((index += !std::is_empty_v<std::decay_t<utils::TypeAtIndex<Is, Comps...>>>), ...);
				});
				return index;
			};

			usize offsets[NUM_NON_TAG_TYPES];

			([&] {
				if constexpr (!std::is_empty_v<std::decay_t<Comps>>) {
					const auto it = std::ranges::find(archetype.comps, get_comp_id<std::decay_t<Comps>>(), &Archetype::CompInfo::id);
					ASSERTF(it != archetype.comps.end(), "Component {} does not exist on Archetype!", utils::get_type_name<std::decay_t<Comps>>());

					offsets[GET_OFFSET_INDEX.template operator()<Comps>()] = it->offset_within_chunk;
				}
			}(), ...);

			usize current_count = 0;
			archetype.for_each_chunk_in_range(index, count, [&](Archetype::Chunk& chunk, const usize start, const usize count) {
				([&] {
					using Comp = std::decay_t<Comps>;
					if constexpr (!std::is_empty_v<Comp>) {
						Comp* dst = reinterpret_cast<Comp*>(&chunk.data[offsets[GET_OFFSET_INDEX.template operator()<Comps>()]] + start * sizeof(Comp));
						for (usize i = 0; i < count; ++i) {
							//std::construct_at(dst + i, comps.template get<utils::index_of_type<Comps, Comps...>()>());
							std::construct_at(dst + i, std::get<utils::index_of_type<Comps, Comps...>()>(comps));
						}
					}
				}(), ...);

				Entity* dst = reinterpret_cast<Entity*>(&chunk.data[archetype.entity_offset_within_chunk + start * sizeof(Entity)]);
				static_assert(std::is_trivially_copy_constructible_v<Entity>);

				memcpy(dst, &entities[current_count], count * sizeof(Entity));

				current_count += count;
			});
		}, &out_entities);

		return out_entities;
	}
#endif

	// NOT thread-safe. Systems with proper access requirements should be the only modifiers. Just don't parallel_for this.
	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entity(ExecContext& context, Comps&&... comps) -> Entity {
		const ArchetypeDesc archetype_desc{
			.comps = CompMask::make<std::decay_t<Comps>...>(),
		};

		ASSERTF((archetype_desc.comps & get_system_access_requirements(context.currently_executing_system).modifies) == archetype_desc.comps,
			"Currently executing system {} does not have the proper access requirements to spawn entity! Missing modification access for components: {}",
			context.currently_executing_system, archetype_desc.comps & ~get_system_access_requirements(context.currently_executing_system).modifies);

		const auto [archetype, archetype_id] = find_or_create_archetype(archetype_desc);

		Entity entity{NO_INIT};
		{
			auto entities_access = entities.write();

			entity = entities_access->reserve_entity();

			// Whether or not we can immediately construct the entity (meaning this system during execution isn't capable of accessing the target archetype). Else enqueue for after execution.
			const AccessRequirements& access_requirements = get_system_access_requirements(context.currently_executing_system);
			const bool can_construct_entity_immediately = !(archetype_desc.comps & (access_requirements.reads | access_requirements.writes)) || context.currently_executing_system_thread != std::this_thread::get_id();

#if 01
			if (can_construct_entity_immediately) {
				//const usize index_within_archetype = archetype.add_entities(Span<const Entity>{{entity}}, std::forward<Comps>(comps)...);
				const usize index_within_archetype = archetype.add_entity(entity, std::forward<Comps>(comps)...);
				entities_access->initialize_entity(entity, EntityDesc{
					.archetype_id = archetype_id,
					.index_within_archetype = index_within_archetype,
				});
			} else {
				context.deferred_actions.enqueue([this, entity, archetype_id, &archetype, comps = utils::make_tuple(std::forward<Comps>(comps)...)](const ExecContext& context) mutable {
					const usize index_within_archetype = utils::make_index_sequence_param_pack<sizeof...(Comps)>([&]<usize... Is>() {
						return archetype.add_entity(entity, std::forward<utils::TypeAtIndex<Is, Comps...>>(std::get<Is>(comps))...);
					});

					entities.write()->initialize_entity(entity, EntityDesc{
						.archetype_id = archetype_id,
						.index_within_archetype = index_within_archetype,
					});
				});
			}
#endif
			if (can_construct_entity_immediately) {// @NOTE: Potentially make this optional.
				const usize index_within_archetype = archetype.add_entity(entity, std::forward<Comps>(comps)...);
				entities_access->initialize_entity(entity, EntityDesc{
					.archetype_id = archetype_id,
					.index_within_archetype = index_within_archetype,
				});
			} else {
				
			}
		}

		return entity;
	}

#if 0
	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto construct_entity_immediate(const Entity uninitialized_entity, Archetype& archetype, const ArchetypeId archetype_id, Comps&&... comps) -> void {
		ASSERTF([&] {
			ScopeSharedLock lock{archetypes_mutex};
			return !(archetype_accessing_systems[archetype_id].mask & executing_systems.mask.atomic_clone());
		}(), "Can not execute {} while systems that access this archetype are currently executing!", __PRETTY_FUNCTION__);

		const usize index_within_archetype = archetype.add_entities({uninitialized_entity}, std::forward<Comps>(comps)...);

		entities.write()->initialize_entity(uninitialized_entity, EntityDesc{
			.archetype_id = archetype_id,
			.index_within_archetype = index_within_archetype,
		});
	}
#endif

	[[nodiscard]] FORCEINLINE auto reserve_entity() -> Entity {
		return entities.write()->reserve_entity();
	}

	[[nodiscard]] FORCEINLINE auto is_entity_valid(const Entity entity) const -> bool {
		return entities.read()->is_entity_valid(entity);
	}

	const App& app;

	Array<UniquePtr<SystemBase>> systems;// Indexed via SystemId.

	mutable SharedMutex system_tasks_mutex;
	Array<WeakPtr<Task>> system_tasks;// Indexed via SystemId. May be NULL or completed.

	mutable SharedMutex conflicting_executing_systems_mutex;
	SystemMask executing_systems;// Mask of systems currently running. Only potentially set for systems with other conflicting systems.

	mutable SharedMutex archetypes_mutex;// @TODO: Should be moved into it's own struct.
	Array<UniquePtr<Archetype>> archetypes;// Indexed via ArchetypeId.
	Map<ArchetypeDesc, ArchetypeId> archetype_desc_to_id;// Lookup for ArchetypeDesc for ArchetypeId.
	Array<ArchetypeMask> comp_archetypes_mask;// Indexed via CompId. A mask of all archetypes that contain this component.
	Array<SystemMask> archetype_accessing_systems;// Indexed via ArchetypeId. All the systems that require access to this archetype.

	RwLock<EntityList> entities;

	SharedLock<MpscMap<ArchetypeId, SharedLock<MpscQueue<WeakPtr<Task>>>>> outgoing_archetype_mod_tasks;

	volatile bool is_pending_destruction = false;

private:
	// Sigh. Just to break circular-dependencies. Modules would be nice.
	[[nodiscard]] auto get_accessing_systems(const ArchetypeDesc& desc) const -> SystemMask;
	[[nodiscard]] auto get_system_access_requirements(const SystemId id) const -> const AccessRequirements&;
};

template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
FORCEINLINE auto ExecContext::spawn_entity(Comps&&... comps) -> Entity {
	return world.spawn_entity(*this, std::forward<Comps>(comps)...);
}
}