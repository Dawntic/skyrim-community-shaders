#include "TexGen.h"
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
	smoothIterations)

//////////////////////////////////////////////////////////////////////////////////
//// Height cache tiles
//////////////////////////////////////////////////////////////////////////////////

void TexGen::ClearHeightTile()
{
	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_WRITE, 0, &mapped));

	// Texels belonging to cells outside the worldspace are never sampled and stay at zero height.
	for (uint y = 0; y < tileSize; ++y)
		memset((uint8_t*)mapped.pData + y * mapped.RowPitch, 0, tileSize * sizeof(uint16_t));

	context->Unmap(cacheOutputTexH->resource.get(), 0);
}

void TexGen::SaveHeightTile(const int2& a_tileOriginCell, int a_cellsPerTile)
{
	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;
	auto path = GetTilePath(worldspaceID, "_H", tileSize, a_cellsPerTile, a_tileOriginCell);

	DirectX::ScratchImage outputImage;
	DX::ThrowIfFailed(outputImage.Initialize2D(DXGI_FORMAT_R16_FLOAT, tileSize, tileSize, 1, 1));

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &mapped));

	const DirectX::Image* image = outputImage.GetImages();
	for (uint y = 0; y < tileSize; ++y)
		memcpy(image->pixels + y * image->rowPitch, (const uint8_t*)mapped.pData + y * mapped.RowPitch, tileSize * sizeof(uint16_t));

	context->Unmap(cacheOutputTexH->resource.get(), 0);
	SaveMapDDS(*image, path);
	logger::info("[TexGen] Saved height tile {}", path.string());
}

// debugging func.
void TexGen::UpdateHeightPreview(const int2& a_tileOriginCell)
{
	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;
	const DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT;

	// The preview holds exactly what the DDS holds, in the same format, unmodified.
	if (!heightPreviewTex || heightPreviewTex->desc.Width != tileSize || heightPreviewTex->desc.Format != format) {
		CD3D11_TEXTURE2D_DESC desc(format, tileSize, tileSize, 1, 1, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, format, 0, 1);

		heightPreviewValid = false;
		heightPreviewTex = eastl::make_unique<Texture2D>(desc, "TexGen::HeightTilePreview");
		heightPreviewTex->CreateSRV(srvDesc);
	}

	D3D11_MAPPED_SUBRESOURCE src;
	DX::ThrowIfFailed(context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &src));

	D3D11_MAPPED_SUBRESOURCE dst;
	DX::ThrowIfFailed(context->Map(heightPreviewTex->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst));

	for (uint y = 0; y < tileSize; ++y)
		memcpy((uint8_t*)dst.pData + y * dst.RowPitch, (const uint8_t*)src.pData + y * src.RowPitch, tileSize * sizeof(uint16_t));

	context->Unmap(heightPreviewTex->resource.get(), 0);
	context->Unmap(cacheOutputTexH->resource.get(), 0);

	heightPreviewOrigin = a_tileOriginCell;
	heightPreviewValid = true;
}

void TexGen::GenerateHeightMap()
{
	static constexpr float CELL = worldCellSize;
	static constexpr int2 startCell = int2(-57, -43);  // same as dyndolod
	static constexpr int2 endCell = int2(61, 50);      // same as dyndolod, exclusive

	auto context = globals::d3d::context;
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
				ClearHeightTile();
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

		if (!cacheOutputTexH || cacheOutputTexH->desc.Width != tileSize || cacheOutputTexH->desc.Height != tileSize) {
			CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R16_FLOAT, tileSize, tileSize, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ);
			cacheOutputTexH = eastl::make_unique<Texture2D>(desc, "TexGen::HeightCacheTile");
		}

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

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped));

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
			uint16_t* tex = (uint16_t*)((uint8_t*)mapped.pData + texCoord.y * mapped.RowPitch);
			tex[texCoord.x] = DirectX::PackedVector::XMConvertFloatToHalf(groundHeight);
		}
	}
	context->Unmap(cacheOutputTexH->resource.get(), 0);

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

		eastl::unique_ptr<Texture2D> smoothedHeight;
		if (a_smoothHeight) {
			smoothedHeight = SmoothHeightMap(downscaledHeightSRV.get(), mapSize);
			if (!smoothedHeight) {
				heightMapSRV = previousHeightMapSRV;
				return false;
			}
			heightMapSRV = smoothedHeight->srv.get();
		}

		generated = GenerateNormalMap(mapSize) && GenerateCardinalOcclusionMaps(mapSize);
		heightMapSRV = previousHeightMapSRV;

		if (generated) {
			logger::info("[TexGen] Built derived maps at {}x{} from {}{}", width, height, heightAtlasPath.string(),
				a_smoothHeight ? " (flattened)" : "");
			NotifyCacheMapsChanged();
		}
	}

	const auto bentNormalPath = MakeAtlasPath(cachePath, worldspaceID, "_BN", heightAtlasRange.minCell, heightAtlasRange.maxCell);
	const auto heightTiles = LoadHeightTileManifest(worldspaceID);
	const bool bentNormalTilesMissing = std::ranges::any_of(heightTiles, [&](const HeightTileFile& tile) {
		if (tile.tileSize != (uint)settings.cacheAtlasTileSize || tile.cellsPerTile != settings.cacheAtlasTileCells)
			return false;
		return !std::filesystem::exists(GetTilePath(worldspaceID, "_BN", tile.tileSize, tile.cellsPerTile, tile.originCell), ec);
	});

	if ((a_forceRebuild || !std::filesystem::exists(bentNormalPath, ec) || bentNormalTilesMissing) && !bentNormalTileGen)
		generated = StartBentNormalTiles() && generated;

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
		ImGui::BeginDisabled(!heightGenRunning && worldspaceID.empty());
		if (ImGui::Button(heightGenRunning ? "Stop Height Generation" : "Generate Height Map")) {
			heightGenRunning = !heightGenRunning;
			heightGenSingleTile = false;
			heightGenInit = true;
		}
		ImGui::EndDisabled();
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
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The same set, derived from a flattened copy of the downscaled height map, so the normals and\n"
				"the occlusion set lose the fine relief while keeping cliffs and mountain fronts.\n"
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
