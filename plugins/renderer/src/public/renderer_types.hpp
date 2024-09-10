#pragma once

#include "core_include.hpp"

#include <glm/glm.hpp>

struct Vertex {
	glm::vec3 position;
	f32 uv_x;
	glm::vec3 normal;
	f32 uv_y;
	glm::vec4 color;
};

struct Camera {
	f32 fov = 90.f;
	struct {
		f32 near = 0.1f;
		f32 far = 10000.f;
	} clipping_plane;
};