#include "TexGen.h"
#include "TexGen/TerrainHeightSource.h"
#include "TexGenHelpers.h"

#include "Deferred.h"
#include "Features/Skylighting.h"

#include "Menu/ThemeManager.h"
#include "State.h"
#include "Utils/D3D.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "feature.texgen."

using namespace TexGenHelpers;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TexGen::Settings,
	cacheProgressX,
	cacheProgressY,
	cacheTileCells,
	cacheTileSize,
	cacheAtlasMinCellX,
	cacheAtlasMinCellY,
	cacheAtlasTileSize,
	cacheAtlasTileCells,
	cacheAtlasTilesX,
	cacheAtlasTilesY,
	dynDOLODPath,
	smoothRadius,
	smoothFlattenHeight,
	smoothRolloff,
	smoothIterations,
	skipBentNormalTiles)

//////////////////////////////////////////////////////////////////////////////////
//// Height cache tiles
//////////////////////////////////////////////////////////////////////////////////

void TexGen::ClearHeightTile(uint a_tileSize)
{
	heightTileSize = a_tileSize;
	heightTilePixels.assign((size_t)a_tileSize * a_tileSize, uint16_t(0));
}

void TexGen::SaveHeightTile(const int2& a_tileOriginCell, int a_cellsPerTile)
{
	const uint tileSize = heightTileSize;
	if (heightTilePixels.size() != (size_t)tileSize * tileSize) {
		logger::error("[TexGen] Height tile buffer is {} texels, expected {}", heightTilePixels.size(), (size_t)tileSize * tileSize);
		return;
	}

	auto path = GetTilePath(worldspaceID, "_H", tileSize, a_cellsPerTile, a_tileOriginCell);

	DirectX::ScratchImage outputImage;
	DX::ThrowIfFailed(outputImage.Initialize2D(DXGI_FORMAT_R16_FLOAT, tileSize, tileSize, 1, 1));

	const DirectX::Image* image = outputImage.GetImages();
	for (uint y = 0; y < tileSize; ++y)
		memcpy(image->pixels + y * image->rowPitch, heightTilePixels.data() + (size_t)y * tileSize, tileSize * sizeof(uint16_t));

	SaveMapDDS(*image, path);
	logger::info("[TexGen] Saved height tile {}", path.string());
}

// debugging func.
void TexGen::UpdateHeightPreview(const int2& a_tileOriginCell)
{
	auto context = globals::d3d::context;
	const uint tileSize = heightTileSize;
	const DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT;

	if (tileSize == 0 || heightTilePixels.size() != (size_t)tileSize * tileSize)
		return;

	// The preview holds exactly what the DDS holds, in the same format, unmodified.
	if (!heightPreviewTex || heightPreviewTex->desc.Width != tileSize || heightPreviewTex->desc.Format != format) {
		CD3D11_TEXTURE2D_DESC desc(format, tileSize, tileSize, 1, 1, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, format, 0, 1);

		heightPreviewValid = false;
		heightPreviewTex = eastl::make_unique<Texture2D>(desc, "TexGen::HeightTilePreview");
		heightPreviewTex->CreateSRV(srvDesc);
	}

	D3D11_MAPPED_SUBRESOURCE dst;
	DX::ThrowIfFailed(context->Map(heightPreviewTex->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst));

	for (uint y = 0; y < tileSize; ++y)
		memcpy((uint8_t*)dst.pData + y * dst.RowPitch, heightTilePixels.data() + (size_t)y * tileSize, tileSize * sizeof(uint16_t));

	context->Unmap(heightPreviewTex->resource.get(), 0);

	heightPreviewOrigin = a_tileOriginCell;
	heightPreviewValid = true;
}

void TexGen::GenerateHeightMap()
{
	static constexpr float CELL = worldCellSize;
	static constexpr int2 startCell = int2(-57, -43);  // same as dyndolod
	static constexpr int2 endCell = int2(61, 50);      // same as dyndolod, exclusive

	auto tes = RE::TES::GetSingleton();
	auto player = RE::PlayerCharacter::GetSingleton();
	auto worldSpace = player ? player->GetWorldspace() : nullptr;

	if (!tes || !worldSpace) {
		logger::error("[TexGen] tes or worldspace INVALID");
		return;
	}

	RE::PlayerCamera::GetSingleton()->GetRuntimeData2().idleTimer = 0;

	// Latched at the start of a run so changing the settings mid-run can't desync the tile layout.
	static int cellsPerTile = 8;
	static uint tileSize = 1024;
	static int2 currentTile = int2(0, 0);
	static int2 currentCellXY = startCell;
	static int cellIndexInTile = 0;
	static RE::NiPoint3 worldPositionSet = RE::NiPoint3();
	static int settleFrames = 0;

	if (heightGenInit) {  // latch the layout before anything derives from it
		cellsPerTile = GetHeightTileCells();
		tileSize = GetHeightTileSize();
	}

	const int texelsPerCell = (int)tileSize / cellsPerTile;
	const float worldRes = CELL / (float)texelsPerCell;  // world units per texel

	// Tile grid bounds, inclusive, anchored to the worldspace cell grid.
	const int2 startTile = int2(FloorDiv(startCell.x, cellsPerTile), FloorDiv(startCell.y, cellsPerTile));
	const int2 endTile = int2(FloorDiv(endCell.x - 1, cellsPerTile), FloorDiv(endCell.y - 1, cellsPerTile));

	auto inWorldRange = [&](const int2& cell) {
		if (heightGenSingleTile)  // the player's tile is generated whole, wherever it sits
			return true;
		return cell.x >= startCell.x && cell.x < endCell.x && cell.y >= startCell.y && cell.y < endCell.y;
	};

	// Walk to the next cell of the current tile that actually lies inside the worldspace.
	auto advanceToNextCell = [&]() {
		const int cellsInTile = cellsPerTile * cellsPerTile;
		while (++cellIndexInTile < cellsInTile) {
			int2 candidate = currentTile * cellsPerTile + int2(cellIndexInTile % cellsPerTile, cellIndexInTile / cellsPerTile);
			if (inWorldRange(candidate)) {
				currentCellXY = candidate;
				return true;
			}
		}
		return false;
	};

	auto advanceToNextTile = [&]() {
		if (++currentTile.x > endTile.x) {
			currentTile.x = startTile.x;
			if (++currentTile.y > endTile.y)
				return false;
		}
		return true;
	};

	// Start the current tile, skipping tiles that are entirely outside the worldspace.
	auto beginTile = [&]() {
		while (true) {
			cellIndexInTile = -1;
			if (advanceToNextCell()) {
				ClearHeightTile(tileSize);
				return true;
			}
			if (!advanceToNextTile())
				return false;
		}
	};

	if (heightGenInit) {
		RE::GetINISetting("iFPSClamp:General")->data.i = 0;
		RE::GetINISetting("bLockFramerate:Display")->data.b = false;
		RE::GetINISetting("iVSyncPresentInterval:Display")->data.b = false;
		RE::GetINISetting("bBorderRegionsEnabled:General")->data.b = false;
		RE::GetINISetting("fMaxTime:HAVOK")->data.f = 0.001f;

		cellsDone = 0;
		tilesDone = 0;
		settleFrames = 0;

		// Size the staging tile up front: beginTile only clears it when it finds a cell to sample.
		ClearHeightTile(tileSize);

		if (heightGenSingleTile) {
			// Whichever tile holds the requested cell, unclamped so it also works outside the
			// range a full run covers.
			currentTile = int2(FloorDiv(heightGenTargetCell.x, cellsPerTile), FloorDiv(heightGenTargetCell.y, cellsPerTile));
		} else {
			// Resume on a tile boundary; a partially generated tile is regenerated from scratch.
			currentTile = int2(FloorDiv(settings.cacheProgressX, cellsPerTile), FloorDiv(settings.cacheProgressY, cellsPerTile));
			currentTile.x = std::clamp(currentTile.x, startTile.x, endTile.x);
			currentTile.y = std::clamp(currentTile.y, startTile.y, endTile.y);
		}

		beginTile();

		logger::info("[TexGen] Generating {0} height cache: {1}x{1} tiles of {2}x{2} cells, {3} texels per cell, {4} units per texel, 16 bit float",
			heightGenSingleTile ? "single tile" : "full", tileSize, cellsPerTile, texelsPerCell, worldRes);

		SetWorldPosition(currentCellXY, worldPositionSet);
		settleFrames = heightSettleFrames;
		heightGenInit = false;
		return;
	}

	if (!IsPositionValid(worldPositionSet)) {
		SetWorldPosition(currentCellXY, worldPositionSet);
		return;
	}

	if (settleFrames > 0) {
		settleFrames--;
		return;
	}

	// write heightmap

	const int2 localCell = currentCellXY - currentTile * cellsPerTile;
	const float2 cellOrigin = float2((float)currentCellXY.x, (float)currentCellXY.y) * CELL;
	const float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), player->GetParentCell());

	for (int y = 0; y < texelsPerCell; ++y) {
		for (int x = 0; x < texelsPerCell; ++x) {
			float2 worldXY = cellOrigin + float2((float)x, (float)y) * worldRes;

			float landHeight;
			tes->GetLandHeight(RE::NiPoint3(worldXY.x, worldXY.y, 0), landHeight);

			float groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 5000);

			groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

			int2 texCoord = localCell * texelsPerCell + int2(x, y);
			heightTilePixels[(size_t)texCoord.y * tileSize + texCoord.x] = DirectX::PackedVector::XMConvertFloatToHalf(groundHeight);
		}
	}

	cellsDone += 1;

	if (!advanceToNextCell()) {
		// Tile complete: flush it to disk before moving on.
		SaveHeightTile(currentTile * cellsPerTile, cellsPerTile);
		UpdateHeightPreview(currentTile * cellsPerTile);
		tilesDone += 1;

		if (heightGenSingleTile) {  // one-off, leaves the full run's progress alone
			logger::info("[TexGen] Single height tile complete: {} cells", cellsDone);
			heightGenRunning = false;
			heightGenInit = true;
			return;
		}

		if (!advanceToNextTile() || !beginTile()) {
			logger::info("[TexGen] Height cache complete: {} tiles, {} cells", tilesDone, cellsDone);
			if (FinalizeHeightTiles()) {
				settings.cacheProgressX = startCell.x;
				settings.cacheProgressY = startCell.y;
				globals::state->Save();
			}
			heightGenRunning = false;
			heightGenInit = true;
			return;
		}

		// Progress is stored as the origin cell of the tile now in flight.
		settings.cacheProgressX = currentTile.x * cellsPerTile;
		settings.cacheProgressY = currentTile.y * cellsPerTile;
		globals::state->Save();
	}

	SetWorldPosition(currentCellXY, worldPositionSet);
	// after SetWorldPosition:
	settleFrames = heightSettleFrames;  // let terrain settle
}

