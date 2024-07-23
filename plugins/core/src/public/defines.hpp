#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <span>
#include <memory>
#include <string>
#include <optional>
#include <variant>
#include <functional>
#include <fmt/printf.h>
#include <fmt/color.h>
#include <stacktrace>

#if defined(__clang__)

#define COMPILER_CLANG 1
#define COMPILER_GCC 0
#define COMPILER_MSVC 0
#define COMPILER_NAME "Clang"

#elif defined(__GNUC__)

#define COMPILER_CLANG 0
#define COMPILER_GCC 1
#define COMPILER_MSVC 0
#define COMPILER_NAME "GNU GCC"

#elif defined(_MSC_VER)

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
#define FORCEINLINE __attribute__((always_inline)) inline
#define NOINLINE __attribute__((noinline))

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
	FORCEINLINE constexpr Enum& operator|=(Enum& Lhs, const Enum Rhs) 			{ return Lhs = (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr Enum& operator&=(Enum& Lhs, const Enum Rhs) 			{ return Lhs = (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr Enum& operator^=(Enum& Lhs, const Enum Rhs) 			{ return Lhs = (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr Enum  operator| (const Enum Lhs, const Enum Rhs)	{ return (Enum)((__underlying_type(Enum))Lhs | (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr Enum  operator& (const Enum Lhs, const Enum Rhs) 	{ return (Enum)((__underlying_type(Enum))Lhs & (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr Enum  operator^ (const Enum Lhs, const Enum Rhs) 	{ return (Enum)((__underlying_type(Enum))Lhs ^ (__underlying_type(Enum))Rhs); } \
	FORCEINLINE constexpr bool  operator! (const Enum E)             				{ return !(__underlying_type(Enum))E; } \
	FORCEINLINE constexpr Enum  operator~ (const Enum E)             				{ return (Enum)~(__underlying_type(Enum))E; }

#define NON_COPYABLE(Type) \
	Type(Type&&) = delete; \
	Type(const Type&) = delete; \
	Type& operator=(const Type&) = delete; \
	Type& operator=(Type&&) = delete;

// Helper macro to perfectly forward an auto&& type (annoying it has to be a macro).
#define FORWARD_AUTO(value) std::forward<decltype(value)>(value)

// Yeah. I'm doing that.
#define null nullptr

using i8 = int8_t;
using u8 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using isize = std::make_signed_t<size_t>;
using usize = size_t;
using uptr = uintptr_t;

using f32 = float;
using f64 = double;

static_assert(sizeof(f32) == 4);
static_assert(sizeof(f64) == 8);

// @NOTE: Requires ifdefs per-platform. This should be correct for all our use-cases though.
constexpr usize CACHE_LINE_SIZE = 64;

enum NoInit { NO_INIT };

namespace cpts {
template <typename T>
concept Struct = std::is_class_v<T>;

template <typename T>
concept Class = std::is_class_v<T>;

template <typename T>
concept Enum = std::is_enum_v<T>;

template <typename T>
concept EnumWithCount = requires {
	{ T::COUNT } -> std::same_as<T>;
} && Enum<T>;

template <typename T>
concept Fundamental = std::is_fundamental_v<T>;

template <typename T>
concept Pointer = std::is_pointer_v<T>;

template <typename T, typename... Args>
concept Invokable = requires(T&& t, Args&&... args) {
	{ std::invoke(std::forward<T>(t), std::forward<Args>(args)...) };
};

template <typename T, typename Return, typename... Args>
concept InvokableReturns = requires(T&& t, Args&&... args) {
	{ std::invoke(std::forward<T>(t), std::forward<Args>(args)...) } -> std::same_as<Return>;
};
}

namespace utils {
// Executes a callable and forces it to not inline.
template <typename... Args, cpts::Invokable<Args&&...> Fn>
NOINLINE auto noinline_exec(Fn&& fn, Args&&... args) -> decltype(auto) {
	return (std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...));
}

template <cpts::EnumWithCount T>
consteval auto enum_count() {
	return static_cast<__underlying_type(T)>(T::COUNT);
}
}