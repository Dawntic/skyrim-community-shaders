#pragma once

#include "TexGen.h"
#include "Utils/D3D.h"

#include <fstream>
#include <iomanip>

#include <DirectXPackedVector.h>

namespace TexGenHelpers
{
	inline HRESULT LoadDDSLevelZero(
		ID3D11Device* device,
		const std::filesystem::path& path,
		ID3D11Resource** resource,
		ID3D11ShaderResourceView** srv)
	{
		return DirectX::CreateDDSTextureFromFileEx(
			device,
			path.c_str(),
			0,
			D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE,
			0,
			0,
			DirectX::DDS_LOADER_IGNORE_MIPS,
			resource,
			srv,
			nullptr);
	}

	// Every generated LOD map is a single surface: one mip level, one array slice, no cube faces.
	// Written through explicit metadata so the property is enforced at the call rather than being an
	// accident of which SaveToDDSFile overload was picked.
	inline void SaveMapDDS(const DirectX::Image& image, const std::filesystem::path& path)
	{
		DirectX::TexMetadata metadata = {};
		metadata.width = image.width;
		metadata.height = image.height;
		metadata.depth = 1;
		metadata.arraySize = 1;
		metadata.mipLevels = 1;
		metadata.format = image.format;
		metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;

		DX::ThrowIfFailed(DirectX::SaveToDDSFile(&image, 1, metadata, DirectX::DDS_FLAGS_NONE, path.c_str()));
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

	inline void FillImage(const DirectX::Image& image, const float4& fill)
	{
		const float raw[4] = { fill.x, fill.y, fill.z, fill.w };
		const size_t channels = image.format == DXGI_FORMAT_R16_FLOAT ? 1 : 4;

		for (size_t y = 0; y < image.height; ++y) {
			auto row = image.pixels + y * image.rowPitch;
			for (size_t x = 0; x < image.width; ++x)
				for (size_t c = 0; c < channels; ++c)
					((uint16_t*)row)[x * channels + c] = DirectX::PackedVector::XMConvertFloatToHalf(raw[c]);
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
		if (parts.size() < 4 || parts[1] != "32")
			return false;

		try {
			cellX = std::stoi(parts[2]);
			cellY = std::stoi(parts[3]);
		} catch (...) {
			return false;
		}

		return true;
	}
	inline std::string GetCurrentWorldspaceID()
	{
		auto worldspace = RE::TES::GetSingleton()->GetRuntimeData2().worldSpace;
		while (worldspace && worldspace->parentWorld)
			worldspace = worldspace->parentWorld;

		return worldspace ? std::string(worldspace->GetFormEditorID()) : std::string();
	}

	inline int2 WorldToCell(float worldX, float worldY)
	{
		return int2((int)std::floor(worldX / TexGen::worldCellSize), (int)std::floor(worldY / TexGen::worldCellSize));
	}

	inline int2 GetTileOriginCell(const int2& cell, int cellsPerTile)
	{
		return int2(FloorDiv(cell.x, cellsPerTile), FloorDiv(cell.y, cellsPerTile)) * cellsPerTile;
	}

	inline std::filesystem::path GetTilePath(const std::string& worldspaceID, const std::string& mapTag, uint tileSize, int cellsPerTile, const int2& originCell)
	{
		return TexGen::cachePath / fmt::format("{}{}{}.{}.{}.{}.dds", worldspaceID, mapTag, tileSize, cellsPerTile, originCell.x, originCell.y);
	}

	inline std::filesystem::path GetHeightTileManifestPath(const std::string& worldspaceID)
	{
		return TexGen::cachePath / (worldspaceID + "_H_tiles.json");
	}

	inline std::vector<HeightTileFile> GetAtlasTiles(const std::string& worldspaceID, const std::string& mapTag, uint tileSize, int cellsPerTile, const TexGen::AtlasCellRange& range)
	{
		std::vector<HeightTileFile> tiles;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(TexGen::cachePath, ec)) {
			HeightTileFile tile;
			if (!ParseTileName(entry.path(), worldspaceID, mapTag, tile) || tile.tileSize != tileSize || tile.cellsPerTile != cellsPerTile)
				continue;

			const int2 tileMax = tile.originCell + int2(cellsPerTile - 1, cellsPerTile - 1);
			if (tile.originCell.x >= range.minCell.x && tile.originCell.y >= range.minCell.y && tileMax.x <= range.maxCell.x && tileMax.y <= range.maxCell.y)
				tiles.push_back(std::move(tile));
		}

		return tiles;
	}

	inline bool SaveHeightTileManifest(const std::string& worldspaceID, const std::vector<HeightTileFile>& tiles)
	{
		json names = json::array();
		for (const auto& tile : tiles)
			names.push_back(tile.path.filename().string());

		std::ofstream manifest(GetHeightTileManifestPath(worldspaceID));
		manifest << std::setw(2) << names;
		return manifest.good();
	}

	inline std::vector<HeightTileFile> LoadHeightTileManifest(const std::string& worldspaceID)
	{
		std::ifstream manifest(GetHeightTileManifestPath(worldspaceID));
		if (!manifest)
			return {};
		json names;
		manifest >> names;

		std::vector<HeightTileFile> tiles;
		for (const auto& name : names) {
			HeightTileFile tile;
			if (ParseTileName(TexGen::cachePath / name.get<std::string>(), worldspaceID, "_H", tile))
				tiles.push_back(std::move(tile));
		}
		return tiles;
	}

	inline size_t DeleteTiles(const std::vector<HeightTileFile>& tiles)
	{
		std::error_code ec;
		size_t removed = 0;
		for (const auto& tile : tiles)
			removed += std::filesystem::remove(tile.path, ec);
		return removed;
	}

	/// @brief Record both layouts an atlas needs: the tile grid it came from, and the area it covers.
	///
	/// These are not the same thing once the atlas is trimmed of empty cells, and conflating them
	/// breaks tile matching: bent normal generation looks for height tiles whose size and cell count
	/// match the recorded tile grid, and finds none if that grid has been overwritten with the
	/// atlas' own dimensions.
	///
	/// @param range The area the atlas covers, which need not align to the tile grid.
	inline bool UpdateAtlasLayout(const std::string& worldspaceID, const TexGen::AtlasCellRange& range, TexGen::Settings& settings)
	{
		auto tiles = LoadHeightTileManifest(worldspaceID);
		if (tiles.empty())
			return false;

		const auto tileSize = tiles.front().tileSize;
		const auto cellsPerTile = tiles.front().cellsPerTile;
		if (!std::ranges::all_of(tiles, [&](const HeightTileFile& tile) { return tile.tileSize == tileSize && tile.cellsPerTile == cellsPerTile; }))
			return false;
		if (cellsPerTile <= 0)
			return false;

		// The tile grid comes from the tiles themselves rather than from the atlas range, which no
		// longer describes it.
		int2 minTileCell = tiles.front().originCell;
		int2 maxTileCell = tiles.front().originCell;
		for (const auto& tile : tiles) {
			minTileCell = int2{ std::min(minTileCell.x, tile.originCell.x), std::min(minTileCell.y, tile.originCell.y) };
			maxTileCell = int2{ std::max(maxTileCell.x, tile.originCell.x), std::max(maxTileCell.y, tile.originCell.y) };
		}

		const int tilesX = (maxTileCell.x - minTileCell.x) / cellsPerTile + 1;
		const int tilesY = (maxTileCell.y - minTileCell.y) / cellsPerTile + 1;

		const int2 cellExtent = range.maxCell - range.minCell + int2(1, 1);
		if (cellExtent.x <= 0 || cellExtent.y <= 0)
			return false;

		const bool changed = settings.cacheAtlasMinCellX != minTileCell.x ||
		                     settings.cacheAtlasMinCellY != minTileCell.y ||
		                     settings.cacheAtlasTileSize != (int)tileSize ||
		                     settings.cacheAtlasTileCells != cellsPerTile ||
		                     settings.cacheAtlasTilesX != tilesX ||
		                     settings.cacheAtlasTilesY != tilesY ||
		                     settings.cacheAtlasCellMinX != range.minCell.x ||
		                     settings.cacheAtlasCellMinY != range.minCell.y ||
		                     settings.cacheAtlasCellsX != cellExtent.x ||
		                     settings.cacheAtlasCellsY != cellExtent.y;

		settings.cacheAtlasMinCellX = minTileCell.x;
		settings.cacheAtlasMinCellY = minTileCell.y;
		settings.cacheAtlasTileSize = (int)tileSize;
		settings.cacheAtlasTileCells = cellsPerTile;
		settings.cacheAtlasTilesX = tilesX;
		settings.cacheAtlasTilesY = tilesY;

		settings.cacheAtlasCellMinX = range.minCell.x;
		settings.cacheAtlasCellMinY = range.minCell.y;
		settings.cacheAtlasCellsX = cellExtent.x;
		settings.cacheAtlasCellsY = cellExtent.y;
		return changed;
	}

	/// @brief Cell bounds of everything in a stitched height atlas that is not empty.
	///
	/// Tiles cover whole tiles of cells, so an atlas always reaches past the worldspace: Tamriel's
	/// cells stop at -57 but its tiles start at -64, leaving a border of seven empty cells on one
	/// side and two on another. Cells the worldspace never defined are left at zero height, so the
	/// filled area is found rather than assumed, which also drops empty ocean cells inside the
	/// nominal bounds.
	///
	/// Whole cells only: a partly filled cell is kept in full.
	/// @return False when the atlas is empty end to end.
	inline bool FindFilledCellBounds(const DirectX::Image& image, const int2& atlasMinCell, const int2& atlasMaxCell, int texelsPerCell, int2& o_minCell, int2& o_maxCell)
	{
		if (texelsPerCell <= 0 || image.format != DXGI_FORMAT_R16_FLOAT)
			return false;

		const int cellsX = atlasMaxCell.x - atlasMinCell.x + 1;
		const int cellsY = atlasMaxCell.y - atlasMinCell.y + 1;
		if (cellsX <= 0 || cellsY <= 0)
			return false;

		std::vector<bool> columnFilled((size_t)cellsX, false);
		std::vector<bool> rowFilled((size_t)cellsY, false);

		for (size_t y = 0; y < image.height; ++y) {
			const auto* row = (const uint16_t*)(image.pixels + y * image.rowPitch);
			const int cellRow = (int)(y / (size_t)texelsPerCell);

			for (size_t x = 0; x < image.width; ++x) {
				// Both signed zeroes read as empty; every unsampled texel is written as plain zero.
				if ((row[x] & 0x7FFF) == 0)
					continue;

				columnFilled[x / (size_t)texelsPerCell] = true;
				rowFilled[cellRow] = true;
			}
		}

		int firstColumn = -1, lastColumn = -1, firstRow = -1, lastRow = -1;
		for (int i = 0; i < cellsX; ++i)
			if (columnFilled[i]) {
				if (firstColumn < 0)
					firstColumn = i;
				lastColumn = i;
			}
		for (int i = 0; i < cellsY; ++i)
			if (rowFilled[i]) {
				if (firstRow < 0)
					firstRow = i;
				lastRow = i;
			}

		if (firstColumn < 0 || firstRow < 0)
			return false;

		// Image rows run north to south while cells count northward, so the row range flips.
		o_minCell = int2(atlasMinCell.x + firstColumn, atlasMaxCell.y - lastRow);
		o_maxCell = int2(atlasMinCell.x + lastColumn, atlasMaxCell.y - firstRow);
		return true;
	}

	/// @brief Copy the cells of a stitched atlas that fall inside a smaller cell range.
	inline bool CropAtlasToCells(const DirectX::Image& image, const int2& atlasMinCell, const int2& atlasMaxCell, const int2& cropMinCell, const int2& cropMaxCell, int texelsPerCell, DirectX::ScratchImage& o_cropped)
	{
		if (texelsPerCell <= 0)
			return false;
		if (cropMinCell.x < atlasMinCell.x || cropMinCell.y < atlasMinCell.y || cropMaxCell.x > atlasMaxCell.x || cropMaxCell.y > atlasMaxCell.y)
			return false;

		const size_t bytesPerPixel = DirectX::BitsPerPixel(image.format) / 8;
		if (bytesPerPixel == 0)
			return false;

		const size_t width = (size_t)(cropMaxCell.x - cropMinCell.x + 1) * texelsPerCell;
		const size_t height = (size_t)(cropMaxCell.y - cropMinCell.y + 1) * texelsPerCell;

		if (FAILED(o_cropped.Initialize2D(image.format, width, height, 1, 1)))
			return false;

		// Left edge counts up from the atlas' west cell; the top edge counts down from its north.
		const size_t offsetX = (size_t)(cropMinCell.x - atlasMinCell.x) * texelsPerCell;
		const size_t offsetY = (size_t)(atlasMaxCell.y - cropMaxCell.y) * texelsPerCell;

		const DirectX::Image* target = o_cropped.GetImages();
		for (size_t y = 0; y < height; ++y) {
			const auto* source = image.pixels + (offsetY + y) * image.rowPitch + offsetX * bytesPerPixel;
			memcpy(target->pixels + y * target->rowPitch, source, width * bytesPerPixel);
		}

		return true;
	}
	inline bool FindAtlas(const std::string& worldspaceID, const std::string& mapTag, std::filesystem::path& o_path, TexGen::AtlasCellRange& o_range)
	{
		if (worldspaceID.empty())
			return false;

		std::error_code ec;
		if (!std::filesystem::exists(TexGen::cachePath, ec))
			return false;

		bool found = false;
		for (const auto& entry : std::filesystem::directory_iterator(TexGen::cachePath, ec)) {
			const auto& path = entry.path();
			if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
				continue;

			TexGen::AtlasCellRange range;
			if (!ParseAtlasName(path, worldspaceID, mapTag, range))
				continue;

			o_path = path;
			o_range = range;
			found = true;
		}

		return found;
	}

	inline bool AtlasTexelToCell(const int2& texel, const TexGen::Settings& settings, int2& o_cell)
	{
		if (settings.cacheAtlasTileSize <= 0 || settings.cacheAtlasTileCells <= 0 || settings.cacheAtlasCellsY <= 0)
			return false;

		const int texelsPerCell = settings.cacheAtlasTileSize / settings.cacheAtlasTileCells;
		if (texelsPerCell <= 0)
			return false;

		// Against the trimmed area the atlas covers, not the tile grid it was stitched from.
		const int maxCellY = settings.cacheAtlasCellMinY + settings.cacheAtlasCellsY - 1;
		o_cell = int2(settings.cacheAtlasCellMinX + FloorDiv(texel.x, texelsPerCell),
			maxCellY - FloorDiv(texel.y, texelsPerCell));
		return true;
	}

	inline bool IsPositionValid(RE::NiPoint3 inputPosition)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto diff = player->GetPosition() - inputPosition;
		return std::max(std::abs(diff.x), std::abs(diff.y)) < TexGen::worldCellSize;
	}

