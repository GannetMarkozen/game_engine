#pragma once

#include "ids.hpp"
#include "system.hpp"
#include "world.hpp"
#include "utils.hpp"
#include "defaults.hpp"
#include "threading/task.hpp"

struct App;
template <typename...> struct Query;

// A wrapper around a reference to a resource. Use as system argument to
// automatically acquire the resource and generate access requirements.
template <typename T>
struct Res {
	using Type = T;

	constexpr Res() = default;
	constexpr Res(const Res&) = default;
	constexpr auto operator=(const Res&) -> Res& = default;

	constexpr explicit Res(T& [[clang::lifetimebound]] value)
		: value{&value} {}

	[[nodiscard]] constexpr auto get() const -> T& { return *value; }
	[[nodiscard]] constexpr auto operator*() const -> T& { return *value; }
	[[nodiscard]] constexpr auto operator->() const -> T* { return value; }

private:
	T* [[clang::lifetimebound]] value;
};

namespace impl {
template <typename> struct IsQuery { static constexpr bool VALUE = false; };
template <typename... Comps> struct IsQuery<Query<Comps...>> { static constexpr bool VALUE = true; };

template <typename> struct IsRes { static constexpr bool VALUE = false; };
template <typename T> struct IsRes<Res<T>> { static constexpr bool VALUE = true; };
}

namespace cpts {
template <typename... Comps>
concept Query = ::impl::IsQuery<Comps...>::VALUE;

template <typename T>
concept Res = ::impl::IsRes<T>::VALUE;

template <typename T>
concept Plugin = requires (T t, App& app) {
	t.init(app);
};
}

struct Ordering {
	GroupId within = get_group_id<group::GameFrame>();
	GroupMask after;// Prerequisites.
	GroupMask before;// Subsequents.
};

struct SystemDesc {
	GroupId group = get_group_id<group::GameFrame>();
	EventId event = get_event_id<event::OnUpdate>();
	AccessRequirements access_requirements;
	Priority priority = Priority::NORMAL;
	Thread thread = Thread::ANY;
};

struct App {
	struct SystemInfo {
		Fn<UniquePtr<SystemBase>()> factory;
		SystemDesc desc;
	};

	App();

	[[nodiscard]] FORCEINLINE static auto build() -> App { return {}; }

	auto register_group(const GroupId group, Ordering ordering) -> App& {
#if ASSERTIONS_ENABLED
		ASSERTF(!registered_groups.has(group), "Double registered group {}!", TypeRegistry<GroupId>::get_type_info(group).name);
		ASSERTF(!ordering.within.is_valid() || registered_groups.has(ordering.within), "The group that {} is within {} must be registered first!", TypeRegistry<GroupId>::get_type_info(group).name, TypeRegistry<GroupId>::get_type_info(ordering.within).name);
		registered_groups.add(group);
#endif
		ASSERTF(!ordering.after.has(group) && !ordering.before.has(group), "Can not schedule group before or after self: {}!", TypeRegistry<GroupId>::get_type_info(group).name);
		ASSERTF(!(ordering.after & ordering.before).mask.has_any_set_bits(), "Can not schedule both before an after another group for group {}!", TypeRegistry<GroupId>::get_type_info(group).name);

#if 0
		if (ordering.within.is_valid()) {
			ordering.before |= group_subsequents[ordering.within];
			ordering.after |= group_prerequisites[ordering.within];
		}

		group_subsequents[group] |= ordering.before;
		ordering.before.for_each([&](const GroupId subsequent) {
			group_subsequents[subsequent].add(group);
		});

		group_prerequisites[group] |= ordering.after;
		ordering.after.for_each([&](const GroupId prerequisite) {
			group_subsequents[prerequisite].add(group);
		});

		return *this;
#endif

#if 0
		if (ordering.within.is_valid()) {
			ordering.before |= group_subsequents[ordering.within];
			ordering.after |= group_prerequisites[ordering.within];
		}

		group_subsequents[group] |= ordering.before;
		group_prerequisites[group] |= ordering.after;

		ordering.before.for_each([&](const GroupId id) {
			//group_subsequents[id].add(group);
			group_prerequisites[id].add(group);
		});

		ordering.after.for_each([&](const GroupId id) {
			//group_prerequisites[id].add(group);
			group_subsequents[id].add(group);
		});
#else

		group_subsequents[group] |= ordering.before;
		group_prerequisites[group] |= ordering.after;

		ordering.before.for_each([&](const GroupId id) {
			//group_subsequents[id].add(group);
			group_prerequisites[id].add(group);
		});

		ordering.after.for_each([&](const GroupId id) {
			//group_prerequisites[id].add(group);
			group_subsequents[id].add(group);
		});

		if (ordering.within.is_valid()) {
			nested_groups[ordering.within].add(group);
		}
#endif

		return *this;
	}

