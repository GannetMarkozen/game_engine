#pragma once

#include "system.hpp"
#include "archetype.hpp"
#include "threading/thread_safe_types.hpp"
#include "threading/task.hpp"
#include <variant>

namespace ecs {
struct App;
struct World;

struct PendingEntityConstruction {
	Entity entity;
	Array<Any> comps;
};

struct ArchetypeTraversal {
	[[nodiscard]] FORCEINLINE constexpr auto operator==(const ArchetypeTraversal& other) const -> bool {
		return from == other.from && to == other.to;
	}

	[[nodiscard]] FORCEINLINE constexpr auto hash() const -> usize {
		if constexpr (std::same_as<usize, u64>) {
			return from.get_value() | static_cast<u64>(to) << 32;
		} else {
			return math::hash_combine(from.get_value(), to.get_value());
		}
	}

	ArchetypeId from, to;
};
}

template <>
struct std::hash<ecs::ArchetypeTraversal> {
	[[nodiscard]] FORCEINLINE constexpr auto operator()(const ecs::ArchetypeTraversal& value) const -> usize {
		return value.hash();
	}
};

namespace ecs {
struct EntityDesc {
	[[nodiscard]] constexpr auto is_initialized() const -> bool {
		return archetype.is_valid();
	}

	[[nodiscard]] constexpr auto is_pending_construction() const -> bool {
		return !is_initialized();
	}

	usize index_within_archetype: sizeof(usize) * 8 - 1 = std::numeric_limits<u64>::max() >> 1;
	usize is_pending_destruction: 1 = false;
	ArchetypeId archetype = ArchetypeId::invalid_id();
	ArchetypeId pending_archetype_traversal = ArchetypeId::invalid_id();
};

struct EntityList {
	static constexpr u32 INVALID_INDEX = std::numeric_limits<u32>::max();

	struct Slot {
		Variant<u32, EntityDesc> desc_or_next{std::in_place_type<u32>, INVALID_INDEX};// @TODO: Remove the Variant and use a union to avoid padding, means more manual management though.
		u32 version = std::numeric_limits<u32>::max();// Starts at max. operator++ will overflow back to 0 for first version.
	};

	struct Chunk {
		static constexpr usize SLOTS_COUNT = math::divide_and_round_up(UINT16_MAX, sizeof(Slot));

		constexpr explicit Chunk(const u32 chunk_index) {
			// Initialize each "next" slot index to point to this slot index + 1.
			for (u32 i = 0; i < SLOTS_COUNT - 1; ++i) {
				std::get<u32>(slots[i].desc_or_next) = i + 1 + chunk_index * SLOTS_COUNT;
			}

			ASSERT(std::get<u32>(slots[SLOTS_COUNT - 1].desc_or_next) == INVALID_INDEX);
		}

		Slot slots[SLOTS_COUNT];
		UniquePtr<Chunk> next = null;
	};

	[[nodiscard]] auto reserve_entity() -> Entity {
		if (next_available_index != INVALID_INDEX) {
			Chunk* chunk = &head;
			for (usize i = 0; i < next_available_index / Chunk::SLOTS_COUNT; ++i, chunk = chunk->next.get());

			Slot& slot = chunk->slots[next_available_index % Chunk::SLOTS_COUNT];
			ASSERT(std::holds_alternative<u32>(slot.desc_or_next));

			const u32 slot_next = std::get<u32>(slot.desc_or_next);

			Entity out{next_available_index, ++slot.version};
			next_available_index = slot_next;

			// Replace with EntityDesc.
			slot.desc_or_next.emplace<EntityDesc>();

			return out;
		} else {
			u32 chunk_index = 0;
			Chunk* tail = &head;
			for (; tail->next; tail = tail->next.get(), ++chunk_index);

			tail->next = std::make_unique<Chunk>(chunk_index + 1);

			Slot& slot = tail->next->slots[0];
			ASSERT(std::holds_alternative<u32>(slot.desc_or_next));

			next_available_index = std::get<u32>(slot.desc_or_next);

			Entity out{next_available_index - 1, ++slot.version};

			slot.desc_or_next.emplace<EntityDesc>();

			return out;
		}
	}

