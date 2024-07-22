#pragma once

#include "types.hpp"

// Whether this type can be "moved" via memcpy and skipping the moved value's destructor.
template <typename>
struct IsTriviallyRelocatable {
	static constexpr bool VALUE = true;
};

template <typename T, typename Allocator>
struct IsTriviallyRelocatable<Array<T, Allocator>> {
	static constexpr bool VALUE = IsTriviallyRelocatable<T>::VALUE;
};

template <typename T, typename HashOp, typename EqualOp, typename Allocator>
struct IsTriviallyRelocatable<Set<T, HashOp, EqualOp, Allocator>> {
	static constexpr bool VALUE = IsTriviallyRelocatable<T>::VALUE;
};

template <typename Key, typename Value, typename HashOp, typename EqualOp, typename Allocator>
struct IsTriviallyRelocatable<Map<Key, Value, HashOp, EqualOp, Allocator>> {
	static constexpr bool VALUE = IsTriviallyRelocatable<Key>::VALUE && IsTriviallyRelocatable<Value>::VALUE;
};

namespace cpts {
template <typename T>
concept TriviallyRelocatable = ::IsTriviallyRelocatable<T>::VALUE;

template <typename T>
concept HasToString = requires(const T t, String out_str) {
	to_string(t, out_str);
};

template <typename T>
concept HasFromString = requires(T t, const StringView in_str) {
	from_string(t, in_str);
};
}