#pragma once

#include "../defines.hpp"
#include "types.hpp"
#include "math.hpp"

namespace core::ecs {
namespace impl {
EXPORT_API inline constinit std::atomic<u32> component_id_incrementer = 0;
}

template<typename T>
inline fn get_component_id() -> u32 {
	static const u32 id = impl::component_id_incrementer++;
	return id;
}

// May change at runtime.
inline fn get_component_count() -> u32 {
	return impl::component_id_incrementer.load(std::memory_order_relaxed);
}

// @TODO: Add THREADED_WRITE (for multiple thread write access. Complicates things so saving for later).
enum class ResourceAccess {
	READ, WRITE,
};
static constexpr usize RESOURCE_ACCESS_COUNT = 2;

struct Query {
	FORCEINLINE fn operator&=(const Query& other) -> Query& {
		#pragma unroll
		for (usize i = 0; i < RESOURCE_ACCESS_COUNT; ++i) {
			bits[i] &= other.bits[i];
		}
		return *this;
	}

	FORCEINLINE fn operator|=(const Query& other) -> Query& {
		#pragma unroll
		for (usize i = 0; i < RESOURCE_ACCESS_COUNT; ++i) {
			bits[i] |= other.bits[i];
		}
		return *this;
	}

	[[nodiscard]]
	FORCEINLINE fn operator&(const Query& other) const -> Query {
		Query copy = *this;
		return copy &= other;
	}

	[[nodiscard]]
	FORCEINLINE fn operator|(const Query& other) const -> Query {
		Query copy = *this;
		return copy |= other;
	}

	template<ResourceAccess ACCESS, typename T>
	FORCEINLINE fn add() -> Query& {
		bits[static_cast<u8>(ACCESS)].insert(get_component_id<T>(), true);
		return *this;
	}

	template<typename T>
	FORCEINLINE fn reads() -> Query& {
		return add<ResourceAccess::READ, T>();
	}

	template<typename T>
	FORCEINLINE fn writes() -> Query& {
		return add<ResourceAccess::WRITE, T>();
	}

	[[nodiscard]]
	FORCEINLINE fn get_access_bits(const ResourceAccess access) -> BitArray<>& {
		return bits[static_cast<u8>(access)];
	}

	[[nodiscard]]
	FORCEINLINE fn get_access_bits(const ResourceAccess access) const -> const BitArray<>& {
		return bits[static_cast<u8>(access)];
	}

private:
	BitArray<> bits[RESOURCE_ACCESS_COUNT];
};
}