	auto free_entity(const Entity entity) -> void {
		assert_is_entity_valid(entity);

		if (entity.get_index() < next_available_index) {
			Chunk* chunk = &head;
			for (u32 i = 0; i < entity.get_index() / Chunk::SLOTS_COUNT; ++i, chunk = chunk->next.get());

			Slot& slot = chunk->slots[entity.get_index() % Chunk::SLOTS_COUNT];
			ASSERT(std::holds_alternative<EntityDesc>(slot.desc_or_next));

			slot.desc_or_next.emplace<u32>(next_available_index);
			next_available_index = entity.get_index();
		} else {
			u32 next = next_available_index;
			u32 chunk_index = 0;
			Chunk* chunk = &head;
			while (true) {
				for (; chunk_index < next / Chunk::SLOTS_COUNT; ++chunk_index, chunk = chunk->next.get());

				Slot& slot = chunk->slots[next % Chunk::SLOTS_COUNT];
				ASSERT(std::holds_alternative<u32>(slot.desc_or_next));

				u32& slot_next = std::get<u32>(slot.desc_or_next);

				if (slot_next == INVALID_INDEX || slot_next > entity.get_index()) {// Point current slot to entity index and slot at entity index to slot_next (insert into linked-list).
					const u32 old_slot_next = slot_next;
					slot_next = entity.get_index();

					for (; chunk_index < entity.get_index() / Chunk::SLOTS_COUNT; ++chunk_index, chunk = chunk->next.get());

					Slot& entity_slot = chunk->slots[entity.get_index() % Chunk::SLOTS_COUNT];
					ASSERT(std::holds_alternative<EntityDesc>(entity_slot.desc_or_next));

					entity_slot.desc_or_next.emplace<u32>(old_slot_next);
					break;
				} else {
					next = slot_next;// Continue.
				}
			}
		}
	}

	[[nodiscard]] auto is_entity_valid(const Entity entity) const -> bool {
		if (!entity) {
			return false;
		}

		const Chunk* chunk = &head;
		for (u32 i = 0; i < entity.get_index() / Chunk::SLOTS_COUNT; ++i) {
			if (!(chunk = chunk->next.get())) {
				return false;
			}
		}

		// Check that versions match.
		return chunk->slots[entity.get_index() % Chunk::SLOTS_COUNT].version == entity.get_version();
	}

	FORCEINLINE auto assert_is_entity_valid(const Entity entity) const -> void {
#if ASSERTIONS_ENABLED
		ASSERTF(!entity.is_null(), "INVALID ENTITY! Entity is NULL!");

		const Chunk* chunk = &head;
		for (u32 i = 0; i < entity.get_index() / Chunk::SLOTS_COUNT; ++i, chunk = chunk->next.get()) {
			ASSERTF(!!chunk, "INVALID ENTITY! Entity index {} is out of range {}!", entity.get_index(), i * Chunk::SLOTS_COUNT);
		}

		const Slot& slot = chunk->slots[entity.get_index() % Chunk::SLOTS_COUNT];
		ASSERTF(slot.version == entity.get_version(), "INVALID ENTITY! Version mismatch (dangling reference): {} != {}!", entity.get_version(), slot.version);
		ASSERTF(std::holds_alternative<EntityDesc>(slot.desc_or_next), "INVALID ENTITY! {} has been freed already!", entity);
#endif
	}

	template <typename Self>
	[[nodiscard]] auto get_entity_desc(this Self&& self, const Entity entity) -> auto& {
		self.assert_is_entity_valid(entity);

		auto* chunk = &self.head;
		for (u32 i = 0; i < entity.get_index() / Chunk::SLOTS_COUNT; ++i, chunk = chunk->next.get());

		auto& slot = chunk->slots[entity.get_index() % Chunk::SLOTS_COUNT];
		ASSERT(std::holds_alternative<EntityDesc>(slot.desc_or_next));

		return std::get<EntityDesc>(slot.desc_or_next);
	}

	[[nodiscard]] auto is_entity_initialized(const Entity entity) const -> bool {
		ASSERTF(is_entity_valid(entity), "{} is invalid!", entity);
		return get_entity_desc(entity).is_initialized();
	}

	Chunk head{0};
	u32 next_available_index = 0;
};

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

#if 0
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
#endif

#if 0
	template <bool ALWAYS_DEFER = false, typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entity(const ExecContext& context, Comps&&... comps) -> Entity {
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

			if (!ALWAYS_DEFER && can_construct_entity_immediately) {
				const usize index_within_archetype = archetype.add_entity(entity, std::forward<Comps>(comps)...);
				EntityDesc& entity_desc = entities_access->get_entity_desc(entity);
				entity_desc.archetype_id = archetype_id;
				entity_desc.index_within_archetype = index_within_archetype;
			} else {
				entities_access.unlock();// No longer needed.

				
			}
		}
	}