void TexGen::VerifyLandDecode()
{
	using namespace TexGenLand;

	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	auto* tes = RE::TES::GetSingleton();
	if (!cell || cell->IsInteriorCell() || !tes) {
		logger::error("[TexGen] Land decode check needs the player in an exterior cell");
		return;
	}

	auto* worldspace = player->GetWorldspace();
	auto* coords = cell->GetCoordinates();
	auto* land = cell->GetRuntimeData().cellLand;
	if (!worldspace || !coords || !land || !land->loadedData) {
		logger::error("[TexGen] Land decode check: no loaded LAND to compare against");
		return;
	}

	const int cellX = coords->cellX;
	const int cellY = coords->cellY;

	LandFileSet files(worldspace);
	if (!files.Valid()) {
		logger::error("[TexGen] Land decode check: no source files for worldspace {}", worldspace->GetFormEditorID());
		return;
	}

	CellHeights decoded;
	if (!files.ReadCell(cellX, cellY, decoded)) {
		logger::error("[TexGen] Land decode check: no VHGT for cell {}, {} (hasCell {})", cellX, cellY, decoded.hasCell);
		return;
	}

	// Test one: against the absolute world Z the engine reports at the player's own position. This
	// is the check that actually matters, because the tiles must hold absolute heights, and it does
	// not care how LoadedLandData happens to be laid out.
	const auto playerPos = player->GetPosition();
	float engineHeight = 0.0f;
	const bool haveEngineHeight = tes->GetLandHeight(playerPos, engineHeight);

	const float localX = playerPos.x - (float)cellX * worldCellSize;
	const float localY = playerPos.y - (float)cellY * worldCellSize;
	const float vertexStep = worldCellSize / (float)(landGridEdge - 1);
	const int col0 = std::clamp((int)(localX / vertexStep), 0, landGridEdge - 2);
	const int row0 = std::clamp((int)(localY / vertexStep), 0, landGridEdge - 2);
	const float fx = localX / vertexStep - (float)col0;
	const float fy = localY / vertexStep - (float)row0;

	const int i00 = row0 * landGridEdge + col0;
	const float south = decoded.heights[i00] + (decoded.heights[i00 + 1] - decoded.heights[i00]) * fx;
	const float north = decoded.heights[i00 + landGridEdge] + (decoded.heights[i00 + landGridEdge + 1] - decoded.heights[i00 + landGridEdge]) * fx;
	const float mineHere = south + (north - south) * fy;

	auto [decodedMin, decodedMax] = std::minmax_element(decoded.heights.begin(), decoded.heights.end());
	const auto extents = land->loadedData->heightExtents;

	logger::info("[TexGen] Land decode check: cell {}, {} in {}", cellX, cellY, worldspace->GetFormEditorID());
	logger::info("[TexGen]   decoded range {:.1f}..{:.1f}, engine heightExtents {:.1f}..{:.1f}, water {:.1f}",
		*decodedMin, *decodedMax, extents.x, extents.y, decoded.waterHeight);

	if (haveEngineHeight) {
		logger::info("[TexGen]   at player {:.1f},{:.1f}: decoded {:.1f} vs GetLandHeight {:.1f} - difference {:.1f}",
			playerPos.x, playerPos.y, mineHere, engineHeight, mineHere - engineHeight);
		if (std::abs(mineHere - engineHeight) < 1.0f)
			logger::info("[TexGen]   ABSOLUTE HEIGHT MATCHES - the decode is correct in world space");
		else
			logger::error("[TexGen]   ABSOLUTE HEIGHT WRONG by {:.1f}", mineHere - engineHeight);
	} else {
		logger::warn("[TexGen]   GetLandHeight failed at the player's position");
	}

	// Test two: against LoadedLandData's per-quad grids. The engine builds four quad meshes, so
	// these may be local to each quad rather than absolute. Scoring the raw residual alongside one
	// with each quad's mean difference removed separates "wrong shape" from "right shape, different
	// base" - if the compensated residual collapses to zero, the decode is fine and only the space
	// differs.
	static constexpr int quadEdge = 17;
	struct Candidate
	{
		const char* name;
		bool quadXFromLowBit;
		bool rowMajorInQuad;
	};
	static constexpr std::array<Candidate, 4> candidates = {
		Candidate{ "quadX=bit0, in-quad row major", true, true },
		Candidate{ "quadX=bit0, in-quad column major", true, false },
		Candidate{ "quadX=bit1, in-quad row major", false, true },
		Candidate{ "quadX=bit1, in-quad column major", false, false },
	};

	const Candidate* best = nullptr;
	float bestCompensated = FLT_MAX;
	float bestRaw = FLT_MAX;
	std::array<float, 4> bestQuadOffsets{};

	for (const auto& candidate : candidates) {
		float rawMax = 0.0f;
		float compensatedMax = 0.0f;
		std::array<float, 4> quadOffsets{};

		for (int quad = 0; quad < 4; ++quad) {
			const int quadX = candidate.quadXFromLowBit ? (quad & 1) : (quad >> 1);
			const int quadY = candidate.quadXFromLowBit ? (quad >> 1) : (quad & 1);

			double sum = 0.0;
			for (int i = 0; i < quadEdge * quadEdge; ++i) {
				const int inRow = candidate.rowMajorInQuad ? i / quadEdge : i % quadEdge;
				const int inCol = candidate.rowMajorInQuad ? i % quadEdge : i / quadEdge;
				const int row = quadY * (quadEdge - 1) + inRow;
				const int col = quadX * (quadEdge - 1) + inCol;
				sum += decoded.heights[row * landGridEdge + col] - land->loadedData->heights[quad][i];
			}
			quadOffsets[quad] = (float)(sum / (double)(quadEdge * quadEdge));

			for (int i = 0; i < quadEdge * quadEdge; ++i) {
				const int inRow = candidate.rowMajorInQuad ? i / quadEdge : i % quadEdge;
				const int inCol = candidate.rowMajorInQuad ? i % quadEdge : i / quadEdge;
				const int row = quadY * (quadEdge - 1) + inRow;
				const int col = quadX * (quadEdge - 1) + inCol;

				const float difference = decoded.heights[row * landGridEdge + col] - land->loadedData->heights[quad][i];
				rawMax = std::max(rawMax, std::abs(difference));
				compensatedMax = std::max(compensatedMax, std::abs(difference - quadOffsets[quad]));
			}
		}

		logger::info("[TexGen]   {}: raw {:.2f}, per-quad offset removed {:.2f}", candidate.name, rawMax, compensatedMax);
		if (compensatedMax < bestCompensated) {
			bestCompensated = compensatedMax;
			bestRaw = rawMax;
			best = &candidate;
			bestQuadOffsets = quadOffsets;
		}
	}

	if (bestCompensated < 1.0f) {
		logger::info("[TexGen]   Shape matches under \"{}\" (residual {:.2f}); quad offsets {:.1f}, {:.1f}, {:.1f}, {:.1f}",
			best->name, bestCompensated, bestQuadOffsets[0], bestQuadOffsets[1], bestQuadOffsets[2], bestQuadOffsets[3]);
		if (bestRaw >= 1.0f)
			logger::info("[TexGen]   LoadedLandData is quad local, not absolute - it is not a usable oracle, trust the GetLandHeight result above");
	} else {
		logger::error("[TexGen]   Shape does NOT match under any ordering (best residual {:.2f} under \"{}\")", bestCompensated, best->name);
	}
}

bool TexGen::StartLandHeightRun(bool a_singleTile, const int2& a_targetCell)
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* worldspace = player ? player->GetWorldspace() : nullptr;

	// A child worldspace that borrows its parent's land carries no LAND records of its own, so the
	// records live wherever the land data does.
	while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
		worldspace = worldspace->parentWorld;

	if (!worldspace) {
		logger::error("[TexGen] Land height run needs an exterior worldspace");
		return false;
	}

	const char* boundsSource = "none";
	if (!TexGenLand::ResolveCellBounds(worldspace, landRunMinCell, landRunMaxCell, boundsSource, true)) {
		logger::error("[TexGen] Could not resolve cell bounds for {}", worldspace->GetFormEditorID());
		return false;
	}

	landRunCellsPerTile = GetHeightTileCells();
	landRunTileSize = GetHeightTileSize();
	landRunWorldspace = worldspace;

	// The tile grid is anchored to the worldspace cell grid and rounded outward, because the atlas
	// layout requires the cell extent to divide evenly by the tile size.
	landRunMinTile = int2(FloorDiv(landRunMinCell.x, landRunCellsPerTile), FloorDiv(landRunMinCell.y, landRunCellsPerTile));
	landRunMaxTile = int2(FloorDiv(landRunMaxCell.x, landRunCellsPerTile), FloorDiv(landRunMaxCell.y, landRunCellsPerTile));

	if (a_singleTile)
		landRunMinTile = landRunMaxTile = int2(FloorDiv(a_targetCell.x, landRunCellsPerTile), FloorDiv(a_targetCell.y, landRunCellsPerTile));

	landRunCurrentTile = landRunMinTile;
	landRunTileTotal = (landRunMaxTile.x - landRunMinTile.x + 1) * (landRunMaxTile.y - landRunMinTile.y + 1);
	landRunSeamMismatches = 0;
	cellsDone = 0;
	tilesDone = 0;

	// SeekCell consults a per-file cell offset table. Touching every plugin once from here keeps a
	// later move onto worker threads off a table that may be built lazily.
	TexGenLand::PrimeFileTables(worldspace, landRunMinCell.x, landRunMinCell.y);

	landRunFiles = std::make_unique<TexGenLand::LandFileSet>(worldspace);
	if (!landRunFiles->Valid()) {
		logger::error("[TexGen] No source plugins for worldspace {}", worldspace->GetFormEditorID());
		landRunFiles.reset();
		landRunWorldspace = nullptr;
		return false;
	}

	heightGenSingleTile = a_singleTile;
	landGenRunning = true;

	const int texelsPerCell = (int)landRunTileSize / landRunCellsPerTile;
	logger::info("[TexGen] Land height run: {} tiles of {}x{} cells at {} texels, cells {},{} to {},{} (bounds from {})",
		landRunTileTotal, landRunCellsPerTile, landRunCellsPerTile, landRunTileSize,
		landRunMinCell.x, landRunMinCell.y, landRunMaxCell.x, landRunMaxCell.y, boundsSource);
	logger::info("[TexGen] {} texels per cell, {:.0f} units per texel, LAND vertices every {:.0f} units",
		texelsPerCell, worldCellSize / (float)texelsPerCell, worldCellSize / (float)(TexGenLand::landGridEdge - 1));

	return true;
}

