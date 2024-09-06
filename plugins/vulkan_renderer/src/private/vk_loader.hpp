#pragma once

#include "vk_types.hpp"

#include "types.hpp"

namespace std::filesystem {
class path;
}

struct GeoSurface {
	u32 start_index;
	u32 count;
};

struct MeshAsset {
	String name;
	Array<GeoSurface> surfaces;
	GpuMeshBuffers mesh_buffers;
};

namespace vkutils {
[[nodiscard]] auto load_gltf_meshes(const std::filesystem::path file_path, const FnRef<void(MeshAsset& mesh, const Span<const u32> indices, const Span<const Vertex> vertices)> upload_mesh_callback) -> Optional<Array<SharedPtr<MeshAsset>>>;
}