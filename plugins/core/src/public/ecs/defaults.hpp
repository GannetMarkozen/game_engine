#pragma once

namespace ecs {
namespace group {
struct GameFrame {};
struct RenderFrame {};
}

namespace event {
struct OnInit {};// Dispatched on world-creation.
struct OnUpdate {};// Dispatched every frame.
}
}