void TexGen::FillHeightTileFromLand(const int2& a_tileOriginCell, TexGenLand::LandFileSet& a_files, bool a_clipToWorldspace, std::vector<uint16_t>& o_pixels)
{
	using namespace TexGenLand;

	const int cellsPerTile = landRunCellsPerTile;
	const uint tileSize = landRunTileSize;
	const int texelsPerCell = (int)tileSize / cellsPerTile;
	const float worldRes = worldCellSize / (float)texelsPerCell;         // world units per texel
	const float vertexStep = worldCellSize / (float)(landGridEdge - 1);  // 128 units between LAND vertices

	o_pixels.assign((size_t)tileSize * tileSize, uint16_t(0));

	CellHeights cell;
	CellHeights westNeighbour;
	bool haveWestNeighbour = false;

	for (int localY = 0; localY < cellsPerTile; ++localY) {
		haveWestNeighbour = false;

		for (int localX = 0; localX < cellsPerTile; ++localX) {
			const int2 cellXY = a_tileOriginCell + int2(localX, localY);

			const bool inWorldspace = cellXY.x >= landRunMinCell.x && cellXY.x <= landRunMaxCell.x &&
			                          cellXY.y >= landRunMinCell.y && cellXY.y <= landRunMaxCell.y;
			if (a_clipToWorldspace && !inWorldspace) {
				haveWestNeighbour = false;
				continue;
			}

			a_files.ReadCell(cellXY.x, cellXY.y, cell);
			if (!cell.hasCell) {
				// No plugin defines this cell, so its texels stay at zero as an unsampled tile
				// region always has.
				haveWestNeighbour = false;
				continue;
			}

			// Adjacent cells each carry their own copy of the shared seam. Vanilla agrees on it; a
			// mismatch means a malformed patch, and is worth naming rather than averaging away.
			if (haveWestNeighbour) {
				for (int row = 0; row < landGridEdge; ++row) {
					if (cell.heights[row * landGridEdge] != westNeighbour.heights[row * landGridEdge + landGridEdge - 1]) {
						++landRunSeamMismatches;
						break;
					}
				}
			}

			for (int ty = 0; ty < texelsPerCell; ++ty) {
				// Texels sample from the cell's south west corner in worldRes steps and never reach
				// the far edge, so the enclosing quad always lies inside this cell's own grid.
				const float vy = (float)ty * worldRes / vertexStep;
				const int row0 = std::min((int)vy, landGridEdge - 2);
				const float fy = vy - (float)row0;

				for (int tx = 0; tx < texelsPerCell; ++tx) {
					const float vx = (float)tx * worldRes / vertexStep;
					const int col0 = std::min((int)vx, landGridEdge - 2);
					const float fx = vx - (float)col0;

					const int i00 = row0 * landGridEdge + col0;
					const float south = cell.heights[i00] + (cell.heights[i00 + 1] - cell.heights[i00]) * fx;
					const float north = cell.heights[i00 + landGridEdge] + (cell.heights[i00 + landGridEdge + 1] - cell.heights[i00 + landGridEdge]) * fx;

					float height = south + (north - south) * fy;
					height = std::max(height, cell.waterHeight);  // lakes and sea read as flat surfaces

					const int texX = localX * texelsPerCell + tx;
					const int texY = localY * texelsPerCell + ty;
					o_pixels[(size_t)texY * tileSize + texX] = DirectX::PackedVector::XMConvertFloatToHalf(height);
				}
			}

			westNeighbour = cell;
			haveWestNeighbour = true;
			++cellsDone;
		}
	}
}

void TexGen::GenerateHeightTiles()
{
	if (!landRunFiles || !landRunWorldspace) {
		landGenRunning = false;
		return;
	}

	const int2 tileOriginCell = landRunCurrentTile * landRunCellsPerTile;

	heightTileSize = landRunTileSize;
	FillHeightTileFromLand(tileOriginCell, *landRunFiles, !heightGenSingleTile, heightTilePixels);

	SaveHeightTile(tileOriginCell, landRunCellsPerTile);
	UpdateHeightPreview(tileOriginCell);
	++tilesDone;

	// West to east, south to north, matching the order the tiles stitch in.
	bool finished = heightGenSingleTile;
	if (!finished) {
		if (++landRunCurrentTile.x > landRunMaxTile.x) {
			landRunCurrentTile.x = landRunMinTile.x;
			finished = ++landRunCurrentTile.y > landRunMaxTile.y;
		}
	}

	if (!finished)
		return;

	logger::info("[TexGen] Land height run complete: {} tiles, {} cells, {} seam mismatches", tilesDone, cellsDone, landRunSeamMismatches);
	if (landRunSeamMismatches > 0)
		logger::warn("[TexGen] {} cells disagree with their west neighbour on the shared edge; expect a one texel seam there", landRunSeamMismatches);

	const bool wasSingleTile = heightGenSingleTile;
	landGenRunning = false;
	landRunFiles.reset();
	landRunWorldspace = nullptr;

	if (!wasSingleTile)
		FinalizeHeightTiles();
}

void TexGen::DiffHeightTileAgainstDisk(const int2& a_cell)
{
	// The raycast walk is the only path that sees statics, so the shape of the disagreement is the
	// real test: near zero over open terrain, one sided and positive wherever a static stands.
	if (!StartLandHeightRun(true, a_cell)) {
		logger::error("[TexGen] Tile diff: could not resolve the worldspace");
		return;
	}

	// Borrow the run's latched layout and file set, then stand it back down; nothing is written.
	const int2 tileOriginCell = landRunCurrentTile * landRunCellsPerTile;
	const uint tileSize = landRunTileSize;
	const auto path = GetTilePath(worldspaceID, "_H", tileSize, landRunCellsPerTile, tileOriginCell);

	std::vector<uint16_t> landPixels;
	FillHeightTileFromLand(tileOriginCell, *landRunFiles, false, landPixels);

	landGenRunning = false;
	landRunFiles.reset();
	landRunWorldspace = nullptr;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		logger::error("[TexGen] Tile diff: {} is not on disk. Generate it with the raycast reference first.", path.string());
		return;
	}

	DirectX::ScratchImage reference;
	if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, reference))) {
		logger::error("[TexGen] Tile diff: failed to load {}", path.string());
		return;
	}

	const DirectX::Image* image = reference.GetImages();
	if (!image || image->width != tileSize || image->height != tileSize || image->format != DXGI_FORMAT_R16_FLOAT) {
		logger::error("[TexGen] Tile diff: {} is not a {}x{} R16_FLOAT tile", path.string(), tileSize, tileSize);
		return;
	}

	std::vector<float> differences;
	differences.reserve((size_t)tileSize * tileSize);
	double signedSum = 0.0;
	float worst = 0.0f;
	int2 worstAt = int2(0, 0);

	for (uint y = 0; y < tileSize; ++y) {
		const uint16_t* row = (const uint16_t*)(image->pixels + (size_t)y * image->rowPitch);
		for (uint x = 0; x < tileSize; ++x) {
			const float referenceHeight = DirectX::PackedVector::XMConvertHalfToFloat(row[x]);
			const float landHeight = DirectX::PackedVector::XMConvertHalfToFloat(landPixels[(size_t)y * tileSize + x]);

			// Texels neither side sampled sit at exactly zero in both; counting them would bury the
			// signal under a mass of perfect matches.
			if (referenceHeight == 0.0f && landHeight == 0.0f)
				continue;

			const float difference = referenceHeight - landHeight;
			signedSum += difference;
			differences.push_back(std::abs(difference));

			if (std::abs(difference) > worst) {
				worst = std::abs(difference);
				worstAt = int2((int)x, (int)y);
			}
		}
	}

	if (differences.empty()) {
		logger::warn("[TexGen] Tile diff: both tiles are empty at cell {}, {}", tileOriginCell.x, tileOriginCell.y);
		return;
	}

	std::sort(differences.begin(), differences.end());
	const float median = differences[differences.size() / 2];
	const float p95 = differences[(size_t)((double)differences.size() * 0.95)];
	const double mean = signedSum / (double)differences.size();

	logger::info("[TexGen] Tile diff at cell {}, {} against {}", tileOriginCell.x, tileOriginCell.y, path.filename().string());
	logger::info("[TexGen]   {} compared texels: p50 {:.1f}, p95 {:.1f}, max {:.1f} at texel {}, {}",
		differences.size(), median, p95, worst, worstAt.x, worstAt.y);
	logger::info(
		"[TexGen]   mean signed difference (reference minus LAND) {:.1f} - a positive mean is expected, "
		"it is the statics the raycast saw and LAND does not carry",
		mean);
}

bool TexGen::FinalizeHeightTiles()
{
	if (!EnsureHeightAtlas(worldspaceID, true))
		return false;

	NotifyCacheMapsChanged();
	return true;
}
//////////////////////////////////////////////////////////////////////////////////
//// Settings and lifecycle
//////////////////////////////////////////////////////////////////////////////////

void TexGen::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TexGen::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TexGen::RestoreDefaultSettings()
{
	settings = {};
}

void TexGen::Prepass()
{
	UpdateWorldspaceID();

	// Both bakes step one unit of work per frame; a whole run in one call would sit far past the
	// driver timeout and the height walk has to let terrain stream in between teleports.
	if (heightGenRunning)
		GenerateHeightMap();

	if (landGenRunning)
		GenerateHeightTiles();

	UpdateBentNormalTiles();
}

void TexGen::UpdateWorldspaceID()
{
	// Interiors have no worldspace; keep the last exterior one so the UI and any queued bake still
	// know which cache they are working on.
	auto current = GetCurrentWorldspaceID();
	if (!current.empty())
		worldspaceID = std::move(current);
}

void TexGen::NotifyCacheMapsChanged()
{
	if (globals::features::skylighting.loaded)
		globals::features::skylighting.InvalidateCacheMaps();
}

//////////////////////////////////////////////////////////////////////////////////
//// Cache layout helpers
//////////////////////////////////////////////////////////////////////////////////

float4 TexGen::GetAtlasWorldBound() const
{
	return heightAtlasRange.WorldBounds();
}

bool TexGen::ResolveHeightAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild)
{
	heightAtlasRange = {};
	if (!EnsureHeightAtlas(a_worldspaceID, a_forceRebuild))
		return false;

	if (!FindAtlas(a_worldspaceID, "_H", o_path, heightAtlasRange))
		return false;

	return true;
}

bool TexGen::ResolveBentNormalAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild)
{
	if (!EnsureBentNormalAtlas(a_worldspaceID, a_forceRebuild))
		return false;

	o_path = MakeAtlasPath(cachePath, a_worldspaceID, "_BN", heightAtlasRange.minCell, heightAtlasRange.maxCell);
	return std::filesystem::exists(o_path);
}

