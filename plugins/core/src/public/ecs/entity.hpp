#pragma once

#include "defines.hpp"

struct alignas(8) Entity {
	constexpr Entity() : value{UINT64_MAX} {}
	explicit Entity(NoInit) {}
	constexpr explicit Entity(const u32 in_index, const u32 in_version) : index{in_index}, version{in_version} {}
	constexpr Entity(const Entity&) = default;
	constexpr fn operator=(const Entity&) -> Entity& = default;

	union {
		struct {
			u32 index, version;
		};
		u64 value;
	};
};

static_assert(sizeof(Entity) == sizeof(u64));
static_assert(alignof(Entity) == alignof(u64));