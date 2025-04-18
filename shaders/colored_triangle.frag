#version 450

// Shader input from vertex shader.
layout(location=0) in vec3 in_color;

// Output write.
layout(location=0) out vec4 out_frag_color;

void main() {
	out_frag_color = vec4(in_color, 1.f);
}