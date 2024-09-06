#version 450

layout(location=0) out vec3 out_color;

void main() {
	const vec3 positions[] = {
		vec3(1.f, 1.f, 0.f),
		vec3(-1.f, 1.f, 0.f),
		vec3(0.f, -1.f, 0.f),
	};

	const vec3 colors[] = {
		vec3(1.f, 0.f, 0.f),
		vec3(0.f, 1.f, 0.f),
		vec3(0.f, 0.f, 1.f),
	};

	// Output the position of each vertex.
	gl_Position = vec4(positions[gl_VertexIndex], 1.f);
	out_color = colors[gl_VertexIndex];
}