	inline float GetRayIntersectionHeight(float3 position, float rayOffset)
	{
		static constexpr int MAX_ATTEMPTS = 10;
		static float prevZ = 0.0f;
		auto player = RE::PlayerCharacter::GetSingleton();
		auto cell = player->GetParentCell();
		auto bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
		auto hkpWorld = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
		if (!hkpWorld)
			return prevZ;

		float scale = RE::bhkWorld::GetWorldScale();
		float2 posScaledXY = float2(position.x * scale, position.y * scale);
		float currentZ = position.z + rayOffset;
		float endZ = position.z - rayOffset;

		for (int i = 0; i < MAX_ATTEMPTS; i++) {
			RE::hkpWorldRayCastInput input;
			input.from.quad.m128_f32[0] = posScaledXY.x;
			input.from.quad.m128_f32[1] = posScaledXY.y;
			input.from.quad.m128_f32[2] = currentZ * scale;
			input.from.quad.m128_f32[3] = 0;
			input.to.quad.m128_f32[0] = posScaledXY.x;
			input.to.quad.m128_f32[1] = posScaledXY.y;
			input.to.quad.m128_f32[2] = endZ * scale;
			input.to.quad.m128_f32[3] = 0;

			RE::hkpWorldRayCastOutput output;
			hkpWorld->CastRay(input, output);
			if (!output.HasHit())
				return prevZ;

			auto collisionObj = output.rootCollidable->GetCollisionLayer();
			if (!(collisionObj == RE::COL_LAYER::kTerrain || collisionObj == RE::COL_LAYER::kGround || collisionObj == RE::COL_LAYER::kStatic)) {
				float rayLength = currentZ - endZ;
				currentZ = currentZ - output.hitFraction * rayLength - 50.0f;
				continue;
			}

			float rayLength = currentZ - endZ;
			float hitZ = currentZ - output.hitFraction * rayLength;
			prevZ = hitZ;
			return hitZ;
		}

		return prevZ;
	}

	inline void SetWorldPosition(const int2& currentCellXY, RE::NiPoint3& o_worldPos)
	{
		auto tes = RE::TES::GetSingleton();
		auto player = RE::PlayerCharacter::GetSingleton();
		float2 worldXY = float2((float)currentCellXY.x, (float)currentCellXY.y) * TexGen::worldCellSize;

		float landHeight;
		tes->GetLandHeight(RE::NiPoint3(worldXY.x, worldXY.y, 0), landHeight);

		float groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 15000);
		float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), player->GetParentCell());
		groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

		o_worldPos = RE::NiPoint3(worldXY.x, worldXY.y, groundHeight + 1500.0f);
		player->SetPosition(o_worldPos, false);
	}
}
