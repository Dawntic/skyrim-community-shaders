#include "TerrainHeightSource.h"

#include "Utils/Game.h"

namespace
{
	// VHGT is a fixed 1096 byte payload: a base offset, a 33x33 grid of signed deltas, three pad bytes.
	struct VHGTData
	{
		float offset;
		std::int8_t deltas[TexGenLand::landVertCount];
		std::uint8_t padding[3];
	};
	static_assert(sizeof(VHGTData) == 1096);

	constexpr float kHeightScale = 8.0f;  // LAND deltas are stored in eighths of a game unit
	constexpr int kMaxWorldspaceCells = 512;

	float ByteSwapIfNeeded(RE::TESFile* a_file, float a_value)
	{
		if (!a_file->isBigEndian)
			return a_value;
		return std::bit_cast<float>(_byteswap_ulong(std::bit_cast<std::uint32_t>(a_value)));
	}

	// Cell records use sentinels and occasionally garbage for absent water, and a bad value here
	// would clamp a whole cell flat. Same guard WaterCache applies.
	bool IsUsableWaterHeight(float a_height)
	{
		return std::isfinite(a_height) && a_height != FLT_MIN && a_height != FLT_MAX && std::fabs(a_height) < 50000.0f;
	}
}

namespace TexGenLand
{
	LandFileSet::LandFileSet(RE::TESWorldSpace* a_worldSpace) :
		worldSpace(a_worldSpace)
	{
		if (!worldSpace)
			return;

		auto* sourceFiles = worldSpace->sourceFiles.array;
		if (!sourceFiles)
			return;

		files.reserve(sourceFiles->size());
		for (auto* file : *sourceFiles) {
			// Never seek the engine's own TESFile - its cursor is shared with cell streaming.
			if (auto* duplicate = file ? file->Duplicate() : nullptr)
				files.push_back(duplicate);
		}
	}

	LandFileSet::~LandFileSet()
	{
		for (auto* file : files)
			file->CloseTES(true);
	}

	bool LandFileSet::ReadCell(int a_cellX, int a_cellY, CellHeights& o_out)
	{
		o_out = {};
		if (!Valid())
			return false;

		const auto x = static_cast<std::int32_t>(a_cellX);
		const auto y = static_cast<std::int32_t>(a_cellY);

		// Phase one: the winning CELL is the one in the highest priority plugin that defines it.
		// Its XCLW is the water height that a later plugin's edit would have overridden.
		int index = static_cast<int>(files.size()) - 1;
		float waterHeight = FLT_MAX;
		for (; index >= 0; --index) {
			if (!files[index]->SeekCell(worldSpace, x, y))
				continue;

			o_out.hasCell = true;
			if (files[index]->SeekNextSubrecordType(Util::FCC("XCLW"))) {
				float raw = 0.0f;
				if (files[index]->ReadData(&raw, sizeof(raw)))
					waterHeight = ByteSwapIfNeeded(files[index], raw);
			}
			break;
		}

		if (!o_out.hasCell)
			return false;

		o_out.waterHeight = IsUsableWaterHeight(waterHeight) ? waterHeight : worldSpace->defaultWaterHeight;
		if (!IsUsableWaterHeight(o_out.waterHeight))
			o_out.waterHeight = -FLT_MAX;  // no usable water plane, so the clamp becomes a no-op

		// Phase two: carry on from the same plugin. A file cannot hold a LAND child without holding
		// the CELL, so the first one from here down that has both wins.
		for (; index >= 0; --index) {
			if (!files[index]->SeekCell(worldSpace, x, y) || !files[index]->SeekLandscapeForCurrentCell())
				continue;
			if (!files[index]->SeekNextSubrecordType(Util::FCC("VHGT")))
				continue;

			VHGTData data{};
			if (!files[index]->ReadData(&data, sizeof(data)))
				continue;

			float rowBase = ByteSwapIfNeeded(files[index], data.offset);
			for (int row = 0; row < landGridEdge; ++row) {
				// The first delta of each row advances the running column-zero height; the rest
				// accumulate along the row from there.
				rowBase += static_cast<float>(data.deltas[row * landGridEdge]);

				float running = 0.0f;
				for (int col = 0; col < landGridEdge; ++col) {
					if (col != 0)
						running += static_cast<float>(data.deltas[row * landGridEdge + col]);
					o_out.heights[row * landGridEdge + col] = (running + rowBase) * kHeightScale;
				}
			}

			o_out.hasLand = true;
			break;
		}

		if (!o_out.hasLand)
			o_out.heights.fill(worldSpace->defaultLandHeight);

		return o_out.hasLand;
	}

	bool ResolveCellBounds(RE::TESWorldSpace* a_worldSpace, int2& o_minCell, int2& o_maxCell, const char*& o_source)
	{
		o_source = "none";
		if (!a_worldSpace || a_worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoLandscape))
			return false;

		auto plausible = [](const int2& min, const int2& max) {
			const int width = max.x - min.x + 1;
			const int height = max.y - min.y + 1;
			return width > 0 && height > 0 && width <= kMaxWorldspaceCells && height <= kMaxWorldspaceCells;
		};

		// MNAM names the map's corners. North west carries the minimum X but the *maximum* Y, so the
		// two corners cross over; reading them straight through mirrors the whole worldspace.
		const auto& map = a_worldSpace->worldMapData;
		int2 minCell{ map.nwCellX, map.seCellY };
		int2 maxCell{ map.seCellX, map.nwCellY };
		if (plausible(minCell, maxCell)) {
			o_minCell = minCell;
			o_maxCell = maxCell;
			o_source = "MNAM";
			return true;
		}

		// NAM0/NAM9 are world units, and the maximum is the exclusive corner.
		std::int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
		Util::WorldToCell(a_worldSpace->minimumCoords, minX, minY);
		Util::WorldToCell(a_worldSpace->maximumCoords, maxX, maxY);
		minCell = int2{ minX, minY };
		maxCell = int2{ maxX - 1, maxY - 1 };
		if (plausible(minCell, maxCell)) {
			o_minCell = minCell;
			o_maxCell = maxCell;
			o_source = "NAM0/NAM9";
			return true;
		}

		// Last resort: the extent of whatever cells the plugins actually defined.
		bool any = false;
		for (const auto& [key, cell] : a_worldSpace->cellMap) {
			const int2 coords{ key.x, key.y };
			if (!any) {
				minCell = maxCell = coords;
				any = true;
				continue;
			}
			minCell = int2{ std::min(minCell.x, coords.x), std::min(minCell.y, coords.y) };
			maxCell = int2{ std::max(maxCell.x, coords.x), std::max(maxCell.y, coords.y) };
		}

		if (any && plausible(minCell, maxCell)) {
			o_minCell = minCell;
			o_maxCell = maxCell;
			o_source = "cellMap";
			return true;
		}

		return false;
	}

	void PrimeFileTables(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY)
	{
		if (!a_worldSpace)
			return;

		LandFileSet primer(a_worldSpace);
		CellHeights scratch;
		primer.ReadCell(a_cellX, a_cellY, scratch);
	}
}
