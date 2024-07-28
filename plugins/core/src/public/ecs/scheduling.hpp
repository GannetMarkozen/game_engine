#pragma once

#include "types.hpp"
#include "type_registry.hpp"

#if 0
namespace ecs {
namespace cpts {
template <typename T>
concept ExecutionGroup = ::cpts::EnumWithCount<T>;

template <typename T>
concept EventGroup = std::is_class_v<T> && std::is_empty_v<T>;
}

namespace impl {
template <::cpts::Enum auto VALUE>
struct EnumValueTagType;
}

struct EventGroupId final : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

static constexpr ExecutionGroupId VALUE = EventGroupId{static_cast<u16>(0)};

template <cpts::ExecutionGroup auto VALUE>
[[nodiscard]] FORCEINLINE auto get_execution_group_id() -> ExecutionGroupId {
	return TypeRegistry<ExecutionGroupId>::get_id<impl::EnumValueTagType<VALUE>>();
};

[[nodiscard]] FORCEINLINE auto get_num_execution_groups() -> usize {
	return TypeRegistry<ExecutionGroupId>::get_num_registered_types();
}

template <cpts::EventGroup T>
[[nodiscard]] FORCEINLINE auto get_event_group_id() -> EventGroupId {
	return TypeRegistry<EventGroupId>::get_id<T>();
}

[[nodiscard]] FORCEINLINE auto get_num_event_groups() -> usize {
	return TypeRegistry<EventGroupId>::get_num_registered_types();
}

struct Scheduler {
	struct ExecutionGroup {
		
	};
};
}
#endif