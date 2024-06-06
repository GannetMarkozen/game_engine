#pragma once

#include "../defines.hpp"


struct GroupInfo {
	const char* name;
	Array<const char*> entry_names;
};

template<Enum T>
struct GroupTraits {
	static fn get() -> GroupInfo {
		return GroupInfo{
			.name = "None"
		};
	};
};

namespace core::ecs {
struct World;

struct SystemContext {
	World* world;
};

struct Ordering {
	// @TODO: Add debug information / bad reflection for groups.
	struct Group {
		u32 group_index;
		u32 group_value;
	};

	[[nodiscard]]
	explicit Ordering(const Enum auto group)
		: group{get_group(group)} {}

	template<Enum T>
	inline static fn get_group(const T group) -> Group {
		static const u32 group_index = [] {
			const auto old_size = group_infos.size();
			group_infos.push_back(GroupTraits<T>::get());
			return old_size;
		}();

		return Group{
			.group_index = group_index,
			.group_value = static_cast<u32>(group),
		};
	}

	template<Enum T>
	inline static fn get_group_info() -> const GroupInfo& {
		return group_infos[get_group(T{}).group_index];
	}

	fn add_prerequisite_group(const Enum auto group) -> Ordering& {
		prerequisite_groups.push_back(get_group(group));
		return *this;
	}

	fn add_subsequent_group(const Enum auto group) -> Ordering& {
		subsequent_groups.push_back(get_group(group));
		return *this;
	}

	Group group;
	Array<Group> prerequisite_groups;
	Array<Group> subsequent_groups;

private:
	EXPORT_API inline static constinit Array<GroupInfo> group_infos;
};

struct SystemInfo {
	const char* name = "None";
	Optional<Ordering> ordering;
	Optional<f32> tick_interval;
};

struct SystemBase {
	virtual ~SystemBase() = default;
	virtual fn execute(SystemContext& context) -> void = 0;
};
}