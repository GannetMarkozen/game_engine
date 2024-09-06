#include "vk_loader.hpp"

#include "assert.hpp"

#include "fastgltf/core.hpp"
#include "glm/gtx/quaternion.hpp"
#include "types.hpp"

#include <filesystem>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

namespace vkutils {
auto load_gltf_meshes(const std::filesystem::path file_path, const FnRef<void(MeshAsset& mesh, const Span<const u32> indices, const Span<const Vertex> vertices)> upload_mesh_callback) -> Optional<Array<SharedPtr<MeshAsset>>> {
	fmt::println("Loading GLTF: ", file_path.string());

	auto result = fastgltf::GltfDataBuffer::FromPath(file_path);
	if (!result) [[unlikely]] {
		WARN("Failed to find GLTF from file path {}!", file_path.string());
		return NULL_OPTIONAL;
	}

	auto& data = result.get();

	static constexpr auto GLTF_OPTIONS = fastgltf::Options::LoadExternalBuffers;

	fastgltf::Asset gltf;
	fastgltf::Parser parser;

	if (auto loaded_gltf = parser.loadGltfBinary(data, file_path.parent_path(), GLTF_OPTIONS)) [[likely]] {
		gltf = std::move(loaded_gltf.get());
	} else {
		WARN("Failed to load GLTF from file path {}!", file_path.string());
		return NULL_OPTIONAL;
	}

	Array<SharedPtr<MeshAsset>> meshes;

	Array<u32> indices;
	Array<Vertex> vertices;

	for (auto& mesh : gltf.meshes) {
		indices.clear();
		vertices.clear();

		MeshAsset mesh_asset{
			.name = mesh.name.c_str(),
		};

		for (auto& primitive : mesh.primitives) {
			#if 0
			GeoSurface surface{
				.start_index = static_cast<u32>(indices.size()),
				.count = static_cast<u32>(gltf.accessors[primitive.indicesAccessor.value()].count),
			};
			#else
			mesh_asset.surfaces.push_back(GeoSurface{
				.start_index = static_cast<u32>(indices.size()),
				.count = static_cast<u32>(gltf.accessors[primitive.indicesAccessor.value()].count),
			});
			#endif

			const usize initial_vertex = vertices.size();

			// Load indices.
			{
				fastgltf::Accessor& index_accessor = gltf.accessors[primitive.indicesAccessor.value()];

				indices.reserve(indices.size() + index_accessor.count);

				fastgltf::iterateAccessor<u32>(gltf, index_accessor, [&](const u32 index) {
					indices.push_back(index + initial_vertex);
				});
			}

			// Load vertex positions.
			{
				fastgltf::Accessor& position_accessor = gltf.accessors[primitive.findAttribute("POSITION")->accessorIndex];
				vertices.reserve(vertices.size() + position_accessor.count);

				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, position_accessor, [&](const glm::vec3& RESTRICT position, const usize index) {
					vertices.push_back(Vertex{
						.position = position,
						.uv_x = 0.f,
						.normal{1.f, 0.f, 0.f},
						.uv_y = 0.f,
						.color{1.f},
					});
				});
			}

			// Load vertex normals.
			const auto normals = primitive.findAttribute("NORMAL");
			if (normals != primitive.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).accessorIndex], [&](const glm::vec3& RESTRICT normal, const usize index) {
					vertices[index + initial_vertex].normal = normal;
				});
			}

			// Load UVs.
			const auto uvs = primitive.findAttribute("TEXCOORD_0");
			if (uvs != primitive.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uvs).accessorIndex], [&](const glm::vec2& RESTRICT uv, const usize index) {
					vertices[index + initial_vertex].uv_x = uv.x;
					vertices[index + initial_vertex].uv_y = uv.y;
				});
			}

			const auto colors = primitive.findAttribute("COLOR_0");
			if (colors != primitive.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*colors).accessorIndex], [&](const glm::vec4& RESTRICT color, const usize index) {
					vertices[index + initial_vertex].color = color;
				});
			}
		}

		static constexpr bool OVERRIDE_COLORS = true;
		if constexpr (OVERRIDE_COLORS) {
			for (auto& vertex : vertices) {
				vertex.color = glm::vec4{vertex.normal, 1.f};
			}
		}

		upload_mesh_callback(mesh_asset, indices, vertices);

		meshes.push_back(std::make_shared<MeshAsset>(std::move(mesh_asset)));
	}

	return {std::move(meshes)};
}
}