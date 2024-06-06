#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <array>
#include <span>
#include <memory>
#include <string>
#include <optional>
#include <functional>
#include <fmt/printf.h>
#include <fmt/color.h>
#include <stacktrace>

#ifdef __clang__

#define COMPILER_CLANG 1
#define COMPILER_GCC 0
#define COMPILER_MSVC 0
#define COMPILER_NAME "Clang"

#elifdef __GNUC__

#define COMPILER_CLANG 0
#define COMPILER_GCC 1
#define COMPILER_MSVC 0
#define COMPILER_NAME "GNU GCC"

#elifdef __MSC_VER

#define COMPILER_CLANG 0
#define COMPILER_GCC 0
#define COMPILER_MSVC 1
#define COMPILER_NAME "Microsoft Visual Studio"

#else

static_assert(false, "Unknown compiler!");

#endif

#ifndef EXPORT_API
#define EXPORT_API __declspec(dllexport)
#else
static_assert(false, "EXPORT_API already defined!");
#endif

#if COMPILER_GCC || COMPILER_CLANG

#define ASSUME(...) __builtin_assume(__VA_ARGS__)
#define UNREACHABLE __builtin_unreachable()

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

#ifndef NOINLINE
#define NOINLINE __attribute__((noinline))
#endif

#elif COMPILER_MSVC

#define ASSUME(...) __assume(__VA_ARGS__)
#define UNREACHABLE __assume(0)
#define FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)

#else
static_assert(false, "Unknown compiler");
#endif

#if COMPILER_GCC || COMPILER_CLANG || COMPILER_MSVC
#define RESTRICT __restrict
#else
#define RESTRICT
#endif

#define DECLARE_ENUM_CLASS_FLAGS(Enum) \
	inline constexpr Enum& operator|=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum& operator&=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum& operator^=(Enum& Lhs, Enum Rhs) { return Lhs = (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator| (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator& (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	inline constexpr Enum  operator^ (Enum  Lhs, Enum Rhs) { return (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	inline constexpr bool  operator! (Enum  E)             { return !(__underlying_type(Enum))E; } \
	inline constexpr Enum  operator~ (Enum  E)             { return (Enum)~(__underlying_type(Enum))E; }

#define NON_COPYABLE(Type) \
	Type(Type&&) = delete; \
	Type(const Type&) = delete; \
	Type& operator=(const Type&) = delete; \
	Type& operator=(Type&&) = delete;

// Helper macro to perfectly forward an auto&& type (annoying it has to be a macro).
#define FORWARD_AUTO(value) std::forward<std::decay_t<decltype(value)>>(value)

#define fn auto

using i8 = int8_t;
using u8 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using usize = size_t;
using uptr = uintptr_t;

using f32 = float;
using f64 = double;

static_assert(sizeof(f32) == 4);
static_assert(sizeof(f64) == 8);

// @NOTE: Requires ifdefs per-platform. This should be correct for all our use-cases though.
constexpr usize CACHE_LINE_SIZE = 64;

enum NoInit { NO_INIT };

template<typename T>
concept Enum = std::is_enum_v<T>;

template<typename T, typename... Args>
concept Invokable = requires(T&& t, Args&&... args) {
	{ std::invoke(std::forward<T>(t), std::forward<Args>(args)...) };
};

template<typename T, typename Return, typename... Args>
concept InvokableReturns = requires(T&& t, Args&&... args) {
	{ std::invoke(std::forward<T>(t), std::forward<Args>(args)...) } -> std::same_as<Return>;
};

template<typename T, typename Allocator = std::allocator<T>>
using Array = std::vector<T, Allocator>;

template<typename T, usize EXTENT = std::dynamic_extent>
using Span = std::span<T, EXTENT>;

template<typename T, usize SIZE>
using StaticArray = std::array<T, SIZE>;

template<typename T1, typename T2>
using Pair = std::pair<T1, T2>;

using String = std::string;
using StringView = std::string_view;

template<typename... Ts>
using Tuple = std::tuple<Ts...>;

template<typename Key, typename Value, typename HashOp = std::hash<Key>, typename EqualOp = std::equal_to<Key>, typename Allocator = std::allocator<Pair<Key, Value>>>
using Map = std::unordered_map<Key, Value, HashOp, EqualOp, Allocator>;

template<typename T, typename HashOp = std::hash<T>, typename EqualOp = std::equal_to<T>, typename Allocator = std::allocator<T>>
using Set = std::unordered_set<T, HashOp, EqualOp, Allocator>;

template<typename T, typename Deleter = std::default_delete<T>>
using UniquePtr = std::unique_ptr<T, Deleter>;

template<typename T>
using SharedPtr = std::shared_ptr<T>;

template<typename T>
using WeakPtr = std::weak_ptr<T>;

template<typename T>
using Optional = std::optional<T>;

template<typename>
class Fn;

template<typename Return, typename... Params>
class Fn<Return(Params...)> final : public std::function<Return(Params...)> {
	using std::function<Return(Params...)>::function;
};