//////////////////////////////////////////////////////////////////////////////////
//// GPU generators
//////////////////////////////////////////////////////////////////////////////////
bool TexGen::BuildDerivedMaps(const std::string& a_worldspaceID, bool a_forceRebuild, bool a_smoothHeight)
{
	worldspaceID = a_worldspaceID;

	std::filesystem::path heightAtlasPath;
	if (!ResolveHeightAtlas(worldspaceID, heightAtlasPath))
		return false;

	const auto downscaledHeightPath = MakeAtlasPath(cachePath, worldspaceID, "_HD", heightAtlasRange.minCell, heightAtlasRange.maxCell);
	static constexpr std::array<const char*, 5> outputTags = { "_N", "_CO", "_CO2", "_DO", "_DO2" };

	DirectX::TexMetadata heightMetadata;
	DX::ThrowIfFailed(DirectX::GetMetadataFromDDSFile(heightAtlasPath.c_str(), DirectX::DDS_FLAGS_NONE, heightMetadata));
	const uint width = std::max(1u, (uint)std::lround((double)heightMetadata.width * derivedHeightScale));
	const uint height = std::max(1u, (uint)std::lround((double)heightMetadata.height * derivedHeightScale));

	auto hasExpectedDimensions = [&](const std::filesystem::path& a_path) {
		DirectX::TexMetadata metadata;
		return SUCCEEDED(DirectX::GetMetadataFromDDSFile(a_path.c_str(), DirectX::DDS_FLAGS_NONE, metadata)) &&
		       metadata.width == width && metadata.height == height && metadata.mipLevels == 1;
	};

	bool rebuild = a_forceRebuild || !hasExpectedDimensions(downscaledHeightPath);
	std::error_code ec;
	for (const auto* tag : outputTags) {
		const auto path = MakeAtlasPath(cachePath, worldspaceID, tag, heightAtlasRange.minCell, heightAtlasRange.maxCell);
		rebuild = !hasExpectedDimensions(path) || rebuild;
	}

	if (!rebuild) {
		const auto sourceWriteTime = std::filesystem::last_write_time(heightAtlasPath, ec);
		const auto downscaledWriteTime = std::filesystem::last_write_time(downscaledHeightPath, ec);
		rebuild = sourceWriteTime > downscaledWriteTime;
	}

	bool generated = true;
	if (rebuild) {
		using namespace DirectX;

		ScratchImage sourceImage;
		DX::ThrowIfFailed(LoadFromDDSFile(heightAtlasPath.c_str(), DDS_FLAGS_NONE, nullptr, sourceImage));

		const Image* source = sourceImage.GetImages();

		ScratchImage downscaledImage;
		DX::ThrowIfFailed(Resize(*source, width, height, TEX_FILTER_DEFAULT, downscaledImage));
		SaveMapDDS(*downscaledImage.GetImages(), downscaledHeightPath);

		winrt::com_ptr<ID3D11Resource> heightResource;
		DX::ThrowIfFailed(CreateTexture(
			globals::d3d::device,
			downscaledImage.GetImages(),
			downscaledImage.GetImageCount(),
			downscaledImage.GetMetadata(),
			heightResource.put()));

		winrt::com_ptr<ID3D11ShaderResourceView> downscaledHeightSRV;
		DX::ThrowIfFailed(globals::d3d::device->CreateShaderResourceView(heightResource.get(), nullptr, downscaledHeightSRV.put()));

		auto previousHeightMapSRV = heightMapSRV;
		heightMapSRV = downscaledHeightSRV.get();
		const int2 mapSize = int2((int)width, (int)height);

		generated = GenerateNormalMap(mapSize);

		eastl::unique_ptr<Texture2D> smoothedHeight;
		if (generated && a_smoothHeight) {
			smoothedHeight = SmoothHeightMap(downscaledHeightSRV.get(), mapSize);
			if (!smoothedHeight) {
				heightMapSRV = previousHeightMapSRV;
				return false;
			}
			heightMapSRV = smoothedHeight->srv.get();
		}

		generated = generated && GenerateCardinalOcclusionMaps(mapSize);
		heightMapSRV = previousHeightMapSRV;

		if (generated) {
			logger::info("[TexGen] Built derived maps at {}x{} from {}{}", width, height, heightAtlasPath.string(),
				a_smoothHeight ? " (flattened occlusion)" : "");
			NotifyCacheMapsChanged();
		}
	}

	if (!settings.skipBentNormalTiles) {
		const auto bentNormalPath = MakeAtlasPath(cachePath, worldspaceID, "_BN", heightAtlasRange.minCell, heightAtlasRange.maxCell);
		const auto heightTiles = LoadHeightTileManifest(worldspaceID);
		const bool bentNormalTilesMissing = std::ranges::any_of(heightTiles, [&](const HeightTileFile& tile) {
			if (tile.tileSize != (uint)settings.cacheAtlasTileSize || tile.cellsPerTile != settings.cacheAtlasTileCells)
				return false;
			return !std::filesystem::exists(GetTilePath(worldspaceID, "_BN", tile.tileSize, tile.cellsPerTile, tile.originCell), ec);
		});

		if ((a_forceRebuild || !std::filesystem::exists(bentNormalPath, ec) || bentNormalTilesMissing) && !bentNormalTileGen)
			generated = StartBentNormalTiles() && generated;
	}

	return generated;
}

bool TexGen::GenerateNormalMap(const int2& a_mapSize)
{
	auto context = globals::d3d::context;

	eastl::unique_ptr<Texture2D> output;
	winrt::com_ptr<ID3D11ComputeShader> computeShader;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R16G16B16A16_FLOAT, (uint)a_mapSize.x, (uint)a_mapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	output = eastl::make_unique<Texture2D>(desc, "TexGen::NormalMap");
	output->CreateUAV(uavDesc);
	computeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "NORMALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	ID3D11UnorderedAccessView* uav = output->uav.get();
	context->CSSetShader(computeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &heightMapSRV);

	CacheGenCBStruct data = {
		.TexParams = float4((float)a_mapSize.x, (float)a_mapSize.y, 0.0f, 0.0f),
		.GridBounds = GetAtlasWorldBound()
	};
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->Dispatch((a_mapSize.x + 7) / 8, (a_mapSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);

	DirectX::ScratchImage captured;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, context, output->resource.get(), captured));
	SaveMapDDS(*captured.GetImages(), MakeAtlasPath(cachePath, worldspaceID, "_N", heightAtlasRange.minCell, heightAtlasRange.maxCell));
	return true;
}

bool TexGen::GenerateCardinalOcclusionMaps(const int2& a_mapSize)
{
	static constexpr std::array<const char*, 4> outputTags = { "_CO", "_CO2", "_DO", "_DO2" };

	std::array<eastl::unique_ptr<Texture2D>, 4> outputs;
	winrt::com_ptr<ID3D11ComputeShader> computeShader;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R16G16B16A16_FLOAT, (uint)a_mapSize.x, (uint)a_mapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	for (size_t i = 0; i < outputs.size(); ++i) {
		const auto name = std::format("TexGen::Occlusion{}", outputTags[i]);
		outputs[i] = eastl::make_unique<Texture2D>(desc, name.c_str());
		outputs[i]->CreateUAV(uavDesc);
	}

	computeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "CARDINALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	auto context = globals::d3d::context;
	ID3D11UnorderedAccessView* uavs[4];
	for (size_t i = 0; i < outputs.size(); ++i)
		uavs[i] = outputs[i]->uav.get();

	context->CSSetShader(computeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	context->CSSetShaderResources(0, 1, &heightMapSRV);

	ID3D11SamplerState* linearSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linearSampler);

	CacheGenCBStruct data = {
		.TexParams = float4((float)a_mapSize.x, (float)a_mapSize.y, 0.0f, 0.0f),
		.GridBounds = GetAtlasWorldBound()
	};
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->Dispatch((a_mapSize.x + 7) / 8, (a_mapSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[4] = {};
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);

	DirectX::ScratchImage captured;
	for (size_t i = 0; i < outputs.size(); ++i) {
		DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, context, outputs[i]->resource.get(), captured));
		SaveMapDDS(*captured.GetImages(), MakeAtlasPath(cachePath, worldspaceID, outputTags[i], heightAtlasRange.minCell, heightAtlasRange.maxCell));
	}
	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// Height smoothing
//////////////////////////////////////////////////////////////////////////////////

void TexGen::DispatchHeightSmoothPass(ID3D11ComputeShader* a_computeShader, ID3D11ShaderResourceView* a_source, Texture2D* a_target, const int2& a_axis)
{
	auto context = globals::d3d::context;

	context->CSSetShader(a_computeShader, nullptr, 0);

	ID3D11UnorderedAccessView* uav = a_target->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 1, &a_source);

	const uint width = a_target->desc.Width;
	const uint height = a_target->desc.Height;

	CacheGenCBStruct data = {
		.TexParams = float4((float)width, (float)height, 0.0f, 0.0f),
		.GridBounds = GetAtlasWorldBound(),
		.SmoothParams = float4((float)a_axis.x, (float)a_axis.y, (float)std::clamp(settings.smoothRadius, 1, smoothMaxRadius), 0.0f),
		.SmoothRange = float4(std::max(settings.smoothFlattenHeight, 0.01f), std::max(settings.smoothRolloff, 1.0f), heightRangeMin, heightRangeMax)
	};
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	// The texture written here is the one read next, and D3D silently drops an SRV still bound as a UAV.
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
}

eastl::unique_ptr<Texture2D> TexGen::SmoothHeightMap(ID3D11ShaderResourceView* a_source, const int2& a_mapSize)
{
	winrt::com_ptr<ID3D11ComputeShader> smoothCS;
	smoothCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "HEIGHT_SMOOTH", "" } }, "cs_5_0")));
	if (!smoothCS) {
		logger::error("[TexGen] Failed to compile the height smoothing shader");
		return nullptr;
	}

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32_FLOAT, (uint)a_mapSize.x, (uint)a_mapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, desc.Format);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	auto smoothTexA = eastl::make_unique<Texture2D>(desc, "TexGen::SmoothedHeightA");
	auto smoothTexB = eastl::make_unique<Texture2D>(desc, "TexGen::SmoothedHeightB");
	for (auto* tex : { smoothTexA.get(), smoothTexB.get() }) {
		tex->CreateSRV(srvDesc);
		tex->CreateUAV(uavDesc);
	}

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Horizontal then vertical, so a pair always lands back in A and neither target needs clearing.
	const int iterations = std::clamp(settings.smoothIterations, 1, smoothMaxIterations);
	for (int i = 0; i < iterations; ++i) {
		DispatchHeightSmoothPass(smoothCS.get(), i == 0 ? a_source : smoothTexA->srv.get(), smoothTexB.get(), int2(1, 0));
		DispatchHeightSmoothPass(smoothCS.get(), smoothTexB->srv.get(), smoothTexA.get(), int2(0, 1));
	}

	logger::info("[TexGen] Flattened the {}x{} height map: up to {:.0f} game units, radius {} x {} iterations",
		a_mapSize.x, a_mapSize.y, settings.smoothFlattenHeight, std::clamp(settings.smoothRadius, 1, smoothMaxRadius), iterations);

	return smoothTexA;
}

//////////////////////////////////////////////////////////////////////////////////
//// xLODGen albedo atlas
//////////////////////////////////////////////////////////////////////////////////

