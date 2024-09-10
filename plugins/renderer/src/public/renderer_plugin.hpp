#pragma once

#include "core_include.hpp"

struct App;

namespace renderer {
constexpr usize NUM_FRAMES_IN_FLIGHT = 2;

struct IsRendering {
	bool value = false;
};
}

enum class PresentMode : u8 {
	IMMEDIATE, FIFO, FIFO_RELAXED, MAILBOX
};

struct WindowConfig {
	const char* title = "Untitled";
	struct {
		usize width = 1980;
		usize height = 1080;
	} extent;
};

namespace group::renderer {
struct Init {};
struct Deinit {};
struct Extract {};

struct Draw {};
struct HandleWindowEvents {};
struct OpaquePass {};
struct TranslucentPass {};
}

struct RendererPlugin {
	PresentMode present_mode = PresentMode::MAILBOX;
	WindowConfig window_config;

	auto init(App& app) -> void;
};
