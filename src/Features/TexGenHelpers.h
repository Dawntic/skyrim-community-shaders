#pragma once

#include "TexGen.h"

namespace TexGenHelpers
{
	// Every generated LOD map is a single surface: one mip level, one array slice, no cube faces.
	// Written through explicit metadata so the property is enforced at the call rather than being an
	// accident of which SaveToDDSFile overload was picked.
	inline bool SaveMapDDS(const DirectX::Image& image, const std::filesystem::path& path)
	{
		DirectX::TexMetadata metadata = {};
		metadata.width = image.width;
		metadata.height = image.height;
		metadata.depth = 1;
		metadata.arraySize = 1;
		metadata.mipLevels = 1;
		metadata.format = image.format;
		metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

		HRESULT hr = DirectX::SaveToDDSFile(&image, 1, metadata, DirectX::DDS_FLAGS_NONE, path.c_str());
		if (FAILED(hr)) {
			logger::error("[TexGen] Failed to save {}: {:X}", path.string(), (uint32_t)hr);
			return false;
		}

		return true;
	}

	// Floor division; the tile grid is anchored to the worldspace cell grid, so negative cell
	// coordinates must round towards -inf rather than towards zero.
	inline int FloorDiv(int a, int b)
	{
		int q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			--q;
		return q;
	}

	// Split a file stem on '.', the separator both the tile and atlas names use.
	inline std::vector<std::string> SplitStem(const std::filesystem::path& path)
	{
		std::vector<std::string> parts;
		std::string current;
		for (char c : path.stem().string()) {
			if (c == '.') {
				parts.push_back(current);
				current.clear();
			} else
				current += c;
		}
		parts.push_back(current);

		return parts;
	}

	// "<Worldspace><mapTag>.<minCellX>.<minCellY>.<maxCellX>.<maxCellY>.dds", e.g. Tamriel_H.-64.-48.63.55.dds.
	// The cell range is inclusive at both ends, so the extent an atlas covers is readable from the file
	// itself instead of from settings that may since have moved on.
	inline std::filesystem::path MakeAtlasPath(const std::filesystem::path& dir, const std::string& worldspaceID, const std::string& mapTag, const int2& minCell, const int2& maxCell)
	{
		return dir / fmt::format("{}{}.{}.{}.{}.{}.dds", worldspaceID, mapTag, minCell.x, minCell.y, maxCell.x, maxCell.y);
	}

	inline bool ParseAtlasName(const std::filesystem::path& path, const std::string& worldspaceID, const std::string& mapTag, TexGen::AtlasCellRange& o_range)
	{
		auto parts = SplitStem(path);

		// Five parts, and the first is the bare tag: a tile name carries its texel size there instead.
		if (parts.size() != 5 || parts[0] != worldspaceID + mapTag)
			return false;

		try {
			o_range.minCell = int2(std::stoi(parts[1]), std::stoi(parts[2]));
			o_range.maxCell = int2(std::stoi(parts[3]), std::stoi(parts[4]));
		} catch (...) {
			return false;
		}

		if (o_range.maxCell.x < o_range.minCell.x || o_range.maxCell.y < o_range.minCell.y)
			return false;

		o_range.valid = true;
		return true;
	}