#endif

	static inline Atomic<u32> count = 0;

	// Enqueues a task that will run before / after any accessing systems (biasing before if possible) so it will be thread-safe to modify the archetype (assuming nothing else is also accessing the archetype).
	template <bool ASSUMES_LOCKED = false>
	auto enqueue_archetype_mod_task(::cpts::Invokable<const SharedPtr<Task>&> auto&& fn, const Span<const Archetype*> archetypes, const Priority priority = Priority::HIGH, const Thread thread = Thread::ANY) -> SharedPtr<Task> {
		ASSERT(!archetypes.empty());

		SharedPtr<Task> out_task = Task::make(std::move(fn), priority, thread);

		// Schedule out_task against systems (biasing to execute before other systems if possible).
		{
			UniqueSharedLock _{ASSUMES_LOCKED ? null : &system_tasks_mutex};

			for (const auto* archetype : archetypes) {
				get_accessing_systems(archetype->description).for_each([&](const SystemId id) {
					auto task = system_tasks[id].lock();
					if (task) {// First try and schedule out_task before the system task, if that fails then schedule out_task after system_task. Guaranteed order dependance as long as systems are ordered.
					#if 0
						const auto result = out_task->add_subsequent(task);
						ASSERT(result != Task::AddSubsequentResult::PREREQUISITE_ALREADY_COMPLETED);

						if (result == Task::AddSubsequentResult::SUBSEQUENT_ALREADY_EXECUTING) {
							const auto other_result = task->add_subsequent(out_task);
							if (other_result == Task::AddSubsequentResult::SUCCESS) {
								fmt::println("Enqueueing after {}", get_type_info(id).name);
							}
						} else if (result == Task::AddSubsequentResult::SUCCESS) {
							fmt::println("Enqueueing before {}", get_type_info(id).name);
						}
						#endif

						const auto result = Task::try_add_subsequent_else_add_prerequisite(out_task, task);
						if (result != Task::TryAddSubsequentElsePrerequisiteResult::FAIL) {
							if (result == Task::TryAddSubsequentElsePrerequisiteResult::ADDED_SUBSEQUENT) {
								fmt::println("Enqueued before {}", get_type_info(id).name);
							} else {
								fmt::println("Enqueued after {}", get_type_info(id).name);
							}
						} else {
							fmt::println("{} failed to do thing", get_type_info(id).name);
						}
					}
				});
			}
		}

		task::enqueue(out_task, priority, thread);

		return out_task;
	}

	static inline constinit Atomic<bool> exclusively_executing = false;

	template <bool ASSUMES_LOCKED = false>
	auto enqueue_archetype_ctor_dtor_mod_task(Archetype& archetype, const ArchetypeId archetype_id, const Priority priority = Priority::HIGH, const Thread thread = Thread::ANY) -> SharedPtr<Task> {
		const Archetype* archetype_ptr = &archetype;// Need a memory address to create a span.
		return enqueue_archetype_mod_task<ASSUMES_LOCKED>([this, &archetype, archetype_id](const SharedPtr<Task>& this_task) {
			const bool previous = exclusively_executing.exchange(true);
			ASSERTF(!previous, "Not executing exclusively!");

			UniqueLock<Mutex> l1{null};
			UniqueExclusiveLock<SharedMutex> l2{null};

			u32 num_retries = 0;
			while (true) {
				l1 = UniqueLock{pending_entity_ctor_dtor.get_mutex()};
				if (auto result = UniqueExclusiveLock<SharedMutex>::from_try_lock(entities.get_mutex())) {
					l2 = std::move(*result);
					break;
				}

				l1.unlock();
				l2.unlock();

				thread::exponential_yield(num_retries);
			}

			fmt::println("Count == {}", count++);
			auto [dtors, ctors] = [&] {
				const auto it = pending_entity_ctor_dtor.get_unsafe().find(archetype_id);
				ASSERT(it != pending_entity_ctor_dtor.get_unsafe().end());

				auto out = std::move(it->second);
				pending_entity_ctor_dtor.get_unsafe().erase(archetype_id);

				return out;
			}();

			ASSERT(!dtors.empty() || !ctors.empty());

			auto* access = &entities.get_unsafe();

			// First destroy entities.
			for (const Entity entity : dtors) {
				const usize index_within_archetype = access->get_entity_desc(entity).index_within_archetype;
				const bool is_last_entity = index_within_archetype + 1 == archetype.num_entities;

				archetype.remove_at(index_within_archetype);

				if (!is_last_entity) {
					const Entity swapped_entity = archetype.get_entity(index_within_archetype);
					access->get_entity_desc(swapped_entity).index_within_archetype = index_within_archetype;
				}
			}

			// Spawn entities.
			if (!ctors.empty()) {
				const usize start = archetype.add_uninitialized_entities(ctors.size());

				// Assign entity indices within chunk.
				for (usize i = 0; i < ctors.size(); ++i) {
					access->get_entity_desc(ctors[i].entity).index_within_archetype = start + i;
				}

				usize num_already_constructed = 0;
				archetype.for_each_chunk_from_start(start, [&](Archetype::Chunk& chunk, const usize index_within_chunk, const usize count) {
					ASSERT(&chunk);
					ASSERTF(count + num_already_constructed <= ctors.size(), "Attempted to iterate over {} entities when size is {}!",
						count + num_already_constructed, ctors.size());

					// @TODO: This would be much faster if entities and components passed in were contiguously aligned.
					for (usize i = 0; i < count; ++i) {
						// Construct components.
						for (Any& comp : ctors[i + num_already_constructed].comps) {
							ASSERT(!!comp.get_type());

							const auto& type_info = *comp.get_type();

							const auto it = std::ranges::find_if(archetype.comps, [&](const Archetype::CompInfo& info) { return &type_info == &get_type_info(info.id); });
							ASSERTF(it != archetype.comps.end(), "Component {} does not exist on archetype!", type_info.name);

							void* dst = &chunk.data[it->offset_within_chunk + (index_within_chunk + i) * type_info.size];
							void* src = comp.get_data();

							type_info.move_construct(dst, src, 1);
						}

						// Construct entities.
						Entity* dst = reinterpret_cast<Entity*>(&chunk.data[archetype.entity_offset_within_chunk + (index_within_chunk + i) * sizeof(Entity)]);

						std::construct_at(dst, ctors[i + num_already_constructed].entity);
					}

					num_already_constructed += count;
				});
			}

			exclusively_executing.store(false);
		}, Span<const Archetype*>{&archetype_ptr, 1}, priority, thread);
	}

	template <typename... Comps> requires (sizeof...(Comps) > 0 && (!std::is_empty_v<std::decay_t<Comps>> || ...))
	auto spawn_entity(Comps&&... comps) -> Entity {
		const ArchetypeDesc archetype_desc{
			.comps = CompMask::make<std::decay_t<Comps>...>(),
		};

		const auto [archetype, archetype_id] = find_or_create_archetype(archetype_desc);

		const Entity entity = entities.write()->reserve_entity();

		PendingEntityConstruction ctor{
			.entity = entity,
		};

		((ctor.comps.emplace_back(Any::make<std::decay_t<Comps>>(std::forward<Comps>(comps)))), ...);

		const bool first_inserted_for_archetype = [&] {
			const auto access = pending_entity_ctor_dtor.lock();

			const auto [it, inserted] = access->try_emplace(archetype_id);
			ASSERT(it != access->end());

			auto& ctors = it->second.second;

			ctors.push_back(std::move(ctor));

			return inserted;
		}();


		// Enqueue the task that will handle constructing the entities.
		if (first_inserted_for_archetype) {
			enqueue_archetype_ctor_dtor_mod_task(archetype, archetype_id);
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

	template <bool ASSUMES_LOCKED = false>
	auto for_each_accessing_system_task(const CompMask& comps, ::cpts::Invokable<SharedPtr<Task>> auto&& fn) const -> void {
		const SystemMask systems = get_accessing_systems(ArchetypeDesc{
			.comps = comps,
		});

		UniqueSharedLock _{ASSUMES_LOCKED ? null : &system_tasks_mutex};

		systems.for_each([&](const SystemId id) {
			auto task = system_tasks[id].lock();
			if (task && !task->has_completed()) {
				std::invoke(FORWARD_AUTO(fn), std::move(task));
			}
		});
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

	Lock<Map<ArchetypeId, Pair<Array<Entity>, Array<PendingEntityConstruction>>>> pending_entity_ctor_dtor;
	Lock<Map<ArchetypeTraversal, Array<PendingEntityConstruction>>> pending_entity_archetype_traversal;

	volatile bool is_pending_destruction = false;

	RwLock<EntityList> entities;// At bottom because it has a huge inline allocation.

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