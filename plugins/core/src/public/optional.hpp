#pragma once

#include "defines.hpp"
#include "assert.hpp"

enum NullOptional {
	NULL_OPTIONAL
};

template <typename T>
struct Optional {
	template <typename>
	friend struct Optional;

	FORCEINLINE constexpr Optional()
		: allocated{false} {}

	FORCEINLINE constexpr Optional(NullOptional)
		: Optional{} {}

	FORCEINLINE constexpr Optional(NoInit) {}

	FORCEINLINE constexpr Optional(const T& value)
		: value{value}, allocated{true} {}

	FORCEINLINE constexpr Optional(T&& value)
		: value{std::move(value)}, allocated{true} {}

	template <typename Other> requires std::constructible_from<T, const Other&>
	FORCEINLINE constexpr Optional(const Optional<Other>& other) {
		if ((allocated = other.allocated)) {
			new(&value) T{*other};
		}
	}

	template <typename Other> requires std::constructible_from<T, Other&&>
	FORCEINLINE constexpr Optional(Optional<Other>&& other) noexcept {
		if ((allocated = other.allocated)) {
			new(&value) T{std::move(*other)};
		}
	}

	template <typename... Args> requires std::constructible_from<T, Args&&...>
	FORCEINLINE constexpr Optional(Args&&... args)
		: value{std::forward<Args>(args)...}, allocated{true} {}

	template <typename Other> requires std::constructible_from<T, const Other&>
	FORCEINLINE constexpr auto operator=(const Optional<Other>& other) -> Optional& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		reset();

		if ((allocated = other.allocated)) {
			new(&value) T{*get()};
		}

		return *this;
	}

	template <typename Other> requires std::constructible_from<T, Other&&>
	FORCEINLINE constexpr auto operator=(Optional<Other>&& other) noexcept -> Optional& {
		if (this == &other) [[unlikely]] {
			return *this;
		}

		reset();

		if ((allocated = other.allocated)) {
			new(&value) T{std::move(*other)};
		}

		return *this;
	}

	FORCEINLINE constexpr ~Optional() {
		reset();
	}

	[[nodiscard]] FORCEINLINE constexpr auto has_value() const -> bool {
		return allocated;
	}

	[[nodiscard]] FORCEINLINE constexpr operator bool() const {
		return allocated;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get() & -> T& {
		ASSERTF(allocated, "Attempted to dereference NULL Optional!");
		return value;
	}

	[[nodiscard]] FORCEINLINE constexpr auto get() && -> T&& {
		ASSERTF(allocated, "Attempted to dereference NULL Optional!");
		return std::move(value);
	}

	[[nodiscard]] FORCEINLINE constexpr auto get() const -> const T& {
		ASSERTF(allocated, "Attempted to dereference NULL Optional!");
		return value;
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto operator*(this Self&& self) -> decltype(auto) {
		return (std::forward<Self>(self).get());
	}

	template <typename Self>
	[[nodiscard]] FORCEINLINE constexpr auto operator->(this Self&& self) -> decltype(auto) {
		return (&std::forward<Self>(self).get());
	}

	FORCEINLINE constexpr auto reset() -> bool {
		if (!allocated) {
			return false;
		}

		if constexpr (!std::is_trivially_destructible_v<T>) {
			value.~T();
		}
		allocated = false;

		return true;
	}

	// Destroys the current value and constructs a new one in it's place.
	template <typename Self, typename... Args> requires (!std::is_const_v<Self> && std::constructible_from<T, Args&&...>)
	FORCEINLINE constexpr auto emplace(this Self&& self, Args&&... args) -> decltype(auto) {
		self.reset();

		new(self.data) T{std::forward<Args>(args)...};
		self.allocated = true;

		return (std::forward<Self>(self).get());
	}

	// Assigns the value. Constructs if necessary.
	template <typename Self, typename Other> requires (!std::is_const_v<Self> && std::assignable_from<T, Other&&>)
	FORCEINLINE constexpr auto assign(this Self&& self, Other&& other) -> decltype(auto) {
		if (!self.allocated) {
			if constexpr (std::constructible_from<T, Other&&>) {
				new(&self.value) T{std::forward<Other>(other)};
			} else {
				self.emplace() = std::forward<Other>(other);
			}

			self.allocated = true;
		} else {
			self.get() = std::forward<Other>(other);
		}

		return (std::forward<Self>(self).get());
	}

private:
	union {
		T value;
	};
	bool allocated;
};