	// A rebuild writes a new name whenever the cell range changed, so the previous atlas has to go or
	// the lookup would find two. Only files matching this map's atlas pattern are touched.
	inline void RemoveExistingAtlases(const std::filesystem::path& dir, const std::string& worldspaceID, const std::string& mapTag)
	{
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
			const auto& path = entry.path();
			if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
				continue;

			TexGen::AtlasCellRange range;
			if (!ParseAtlasName(path, worldspaceID, mapTag, range))
				continue;

			if (std::filesystem::remove(path, ec))
				logger::info("[TexGen] Replaced previous atlas {}", path.string());
		}
	}

	// Encode a height in game units into the xLODGen 16 bit unsigned representation:
	// zero height is 32767, one step is 8 game units, so height = (encoded - 32767) * 8.
	inline uint16_t EncodeHeight16(float height)
	{
		float encoded = std::round(height / TexGen::heightExportScale) + TexGen::heightExportOffset;
		return (uint16_t)std::clamp(encoded, 0.0f, 65535.0f);
	}

	// Convert one row of sampled heights into the stored representation: either the 16 bit encoding
	// above or the raw game units. Shared by the DDS writer and the UI preview so both hold the
	// exact same data.
	inline void ConvertHeightRow(const float* src, uint8_t* dst, uint count, bool export16Bit)
	{
		if (export16Bit) {
			auto encoded = (uint16_t*)dst;
			for (uint x = 0; x < count; ++x)
				encoded[x] = EncodeHeight16(src[x]);
		} else {
			memcpy(dst, src, count * sizeof(float));
		}
	}

	inline DXGI_FORMAT HeightStorageFormat(bool export16Bit)
	{
		return export16Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT;
	}

	struct HeightTileFile
	{
		std::filesystem::path path;
		int2 originCell;
		int cellsPerTile;
		uint tileSize;
	};

	// Matches the names GetTilePath writes: "<Worldspace><mapTag><tileSize>.<cellsPerTile>.<originX>.<originY>".
	inline bool ParseTileName(const std::filesystem::path& path, const std::string& worldspaceID, const std::string& mapTag, HeightTileFile& o_tile)
	{
		auto parts = SplitStem(path);

		if (parts.size() != 4)
			return false;

		auto prefix = worldspaceID + mapTag;
		if (!parts[0].starts_with(prefix))
			return false;

		auto tileSizeText = parts[0].substr(prefix.size());
		if (tileSizeText.empty() || !std::ranges::all_of(tileSizeText, [](char c) { return std::isdigit((unsigned char)c) != 0; }))
			return false;

		try {
			o_tile.tileSize = (uint)std::stoul(tileSizeText);
			o_tile.cellsPerTile = std::stoi(parts[1]);
			o_tile.originCell = int2(std::stoi(parts[2]), std::stoi(parts[3]));
		} catch (...) {
			return false;
		}

		if (o_tile.tileSize == 0 || o_tile.cellsPerTile <= 0)
			return false;

		o_tile.path = path;
		return true;
	}

	// Fill an image with a constant, expressed normalised for UNORM formats and raw for float ones.
	inline void FillImage(const DirectX::Image& image, const float4& unormFill, const float4& floatFill)
	{
		const float unorm[4] = { unormFill.x, unormFill.y, unormFill.z, unormFill.w };
		const float raw[4] = { floatFill.x, floatFill.y, floatFill.z, floatFill.w };

		size_t channels = 0;
		switch (image.format) {
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R32_FLOAT:
			channels = 1;
			break;
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			channels = 4;
			break;
		default:
			memset(image.pixels, 0, image.slicePitch);
			return;
		}

		const bool isUnorm = image.format == DXGI_FORMAT_R16_UNORM || image.format == DXGI_FORMAT_R16G16B16A16_UNORM;

		for (size_t y = 0; y < image.height; ++y) {
			auto row = image.pixels + y * image.rowPitch;
			for (size_t x = 0; x < image.width; ++x) {
				for (size_t c = 0; c < channels; ++c) {
					if (isUnorm)
						((uint16_t*)row)[x * channels + c] = (uint16_t)std::clamp(unorm[c] * 65535.0f, 0.0f, 65535.0f);
					else
						((float*)row)[x * channels + c] = raw[c];
				}
			}
		}
	}

	struct TileInfo
	{
		int cellX, cellY;
		DirectX::ScratchImage image;
	};

	inline bool ParseLODTile(const std::filesystem::path& path, int& cellX, int& cellY)
	{
		auto parts = SplitStem(path);
		if (parts.size() < 4)
			return false;

		try {
			cellX = std::stoi(parts[2]);
			cellY = std::stoi(parts[3]);
		} catch (...) {
			return false;
		}

		return true;
	}
}