// needs to support all input texture sizes
bool TexGen::BuildLODAtlas(const std::string& a_worldspaceID)
{
	worldspaceID = a_worldspaceID;
	std::vector<TileInfo> tiles;

	using namespace DirectX;

	std::filesystem::path heightAtlasPath;
	if (!ResolveHeightAtlas(worldspaceID, heightAtlasPath))
		return false;

	const auto outputPath = MakeAtlasPath(cachePath, worldspaceID, "_A", heightAtlasRange.minCell, heightAtlasRange.maxCell);

	TexMetadata heightMetadata;
	DX::ThrowIfFailed(GetMetadataFromDDSFile(heightAtlasPath.c_str(), DDS_FLAGS_NONE, heightMetadata));
	const size_t outputWidth = std::max<size_t>(1, std::lround((double)heightMetadata.width * derivedHeightScale));
	const size_t outputHeight = std::max<size_t>(1, std::lround((double)heightMetadata.height * derivedHeightScale));

	static constexpr int lodCellsPerTile = 32;
	const int2 atlasMinCell = heightAtlasRange.minCell;
	const int2 atlasMaxCellExclusive = heightAtlasRange.maxCell + int2(1, 1);
	const int2 atlasCellExtent = atlasMaxCellExclusive - atlasMinCell;

	const std::filesystem::path lodPath = settings.dynDOLODPath;
	std::error_code ec;
	if (!std::filesystem::exists(lodPath, ec)) {
		logger::error("[TexGen] xLODGen output folder {} not found, cannot build {}", lodPath.string(), outputPath.filename().string());
		return false;
	}

	for (auto& entry : std::filesystem::directory_iterator(lodPath, ec)) {
		auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		std::string stem = path.stem().string();
		if (stem.size() >= 2 && stem[stem.size() - 2] == '_')
			continue;

		TileInfo ti;
		if (!ParseLODTile(path, ti.cellX, ti.cellY))
			continue;

		DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, nullptr, ti.image));

		// Convert to RGBA32 for uniform blitting
		auto img = ti.image.GetImage(0, 0, 0);
		if (img->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			ScratchImage converted;
			DX::ThrowIfFailed(Convert(*img, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted));
			ti.image = std::move(converted);
		}

		tiles.push_back(std::move(ti));
	}

	logger::info("[TexGen] LOD tile count: {}", tiles.size());
	if (tiles.empty())
		return false;

	const Image* firstTile = tiles.front().image.GetImage(0, 0, 0);
	const size_t lodTileSize = firstTile->width;
	if (firstTile->height != lodTileSize || lodTileSize % lodCellsPerTile != 0) {
		logger::error("[TexGen] LOD32 tile dimensions must be square and divisible by {}", lodCellsPerTile);
		return false;
	}

	const size_t texelsPerCell = lodTileSize / lodCellsPerTile;
	const size_t sourceWidth = (size_t)atlasCellExtent.x * texelsPerCell;
	const size_t sourceHeight = (size_t)atlasCellExtent.y * texelsPerCell;

	ScratchImage atlas;
	DX::ThrowIfFailed(atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, sourceWidth, sourceHeight, 1, 1));
	const Image* atlasImage = atlas.GetImage(0, 0, 0);
	memset(atlasImage->pixels, 0, atlasImage->slicePitch);

	size_t blitted = 0;
	for (auto& tile : tiles) {
		const Image* source = tile.image.GetImage(0, 0, 0);
		if (source->width != lodTileSize || source->height != lodTileSize)
			continue;

		const int copyMinX = std::max(atlasMinCell.x, tile.cellX);
		const int copyMinY = std::max(atlasMinCell.y, tile.cellY);
		const int copyMaxX = std::min(atlasMaxCellExclusive.x, tile.cellX + lodCellsPerTile);
		const int copyMaxY = std::min(atlasMaxCellExclusive.y, tile.cellY + lodCellsPerTile);
		if (copyMinX >= copyMaxX || copyMinY >= copyMaxY)
			continue;

		const size_t sourceX = (size_t)(copyMinX - tile.cellX) * texelsPerCell;
		const size_t sourceY = (size_t)(tile.cellY + lodCellsPerTile - copyMaxY) * texelsPerCell;
		const size_t destinationX = (size_t)(copyMinX - atlasMinCell.x) * texelsPerCell;
		const size_t destinationY = (size_t)(atlasMaxCellExclusive.y - copyMaxY) * texelsPerCell;
		const size_t copyWidth = (size_t)(copyMaxX - copyMinX) * texelsPerCell;
		const size_t copyHeight = (size_t)(copyMaxY - copyMinY) * texelsPerCell;

		for (size_t y = 0; y < copyHeight; ++y) {
			uint8_t* destination = atlasImage->pixels + (destinationY + y) * atlasImage->rowPitch + destinationX * 4;
			const uint8_t* sourceRow = source->pixels + (sourceY + y) * source->rowPitch + sourceX * 4;
			memcpy(destination, sourceRow, copyWidth * 4);
		}
		blitted++;
	}

	if (blitted == 0)
		return false;

	ScratchImage resizedAtlas;
	const Image* outputImage = atlasImage;
	if (sourceWidth != outputWidth || sourceHeight != outputHeight) {
		DX::ThrowIfFailed(Resize(*atlasImage, outputWidth, outputHeight, TEX_FILTER_DEFAULT, resizedAtlas));
		outputImage = resizedAtlas.GetImages();
	}

	SaveMapDDS(*outputImage, outputPath);

	logger::info("[TexGen] Built LOD atlas {}: {}x{}, cells {},{} to {},{}",
		outputPath.string(), outputImage->width, outputImage->height,
		heightAtlasRange.minCell.x, heightAtlasRange.minCell.y, heightAtlasRange.maxCell.x, heightAtlasRange.maxCell.y);

	NotifyCacheMapsChanged();
	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// Tile stitching
//////////////////////////////////////////////////////////////////////////////////

bool TexGen::StitchTileAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, const float4& fill, TileAtlasResult& o_result, const AtlasCellRange* a_range)
{
	using namespace DirectX;

	if (a_worldspaceID.empty())
		return false;

	std::error_code ec;
	if (!std::filesystem::exists(cachePath, ec))
		return false;

	std::vector<HeightTileFile> tiles;
	for (const auto& entry : std::filesystem::directory_iterator(cachePath, ec)) {
		const auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		HeightTileFile tile;
		if (ParseTileName(path, a_worldspaceID, a_mapTag, tile))
			tiles.push_back(std::move(tile));
	}

	if (tiles.empty()) {
		logger::warn("[TexGen] No {}{} tiles found, cannot build atlas", a_worldspaceID, a_mapTag);
		return false;
	}

	if (a_range)
		std::erase_if(tiles, [&](const HeightTileFile& tile) {
			return tile.tileSize != (uint)settings.cacheAtlasTileSize || tile.cellsPerTile != settings.cacheAtlasTileCells;
		});

	if (tiles.empty()) {
		logger::warn("[TexGen] No {}{} tiles match the height atlas layout", a_worldspaceID, a_mapTag);
		return false;
	}

	// Tiles from several runs may be present; keep the highest resolution layout and drop the rest,
	// since tiles of different sizes cannot share one grid.
	const HeightTileFile* best = &tiles[0];
	for (const auto& tile : tiles) {
		int tileTexelsPerCell = (int)tile.tileSize / tile.cellsPerTile;
		int bestTexelsPerCell = (int)best->tileSize / best->cellsPerTile;
		if (tileTexelsPerCell > bestTexelsPerCell || (tileTexelsPerCell == bestTexelsPerCell && tile.tileSize > best->tileSize))
			best = &tile;
	}

	const uint tileSize = best->tileSize;
	const int cellsPerTile = best->cellsPerTile;
	std::erase_if(tiles, [&](const HeightTileFile& tile) { return tile.tileSize != tileSize || tile.cellsPerTile != cellsPerTile; });

	int2 minOrigin;
	int2 maxOrigin;
	if (a_range) {
		minOrigin = a_range->minCell;
		maxOrigin = a_range->maxCell - int2(cellsPerTile - 1, cellsPerTile - 1);
		std::erase_if(tiles, [&](const HeightTileFile& tile) {
			return tile.originCell.x < minOrigin.x || tile.originCell.y < minOrigin.y || tile.originCell.x > maxOrigin.x || tile.originCell.y > maxOrigin.y;
		});
	} else {
		minOrigin = maxOrigin = tiles[0].originCell;
		for (const auto& tile : tiles) {
			minOrigin.x = std::min(minOrigin.x, tile.originCell.x);
			minOrigin.y = std::min(minOrigin.y, tile.originCell.y);
			maxOrigin.x = std::max(maxOrigin.x, tile.originCell.x);
			maxOrigin.y = std::max(maxOrigin.y, tile.originCell.y);
		}
	}
	if (tiles.empty()) {
		logger::warn("[TexGen] No {}{} tiles fall within the height atlas range", a_worldspaceID, a_mapTag);
		return false;
	}

	const size_t tilesX = (size_t)((maxOrigin.x - minOrigin.x) / cellsPerTile) + 1;
	const size_t tilesY = (size_t)((maxOrigin.y - minOrigin.y) / cellsPerTile) + 1;
	const DXGI_FORMAT format = a_mapTag == "_H" ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
	const size_t bytesPerTexel = BitsPerPixel(format) / 8;

	const size_t atlasWidth = tilesX * tileSize;
	const size_t atlasHeight = tilesY * tileSize;

	ScratchImage atlas;
	DX::ThrowIfFailed(atlas.Initialize2D(format, atlasWidth, atlasHeight, 1, 1));

	const Image* atlasImage = atlas.GetImages();
	FillImage(*atlasImage, fill);

	// The atlas is north up with a top left origin, so both the tile order and each tile's rows are
	// mirrored on the way in: the tiles themselves are stored south to north.
	size_t blitted = 0;
	for (const auto& tile : tiles) {
		ScratchImage tileImage;
		DX::ThrowIfFailed(LoadFromDDSFile(tile.path.c_str(), DDS_FLAGS_NONE, nullptr, tileImage));

		const Image* src = tileImage.GetImages();
		if (src->width != tileSize || src->height != tileSize) {
			logger::warn("[TexGen] Skipping tile {}, does not match the atlas layout", tile.path.string());
			continue;
		}

		ScratchImage convertedImage;
		if (src->format != format) {
			DX::ThrowIfFailed(Convert(*src, format, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, convertedImage));
			src = convertedImage.GetImages();
		}

		size_t tileColumn = (size_t)((tile.originCell.x - minOrigin.x) / cellsPerTile);
		size_t tileRow = (size_t)((maxOrigin.y - tile.originCell.y) / cellsPerTile);  // north first
		size_t dstX = tileColumn * tileSize;
		size_t dstY = tileRow * tileSize;

		for (uint y = 0; y < tileSize; ++y) {
			auto dst = atlasImage->pixels + (dstY + y) * atlasImage->rowPitch + dstX * bytesPerTexel;
			memcpy(dst, src->pixels + (tileSize - 1 - y) * src->rowPitch, tileSize * bytesPerTexel);
		}

		blitted++;
	}

	if (blitted == 0) {
		logger::error("[TexGen] No usable {}{} tiles, atlas not written", a_worldspaceID, a_mapTag);
		return false;
	}

	o_result.image = std::move(atlas);
	o_result.minOriginCell = minOrigin;
	o_result.maxOriginCell = maxOrigin;
	o_result.tileCounts = int2((int)tilesX, (int)tilesY);
	o_result.tileSize = tileSize;
	o_result.cellsPerTile = cellsPerTile;

	return true;
}

