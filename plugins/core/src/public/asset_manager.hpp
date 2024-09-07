#pragma once

#include <filesystem>

#include "types.hpp"
#include "rtti.hpp"

namespace cpts {
template <typename T>
concept Asset = requires (const std::filesystem::path& path) {
	{ T::load(path) } -> std::same_as<T>;
};
}

struct AssetBase {
	
};

struct Asset {
	u32 value;
};

struct AssetManager {
	// @NOTE: May return NULL.
	template <cpts::Asset T>
	[[nodiscard]] auto try_load(const std::filesystem::path& path) const -> SharedPtr<T> {
		const auto result = loaded_assets.find(path);
		if (result != loaded_assets.end()) {
			return std::static_pointer_cast<T>(result->second.lock());
		} else {
			return null;
		}
	}

	template <cpts::Asset T>
	[[nodiscard]] auto load(const std::filesystem::path& path) -> SharedPtr<T> {
		if (auto found = try_load<T>(path)) [[likely]] {
			return found;
		}

		// Load the object.
		auto new_asset = std::make_shared<T>(T::load(path));
		loaded_assets.insert(path, new_asset);

		return new_asset;
	}

private:
	Map<std::filesystem::path, WeakPtr<void>> loaded_assets;
};