#pragma once

#include "defines.hpp"
#include "types.hpp"

struct Entity {
	constexpr Entity(const Entity&) = default;
	constexpr auto operator=(const Entity&) -> Entity& = default;

	FORCEINLINE constexpr Entity()
		: value{UINT64_MAX} {}

	FORCEINLINE constexpr explicit Entity(const u32 index, const u32 version)
		: index{index}, version{version} {}

	FORCEINLINE constexpr explicit Entity(NoInit) {}

	[[nodiscard]] FORCEINLINE constexpr auto get_index() const -> u32 {
		return index;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get_version() const -> u32 {
		return version;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get_value() const -> u64 {
		return value;
	}

	// Whether or not this entity points to anything at all.
	// @NOTE: This returning false does not necessarily mean the entity is valid!
	[[nodiscard]] FORCEINLINE constexpr auto is_null() const -> bool {
		return value == UINT64_MAX;
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return !is_null();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator!() const -> bool {
		return is_null();
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator==(const Entity& other) const -> bool {
		return value == other.value;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator!=(const Entity& other) const -> bool {
		return value != other.value;
	}

	[[nodiscard]] FORCEINLINE constexpr auto operator<=>(const Entity& other) const {
		return value <=> other.value;
	}

private:
	union {
		struct {
			u32 index, version;
		};
		u64 value;
	};
};

namespace std {
template <>
struct hash<Entity> {
	[[nodiscard]] FORCEINLINE constexpr auto operator()(const Entity& entity) {
		return std::hash<u64>{}(entity.get_value());
	}
};
}

constexpr Entity NULL_ENTITY{};

static_assert(sizeof(Entity) == sizeof(u64));
static_assert(alignof(Entity) == alignof(u64));

template <>
struct fmt::formatter<Entity> {
	constexpr auto parse(fmt::format_parse_context& context) {
		return context.begin();
	}

	template <typename FmtContext>
	auto format(const Entity entity, FmtContext& context) {
		if (entity.is_null()) {
			return fmt::format_to(context.out(), "Entity{{ NULL }}");
		} else {
			return fmt::format_to(context.out(), "Entity{{ .index = {}, .version = {} }}", entity.get_index(), entity.get_version());
		}
	}
};