	template <typename T>
	auto register_group(Ordering ordering = {}) -> App& {
		return register_group(get_group_id<T>(), std::move(ordering));
	}

	template <cpts::System T, typename... Args> requires std::constructible_from<T, Args&&...>
	auto register_system(SystemDesc desc = {}, Args&&... args) -> App& {
#if ASSERTIONS_ENABLED
		ASSERTF(!registered_systems.has<T>(), "Double registered system {}!", utils::get_type_name<T>());
		registered_systems.add<T>();
#endif

		group_systems[desc.group].add(get_system_id<T>());

		event_systems[desc.event].add(get_system_id<T>());

		struct System final : public SystemBase {// @NOTE: Could be better to just store a void* and an execute fn.
			constexpr System(const Args&... args)
				: system{args...} {}

			virtual auto execute(ExecContext& context) -> void override {
				system.execute(context);
			}

			T system;
		};

		if constexpr (requires { { T::get_access_requirements() } -> std::same_as<AccessRequirements>; }) {
			desc.access_requirements |= T::get_access_requirements();
		}

		system_create_infos[get_system_id<T>()] = SystemInfo{
			.factory = std::bind([](const Args&... args) -> UniquePtr<SystemBase> {
				return std::make_unique<System>(args...);
			}, std::forward<Args>(args)...),
			.desc = std::move(desc),
		};

		return *this;
	}

	// @NOTE: Super evil. Parses raw function arguments and creates a system representing those arguments as members (with Res<> being an exception). Closer matches Bevy syntax.
	template <auto FN>
	struct FnSystem {
		using RawArgs = typename utils::FnSig<decltype(FN)>::ArgTypes;

		static_assert(std::convertible_to<ExecContext&, utils::TypeAtIndexInContainer<0, RawArgs>>, "System must have ExecContext& as it's first argument!");

		// System args excluding ExecContext.
		using Args = std::decay_t<decltype(utils::make_index_sequence_param_pack<std::tuple_size_v<RawArgs> - 1>([]<usize... Is> {
			return Tuple<std::decay_t<utils::TypeAtIndexInContainer<Is + 1, RawArgs>>...>{};
		}))>;

		// Resources will be retrieved, every other argument will become a member.
		using Members = utils::FilterContainer<[]<typename T> { return !cpts::Res<std::decay_t<T>>; }, Args>;

		// Parses arguments and detects access requirements.
		[[nodiscard]] static auto get_access_requirements() -> AccessRequirements {
			AccessRequirements out;

			[&]<usize I>(this auto&& self) -> void {
				if constexpr (I < std::tuple_size_v<Args>) {
					using T = utils::TypeAtIndexInContainer<I, Args>;

					if constexpr (cpts::Query<std::decay_t<T>>) {// Parse query components access.
						using Comps = typename std::decay_t<T>::Types;
						[&]<usize COMP_INDEX>(this auto&& self) -> void {
							if constexpr (COMP_INDEX < std::tuple_size_v<Comps>) {
								using Comp = utils::TypeAtIndexInContainer<COMP_INDEX, Comps>;

								if constexpr (std::is_const_v<Comp>) {
									out.comps.reads.add<std::decay_t<Comp>>();
								} else {
									out.comps.writes.add<std::decay_t<Comp>>();
								}

								self.template operator()<COMP_INDEX + 1>();
							}
						}.template operator()<0>();
					} else if constexpr (cpts::Res<std::decay_t<T>>) {// Parse resource access.
						using Res = typename T::Type;

						if constexpr (std::is_const_v<Res>) {
							out.resources.reads.add<std::decay_t<Res>>();
						} else {
							out.resources.writes.add<std::decay_t<Res>>();
						}
					}

					self.template operator()<I + 1>();
				}
			}.template operator()<0>();

			return out;
		}

