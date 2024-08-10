#pragma once

#include "system_base.hpp"
#include "archetype.hpp"
#include "entity_list.hpp"
#include "threading/thread_safe_types.hpp"
#include "threading/task.hpp"

namespace task { struct Task; }

namespace ecs {
struct App;
struct World;

// @TODO: Archetype construction, deferred entity initialization, observers, resources, system "activation".
struct World {
	struct EntityCtorNode {
		Fn<void()> ctor;
		Atomic<EntityCtorNode*> next = null;
	};

	struct EntityCtorHead {
		constexpr EntityCtorHead() = default;

		// @NOTE: Must be copy constructible / assignable to be within an Array.
		// This doesn't need to be atomic though as the elements will be locked.
		EntityCtorHead(const EntityCtorHead& other)
			: head{other.head.load(std::memory_order_relaxed)}
#if ASSERTIONS_ENABLED
				, is_dequeueing{other.is_dequeueing.load(std::memory_order_relaxed)}
#endif
			 {}

		auto operator=(const EntityCtorHead& other) -> EntityCtorHead& { std::construct_at(this, other); return *this; }

		Atomic<EntityCtorNode*> head = null;
#if ASSERTIONS_ENABLED
		Atomic<bool> is_dequeueing = false;
#endif
	};

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
	// Thread-safe. Spawning will be deferred and ran before any other systems that could access this entity.
	template <typename... Comps>
	auto spawn(Comps&&... comps) -> Entity {
		static const ArchetypeDesc DESC{
			.comps = CompMask::make<std::decay_t<Comps>...>(),
		};

		const SystemMask accessing_systems = get_accessing_systems(DESC);

		Entity entity = reserve_entity();

		// @TODO: Handle adding / removing components before entity construction && batch operations within a single-task.
		SharedPtr<task::Task> construct_entity_task = task::Task::make(std::bind([this, entity](const SharedPtr<task::Task>& this_task, Comps&&... comps) {
			Archetype& archetype = find_or_create_archetype(DESC);
			const usize index = archetype.add_uninitialized_entities();

			// Construct each element.
			(std::construct_at(&archetype.get<std::decay_t<Comps>>(index), std::forward<Comps>(comps)), ...);
		}, std::forward<Comps>(comps)...), task::Priority::HIGH);

		{
			ScopeSharedLock lock{system_tasks_mutex};

			accessing_systems.for_each([&](const SystemId id) {
				if (auto subsequent = system_tasks[id].lock()) {
					construct_entity_task->add_subsequent(std::move(subsequent));
				}
			});
		}

		task::enqueue(std::move(construct_entity_task), task::Priority::HIGH);

		return entity;
	}
#endif

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
	auto spawn(Comps&&... comps) -> Entity {
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

	[[nodiscard]] FORCEINLINE auto reserve_entity() -> Entity {
		ScopeLock lock{entities_mutex};
		return entities.reserve_entity();
	}

	[[nodiscard]] FORCEINLINE auto is_entity_valid(const Entity entity) const -> bool {
		ScopeSharedLock lock{entities_mutex};
		return entities.is_entity_valid(entity);
	}

	const App& app;

	Array<UniquePtr<SystemBase>> systems;// Indexed via SystemId.

	mutable SharedMutex system_tasks_mutex;
	Array<WeakPtr<task::Task>> system_tasks;// Indexed via SystemId. May be NULL or completed.

	mutable SharedMutex conflicting_executing_systems_mutex;
	SystemMask executing_systems;// Mask of systems currently running. Only potentially set for systems with other conflicting systems.

	mutable SharedMutex archetypes_mutex;
	Array<UniquePtr<Archetype>> archetypes;// Indexed via ArchetypeId.
	Map<ArchetypeDesc, ArchetypeId> archetype_desc_to_id;// Lookup for ArchetypeDesc for ArchetypeId.
	Array<ArchetypeMask> comp_archetypes_mask;// Indexed via CompId. A mask of all archetypes that contain this component.
	Array<SystemMask> archetype_accessing_systems;// Indexed via ArchetypeId. All the systems that require access to this archetype.
	Array<EntityCtorHead> archetype_entity_ctors;// Indexed via ArchetypeId. Pending entity constructors. @NOTE: Needs to be freed upon destruction.

	mutable SharedMutex entities_mutex;
	EntityList entities;

	volatile bool is_pending_destruction = false;

private:
	// Sigh. Just to break circular-dependencies. Modules would be nice.
	[[nodiscard]] auto get_accessing_systems(const ArchetypeDesc& desc) const -> SystemMask;
};
}