bool TexGen::EnsureHeightAtlas(const std::string& a_worldspaceID, bool a_forceRebuild)
{
	using namespace DirectX;

	if (a_worldspaceID.empty())
		return false;

	std::filesystem::path existingPath;
	if (!a_forceRebuild && FindAtlas(a_worldspaceID, "_H", existingPath, heightAtlasRange)) {
		if (UpdateAtlasLayout(a_worldspaceID, heightAtlasRange, settings))
			globals::state->Save();
		return true;
	}

	// Gaps between tiles read as zero height, matching how the tiles clear unsampled texels.
	TileAtlasResult result;
	if (!StitchTileAtlas(a_worldspaceID, "_H", float4(0.0f, 0.0f, 0.0f, 0.0f), result))
		return false;

	const Image* atlasImage = result.image.GetImages();
	const int2 minOrigin = result.minOriginCell;
	const int2 maxOrigin = result.maxOriginCell;
	const uint tileSize = result.tileSize;
	const int cellsPerTile = result.cellsPerTile;

	// Record the layout so atlas texels can be mapped back to cells later.
	settings.cacheAtlasMinCellX = minOrigin.x;
	settings.cacheAtlasMinCellY = minOrigin.y;
	settings.cacheAtlasTileSize = (int)tileSize;
	settings.cacheAtlasTileCells = cellsPerTile;
	settings.cacheAtlasTilesX = result.tileCounts.x;
	settings.cacheAtlasTilesY = result.tileCounts.y;

	globals::state->Save();

	// The name carries the inclusive cell range the atlas covers, so its extent is always readable
	// from the file itself rather than from whatever the settings happen to hold.
	const int2 maxCell = maxOrigin + int2(cellsPerTile - 1, cellsPerTile - 1);
	auto atlasPath = MakeAtlasPath(cachePath, a_worldspaceID, "_H", minOrigin, maxCell);

	heightAtlasRange.minCell = minOrigin;
	heightAtlasRange.maxCell = maxCell;
	heightAtlasRange.valid = true;
	// Loading a legacy tile-only cache follows the same finalization path as a newly completed bake.
	// Preserve the names before removing the tile DDS files because bent-normal generation only
	// needs their layout after this point.
	auto tiles = GetAtlasTiles(a_worldspaceID, "_H", tileSize, cellsPerTile, heightAtlasRange);
	if (!SaveHeightTileManifest(a_worldspaceID, tiles))
		return false;

	SaveMapDDS(*atlasImage, atlasPath);

	const auto removedTiles = DeleteTiles(tiles);
	logger::info("[TexGen] Saved height tile manifest and removed {} temporary tiles", removedTiles);

	logger::info("[TexGen] Built height atlas {}: {}x{}, cells {},{} to {},{}",
		atlasPath.string(), atlasImage->width, atlasImage->height,
		minOrigin.x, minOrigin.y, maxCell.x, maxCell.y);

	return true;
}

bool TexGen::EnsureBentNormalAtlas(const std::string& a_worldspaceID, bool a_forceRebuild)
{
	using namespace DirectX;

	if (a_worldspaceID.empty())
		return false;

	std::filesystem::path heightPath;
	heightAtlasRange = {};
	if (!FindAtlas(a_worldspaceID, "_H", heightPath, heightAtlasRange))
		return false;

	TexMetadata heightMetadata;
	DX::ThrowIfFailed(GetMetadataFromDDSFile(heightPath.c_str(), DDS_FLAGS_NONE, heightMetadata));
	const size_t outputWidth = std::max<size_t>(1, std::lround((double)heightMetadata.width * derivedHeightScale));
	const size_t outputHeight = std::max<size_t>(1, std::lround((double)heightMetadata.height * derivedHeightScale));

	if (UpdateAtlasLayout(a_worldspaceID, heightAtlasRange, settings))
		globals::state->Save();

	auto atlasPath = MakeAtlasPath(cachePath, a_worldspaceID, "_BN", heightAtlasRange.minCell, heightAtlasRange.maxCell);
	if (!a_forceRebuild && std::filesystem::exists(atlasPath)) {
		TexMetadata bentNormalMetadata;
		DX::ThrowIfFailed(GetMetadataFromDDSFile(atlasPath.c_str(), DDS_FLAGS_NONE, bentNormalMetadata));
		if (bentNormalMetadata.width == outputWidth && bentNormalMetadata.height == outputHeight && bentNormalMetadata.mipLevels == 1)
			return true;
	}

	// Gaps read as an unoccluded surface: normal straight up, encoded, and full visibility.
	TileAtlasResult result;
	if (!StitchTileAtlas(a_worldspaceID, "_BN", float4(0.5f, 0.5f, 1.0f, 1.0f), result, &heightAtlasRange))
		return false;

	const Image* atlasImage = result.image.GetImages();
	if (atlasImage->width != heightMetadata.width || atlasImage->height != heightMetadata.height) {
		logger::error("[TexGen] Bent normal atlas dimensions {}x{} do not match height atlas {}x{}",
			atlasImage->width, atlasImage->height, heightMetadata.width, heightMetadata.height);
		return false;
	}

	ScratchImage resizedAtlas;
	const Image* outputImage = atlasImage;
	if (atlasImage->width != outputWidth || atlasImage->height != outputHeight) {
		DX::ThrowIfFailed(Resize(*atlasImage, outputWidth, outputHeight, TEX_FILTER_DEFAULT, resizedAtlas));
		outputImage = resizedAtlas.GetImages();
	}

	SaveMapDDS(*outputImage, atlasPath);

	logger::info("[TexGen] Built bent normal atlas {}: {}x{}, cells {},{} to {},{}",
		atlasPath.string(), outputImage->width, outputImage->height,
		heightAtlasRange.minCell.x, heightAtlasRange.minCell.y, heightAtlasRange.maxCell.x, heightAtlasRange.maxCell.y);

	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// LOD bent normal tiles
//////////////////////////////////////////////////////////////////////////////////

void TexGen::DispatchBentNormalSweep(Texture2D* a_accumTex, const int2& tileOriginAtlasPx)
{
	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	auto context = globals::d3d::context;
	const int tileSize = (int)a_accumTex->desc.Width;

	const float4 bounds = GetAtlasWorldBound();
	const float2 worldPerTexel = float2(
		(bounds.z - bounds.x) / (float)bentNormalAtlasSize.x,
		(bounds.w - bounds.y) / (float)bentNormalAtlasSize.y);

	// Each azimuth adds its wedge to the accumulator, so it starts empty.
	const float clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearUnorderedAccessViewFloat(a_accumTex->uav.get(), clearValue);

	ID3D11ShaderResourceView* heightSRV = bentNormalHeightSRV.get();
	context->CSSetShaderResources(0, 1, &heightSRV);
	context->CSSetShader(bentNormalSweepCS.get(), nullptr, 0);

	ID3D11UnorderedAccessView* uavs[2] = { a_accumTex->uav.get(), bentNormalHullUAV.get() };
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	for (int azimuth = 0; azimuth < bentNormalAzimuths; ++azimuth) {
		const float phi = ((float)azimuth + 0.5f) * (2.0f * std::numbers::pi_v<float> / (float)bentNormalAzimuths);

		// The direction the per texel march walks, in the shader's y up world space.
		const float2 worldDir = float2(std::cos(phi), std::sin(phi));
		// Atlas rows run north to south, so the texel space direction has y negated.
		const float2 atlasDir = float2(worldDir.x, -worldDir.y);

		// Step along the dominant axis so the minor axis moves at most one texel per step; every
		// tile texel then lands on exactly one line, with no gaps and no double writes.
		const bool transpose = std::abs(atlasDir.y) > std::abs(atlasDir.x);
		const float majorComponent = transpose ? atlasDir.y : atlasDir.x;
		const float minorComponent = transpose ? atlasDir.x : atlasDir.y;
		const float slope = minorComponent / majorComponent;
		const float majorStep = majorComponent > 0.0f ? 1.0f : -1.0f;

		const int2 tileMin = transpose ? int2(tileOriginAtlasPx.y, tileOriginAtlasPx.x) : tileOriginAtlasPx;

		// A line is minor = offset + round(slope * major); the offsets that cross the tile span
		// its minor extent plus however far the line drifts across the tile's major extent.
		const float driftA = std::round(slope * (float)tileMin.x);
		const float driftB = std::round(slope * (float)(tileMin.x + tileSize - 1));
		const int driftMin = (int)std::min(driftA, driftB);
		const int driftMax = (int)std::max(driftA, driftB);
		const int firstLine = tileMin.y - driftMax;
		const int lineCount = tileSize + (driftMax - driftMin);

		const float worldPerMajor = transpose ? worldPerTexel.y : worldPerTexel.x;
		const float worldPerMinor = transpose ? worldPerTexel.x : worldPerTexel.y;
		const float stepWorldDist = std::sqrt(worldPerMajor * worldPerMajor + (slope * worldPerMinor) * (slope * worldPerMinor));

		CacheGenCBStruct data = {
			.TexParams = float4((float)tileSize, (float)tileSize, 0.0f, 0.0f),
			.GridBounds = GetAtlasWorldBound()
		};
		data.SweepDir = float4(worldDir.x, worldDir.y, slope, majorStep);
		data.SweepParams = float4((float)firstLine, (float)lineCount, transpose ? 1.0f : 0.0f, stepWorldDist);
		data.SweepRect = float4((float)tileOriginAtlasPx.x, (float)tileOriginAtlasPx.y, (float)tileSize, 0.0f);
		cacheGenBuffer->Update(data);

		context->Dispatch((lineCount + 63) / 64, 1, 1);
	}

	// Resolve the accumulated integral in place
	CacheGenCBStruct data = {
		.TexParams = float4((float)tileSize, (float)tileSize, 0.0f, 0.0f),
		.GridBounds = GetAtlasWorldBound()
	};
	cacheGenBuffer->Update(data);

	ID3D11UnorderedAccessView* resolveUAVs[2] = { a_accumTex->uav.get(), nullptr };
	context->CSSetShader(bentNormalFinalizeCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 2, resolveUAVs, nullptr);
	context->Dispatch((tileSize + 7) / 8, (tileSize + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);
}

bool TexGen::StartBentNormalTiles()
{
	UpdateWorldspaceID();

	if (worldspaceID.empty()) {
		logger::error("[TexGen] No worldspace known, cannot generate bent normal tiles");
		return false;
	}

	// The atlas dimensions the height tiles were stitched into; the regions are relative to these.
	std::filesystem::path atlasPath;
	if (!ResolveHeightAtlas(worldspaceID, atlasPath)) {
		logger::error("[TexGen] No height atlas found, cannot generate bent normal tiles");
		return false;
	}

	DirectX::TexMetadata metadata;
	DX::ThrowIfFailed(DirectX::GetMetadataFromDDSFile(atlasPath.c_str(), DirectX::DDS_FLAGS_NONE, metadata));

	const int cellsPerTile = settings.cacheAtlasTileCells;
	const int2 cellExtent = heightAtlasRange.maxCell - heightAtlasRange.minCell + int2(1, 1);
	if (cellsPerTile <= 0 || cellExtent.x % cellsPerTile != 0 || cellExtent.y % cellsPerTile != 0) {
		logger::error("[TexGen] Invalid height atlas tile layout");
		return false;
	}

	const int tilesX = cellExtent.x / cellsPerTile;
	const int tilesY = cellExtent.y / cellsPerTile;
	if (metadata.width % tilesX != 0 || metadata.height % tilesY != 0) {
		logger::error("[TexGen] Height atlas dimensions do not divide evenly into its tile layout");
		return false;
	}

	const int tileSizeX = (int)metadata.width / tilesX;
	const int tileSizeY = (int)metadata.height / tilesY;
	if (tileSizeX != tileSizeY) {
		logger::error("[TexGen] Height atlas tiles are not square: {}x{}", tileSizeX, tileSizeY);
		return false;
	}

	const int tileSize = tileSizeX;
	const bool layoutChanged =
		settings.cacheAtlasMinCellX != heightAtlasRange.minCell.x ||
		settings.cacheAtlasMinCellY != heightAtlasRange.minCell.y ||
		settings.cacheAtlasTileSize != tileSize ||
		settings.cacheAtlasTilesX != tilesX ||
		settings.cacheAtlasTilesY != tilesY;
	settings.cacheAtlasMinCellX = heightAtlasRange.minCell.x;
	settings.cacheAtlasMinCellY = heightAtlasRange.minCell.y;
	settings.cacheAtlasTileSize = tileSize;
	settings.cacheAtlasTilesX = tilesX;
	settings.cacheAtlasTilesY = tilesY;
	if (layoutChanged)
		globals::state->Save();

	bentNormalAtlasSize = int2((int)metadata.width, (int)metadata.height);

	// Mirror the completed height run exactly, using the manifest retained when its temporary tiles
	// were removed.
	bentNormalTileQueue.clear();
	for (const auto& tile : LoadHeightTileManifest(worldspaceID))
		if (tile.tileSize == (uint)tileSize && tile.cellsPerTile == cellsPerTile)
			bentNormalTileQueue.push_back(tile.originCell);

	if (bentNormalTileQueue.empty()) {
		logger::error("[TexGen] No height tiles matching the atlas layout, nothing to generate");
		return false;
	}

	bentNormalHeightResource = nullptr;
	bentNormalHeightSRV = nullptr;
	DX::ThrowIfFailed(LoadDDSLevelZero(
		globals::d3d::device, atlasPath, bentNormalHeightResource.put(), bentNormalHeightSRV.put()));

	std::ranges::sort(bentNormalTileQueue, [](const int2& a, const int2& b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});

	// One float output tile is reused for the whole run.
	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R16G16B16A16_FLOAT, (uint)tileSize, (uint)tileSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	bentNormalTileTex = eastl::make_unique<Texture2D>(desc, "TexGen::BentNormalTile");
	bentNormalTileTex->CreateUAV(uavDesc);

	bentNormalSweepCS = nullptr;
	bentNormalFinalizeCS = nullptr;
	bentNormalSweepCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "SWEEP", "" } }, "cs_5_0")));
	bentNormalFinalizeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "SWEEP_FINALIZE", "" } }, "cs_5_0")));

	// Hull scratch, one slot per line of the widest azimuth. A line drifts at most one minor texel
	// per major step, so a tile is never covered by more than twice its edge in lines.
	{
		const uint maxLines = (uint)tileSize * 2 + 2;

		D3D11_BUFFER_DESC bufferDesc = {
			.ByteWidth = maxLines * bentNormalHullCapacity * (uint)sizeof(float) * 2,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = (uint)sizeof(float) * 2
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavBufferDesc = {
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D11_UAV_DIMENSION_BUFFER,
			.Buffer = { .FirstElement = 0, .NumElements = maxLines * bentNormalHullCapacity, .Flags = 0 }
		};

		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&bufferDesc, nullptr, bentNormalHullBuffer.put()));
		DX::ThrowIfFailed(globals::d3d::device->CreateUnorderedAccessView(bentNormalHullBuffer.get(), &uavBufferDesc, bentNormalHullUAV.put()));

		Util::SetResourceName(bentNormalHullBuffer.get(), "TexGen::BentNormalHullStack");
		Util::SetResourceName(bentNormalHullUAV.get(), "TexGen::BentNormalHullStack UAV");
	}

	bentNormalTileIndex = 0;
	bentNormalTileGen = true;

	logger::info("[TexGen] Generating {0} bent normal tiles at {1}x{1} from a {2}x{3} atlas",
		bentNormalTileQueue.size(), tileSize, bentNormalAtlasSize.x, bentNormalAtlasSize.y);

	return true;
}