		auto execute(ExecContext& context) -> void {
			utils::make_index_sequence_param_pack<std::tuple_size_v<Args>>([&]<usize... Is> {
				std::invoke(FN, context, [&] -> decltype(auto) {
					using T = utils::TypeAtIndexInContainer<Is, Args>;
					if constexpr (cpts::Res<std::decay_t<T>>) {
						using UnderlyingType = typename T::Type;
						if constexpr (std::is_const_v<UnderlyingType>) {
							return Res<UnderlyingType>{context.get_res<std::decay_t<UnderlyingType>>()};
						} else {
							return Res<UnderlyingType>{context.get_mut_res<std::decay_t<UnderlyingType>>()};
						}
					} else {
						static constexpr usize INDEX = [] consteval {
							usize count = 0;
							static constexpr usize I = Is;
							((count += Is < I && !cpts::Res<std::decay_t<utils::TypeAtIndexInContainer<Is, Args>>>), ...);
							return count;
						}();

						return (std::get<INDEX>(args));
					}
				}()...);
			});
		}

		NO_UNIQUE_ADDRESS Members args = {};
	};

	template <auto FN>
	auto register_system(SystemDesc desc = {}) -> App& {
		return register_system<FnSystem<FN>>(std::move(desc));
	}

	template <typename T, typename... Args> requires std::constructible_from<T, const Args&...>
	auto register_resource(Args&&... args) -> App& {
		resource_factories[get_res_id<T>()] = std::bind([](const Args&... args) -> void* {
			return new T{args...};
		}, std::forward<Args>(args)...);
#if ASSERTIONS_ENABLED
		registered_resources.add<T>();
#endif
		return *this;
	}

	template <typename T>
	auto register_resource(T&& resource) -> App& {
		resource_factories[get_res_id<std::decay_t<T>>()] = [resource = std::forward<T>(resource)] -> void* {
			return new std::decay_t<T>{resource};
		};
#if ASSERTIONS_ENABLED
		registered_resources.add<std::decay_t<T>>();
#endif
		return *this;
	}

	auto register_plugin(cpts::Plugin auto&& plugin) -> App& {
		plugin.init(*this);
		return *this;
	}

	auto run(const usize num_worker_threads = std::thread::hardware_concurrency() - 1) -> void;

	Array<GroupMask> group_subsequents;// Indexed via GroupId.
	Array<GroupMask> group_prerequisites;// Indexed via GroupId.
	GroupArray<GroupMask> nested_groups;
	Array<SystemMask> group_systems;// Indexed via GroupId.

	Array<SystemMask> comp_accessing_systems;// Indexed via CompId. All systems requiring access to this comp.

	Array<SystemInfo> system_create_infos;// Indexed via SystemId.
	Array<SystemMask> concurrent_conflicting_systems;// Indexed via SystemId.

	Array<SystemMask> event_systems;// Indexed via EventId. All systems registered to a given event.
	Array<SystemMask> event_system_prerequisites;// Indexed via EventId. These are the systems that need to complete execution before dispatching a new event.
	Array<GroupMask> event_root_groups;// Indexed via EventId.

	GroupMask root_groups;// Groups with no prerequisites.
	GroupMask leaf_groups;// Groups with no subsequents.

	ResArray<Fn<void*()>> resource_factories;

#if ASSERTIONS_ENABLED
	GroupMask registered_groups;
	SystemMask registered_systems;
	ResMask registered_resources;
#endif

	Optional<World> world;// Constructed in run().
};