#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "input_structures.glsl"

layout(location=0) out vec3 out_normal;
layout(location=1) out vec3 out_color;
layout(location=2) out vec2 out_uv;

struct Vertex {
	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
	Vertex vertices[];
};

// Push constants block.
layout(push_constant) uniform PushConstants {
	mat4 render_matrix;
	VertexBuffer vertex_buffer;
} push_constants;

void main() {
	const Vertex vertex = push_constants.vertex_buffer.vertices[gl_VertexIndex];
	const vec4 position = vec4(vertex.position, 1.f);

	gl_Position = scene_data.view_proj * push_constants.render_matrix * position;

	out_normal = (push_constants.render_matrix * vec4(vertex.normal, 0.f)).xyz;
	out_color = vertex.color.xyz * material_data.color_factors.xyz;
	out_uv = vec2(vertex.uv_x, vertex.uv_y);
}