void TexGen::UpdateBentNormalTiles()
{
	if (!bentNormalTileGen)
		return;

	// One tile per frame; a whole set in a single call would sit far past the driver timeout.
	if (bentNormalTileIndex < bentNormalTileQueue.size()) {
		const int tileSize = settings.cacheAtlasTileSize;
		const int2 tileOriginCell = bentNormalTileQueue[bentNormalTileIndex];
		const int cellsPerTile = settings.cacheAtlasTileCells;

		// The tile's north west corner in atlas texels; atlas rows run north first, hence the flip.
		const int tileColumn = (tileOriginCell.x - settings.cacheAtlasMinCellX) / cellsPerTile;
		const int maxOriginCellY = settings.cacheAtlasMinCellY + (settings.cacheAtlasTilesY - 1) * cellsPerTile;
		const int tileRow = (maxOriginCellY - tileOriginCell.y) / cellsPerTile;

		DispatchBentNormalSweep(bentNormalTileTex.get(), int2(tileColumn * tileSize, tileRow * tileSize));

		DirectX::ScratchImage captured;
		DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, bentNormalTileTex->resource.get(), captured));

		const DirectX::Image* image = captured.GetImages();
		auto path = GetTilePath(worldspaceID, "_BN", (uint)tileSize, cellsPerTile, tileOriginCell);
		SaveMapDDS(*image, path);
		logger::info("[TexGen] Saved bent normal tile {}", path.string());
		bentNormalTileIndex++;
	}

	if (bentNormalTileIndex >= bentNormalTileQueue.size()) {
		logger::info("[TexGen] Bent normal tiles complete: {} tiles", bentNormalTileQueue.size());
		EnsureBentNormalAtlas(worldspaceID, true);

		bentNormalTileGen = false;
		bentNormalTileQueue.clear();
		bentNormalTileIndex = 0;
		bentNormalTileTex = nullptr;
		bentNormalHeightSRV = nullptr;
		bentNormalHeightResource = nullptr;
		bentNormalSweepCS = nullptr;
		bentNormalFinalizeCS = nullptr;
		bentNormalHullUAV = nullptr;
		bentNormalHullBuffer = nullptr;

		NotifyCacheMapsChanged();
	}
}

//////////////////////////////////////////////////////////////////////////////////
//// Settings UI
//////////////////////////////////////////////////////////////////////////////////

void TexGen::DrawOverlay()
{
	if (!bentNormalTileGen)
		return;

	const float scale = Util::GetUIScale();
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;
	float yPos = pos;

	static constexpr std::array<const char*, 3> stackedWindows = {
		"ShaderCompilationInfo",
		"ShaderBlockingInfo",
		"UWCacheCreationInfo"
	};
	for (const auto* name : stackedWindows) {
		if (auto* window = ImGui::FindWindowByName(name); window && window->Active)
			yPos = std::max(yPos, window->Pos.y + window->Size.y + ImGui::GetStyle().ItemSpacing.y);
	}

	const float percent = bentNormalTileQueue.empty() ? 0.0f : (float)bentNormalTileIndex / (float)bentNormalTileQueue.size();
	const auto progress = fmt::format("{}/{} ({:2.1f}%)", bentNormalTileIndex, bentNormalTileQueue.size(), percent * 100.0f);

	ImGui::SetNextWindowPos(ImVec2(pos, yPos));
	if (ImGui::Begin("TexGenCacheCreationInfo", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
				ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::TextUnformatted("Generating TexGen Cache: Bent Normals");
		ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), progress.c_str());
	}
	ImGui::End();
}

