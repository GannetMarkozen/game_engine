#pragma once

#include "types.hpp"
#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

namespace math {
template <std::floating_point T> using Vec3 = glm::vec<3, T>;
template <std::floating_point T> using Vec2 = glm::vec<2, T>;
template <std::floating_point T> using Mat4x4 = glm::mat<4, 4, T>;
template <std::floating_point T> using Mat3x3 = glm::mat<3, 3, T>;
template <std::floating_point T> using Quat = glm::qua<T>;

// @NOTE: If we ever need double-precision world-coordinates, we'd only need to change translation.
// @NOTE: Not optimized.
template <std::floating_point T>
struct Transform {
	[[nodiscard]] static consteval auto identity() -> Transform {
		return {
			.translation{0.0, 0.0, 0.0},
			.rotation{1.0, 0.0, 0.0, 0.0},
			.scale{1.0, 1.0, 1.0},
		};
	}

	constexpr auto operator*=(const Transform& other) -> Transform& {
		translation = other.rotation * (other.scale * translation) + other.translation;
		rotation = other.rotation * rotation;
		scale *= other.scale;
		return *this;
	}

	[[nodiscard]] friend constexpr auto operator*(const Transform& a, const Transform& b) -> Transform {
		return a *= b;
	}

	[[nodiscard]] constexpr auto operator*(const Vec3<T>& v) -> Vec3<T> {
		return translation + rotation * v;
	}

	constexpr auto inverse() -> Transform& {
		translation *= -1.0;
		rotation = glm::inverse(rotation);
	}

	[[nodiscard]] constexpr auto get_inverse() const -> Transform {
		return auto{*this}.inverse();
	}

	[[nodiscard]] constexpr auto make_relative(const Transform& base) const -> Transform {
		return *this * base.get_inverse();
	}

	Vec3<T> translation;
	Quat<T> rotation;
	Vec3<T> scale;
};
}

using Vec3 = math::Vec3<f32>;
using Vec2 = math::Vec2<f32>;
using Mat4x4 = math::Mat4x4<f32>;
using Mat3x3 = math::Mat3x3<f32>;
using Quat = math::Quat<f32>;
using Transform = math::Transform<f32>;