void TexGen::DrawSettings()
{
	if (!ImGui::BeginTabBar("##TexGenTabs"))
		return;

	if (ImGui::BeginTabItem("Generation")) {
		ImGui::BeginDisabled(IsGenerating() || worldspaceID.empty());
		if (ImGui::Button("Generate Height Map"))
			StartLandHeightRun(false, int2(0, 0));
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Reads terrain heights straight from the plugin LAND records and writes the height\n"
				"tiles for the whole worldspace. The player is not moved and no cell has to be loaded.");

		if (landGenRunning) {
			ImGui::SameLine();
			if (ImGui::Button("Stop")) {
				landGenRunning = false;
				landRunFiles.reset();
				landRunWorldspace = nullptr;
			}

			const float percent = landRunTileTotal > 0 ? (float)tilesDone / (float)landRunTileTotal : 0.0f;
			ImGui::ProgressBar(percent, ImVec2(0.0f, 0.0f), fmt::format("{}/{} tiles", tilesDone, landRunTileTotal).c_str());
			ImGui::Text("Tile at cell %d, %d - %d cells read", landRunCurrentTile.x * landRunCellsPerTile, landRunCurrentTile.y * landRunCellsPerTile, cellsDone);
		}

		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem("Debugging")) {
		ImGui::TextWrapped(
			"Bakes the terrain LOD map cache into %s.\n"
			"Generation teleports the player across the worldspace and takes a long time; the features "
			"that render with these maps load them from disk and do not need this feature at runtime.",
			cachePath.string().c_str());

		ImGui::Separator();

		ImGui::Text("Worldspace: %s", worldspaceID.empty() ? "N/A" : worldspaceID.c_str());

		if (auto* worldspace = RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetWorldspace() : nullptr) {
			int2 minCell, maxCell;
			const char* boundsSource = "none";
			if (TexGenLand::ResolveCellBounds(worldspace, minCell, maxCell, boundsSource))
				ImGui::BulletText("Cell bounds %d,%d to %d,%d (from %s)", minCell.x, minCell.y, maxCell.x, maxCell.y, boundsSource);
			else
				ImGui::BulletText("Cell bounds unresolved");
		}

		if (ImGui::Button("Check LAND Decode"))
			VerifyLandDecode();
		ImGui::SameLine();
		ImGui::BeginDisabled(IsGenerating() || worldspaceID.empty());
		if (ImGui::Button("Generate Tile At Player (LAND)")) {
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				auto playerPos = player->GetPosition();
				StartLandHeightRun(true, WorldToCell(playerPos.x, playerPos.y));
			}
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Rebuilds just the tile under the player from LAND records. Pair it with the raycast\n"
				"button below on the same tile to diff the two.");

		ImGui::BeginDisabled(IsGenerating() && !heightGenRunning);
		if (ImGui::Button(heightGenRunning ? "Stop Raycast Walk" : "Generate Height Map (raycast reference)")) {
			heightGenRunning = !heightGenRunning;
			heightGenSingleTile = false;
			heightGenInit = true;
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The original bake: teleports the player cell by cell and raycasts Havok collision.\n"
				"Hours to run, but it is the only path that sees statics, so it stays as the reference\n"
				"the LAND output is checked against.");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Decodes the player's cell straight from the plugins and compares all 1089 vertices\n"
				"against the copy the engine loaded. Results go to the log; an exact match proves the\n"
				"decode. Needs the player standing in an exterior cell.");

		ImGui::InputText("DynDOLOD Worldspace Directory", &settings.dynDOLODPath);
		ImGui::TextWrapped("Select the DynDOLOD terrain-texture folder for the worldspace you want to generate textures for.");

		ImGui::BeginDisabled(settings.dynDOLODPath.empty() || worldspaceID.empty());
		if (ImGui::Button("Generate albedo atlas"))
			BuildLODAtlas(worldspaceID);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stitches the xLODGen LOD tiles in\n%s\ninto one albedo atlas.", settings.dynDOLODPath.c_str());

		ImGui::BeginDisabled(worldspaceID.empty() || IsGenerating());
		if (ImGui::Button("Generate Normal, AO and Bent Normal"))
			BuildDerivedMaps(worldspaceID, true);
		ImGui::EndDisabled();

		ImGui::BeginDisabled(worldspaceID.empty() || IsGenerating());
		if (ImGui::Button("Generate Normal, AO and Bent Normal (Flattened)"))
			BuildDerivedMaps(worldspaceID, true, true);
		ImGui::EndDisabled();

		ImGui::Checkbox("Skip Bent Normal Tiles", &settings.skipBentNormalTiles);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Leave the bent normal tiles and atlas untouched when the buttons above rebuild the derived maps.\n"
				"They are the slowest part of the bake, so this keeps normal and occlusion iteration quick.\n"
				"Use \"Generate Bent Normal Tiles\" below to rebuild them explicitly.");

		ImGui::SliderFloat("Map Scale down from 16k", &derivedHeightScale, 0.01, 1.0);

		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The same set, with the occlusion maps derived from a flattened copy of the downscaled height\n"
				"map, so they lose the fine relief while keeping cliffs and mountain fronts. The normal map is\n"
				"always built from the unflattened heights.\n"
				"The flattened map is never written to disk; %s_HD keeps the unflattened heights.",
				worldspaceID.empty() ? "<Worldspace>" : worldspaceID.c_str());

		ImGui::SliderFloat("Flatten Height", &settings.smoothFlattenHeight, 1.0f, 2000.0f, "%.0f units", ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The height difference the filter flattens outright. A neighbour within this many game units of\n"
				"a texel is averaged in at full weight, so relief up to this tall is removed rather than merely\n"
				"softened; the weight then rolls off, and relief past Preserve Above keeps its edge.");

		ImGui::SliderFloat("Preserve Above", &settings.smoothRolloff, 1.0f, 8.0f, "%.1fx flatten height");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Where the range gate closes, as a multiple of the flatten height: relief at least this tall is\n"
				"never averaged across. At %.1fx that is %.0f game units.",
				settings.smoothRolloff, settings.smoothFlattenHeight * settings.smoothRolloff);

		ImGui::SliderInt("Smooth Radius", &settings.smoothRadius, 1, smoothMaxRadius);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Taps either side of centre in each single axis pass, in texels of the downscaled map.\nSets both the cost and how far the filter reaches.");

		ImGui::SliderInt("Smooth Iterations", &settings.smoothIterations, 1, smoothMaxIterations);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Horizontal plus vertical pass pairs. Reach only grows as the square root of this, so\nraise the radius first and use iterations to push past what one pass can reach.");

		ImGui::BeginDisabled(heightGenRunning);  // the tile layout is latched for the duration of a run

		int cellsIndex = settings.cacheTileCells == 4 ? 0 : 1;
		if (ImGui::Combo("Height Tile Cells", &cellsIndex,
				"4x4 cells\0"
				"8x8 cells\0"))
			settings.cacheTileCells = cellsIndex == 0 ? 4 : 8;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Worldspace cells covered by each height tile.");

		int sizeIndex = settings.cacheTileSize == 512 ? 0 : 1;
		if (ImGui::Combo("Height Tile Resolution", &sizeIndex,
				"512\0"
				"1024\0"))
			settings.cacheTileSize = sizeIndex == 0 ? 512 : 1024;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Texels per height tile edge. Higher resolutions take proportionally longer to generate.");

		ImGui::SliderInt("Wait frames", &heightSettleFrames, 1, 100);

		ImGui::EndDisabled();

		const int texelsPerCell = (int)GetHeightTileSize() / GetHeightTileCells();
		ImGui::Text("%u^2 tile, %d texels per cell, %.0f units per texel, 16 bit float",
			GetHeightTileSize(), texelsPerCell, worldCellSize / (float)texelsPerCell);

		ImGui::BeginDisabled(heightGenRunning || worldspaceID.empty());
		if (ImGui::Button("Generate Tile At Player")) {
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				auto playerPos = player->GetPosition();
				heightGenTargetCell = WorldToCell(playerPos.x, playerPos.y);
				heightGenSingleTile = true;
				heightGenInit = true;
				heightGenRunning = true;
			}
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates only the tile covering the player's current cell, then stops.\nDoes not touch the progress of a full run.");

		ImGui::BeginDisabled(heightGenRunning || worldspaceID.empty());
		if (ImGui::Button("Rebuild Height Atlas")) {
			std::filesystem::path path;
			if (ResolveHeightAtlas(worldspaceID, path, true))
				NotifyCacheMapsChanged();
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stitches every height tile for %s into %s_H.<minX>.<minY>.<maxX>.<maxY>.dds, replacing any earlier one.\nBuilt automatically when the worldspace cache loads and the atlas is missing.",
				worldspaceID.empty() ? "the current worldspace" : worldspaceID.c_str(),
				worldspaceID.empty() ? "<Worldspace>" : worldspaceID.c_str());

		if (heightAtlasRange.valid)
			ImGui::BulletText("Height atlas cells %d,%d to %d,%d", heightAtlasRange.minCell.x, heightAtlasRange.minCell.y, heightAtlasRange.maxCell.x, heightAtlasRange.maxCell.y);

		ImGui::BeginDisabled(heightGenRunning || bentNormalTileGen || worldspaceID.empty() || !heightMapSRV);
		if (ImGui::Button("Generate Bent Normal Tiles"))
			StartBentNormalTiles();

		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates one bent normal tile per height tile from the atlas, matching their\nsize, format and naming. One tile per frame; expect a long, unresponsive run.");

		ImGui::BeginDisabled(IsGenerating() || worldspaceID.empty());
		if (ImGui::Button("Rebuild Bent Normal Atlas")) {
			std::filesystem::path path;
			if (ResolveBentNormalAtlas(worldspaceID, path, true))
				NotifyCacheMapsChanged();
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stitches every bent normal tile into %s_BN.<minX>.<minY>.<maxX>.<maxY>.dds, replacing any earlier one.\nBuilt automatically when the worldspace cache loads and the atlas is missing.",
				worldspaceID.empty() ? "<Worldspace>" : worldspaceID.c_str());

		if (bentNormalTileGen)
			ImGui::Text("Bent normal tiles: %zu / %zu", bentNormalTileIndex, bentNormalTileQueue.size());

		if (heightGenRunning) {
			if (heightGenSingleTile)
				ImGui::Text("Generating single tile at cell %d, %d - %d cells done", heightGenTargetCell.x, heightGenTargetCell.y, cellsDone);
			else
				ImGui::Text("Generating from cell %d, %d - %d tiles / %d cells done", settings.cacheProgressX, settings.cacheProgressY, tilesDone, cellsDone);
		}

		if (heightPreviewValid && heightPreviewTex) {
			static float heightPreviewScale = 0.25f;
			ImGui::SliderFloat("Preview Scale", &heightPreviewScale, 0.1f, 1.0f, "%.2f");

			ImGui::Text("Last tile: origin cell %d, %d", heightPreviewOrigin.x, heightPreviewOrigin.y);
			BUFFER_VIEWER_NODE_BULLET(heightPreviewTex, heightPreviewScale);
		}

		// Cell to world position converter
		static int cellCoords[2] = { 0, 0 };
		ImGui::InputInt2("Cell", cellCoords);

		ImGui::SameLine();
		if (ImGui::Button("From Player")) {
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				auto playerPos = player->GetPosition();
				const int2 playerCell = WorldToCell(playerPos.x, playerPos.y);
				cellCoords[0] = playerCell.x;
				cellCoords[1] = playerCell.y;
			}
		}

		const float2 cellOrigin = float2((float)cellCoords[0], (float)cellCoords[1]) * worldCellSize;
		ImGui::BulletText("SW corner: %.0f, %.0f", cellOrigin.x, cellOrigin.y);
		ImGui::BulletText("Centre:    %.0f, %.0f", cellOrigin.x + worldCellSize * 0.5f, cellOrigin.y + worldCellSize * 0.5f);
		ImGui::BulletText("NE corner: %.0f, %.0f", cellOrigin.x + worldCellSize, cellOrigin.y + worldCellSize);

		const int2 targetTileOrigin = GetTileOriginCell(int2(cellCoords[0], cellCoords[1]), GetHeightTileCells());
		ImGui::BulletText("Tile origin cell: %d, %d", targetTileOrigin.x, targetTileOrigin.y);

		ImGui::BeginDisabled(heightGenRunning || worldspaceID.empty());
		if (ImGui::Button("Generate Tile At Cell")) {
			heightGenTargetCell = int2(cellCoords[0], cellCoords[1]);
			heightGenSingleTile = true;
			heightGenInit = true;
			heightGenRunning = true;
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates the tile containing this cell, teleporting the player through it.\nDoes not touch the progress of a full run.");

		ImGui::BeginDisabled(IsGenerating() || worldspaceID.empty());
		if (ImGui::Button("Generate Tile At Cell (LAND)"))
			StartLandHeightRun(true, int2(cellCoords[0], cellCoords[1]));
		ImGui::SameLine();
		if (ImGui::Button("Diff Tile Against Disk"))
			DiffHeightTileAgainstDisk(int2(cellCoords[0], cellCoords[1]));
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Reads this tile from LAND records and compares it against the tile already written to\n"
				"disk, writing nothing. Bake the tile with the raycast reference first, then diff:\n"
				"the difference should be near zero over open terrain and one sided wherever a static\n"
				"stands. A gradient across the tile means a coordinate or row order bug; noise means a\n"
				"decode bug. Results go to the log.");

		// Atlas texel to cell, using the layout the last atlas build recorded
		static int texelCoords[2] = { 0, 0 };
		ImGui::InputInt2("Atlas Texel", texelCoords);

		int2 texelCell;
		if (!AtlasTexelToCell(int2(texelCoords[0], texelCoords[1]), settings, texelCell)) {
			ImGui::BulletText("Build the atlas to map texels to cells");
		} else {
			ImGui::BulletText("Cell: %d, %d", texelCell.x, texelCell.y);
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy To Cell")) {
				cellCoords[0] = texelCell.x;
				cellCoords[1] = texelCell.y;
			}

			ImGui::BeginDisabled(heightGenRunning);
			if (ImGui::Button("Generate Tile At Texel")) {
				heightGenTargetCell = texelCell;
				heightGenSingleTile = true;
				heightGenInit = true;
				heightGenRunning = true;
			}
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Generates the tile containing this atlas texel.\nTexel 0,0 is the top left corner of %s_H.dds, %d texels per cell.",
					worldspaceID.empty() ? "<Worldspace>" : worldspaceID.c_str(),
					settings.cacheAtlasTileSize / std::max(1, settings.cacheAtlasTileCells));
		}
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}

#undef I18N_KEY_PREFIX
