#include "Skylighting.h"

#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.skylighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	MaxZenith,
	MinDiffuseVisibility,
	MinSpecularVisibility,
	SkyInfluence,
	EnvInfluence,
	cacheProgressX,
	cacheProgressY,
	cacheTileCells,
	cacheTileSize,
	cacheExport16Bit,
	cacheAtlasZeroBase,
	cacheAtlasMinHeight,
	cacheAtlasMinCellX,
	cacheAtlasMinCellY,
	cacheAtlasTileSize,
	cacheAtlasTileCells,
	cacheAtlasTilesX,
	cacheAtlasTilesY,
	cacheBentNormalAtlasScale)

// Every generated LOD map is a single surface: one mip level, one array slice, no cube faces.
// Written through explicit metadata so the property is enforced at the call rather than being an
// accident of which SaveToDDSFile overload was picked.
static bool SaveMapDDS(const DirectX::Image& image, const std::filesystem::path& path)
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
		logger::error("[Skylighting] Failed to save {}: {:X}", path.string(), (uint32_t)hr);
		return false;
	}

	return true;
}

// Floor division; the tile grid is anchored to the worldspace cell grid, so negative cell
// coordinates must round towards -inf rather than towards zero.
static int FloorDiv(int a, int b)
{
	int q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0)))
		--q;
	return q;
}

// Split a file stem on '.', the separator both the tile and atlas names use.
static std::vector<std::string> SplitStem(const std::filesystem::path& path)
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
static std::filesystem::path MakeAtlasPath(const std::filesystem::path& dir, const std::string& worldspaceID, const std::string& mapTag, const int2& minCell, const int2& maxCell)
{
	return dir / fmt::format("{}{}.{}.{}.{}.{}.dds", worldspaceID, mapTag, minCell.x, minCell.y, maxCell.x, maxCell.y);
}

static bool ParseAtlasName(const std::filesystem::path& path, const std::string& worldspaceID, const std::string& mapTag, Skylighting::AtlasCellRange& o_range)
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
static void RemoveExistingAtlases(const std::filesystem::path& dir, const std::string& worldspaceID, const std::string& mapTag)
{
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		const auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		Skylighting::AtlasCellRange range;
		if (!ParseAtlasName(path, worldspaceID, mapTag, range))
			continue;

		if (std::filesystem::remove(path, ec))
			logger::info("[Skylighting] Replaced previous atlas {}", path.string());
	}
}

void Skylighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Skylighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
}

void Skylighting::ResetSkylighting()
{
	auto context = globals::d3d::context;
	UINT clr[1] = { 0 };
	context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	queuedResetSkylighting = false;
}

void Skylighting::DrawSettings()
{
	if (ImGui::Button("Reload Shaders"))
		ClearShaderCache();

	ImGui::SliderFloat("Diffuse Min Visibility", &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat("Specular Min Visibility", &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");

	ImGui::SliderFloat("Sky Influence", &settings.SkyInfluence, 0.01f, 5.0f);
	ImGui::SliderFloat("Environment Influence", &settings.EnvInfluence, 0.01f, 5.0f);

	ImGui::Checkbox("Terrain Lighting Map", &updateTerrainLighting);
	ImGui::Checkbox("Sparse Probe Map", &runSparse);

	static float debugRescale = 1.0f;
	ImGui::SliderFloat("View Resize", &debugRescale, 0.0f, 10.0f);

	//BUFFER_VIEWER_NODE_BULLETA(terrainLightingTex->srv.get(), debugRescale);

	ImGui::BulletText("Probe View");
	if (texSparseProbeArray.get())
		BUFFER_VIEWER_NODE_BULLETA(texSparseProbeArray->srv.get(), debugRescale);

	if (ImGui::Button("Generate albedo and norm")) {
		auto outputPath = cachePath / "Tamriel_A.dds";
		//auto outputPath2 = cachePath / "Tamriel_N.dds";
		BuildAtlas(outputPath, "");
		//BuildAtlas(outputPath2, "_n");
	}

	if (ImGui::Button("Generate card Occl")) {
		GenerateCardinalOcclusionMap();
	}

	if (ImGui::Button("Generate bent normal")) {
		GenerateBentNormalMap();
	}

	if (ImGui::Button("Generate Normal")) {
		GenerateNormalMap();
	}

	{
		ImGui::BeginDisabled(MapGen);  // the tile layout is latched for the duration of a run

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

		ImGui::Checkbox("Export 16 Bit Height", &settings.cacheExport16Bit);
		ImGui::SliderInt("Wait frames", &heightSettleFrames, 1, 100);

		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"On: xLODGen format, 16 bit unsigned with zero height at %d and 8 game units per step\n"
				"(height = (value - %d) * %g).\n"
				"Off: raw 32 bit float game units.",
				(int)heightExportOffset, (int)heightExportOffset, heightExportScale);

		ImGui::EndDisabled();

		const int texelsPerCell = (int)GetHeightTileSize() / GetHeightTileCells();
		ImGui::Text("%u^2 tile, %d texels per cell, %.0f units per texel, %s",
			GetHeightTileSize(), texelsPerCell, 4096.0f / (float)texelsPerCell,
			settings.cacheExport16Bit ? "16 bit unsigned" : "32 bit float");

		if (ImGui::Button(MapGen ? "Stop Height Generation" : "Generate Height Map")) {
			MapGen = !MapGen;
			heightGenSingleTile = false;
			heightGenInit = true;  // stopping discards the in-flight tile; restart on its boundary
		}

		ImGui::SameLine();

		ImGui::BeginDisabled(MapGen);
		if (ImGui::Button("Generate Tile At Player")) {
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				auto playerPos = player->GetPosition();
				heightGenTargetCell = int2((int)std::floor(playerPos.x / worldCellSize), (int)std::floor(playerPos.y / worldCellSize));
				heightGenSingleTile = true;
				heightGenInit = true;
				MapGen = true;
			}
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates only the tile covering the player's current cell, then stops.\nDoes not touch the progress of a full run.");

		ImGui::Checkbox("Zero Base Atlas", &settings.cacheAtlasZeroBase);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Bias the atlas so its lowest point is 0.0 instead of storing absolute heights.\nTakes effect on the next atlas build.");

		if (settings.cacheAtlasZeroBase && settings.cacheAtlasMinHeight != 0.0f)
			ImGui::Text("Last atlas: 0.0 is %.0f game units", settings.cacheAtlasMinHeight);

		ImGui::BeginDisabled(MapGen || cacheWorldspaceID.empty());
		if (ImGui::Button("Rebuild Height Atlas")) {
			if (EnsureHeightAtlas(cacheWorldspaceID, true)) {
				std::filesystem::path path;
				if (FindAtlas(cacheWorldspaceID, "_H", path, heightAtlasRange)) {
					if (HMapSRV) {
						HMapSRV->Release();
						HMapSRV = nullptr;
					}
					DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &HMapSRV);
					DirectX::TexMetadata metadata;
					if (SUCCEEDED(DirectX::GetMetadataFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
						bool is16Bit = metadata.format == DXGI_FORMAT_R16_UNORM;
						HeightMapOffset = (is16Bit && !settings.cacheAtlasZeroBase) ? heightExportOffset / 65535.0f : 0.0f;
						HeightMapScale = is16Bit ? heightExportScale * 65535.0f : 1.0f;
					}
				}
			}
		}

		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stitches every height tile for %s into %s_H.<minX>.<minY>.<maxX>.<maxY>.dds, replacing any earlier one.\nBuilt automatically when the worldspace cache loads and the atlas is missing.",
				cacheWorldspaceID.empty() ? "the current worldspace" : cacheWorldspaceID.c_str(),
				cacheWorldspaceID.empty() ? "<Worldspace>" : cacheWorldspaceID.c_str());

		if (heightAtlasRange.valid)
			ImGui::BulletText("Height atlas cells %d,%d to %d,%d", heightAtlasRange.minCell.x, heightAtlasRange.minCell.y, heightAtlasRange.maxCell.x, heightAtlasRange.maxCell.y);

		ImGui::BeginDisabled(MapGen || (!bentNormalTileGen && (cacheWorldspaceID.empty() || !HMapSRV)));
		if (ImGui::Button(bentNormalTileGen ? "Stop Bent Normal Tiles" : "Generate Bent Normal Tiles")) {
			if (bentNormalTileGen)
				StopBentNormalTiles();
			else
				StartBentNormalTiles();
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates one bent normal tile per height tile from the atlas, matching their\nsize, format and naming. One tile per frame; expect a long, unresponsive run.");

		ImGui::SliderFloat("BN Atlas Scale", &settings.cacheBentNormalAtlasScale, 0.05f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Downscales every tile as the bent normal atlas is stitched.\n0.85 makes it 15%% smaller per edge, so roughly 28%% of the memory saved.");

		if (settings.cacheAtlasTileSize > 0) {
			const int scaledTile = std::clamp((int)std::lround((float)settings.cacheAtlasTileSize * settings.cacheBentNormalAtlasScale), 1, settings.cacheAtlasTileSize);
			const size_t atlasBytes = (size_t)scaledTile * settings.cacheAtlasTilesX * (size_t)scaledTile * settings.cacheAtlasTilesY * 8;
			ImGui::BulletText("BN atlas: %d texel tiles, %dx%d, %zu MB",
				scaledTile, scaledTile * settings.cacheAtlasTilesX, scaledTile * settings.cacheAtlasTilesY, atlasBytes / (1024 * 1024));
		}

		ImGui::BeginDisabled(MapGen || bentNormalTileGen || cacheWorldspaceID.empty());
		if (ImGui::Button("Rebuild Bent Normal Atlas")) {
			if (EnsureBentNormalAtlas(cacheWorldspaceID, true)) {
				std::filesystem::path path;
				if (FindAtlas(cacheWorldspaceID, "_BN", path, bentNormalAtlasRange)) {
					if (BNMapSRV) {
						BNMapSRV->Release();
						BNMapSRV = nullptr;
					}
					DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &BNMapSRV);
				}
			}
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Stitches every bent normal tile into %s_BN.<minX>.<minY>.<maxX>.<maxY>.dds, replacing any earlier one.\nBuilt automatically when the worldspace cache loads and the atlas is missing.",
				cacheWorldspaceID.empty() ? "<Worldspace>" : cacheWorldspaceID.c_str());

		if (bentNormalAtlasRange.valid)
			ImGui::BulletText("BN atlas cells %d,%d to %d,%d", bentNormalAtlasRange.minCell.x, bentNormalAtlasRange.minCell.y, bentNormalAtlasRange.maxCell.x, bentNormalAtlasRange.maxCell.y);

		if (bentNormalTileGen)
			ImGui::Text("Bent normal tiles: %zu / %zu", bentNormalTileIndex, bentNormalTileQueue.size());

		if (BNTileSRV)
			ImGui::BulletText("Streamed BN tile: cell %d, %d (%.0f, %.0f to %.0f, %.0f)",
				bnTileOriginCell.x, bnTileOriginCell.y,
				bnTileWorldBounds.x, bnTileWorldBounds.y, bnTileWorldBounds.z, bnTileWorldBounds.w);
		else if (bnTileOriginValid)
			ImGui::BulletText("Streamed BN tile: none for cell %d, %d", bnTileOriginCell.x, bnTileOriginCell.y);

		if (MapGen) {
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
				cellCoords[0] = (int)std::floor(playerPos.x / worldCellSize);
				cellCoords[1] = (int)std::floor(playerPos.y / worldCellSize);
			}
		}

		const float2 cellOrigin = float2((float)cellCoords[0], (float)cellCoords[1]) * worldCellSize;
		ImGui::BulletText("SW corner: %.0f, %.0f", cellOrigin.x, cellOrigin.y);
		ImGui::BulletText("Centre:    %.0f, %.0f", cellOrigin.x + worldCellSize * 0.5f, cellOrigin.y + worldCellSize * 0.5f);
		ImGui::BulletText("NE corner: %.0f, %.0f", cellOrigin.x + worldCellSize, cellOrigin.y + worldCellSize);

		const int cellsPerTile = GetHeightTileCells();
		int2 targetTileOrigin = int2(FloorDiv(cellCoords[0], cellsPerTile), FloorDiv(cellCoords[1], cellsPerTile)) * cellsPerTile;
		ImGui::BulletText("Tile origin cell: %d, %d", targetTileOrigin.x, targetTileOrigin.y);

		ImGui::BeginDisabled(MapGen);
		if (ImGui::Button("Generate Tile At Cell")) {
			heightGenTargetCell = int2(cellCoords[0], cellCoords[1]);
			heightGenSingleTile = true;
			heightGenInit = true;
			MapGen = true;
		}
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Generates the tile containing this cell, teleporting the player through it.\nDoes not touch the progress of a full run.");

		// Atlas texel to cell, using the layout the last atlas build recorded
		static int texelCoords[2] = { 0, 0 };
		ImGui::InputInt2("Atlas Texel", texelCoords);

		int2 texelCell;
		if (!AtlasTexelToCell(int2(texelCoords[0], texelCoords[1]), texelCell)) {
			ImGui::BulletText("Build the atlas to map texels to cells");
		} else {
			ImGui::BulletText("Cell: %d, %d", texelCell.x, texelCell.y);
			ImGui::SameLine();
			if (ImGui::SmallButton("Copy To Cell")) {
				cellCoords[0] = texelCell.x;
				cellCoords[1] = texelCell.y;
			}

			ImGui::BeginDisabled(MapGen);
			if (ImGui::Button("Generate Tile At Texel")) {
				heightGenTargetCell = texelCell;
				heightGenSingleTile = true;
				heightGenInit = true;
				MapGen = true;
			}
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Generates the tile containing this atlas texel.\nTexel 0,0 is the top left corner of %s_H.dds, %d texels per cell.",
					cacheWorldspaceID.empty() ? "<Worldspace>" : cacheWorldspaceID.c_str(),
					settings.cacheAtlasTileSize / std::max(1, settings.cacheAtlasTileCells));
		}
	}

	ImGui::Separator();

	ImGui::Checkbox("Enable Lighting", (bool*)&settings.toggleLighting);  //tmp
	ImGui::Checkbox("Enable Trees", (bool*)&settings.toggleTrees);
	ImGui::Checkbox("Enable Grass", (bool*)&settings.toggleGrass);
	ImGui::Checkbox("Enable Deferred", (bool*)&settings.toggleDeferred);
	ImGui::Checkbox("Enable Effect", (bool*)&settings.toggleEffect);

	std::string curr_worldspace = "N/A";
	auto tes = RE::TES::GetSingleton();
	if (tes) {
		auto worldspace = tes->GetRuntimeData2().worldSpace;
		if (worldspace) {
			curr_worldspace = worldspace->GetFormEditorID();
		}
	}
	ImGui::Text(fmt::format("Worldspace has cache: {}", worldSpaceCachedMapList.contains(curr_worldspace)).c_str());
	ImGui::Text("Cache is loaded: %s", (cacheWorldspaceID == curr_worldspace) ? "true" : "false");

	if (ImGui::Button("Rebuild Skylighting"))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes below require rebuilding, a loading screen, or moving away from the current location to apply.");

	ImGui::SliderAngle("Max Zenith Angle", &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Smaller angles creates more focused top-down shadow.");
}

void Skylighting::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		texOcclusion = new Texture2D(texDesc);
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateDSV(dsvDesc);
	}

	{
		D3D11_TEXTURE3D_DESC texDesc{
			.Width = probeArrayDims[0],
			.Height = probeArrayDims[1],
			.Depth = probeArrayDims[2],
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = texDesc.Depth }
		};

		texProbeArray = new Texture3D(texDesc);
		texProbeArray->CreateSRV(srvDesc);
		texProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UINT;

		texAccumFramesArray = new Texture3D(texDesc);
		texAccumFramesArray->CreateSRV(srvDesc);
		texAccumFramesArray->CreateUAV(uavDesc);
	}

	{
		CD3D11_TEXTURE2D_DESC texDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, sparseGridSize.x, sparseGridSize.y, 3, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2DARRAY, texDesc.Format, 0, 1, 0, 3);
		CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2DARRAY, texDesc.Format, 0, 0, 3);

		texSparseProbeArray = eastl::make_unique<Texture2D>(texDesc);
		texSparseProbeArray->CreateSRV(srvDesc);
		texSparseProbeArray->CreateUAV(uavDesc);
	}

	{
		CD3D11_TEXTURE2D_DESC texDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, terrainMapSize.x, terrainMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 1, 0, 1);
		CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 0, 1);

		terrainLightingTex = eastl::make_unique<Texture2D>(texDesc);
		terrainLightingTex->CreateSRV(srvDesc);
		terrainLightingTex->CreateUAV(uavDesc);
	}

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;  // Use comparison filtering
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;               // Address mode (Clamp for shadow maps)
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;  // Comparison function
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
	}

	GetCachedWorldspaces();

	CompileComputeShaders();

	// The height cache staging tile is created on demand, since its size depends on the settings.
}

void Skylighting::GetCachedWorldspaces()
{
	for (const auto& entry : std::filesystem::directory_iterator(cachePath)) {
		auto& path = entry.path();
		if (path.extension() == ".dds") {
			auto name = path.stem().string();
			if (name.contains('.'))  // height tiles are "<Worldspace>_H<size>.<cells>.<x>.<y>", not worldspace maps
				continue;
			logger::debug("[Skylighting] Found cache: {}", name);
			if (worldSpaceCachedMapList.contains(name))
				logger::warn("[Skylighting] Error: {} has multiple maps with same name", name);
			worldSpaceCachedMapList.insert(name);
		}
	}
}

bool Skylighting::LoadWorldspaceCache()
{
	static auto tes = RE::TES::GetSingleton();

	auto worldspace = tes->GetRuntimeData2().worldSpace;
	while (worldspace && worldspace->parentWorld)
		worldspace = worldspace->parentWorld;

	if (!worldspace)
		return false;

	std::string newWorldspaceID = worldspace->GetFormEditorID();

	if (cacheWorldspaceID == newWorldspaceID)
		return true;

	cacheWorldspaceID = newWorldspaceID;

	// The streamed tile belongs to the old worldspace
	ReleaseBentNormalTileStream();

	// need to test again texture suffix ig
	//if (!worldSpaceCachedMapList.contains(newWorldspaceID)) {
	//	logger::info("[Skylighting] No cache found for current worldspace");  //tmp otherwise flooding log
	//	return false;
	//}

	logger::info("[Skylighting] Loading cached texture maps...");

	// TODO: Check if xlodgen Lod exists first before trying to generate
	// Should package all these prebuilt

	// The height map has to come first: every generator below reads it, and generating against a
	// null height map would write out empty maps.
	{
		heightAtlasRange = {};
		EnsureHeightAtlas(newWorldspaceID);  // stitch the generated tiles together if it is missing

		std::filesystem::path path;
		auto result = E_FAIL;
		if (FindAtlas(newWorldspaceID, "_H", path, heightAtlasRange)) {
			result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &HMapSRV);
			if (FAILED(result))
				logger::error("[Skylighting] Failed to load height map {}: {:X}", path.string(), (uint32_t)result);
		} else {
			logger::error("[Skylighting] No height atlas found for {}", newWorldspaceID);
		}

		// The decode the cache gen shaders would need: a 16 bit atlas samples as UNORM, so undo the
		// normalisation as well as the xLODGen offset/scale. A raw float atlas is already in game
		// units. A zero based atlas has had its offset folded out already, and reads relative to
		// settings.cacheAtlasMinHeight rather than absolute world Z.
		DirectX::TexMetadata metadata;
		if (SUCCEEDED(DirectX::GetMetadataFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
			bool is16Bit = metadata.format == DXGI_FORMAT_R16_UNORM;
			HeightMapOffset = (is16Bit && !settings.cacheAtlasZeroBase) ? heightExportOffset / 65535.0f : 0.0f;
			HeightMapScale = is16Bit ? heightExportScale * 65535.0f : 1.0f;
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_A.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &AMapSRV);
		if (FAILED(result)) {
			BuildAtlas(path, "");
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_N.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &NMapSRV);
		if (FAILED(result)) {
			//BuildAtlas(path.stem(), "_n");
			GenerateNormalMap();
		}
	}

	{
		bentNormalAtlasRange = {};
		EnsureBentNormalAtlas(newWorldspaceID);  // stitch the generated tiles together if it is missing

		std::filesystem::path path;
		auto result = E_FAIL;
		if (FindAtlas(newWorldspaceID, "_BN", path, bentNormalAtlasRange)) {
			result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &BNMapSRV);

			// A map that exists but will not load is never regenerated over: it is far more likely
			// to be too large for D3D11 than to be corrupt, and overwriting it would throw away a
			// bake that took hours.
			if (FAILED(result))
				logger::error("[Skylighting] {} exists but failed to load ({:X}); leaving it untouched", path.string(), (uint32_t)result);
		} else {
			GenerateBentNormalMap();  // no tiles to stitch, fall back to the full map pass
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_CO.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &COMapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_CO2.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &CO2MapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_DO.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &DOMapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_DO2.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &DO2MapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_DOB.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &DOMapSRVB);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_DO2B.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &DO2MapSRVB);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	//{
	//	auto path = cachePath / (newWorldspaceID + "_NS.dds");
	//	auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &NSMapSRV);
	//	if (FAILED(result)) {
	//		GenerateNormalStepMap();
	//	}
	//}

	return true;
}

// stop using BN tex size var
void Skylighting::GenerateNormalMap()
{
	if (!HMapSRV) {
		logger::error("[Skylighting] No height map loaded, skipping normal map generation");
		return;
	}

	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexN = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> NComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)BNMapSize.x, (uint)BNMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexN = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexN->CreateSRV(nullptr);
	cacheOutputTexN->CreateUAV(uavDesc);

	NComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "NORMALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav = cacheOutputTexN->uav.get();
	context->CSSetShader(NComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto heightSRV = HMapSRV;
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	auto data = MakeCacheGenCB(float2((float)BNMapSize.x, (float)BNMapSize.y));
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto groups = (BNMapSize.x + 7) / 8;
	context->Dispatch(groups, (BNMapSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	// Save output
	auto outputPath = cachePath / (cacheWorldspaceID + "_N.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexN->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	{
		auto path = cachePath / (cacheWorldspaceID + "_N.dds");
		if (NMapSRV)
			NMapSRV->Release();
		NMapSRV = nullptr;
		DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &NMapSRV));
	}

	NComputeShader.release();
}

/*
// change BNMapSize, BNComputeShader
void Skylighting::GenerateNormalStepMap()
{
	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTex = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTex2 = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> BNComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)BNMapSize.x, (uint)BNMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTex = eastl::make_unique<Texture2D>(desc);
	cacheOutputTex->CreateSRV(nullptr);
	cacheOutputTex->CreateUAV(uavDesc);

	cacheOutputTex2 = eastl::make_unique<Texture2D>(desc);
	cacheOutputTex2->CreateSRV(nullptr);
	cacheOutputTex2->CreateUAV(uavDesc);

	BNComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "NORMAL_STEP", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav[2] = { cacheOutputTex->uav.get(), cacheOutputTex2->uav.get() };
	context->CSSetShader(BNComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 2, uav, nullptr);

	//context->CSSetShaderResources(0, 1, &NMapSRV);
	ID3D11ShaderResourceView* srvs[2] = { COMapSRV, HMapSRV };  //globals::features::terrainShadows.texHeightMap->srv.get() };
	context->CSSetShaderResources(0, 2, srvs);                  // need to make sure this exists before func call

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	auto data = MakeCacheGenCB(float2((float)BNMapSize.x, (float)BNMapSize.y));
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto groups = (BNMapSize.x + 7) / 8;
	context->Dispatch(groups, (BNMapSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	// Save output
	auto outputPath = cachePath / (cacheWorldspaceID + "_NS.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTex->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	{
		auto path = cachePath / (cacheWorldspaceID + "_NS.dds");
		//NSMapSRV->Release();
		NSMapSRV = nullptr;
		DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &NSMapSRV));
	}

	BNComputeShader.release();
}
*/
bool Skylighting::FindAtlas(const std::string& worldspaceID, const std::string& mapTag, std::filesystem::path& o_path, AtlasCellRange& o_range) const
{
	if (worldspaceID.empty())
		return false;

	std::error_code ec;
	if (!std::filesystem::exists(cachePath, ec))
		return false;

	bool found = false;
	for (const auto& entry : std::filesystem::directory_iterator(cachePath, ec)) {
		const auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		AtlasCellRange range;
		if (!ParseAtlasName(path, worldspaceID, mapTag, range))
			continue;

		if (found)  // a rebuild removes the old one, so more than one means the folder was edited by hand
			logger::warn("[Skylighting] Multiple {}{} atlases present, using {}", worldspaceID, mapTag, path.string());

		o_path = path;
		o_range = range;
		found = true;
	}

	return found;
}

float4 Skylighting::GetHeightMapBounds() const
{
	// The cell range the atlas on disk actually covers, read from its file name.
	if (heightAtlasRange.valid)
		return heightAtlasRange.WorldBounds();

	// Legacy full worldspace map: cells -57,-43 to 62,51, the range the old generator covered.
	return float4(-233472.0f, -176128.0f, 253952.0f, 208896.0f);
}

float4 Skylighting::GetBentNormalAtlasBounds() const
{
	if (bentNormalAtlasRange.valid)
		return bentNormalAtlasRange.WorldBounds();

	return GetHeightMapBounds();  // the bent normal tiles mirror the height tiles
}

Skylighting::CacheGenCBStruct Skylighting::MakeCacheGenCB(const float2& outputSize, const float4& regionOffsetScale) const
{
	CacheGenCBStruct data;
	data.TexParams = float4(outputSize.x, outputSize.y, HeightMapOffset, HeightMapScale);
	data.RegionOffsetScale = regionOffsetScale;
	data.GridBounds = GetHeightMapBounds();
	return data;
}

bool Skylighting::DispatchBentNormals(ID3D11ComputeShader* computeShader, Texture2D* outputTex, const float4& regionOffsetScale)
{
	if (!computeShader || !outputTex)
		return false;

	if (!HMapSRV) {
		logger::error("[Skylighting] No height map loaded, cannot generate bent normals");
		return false;
	}

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav = outputTex->uav.get();
	context->CSSetShader(computeShader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto heightSRV = HMapSRV;  //globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	auto data = MakeCacheGenCB(float2((float)outputTex->desc.Width, (float)outputTex->desc.Height), regionOffsetScale);
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->Dispatch((outputTex->desc.Width + 7) / 8, (outputTex->desc.Height + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	return true;
}

void Skylighting::GenerateBentNormalMap()
{
	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexBN = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> BNComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)BNMapSize.x, (uint)BNMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexBN = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexBN->CreateSRV(nullptr);
	cacheOutputTexBN->CreateUAV(uavDesc);

	BNComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "BENT_NORMALS", "" } }, "cs_5_0")));

	// Generate map over the whole height map. Never save an output the dispatch did not write:
	// that would overwrite a good map on disk with the uninitialised target.
	if (!DispatchBentNormals(BNComputeShader.get(), cacheOutputTexBN.get(), float4(0.0f, 0.0f, 1.0f, 1.0f))) {
		logger::error("[Skylighting] Bent normal generation did not run, nothing written");
		BNComputeShader.release();
		return;
	}

	// Save output. This pass covers whatever the height map covers, so it is named with that range
	// and is found by the same lookup as a stitched atlas.
	const float4 bounds = GetHeightMapBounds();
	const int2 minCell = int2((int)std::floor(bounds.x / worldCellSize), (int)std::floor(bounds.y / worldCellSize));
	const int2 maxCell = int2((int)std::floor(bounds.z / worldCellSize) - 1, (int)std::floor(bounds.w / worldCellSize) - 1);
	auto outputPath = MakeAtlasPath(cachePath, cacheWorldspaceID, "_BN", minCell, maxCell);

	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexBN->resource.get(), ouputImage));

	RemoveExistingAtlases(cachePath, cacheWorldspaceID, "_BN");
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	bentNormalAtlasRange.minCell = minCell;
	bentNormalAtlasRange.maxCell = maxCell;
	bentNormalAtlasRange.valid = true;

	{
		if (BNMapSRV)
			BNMapSRV->Release();
		BNMapSRV = nullptr;
		DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, outputPath.c_str(), nullptr, &BNMapSRV));
	}

	BNComputeShader.release();
}

void Skylighting::GenerateCardinalOcclusionMap()
{
	if (!HMapSRV) {
		logger::error("[Skylighting] No height map loaded, skipping cardinal occlusion generation");
		return;
	}

	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexCO = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexCO2 = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexDO = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexDO2 = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexDOB = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexDO2B = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> COComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, COMapSize, COMapSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexCO = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexCO->CreateSRV(nullptr);
	cacheOutputTexCO->CreateUAV(uavDesc);

	cacheOutputTexCO2 = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexCO2->CreateSRV(nullptr);
	cacheOutputTexCO2->CreateUAV(uavDesc);

	cacheOutputTexDO = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexDO->CreateSRV(nullptr);
	cacheOutputTexDO->CreateUAV(uavDesc);

	cacheOutputTexDO2 = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexDO2->CreateSRV(nullptr);
	cacheOutputTexDO2->CreateUAV(uavDesc);

	cacheOutputTexDOB = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexDOB->CreateSRV(nullptr);
	cacheOutputTexDOB->CreateUAV(uavDesc);

	cacheOutputTexDO2B = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexDO2B->CreateSRV(nullptr);
	cacheOutputTexDO2B->CreateUAV(uavDesc);

	COComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "CARDINALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav[] = { cacheOutputTexCO->uav.get(), cacheOutputTexCO2->uav.get(), cacheOutputTexDO->uav.get(), cacheOutputTexDO2->uav.get(), cacheOutputTexDOB->uav.get(), cacheOutputTexDO2B->uav.get() };
	context->CSSetShader(COComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uav), uav, nullptr);

	auto heightSRV = HMapSRV;  //globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	auto data = MakeCacheGenCB(float2((float)COMapSize, (float)COMapSize));
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto groups = (COMapSize + 7) / 8;
	context->Dispatch(groups, groups, 1);

	ID3D11UnorderedAccessView* nullUAVs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 6, nullUAVs, nullptr);

	// Save output
	auto outputPath = cachePath / (cacheWorldspaceID + "_CO.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexCO->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	outputPath = cachePath / (cacheWorldspaceID + "_CO2.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexCO2->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	outputPath = cachePath / (cacheWorldspaceID + "_DO.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexDO->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	outputPath = cachePath / (cacheWorldspaceID + "_DO2.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexDO2->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	outputPath = cachePath / (cacheWorldspaceID + "_DOB.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexDOB->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	outputPath = cachePath / (cacheWorldspaceID + "_DO2B.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexDO2B->resource.get(), ouputImage));
	SaveMapDDS(*ouputImage.GetImages(), outputPath);

	COComputeShader.release();
}

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute,
		&updateSparseGridCS,
		&terrainRelightCS,
	};

	for (auto shader : shaderPtrs)
		shader = nullptr;

	CompileComputeShaders();
}

void Skylighting::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &probeUpdateCompute, "UpdateProbesCS.hlsl", { { "DENSE_PROBE_GRID", "" } } },
			{ &updateSparseGridCS, "UpdateProbesCS.hlsl", { { "SPARSE_PROBE_GRID", "" } } },
			{ &terrainRelightCS, "UpdateProbesCS.hlsl", { { "TERRAIN_RELIGHT", "" } } },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

void Skylighting::UpdateDenseProbeGrid()
{
	auto context = globals::d3d::context;

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Skylighting - Update Dense Probes");

	TracyD3D11Zone(globals::state->tracyCtx, "Skylighting - Update Dense Probes");

	std::array<ID3D11ShaderResourceView*, 1> srvs = { texOcclusion->srv.get() };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { texProbeArray->uav.get(), texAccumFramesArray->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { comparisonSampler.get() };

	// Update probe array
	{
		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(probeUpdateCompute.get(), nullptr, 0);
		context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, probeArrayDims[2]);
	}

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void Skylighting::UpdateTerrainLighting()
{
	auto context = globals::d3d::context;

	//GenerateNormalStepMap();

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Skylighting - Terrain Lighting");

	TracyD3D11Zone(state->tracyCtx, "Skylighting - Terrain Lighting");

	auto uav = terrainLightingTex->uav.get();
	context->CSSetShader(terrainRelightCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11SamplerState* sampArray[3] = { globals::deferred->linearSampler, globals::features::physicalSky.sampNoise.get(), globals::features::physicalSky.sampSv.get() };
	context->CSSetSamplers(0, 1, sampArray);

	ID3D11ShaderResourceView* srvs[] = {
		globals::features::physicalSky.texSvLut->srv.get(),
		HMapSRV,  //globals::features::terrainShadows.texHeightMap->srv.get(),
		BNMapSRV,
		COMapSRV,
		CO2MapSRV,
		AMapSRV,
		NMapSRV,
		globals::features::physicalSky.texTrLut->srv.get(),
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	context->Dispatch((terrainMapSize.x + 7) / 8, (terrainMapSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void Skylighting::UpdateSparseProbeGrid()
{
	auto context = globals::d3d::context;

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Skylighting - Update Sparse Probes");

	TracyD3D11Zone(state->tracyCtx, "Skylighting - Update Sparse Probes");

	auto uav = texSparseProbeArray->uav.get();
	context->CSSetShader(updateSparseGridCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11SamplerState* sampArray[3] = { globals::deferred->linearSampler, globals::features::physicalSky.sampNoise.get(), globals::features::physicalSky.sampSv.get() };
	context->CSSetSamplers(0, 3, sampArray);

	ID3D11Buffer* buffer[2] = { globals::features::physicalSky.cloudBuffer->CB(), globals::features::physicalSky.cloudDebugBuffer->CB() };
	context->CSSetConstantBuffers(0, 2, buffer);

	auto& physSky = globals::features::physicalSky;
	//auto& depthTexture = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11ShaderResourceView* srvs[] = {
		HMapSRV,
		physSky.texSvLut->srv.get(),
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		physSky.cloudBaseSRV.get(),  //t8
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	context->Dispatch((sparseGridSize.x + 7) / 8, (sparseGridSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

Skylighting::SkylightingCB Skylighting::GetCommonBufferData(bool a_inWorld)
{
	if (!a_inWorld)
		return Skylighting::SkylightingCB{};

	if (globals::state->isMapMenuOpen)
		return Skylighting::SkylightingCB{};

	auto tes = RE::TES::GetSingleton();
	auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;

	if (!worldspace)
		return Skylighting::SkylightingCB{};

	static float3 prevCellID = { 0, 0, 0 };

	auto eyePosNI = Util::GetEyePosition();
	auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };

	float3 cellSize = {
		occlusionDistance / probeArrayDims[0],
		occlusionDistance / probeArrayDims[1],
		occlusionDistance * .5f / probeArrayDims[2]
	};
	auto cellID = eyePos / cellSize;
	cellID = { round(cellID.x), round(cellID.y), round(cellID.z) };
	auto cellOrigin = cellID * cellSize;
	float3 cellIDDiff = prevCellID - cellID;
	prevCellID = cellID;

	float2 gridSpan = worldspace->maximumCoords - worldspace->minimumCoords;
	gridSpan = float2(std::abs(gridSpan.x), std::abs(gridSpan.y));

	auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());
	const auto& direction = dirLight->GetWorldDirection();
	float3 lightDir = { -direction.x, -direction.y, -direction.z };
	lightDir.Normalize();

	float4 Basis0;
	float4 Basis1;
	BuildOcclusionBasis(lightDir, Basis0, Basis1, 1.0);

	float3 pos = cellOrigin - eyePos;

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionDir = OcclusionDir,
		.PosOffset = float4(pos.x, pos.y, pos.z, 1.0),
		.ArrayOrigin = {
			((int)cellID.x - probeArrayDims[0] / 2) % probeArrayDims[0],
			((int)cellID.y - probeArrayDims[1] / 2) % probeArrayDims[1],
			((int)cellID.z - probeArrayDims[2] / 2) % probeArrayDims[2], 0 },
		.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z },

		.GridTexSize = sparseGridSize,
		.EnvRadianceTexSize = terrainMapSize,
		.GridBounds = float4(worldspace->minimumCoords.x, worldspace->minimumCoords.y, worldspace->maximumCoords.x, worldspace->maximumCoords.y),
		.InvGridTexSize = 1.0f / float2((float)sparseGridSize.x, (float)sparseGridSize.y),
		.InvEnvRadianceTexSize = 1.0f / float2((float)terrainMapSize.x, (float)terrainMapSize.y),
		.GridMinWorldCorner = worldspace->minimumCoords - float2(eyePos.x, eyePos.y),

		.InvGridSpan = 1.0 / gridSpan,
		.HasCache = worldHasCache,
		.toggleLighting = settings.toggleLighting,
		.toggleTrees = settings.toggleTrees,
		.toggleGrass = settings.toggleGrass,
		.toggleDeferred = settings.toggleDeferred,
		.toggleEffect = settings.toggleEffect,

		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility,
		.SkyInfluence = settings.SkyInfluence,
		.EnvInfluence = settings.EnvInfluence,
		.Basis0 = Basis0,
		.Basis1 = Basis1,

		.BentNormalTileBounds = bnTileWorldBounds,
		.BentNormalAtlasBounds = GetBentNormalAtlasBounds(),
		.HasBentNormalTile = BNTileSRV != nullptr,
		.HasBentNormalAtlas = BNMapSRV != nullptr,
	};
}

void Skylighting::Prepass()
{
	if (globals::state->isMapMenuOpen)
		return;

	bool interior = true;

	if (auto sky = globals::game::sky)
		interior = sky->mode.get() != RE::Sky::Mode::kFull;

	if (interior)
		return;

	worldHasCache = LoadWorldspaceCache();

	UpdateBentNormalTileStream();

	UpdateDenseProbeGrid();

	if (updateTerrainLighting)
		UpdateTerrainLighting();

	if (runSparse)
		UpdateSparseProbeGrid();

	auto context = globals::d3d::context;
	// t50: dense probes, t51: sparse probes, t52: bent normal atlas, t53: streamed bent normal tile
	ID3D11ShaderResourceView* srvs[4] = { texProbeArray->srv.get(), texSparseProbeArray->srv.get(), BNMapSRV, BNTileSRV };
	context->PSSetShaderResources(50, 4, srvs);
	context->CSSetShaderResources(50, 4, srvs);
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1));

	stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D));

	MenuOpenCloseEventHandler::Register();
}

//////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////

RE::BSShaderProperty::RenderPassArray* Skylighting::BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl::thunk(
	RE::BSLightingShaderProperty* property,
	RE::BSGeometry* geometry,
	[[maybe_unused]] uint32_t renderMode,
	[[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
{
	auto& skylighting = globals::features::skylighting;

	auto batch = accumulator->GetRuntimeData().batchRenderer;
	batch->geometryGroups[14]->flags &= ~1;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	using enum RE::BSUtilityShader::Flags;

	auto* precipitationOcclusionMapRenderPassList = &property->occlusionPasses;

	precipitationOcclusionMapRenderPassList->Clear();
	if (skylighting.inOcclusion) {
		if (property->flags.any(kSkinned) && property->flags.none(kTreeAnim))
			return precipitationOcclusionMapRenderPassList;
	} else {
		if (property->flags.any(kSkinned))
			return precipitationOcclusionMapRenderPassList;
	}

	if (skylighting.inOcclusion) {
		if (auto userData = geometry->GetUserData()) {
			RE::BSFadeNode* fadeNode = nullptr;

			RE::NiNode* parent = geometry->parent;
			while (parent && !fadeNode) {
				fadeNode = parent->AsFadeNode();
				parent = parent->parent;
			}

			if (fadeNode) {
				if (auto extraData = fadeNode->GetExtraData("BSX")) {
					auto bsxFlags = (RE::BSXFlags*)extraData;
					auto value = static_cast<int32_t>(bsxFlags->value);

					if (value & (static_cast<int32_t>(RE::BSXFlags::Flag::kRagdoll) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kEditorMarker) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kDynamic) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kAddon) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kNeedsTransformUpdate) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kMagicShaderParticles) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kLights) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kBreakable) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kSearchedBreakable))) {
						return precipitationOcclusionMapRenderPassList;
					}
				}
			}
		}
	}

	bool valid = false;

	if (skylighting.inOcclusion) {
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
	} else {
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
	}

	if (valid) {
		if (geometry->worldBound.radius > 32) {
			stl::enumeration<RE::BSUtilityShader::Flags> technique;
			technique.set(RenderDepth);

			if (property->flags.any(kVertexColors)) {
				technique.set(Vc);
			}

			const auto alphaProperty = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get());
			if (alphaProperty && alphaProperty->GetAlphaTesting()) {
				technique.set(Texture);
				technique.set(AlphaTest);
			}

			if (property->flags.any(kLODObjects, kHDLODObjects)) {
				technique.set(LodObject);
			}

			if (property->flags.any(kTreeAnim)) {
				technique.set(TreeAnim);
			}

			if (property->flags.any(kLODLandscape)) {
				technique.set(LodLandscape);
			}

			precipitationOcclusionMapRenderPassList->EmplacePass(
				globals::game::utilityShader,
				property,
				geometry,
				technique.underlying() + static_cast<uint32_t>(ShaderTechnique::UtilityGeneralStart));
		}
	}

	return precipitationOcclusionMapRenderPassList;
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.frameCount % 4;

		float frustumSize = a_frustum->fTop;
		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum);
}

void Skylighting::SetViewFrustumVR::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.frameCount % 4;

		float frustumSize = a_frustum->fTop;
		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum, a_eyeIndex);
}

void Skylighting::RenderOcclusion()
{
	ZoneScopedS(8);
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;

	if (!shaderCache->IsEnabled()) {
		TracyD3D11Zone(globals::state->tracyCtx, "Precipitation Mask");
		state->BeginPerfEvent("Precipitation Mask");
		Main_Precipitation_RenderOcclusion::func();
		state->EndPerfEvent();
		return;
	}

	if (sky) {
		if (!Util::IsInterior()) {
			static bool doPrecip = false;

			auto precip = sky->precip;

			{
				TracyD3D11Zone(globals::state->tracyCtx, "Precipitation Mask");
				state->BeginPerfEvent("Precipitation Mask");

				doPrecip = false;

				auto precipObject = precip->currentPrecip;
				if (!precipObject) {
					precipObject = precip->lastPrecip;
				}
				if (precipObject) {
					precip->SetupMask();
					auto& effect = precipObject->GetGeometryRuntimeData().shaderProperty;
					auto shaderProp = effect.get();
					auto particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
					auto rain = (RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);

					globals::profiler->BeginPass("Skylighting::PrecipMask");
					precip->RenderMask(rain);
					globals::profiler->EndPass();
				}

				state->EndPerfEvent();
			}

			{
				TracyD3D11Zone(globals::state->tracyCtx, "Skylighting Mask");
				state->BeginPerfEvent("Skylighting Mask");

				if (queuedResetSkylighting)
					ResetSkylighting();

				frameCount++;

				auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				RE::BSGraphics::DepthStencilData precipitationCopy = precipitation;

				precipitation.depthSRV = texOcclusion->srv.get();
				precipitation.texture = texOcclusion->resource.get();
				precipitation.views[0] = texOcclusion->dsv.get();

				static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 401590).address());
				float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

				static RE::NiPoint3& PrecipitationShaderDirection = (*(RE::NiPoint3*)REL::RelocationID(515509, 401648).address());
				RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderDirection;

				inOcclusion = true;
				PrecipitationShaderCubeSize = occlusionDistance;

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				float2 vPoint;
				{
					constexpr float rcpRandMax = 1.f / RAND_MAX;
					static int randSeed = std::rand();
					static uint randFrameCount = 0;

					// r2 sequence
					vPoint = float2(randSeed * rcpRandMax) + (float)randFrameCount * float2(0.245122333753f, 0.430159709002f);
					vPoint.x -= static_cast<unsigned long long>(vPoint.x);
					vPoint.y -= static_cast<unsigned long long>(vPoint.y);

					randFrameCount++;
					if (randFrameCount == 1000) {
						randFrameCount = 0;
						randSeed = std::rand();
					}

					// disc transformation
					vPoint.x = sqrt(vPoint.x * sin(settings.MaxZenith));
					vPoint.y *= 6.28318530718f;

					vPoint = { vPoint.x * cos(vPoint.y), vPoint.x * sin(vPoint.y) };
				}

				float3 PrecipitationShaderDirectionF = -float3{ vPoint.x, vPoint.y, sqrt(1 - vPoint.LengthSquared()) };
				PrecipitationShaderDirectionF.Normalize();

				PrecipitationShaderDirection = { PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z };

				static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> _computeProjection{ REL::RelocationID(25643, 26185) };
				{
					ZoneScopedN("Skylighting - Setup Projection");
					_computeProjection(precip, precip->occlusionData.camera);
					precip->SetupMask();
				}

				BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
				{
					TracyD3D11Zone(state->tracyCtx, "Skylighting - Render Height Map");
					globals::profiler->BeginPass("Skylighting::OcclusionMask");
					precip->RenderMask((RE::BSParticleShaderRainEmitter*)rain);
					globals::profiler->EndPass();
				}
				inOcclusion = false;

				OcclusionDir = -float4{ PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z, 0 };
				OcclusionTransform = ((RE::BSParticleShaderRainEmitter*)rain)->occlusionProjection;

				delete rain;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderDirection = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				{
					ZoneScopedN("Skylighting - Restore Projection");
					_computeProjection(precip, precip->occlusionData.camera);
				}

				state->EndPerfEvent();
			}
		}
	}
}
void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	auto& skylighting = globals::features::skylighting;

	if (!skylighting.MapGen)
		skylighting.RenderOcclusion();

	if (skylighting.MapGen)
		skylighting.GenerateHeightMap();

	skylighting.UpdateBentNormalTiles();
}

RE::BSEventNotifyControl Skylighting::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell through a loadscreen, update every frame until completion
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			globals::features::skylighting.queuedResetSkylighting = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}

struct TileInfo
{
	int cellX, cellY;
	DirectX::ScratchImage image;
};

static bool ParseTile(const std::filesystem::path& path, int& cellX, int& cellY)
{
	std::string stem = path.stem().string();
	// split by '.'
	std::vector<std::string> parts;
	std::string cur;
	for (char c : stem) {
		if (c == '.') {
			parts.push_back(cur);
			cur.clear();
		} else
			cur += c;
	}
	parts.push_back(cur);
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

// needs to support all input texture sizes
// Need to cleanup
void Skylighting::BuildAtlas(const std::filesystem::path& outputPath, std::string mapTag)
{
	std::vector<TileInfo> tiles;

	using namespace DirectX;

	for (auto& entry : std::filesystem::directory_iterator(lodPath)) {
		auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		std::string stem = path.stem().string();
		if (stem.rfind("tamriel.32.", 0) != 0)
			continue;

		if (stem[stem.size() - 2] == '_' && (mapTag.empty() || !stem.ends_with(mapTag)))
			continue;

		//logger::info("Stem: {}  :  INtag: {}", stem, mapTag);
		if (mapTag.empty())
			if (!stem.ends_with(mapTag)) {
				logger::info("Found Tag");
				continue;
			}

		TileInfo ti;
		if (!ParseTile(path, ti.cellX, ti.cellY))
			continue;

		HRESULT hr = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, nullptr, ti.image);
		if (FAILED(hr)) {
			logger::info("Failed load: {}", stem);
			return;
		}

		// Convert to RGBA32 for uniform blitting
		auto img = ti.image.GetImage(0, 0, 0);
		if (img && img->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			ScratchImage converted;
			hr = Convert(*img, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
			if (FAILED(hr)) {
				logger::info("Failed Convert: {}", stem);
				return;
			}
			ti.image = std::move(converted);
		}

		logger::info("Added tile");
		tiles.push_back(std::move(ti));
	}

	logger::info("Tile Count: {}", tiles.size());
	if (tiles.empty())
		return;

	size_t lodTexSize = tiles[0].image.GetImage(0, 0, 0)->width;

	// Sort tiles and assign grid positions
	std::sort(tiles.begin(), tiles.end(), [](const TileInfo& a, const TileInfo& b) {
		return a.cellY != b.cellY ? a.cellY > b.cellY : a.cellX < b.cellX;
	});

	std::vector<int> uniqueX, uniqueY;
	for (auto& t : tiles) {
		if (std::find(uniqueX.begin(), uniqueX.end(), t.cellX) == uniqueX.end())
			uniqueX.push_back(t.cellX);
		if (std::find(uniqueY.begin(), uniqueY.end(), t.cellY) == uniqueY.end())
			uniqueY.push_back(t.cellY);
	}
	std::sort(uniqueX.begin(), uniqueX.end());
	std::sort(uniqueY.begin(), uniqueY.end(), std::greater<int>());  // north-up

	size_t atlasW = lodTexSize * uniqueX.size();
	size_t atlasH = lodTexSize * uniqueY.size();

	logger::info("Atlas Size: {}, {}", atlasW, atlasH);

	ScratchImage atlas;
	HRESULT hr = atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, atlasW, atlasH, 1, 1);
	if (FAILED(hr)) {
		logger::info("Failed Init");
		return;
	}

	// Zero-fill
	const Image* atlasImg = atlas.GetImage(0, 0, 0);
	memset(atlasImg->pixels, 0, atlasImg->slicePitch);

	for (auto& t : tiles) {
		logger::info("Doing tile");
		const Image* src = t.image.GetImage(0, 0, 0);
		auto col = std::find(uniqueX.begin(), uniqueX.end(), t.cellX) - uniqueX.begin();
		auto row = std::find(uniqueY.begin(), uniqueY.end(), t.cellY) - uniqueY.begin();

		size_t dstX = col * lodTexSize;
		size_t dstY = row * lodTexSize;

		for (size_t y = 0; y < lodTexSize; ++y) {
			uint8_t* dst = atlasImg->pixels + (dstY + y) * atlasImg->rowPitch + dstX * 4;
			const uint8_t* s = src->pixels + y * src->rowPitch;
			memcpy(dst, s, lodTexSize * 4);
		}
	}

	logger::info("Finished Atlas");

	SaveMapDDS(*atlasImg, outputPath);
}

// Encode a height in game units into the xLODGen 16 bit unsigned representation:
// zero height is 32767, one step is 8 game units, so height = (encoded - 32767) * 8.
static uint16_t EncodeHeight16(float height)
{
	float encoded = std::round(height / Skylighting::heightExportScale) + Skylighting::heightExportOffset;
	return (uint16_t)std::clamp(encoded, 0.0f, 65535.0f);
}

// Convert one row of sampled heights into the stored representation: either the 16 bit encoding
// above or the raw game units. Shared by the DDS writer and the UI preview so both hold the
// exact same data.
static void ConvertHeightRow(const float* src, uint8_t* dst, uint count, bool export16Bit)
{
	if (export16Bit) {
		auto encoded = (uint16_t*)dst;
		for (uint x = 0; x < count; ++x)
			encoded[x] = EncodeHeight16(src[x]);
	} else {
		memcpy(dst, src, count * sizeof(float));
	}
}

static DXGI_FORMAT HeightStorageFormat(bool export16Bit)
{
	return export16Bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT;
}

// "<Worldspace><mapTag><tileSize>.<cellsPerTile>.<originX>.<originY>.dds", e.g. Tamriel_H1024.8.-64.-40.dds
static std::filesystem::path MakeTilePath(const std::filesystem::path& dir, const std::string& worldspaceID, const std::string& mapTag, uint tileSize, int cellsPerTile, const int2& originCell)
{
	return dir / fmt::format("{}{}{}.{}.{}.{}.dds", worldspaceID, mapTag, tileSize, cellsPerTile, originCell.x, originCell.y);
}

bool Skylighting::EnsureHeightTileTexture(uint tileSize)
{
	if (cacheOutputTexH && cacheOutputTexH->desc.Width == tileSize && cacheOutputTexH->desc.Height == tileSize)
		return true;

	// Sampling keeps full float precision; the encode to 16 bit happens on save.
	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32_FLOAT, tileSize, tileSize, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ);

	cacheOutputTexH = nullptr;
	try {
		cacheOutputTexH = eastl::make_unique<Texture2D>(desc, "Skylighting::HeightCacheTile");
	} catch (const std::exception& e) {
		logger::error("[Skylighting] Failed to create {0}x{0} height tile: {1}", tileSize, e.what());
		return false;
	}

	return cacheOutputTexH != nullptr;
}

void Skylighting::ClearHeightTile()
{
	if (!cacheOutputTexH)
		return;

	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;

	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_WRITE, 0, &mapped);
	if (FAILED(hr) || !mapped.pData) {
		logger::error("[Skylighting] Height tile clear failed to map: {:X}", (uint32_t)hr);
		return;
	}

	// Texels belonging to cells outside the worldspace are never sampled and stay at zero height.
	for (uint y = 0; y < tileSize; ++y)
		memset((uint8_t*)mapped.pData + y * mapped.RowPitch, 0, tileSize * sizeof(float));

	context->Unmap(cacheOutputTexH->resource.get(), 0);
}

bool Skylighting::SaveHeightTile(const int2& tileOriginCell, int cellsPerTile)
{
	if (!cacheOutputTexH)
		return false;

	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;
	const bool export16Bit = settings.cacheExport16Bit;

	auto worldspaceID = cacheWorldspaceID.empty() ? std::string("Unknown") : cacheWorldspaceID;
	auto path = MakeTilePath(cachePath, worldspaceID, "_H", tileSize, cellsPerTile, tileOriginCell);

	DirectX::ScratchImage outputImage;
	HRESULT hr = outputImage.Initialize2D(HeightStorageFormat(export16Bit), tileSize, tileSize, 1, 1);
	if (FAILED(hr)) {
		logger::error("[Skylighting] Failed to allocate height tile image: {:X}", (uint32_t)hr);
		return false;
	}

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr) || !mapped.pData) {
		logger::error("[Skylighting] Failed to map height tile for save: {:X}", (uint32_t)hr);
		return false;
	}

	const DirectX::Image* image = outputImage.GetImages();
	for (uint y = 0; y < tileSize; ++y) {
		auto src = (const float*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
		ConvertHeightRow(src, image->pixels + y * image->rowPitch, tileSize, export16Bit);
	}

	context->Unmap(cacheOutputTexH->resource.get(), 0);

	if (!SaveMapDDS(*image, path))
		return false;

	logger::info("[Skylighting] Saved height tile {}", path.string());
	return true;
}

void Skylighting::UpdateHeightPreview(const int2& tileOriginCell)
{
	if (!cacheOutputTexH)
		return;

	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;
	const bool export16Bit = settings.cacheExport16Bit;
	const DXGI_FORMAT format = HeightStorageFormat(export16Bit);

	// The preview holds exactly what the DDS holds, in the same format, unmodified.
	if (!heightPreviewTex || heightPreviewTex->desc.Width != tileSize || heightPreviewTex->desc.Format != format) {
		CD3D11_TEXTURE2D_DESC desc(format, tileSize, tileSize, 1, 1, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, format, 0, 1);

		heightPreviewValid = false;
		heightPreviewTex = nullptr;
		try {
			heightPreviewTex = eastl::make_unique<Texture2D>(desc, "Skylighting::HeightTilePreview");
			heightPreviewTex->CreateSRV(srvDesc);
		} catch (const std::exception& e) {
			logger::error("[Skylighting] Failed to create height tile preview: {}", e.what());
			heightPreviewTex = nullptr;
			return;
		}
	}

	D3D11_MAPPED_SUBRESOURCE src;
	HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &src);
	if (FAILED(hr) || !src.pData) {
		logger::error("[Skylighting] Failed to map height tile for preview: {:X}", (uint32_t)hr);
		return;
	}

	D3D11_MAPPED_SUBRESOURCE dst;
	hr = context->Map(heightPreviewTex->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst);
	if (FAILED(hr) || !dst.pData) {
		logger::error("[Skylighting] Failed to map height preview: {:X}", (uint32_t)hr);
		context->Unmap(cacheOutputTexH->resource.get(), 0);
		return;
	}

	for (uint y = 0; y < tileSize; ++y) {
		auto srcRow = (const float*)((const uint8_t*)src.pData + y * src.RowPitch);
		ConvertHeightRow(srcRow, (uint8_t*)dst.pData + y * dst.RowPitch, tileSize, export16Bit);
	}

	context->Unmap(heightPreviewTex->resource.get(), 0);
	context->Unmap(cacheOutputTexH->resource.get(), 0);

	heightPreviewOrigin = tileOriginCell;
	heightPreviewValid = true;
}

struct HeightTileFile
{
	std::filesystem::path path;
	int2 originCell;
	int cellsPerTile;
	uint tileSize;
};

// Matches the names MakeTilePath writes: "<Worldspace><mapTag><tileSize>.<cellsPerTile>.<originX>.<originY>".
static bool ParseTileName(const std::filesystem::path& path, const std::string& worldspaceID, const std::string& mapTag, HeightTileFile& o_tile)
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
static void FillImage(const DirectX::Image& image, const float4& unormFill, const float4& floatFill)
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

bool Skylighting::StitchTileAtlas(const std::string& worldspaceID, const std::string& mapTag, const float4& unormFill, const float4& floatFill, TileAtlasResult& o_result, float scale)
{
	using namespace DirectX;

	if (worldspaceID.empty())
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
		if (ParseTileName(path, worldspaceID, mapTag, tile))
			tiles.push_back(std::move(tile));
	}

	if (tiles.empty()) {
		logger::warn("[Skylighting] No {}{} tiles found, cannot build atlas", worldspaceID, mapTag);
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

	int2 minOrigin = tiles[0].originCell;
	int2 maxOrigin = tiles[0].originCell;
	for (const auto& tile : tiles) {
		minOrigin.x = std::min(minOrigin.x, tile.originCell.x);
		minOrigin.y = std::min(minOrigin.y, tile.originCell.y);
		maxOrigin.x = std::max(maxOrigin.x, tile.originCell.x);
		maxOrigin.y = std::max(maxOrigin.y, tile.originCell.y);
	}

	const size_t tilesX = (size_t)((maxOrigin.x - minOrigin.x) / cellsPerTile) + 1;
	const size_t tilesY = (size_t)((maxOrigin.y - minOrigin.y) / cellsPerTile) + 1;

	TexMetadata metadata;
	HRESULT hr = GetMetadataFromDDSFile(tiles[0].path.c_str(), DDS_FLAGS_NONE, metadata);
	if (FAILED(hr)) {
		logger::error("[Skylighting] Failed to read {} tile metadata: {:X}", mapTag, (uint32_t)hr);
		return false;
	}

	const DXGI_FORMAT format = metadata.format;
	const size_t bytesPerTexel = BitsPerPixel(format) / 8;

	// Scaling happens per tile so the intermediate never exceeds one tile, and the scaled size is
	// rounded to a whole number of texels so tiles stay aligned and cannot leave seams.
	const uint outTileSize = (uint)std::clamp((int)std::lround((float)tileSize * std::clamp(scale, 0.0f, 1.0f)), 1, (int)tileSize);
	const size_t atlasWidth = tilesX * outTileSize;
	const size_t atlasHeight = tilesY * outTileSize;

	if (outTileSize != tileSize)
		logger::info("[Skylighting] Scaling {} tiles {} -> {} texels ({:.4f} of original)", mapTag, tileSize, outTileSize, (float)outTileSize / (float)tileSize);

	if (atlasWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || atlasHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		logger::warn("[Skylighting] {} atlas is {}x{}, larger than the {} texel D3D11 limit; the file will be written but cannot be loaded as a texture",
			mapTag, atlasWidth, atlasHeight, (uint)D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);

	logger::info("[Skylighting] Stitching {0} atlas: {1}x{2} texels, {3} MB", mapTag, atlasWidth, atlasHeight,
		(atlasWidth * atlasHeight * bytesPerTexel) / (1024 * 1024));

	ScratchImage atlas;
	hr = atlas.Initialize2D(format, atlasWidth, atlasHeight, 1, 1);
	if (FAILED(hr)) {
		logger::error("[Skylighting] Failed to allocate {}x{} atlas: {:X}", atlasWidth, atlasHeight, (uint32_t)hr);
		return false;
	}

	const Image* atlasImage = atlas.GetImages();
	FillImage(*atlasImage, unormFill, floatFill);

	// The atlas is north up with a top left origin, so both the tile order and each tile's rows are
	// mirrored on the way in: the tiles themselves are stored south to north.
	std::vector<std::pair<int2, std::string>> placements;  // atlas texel origin -> source tile file
	size_t blitted = 0;
	for (const auto& tile : tiles) {
		ScratchImage tileImage;
		hr = LoadFromDDSFile(tile.path.c_str(), DDS_FLAGS_NONE, nullptr, tileImage);
		if (FAILED(hr)) {
			logger::warn("[Skylighting] Skipping unreadable tile {}: {:X}", tile.path.string(), (uint32_t)hr);
			continue;
		}

		const Image* src = tileImage.GetImages();
		if (!src || src->width != tileSize || src->height != tileSize || src->format != format) {
			logger::warn("[Skylighting] Skipping tile {}, does not match the atlas layout", tile.path.string());
			continue;
		}

		ScratchImage scaledImage;
		if (outTileSize != tileSize) {
			hr = Resize(*src, outTileSize, outTileSize, TEX_FILTER_DEFAULT, scaledImage);
			if (FAILED(hr)) {
				logger::error("[Skylighting] Failed to scale tile {}: {:X}", tile.path.string(), (uint32_t)hr);
				return false;
			}
			src = scaledImage.GetImages();
		}

		size_t tileColumn = (size_t)((tile.originCell.x - minOrigin.x) / cellsPerTile);
		size_t tileRow = (size_t)((maxOrigin.y - tile.originCell.y) / cellsPerTile);  // north first
		size_t dstX = tileColumn * outTileSize;
		size_t dstY = tileRow * outTileSize;

		for (uint y = 0; y < outTileSize; ++y) {
			auto dst = atlasImage->pixels + (dstY + y) * atlasImage->rowPitch + dstX * bytesPerTexel;
			memcpy(dst, src->pixels + (outTileSize - 1 - y) * src->rowPitch, outTileSize * bytesPerTexel);
		}

		placements.emplace_back(int2((int)dstX, (int)dstY), tile.path.filename().string());
		blitted++;
	}

	if (blitted == 0) {
		logger::error("[Skylighting] No usable {}{} tiles, atlas not written", worldspaceID, mapTag);
		return false;
	}

	o_result.image = std::move(atlas);
	o_result.placements = std::move(placements);
	o_result.minOriginCell = minOrigin;
	o_result.maxOriginCell = maxOrigin;
	o_result.tileCounts = int2((int)tilesX, (int)tilesY);
	o_result.tileSize = outTileSize;
	o_result.cellsPerTile = cellsPerTile;

	return true;
}

bool Skylighting::EnsureHeightAtlas(const std::string& worldspaceID, bool forceRebuild)
{
	using namespace DirectX;

	if (worldspaceID.empty())
		return false;

	std::filesystem::path existingPath;
	if (!forceRebuild && FindAtlas(worldspaceID, "_H", existingPath, heightAtlasRange))
		return true;

	// Gaps between tiles read as zero height, matching how the tiles clear unsampled texels.
	TileAtlasResult result;
	if (!StitchTileAtlas(worldspaceID, "_H", float4(heightExportOffset / 65535.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f), result))
		return false;

	const Image* atlasImage = result.image.GetImages();
	const DXGI_FORMAT format = atlasImage->format;
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

	// Optionally bias the whole atlas so its lowest point becomes 0.0. The scan covers the gap fill
	// too, so the result is the lowest value the atlas actually stores. Applied uniformly, so terrain
	// stays continuous across tile seams; for 16 bit this also reclaims the unused negative half of
	// the range. settings.cacheAtlasMinHeight records what 0.0 means in game units.
	settings.cacheAtlasMinHeight = 0.0f;
	if (settings.cacheAtlasZeroBase) {
		if (format == DXGI_FORMAT_R16_UNORM) {
			uint16_t minEncoded = UINT16_MAX;
			for (size_t y = 0; y < atlasImage->height; ++y) {
				auto row = (const uint16_t*)(atlasImage->pixels + y * atlasImage->rowPitch);
				for (size_t x = 0; x < atlasImage->width; ++x)
					minEncoded = std::min(minEncoded, row[x]);
			}

			for (size_t y = 0; y < atlasImage->height; ++y) {
				auto row = (uint16_t*)(atlasImage->pixels + y * atlasImage->rowPitch);
				for (size_t x = 0; x < atlasImage->width; ++x)
					row[x] = (uint16_t)(row[x] - minEncoded);
			}

			settings.cacheAtlasMinHeight = ((float)minEncoded - heightExportOffset) * heightExportScale;
		} else {
			float minHeight = FLT_MAX;
			for (size_t y = 0; y < atlasImage->height; ++y) {
				auto row = (const float*)(atlasImage->pixels + y * atlasImage->rowPitch);
				for (size_t x = 0; x < atlasImage->width; ++x)
					minHeight = std::min(minHeight, row[x]);
			}

			for (size_t y = 0; y < atlasImage->height; ++y) {
				auto row = (float*)(atlasImage->pixels + y * atlasImage->rowPitch);
				for (size_t x = 0; x < atlasImage->width; ++x)
					row[x] -= minHeight;
			}

			settings.cacheAtlasMinHeight = minHeight;
		}

		logger::info("[Skylighting] Height atlas biased to zero base, 0.0 is {} game units", settings.cacheAtlasMinHeight);
	}
	globals::state->Save();

	// The name carries the inclusive cell range the atlas covers, so its extent is always readable
	// from the file itself rather than from whatever the settings happen to hold.
	const int2 maxCell = maxOrigin + int2(cellsPerTile - 1, cellsPerTile - 1);
	auto atlasPath = MakeAtlasPath(cachePath, worldspaceID, "_H", minOrigin, maxCell);

	RemoveExistingAtlases(cachePath, worldspaceID, "_H");

	if (!SaveMapDDS(*atlasImage, atlasPath))
		return false;

	heightAtlasRange.minCell = minOrigin;
	heightAtlasRange.maxCell = maxCell;
	heightAtlasRange.valid = true;

	logger::info("[Skylighting] Built height atlas {}: {}x{}, cells {},{} to {},{}",
		atlasPath.string(), atlasImage->width, atlasImage->height,
		minOrigin.x, minOrigin.y, maxCell.x, maxCell.y);

	return true;
}

bool Skylighting::EnsureBentNormalAtlas(const std::string& worldspaceID, bool forceRebuild)
{
	using namespace DirectX;

	if (worldspaceID.empty())
		return false;

	std::filesystem::path existingPath;
	if (!forceRebuild && FindAtlas(worldspaceID, "_BN", existingPath, bentNormalAtlasRange))
		return true;

	// Gaps read as an unoccluded surface: normal straight up, encoded, and full visibility.
	TileAtlasResult result;
	if (!StitchTileAtlas(worldspaceID, "_BN", float4(0.5f, 0.5f, 1.0f, 1.0f), float4(0.5f, 0.5f, 1.0f, 1.0f), result, settings.cacheBentNormalAtlasScale))
		return false;

	const Image* atlasImage = result.image.GetImages();

	const int2 minCell = result.minOriginCell;
	const int2 maxCell = result.maxOriginCell + int2(result.cellsPerTile - 1, result.cellsPerTile - 1);
	auto atlasPath = MakeAtlasPath(cachePath, worldspaceID, "_BN", minCell, maxCell);

	RemoveExistingAtlases(cachePath, worldspaceID, "_BN");

	if (!SaveMapDDS(*atlasImage, atlasPath))
		return false;

	bentNormalAtlasRange.minCell = minCell;
	bentNormalAtlasRange.maxCell = maxCell;
	bentNormalAtlasRange.valid = true;

	logger::info("[Skylighting] Built bent normal atlas {}: {}x{}, cells {},{} to {},{}",
		atlasPath.string(), atlasImage->width, atlasImage->height,
		minCell.x, minCell.y, maxCell.x, maxCell.y);

	return true;
}

void Skylighting::ReleaseBentNormalTileStream()
{
	if (BNTileSRV) {
		BNTileSRV->Release();
		BNTileSRV = nullptr;
	}

	bnTileOriginValid = false;
	bnTileWorldBounds = float4(0, 0, 0, 0);
}

void Skylighting::UpdateBentNormalTileStream()
{
	// Baking teleports the player across the worldspace, which would thrash the stream.
	if (MapGen || bentNormalTileGen)
		return;

	const int cellsPerTile = settings.cacheAtlasTileCells;
	const int tileSize = settings.cacheAtlasTileSize;
	if (cellsPerTile <= 0 || tileSize <= 0 || cacheWorldspaceID.empty())
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;

	auto playerPos = player->GetPosition();
	const int2 playerCell = int2((int)std::floor(playerPos.x / worldCellSize), (int)std::floor(playerPos.y / worldCellSize));
	const int2 originCell = int2(FloorDiv(playerCell.x, cellsPerTile), FloorDiv(playerCell.y, cellsPerTile)) * cellsPerTile;

	// Still inside the resident tile. Tracked even when the load failed, so a missing tile is not
	// retried every frame.
	if (bnTileOriginValid && originCell.x == bnTileOriginCell.x && originCell.y == bnTileOriginCell.y)
		return;

	// Unload before loading so only one tile is ever resident.
	ReleaseBentNormalTileStream();

	bnTileOriginCell = originCell;
	bnTileOriginValid = true;

	auto path = MakeTilePath(cachePath, cacheWorldspaceID, "_BN", (uint)tileSize, cellsPerTile, originCell);
	if (!std::filesystem::exists(path)) {
		logger::debug("[Skylighting] No bent normal tile for cell {}, {}", originCell.x, originCell.y);
		return;
	}

	auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &BNTileSRV);
	if (FAILED(result)) {
		logger::error("[Skylighting] Failed to stream bent normal tile {}: {:X}", path.string(), (uint32_t)result);
		BNTileSRV = nullptr;
		return;
	}

	bnTileWorldBounds = float4(
		(float)originCell.x * worldCellSize,
		(float)originCell.y * worldCellSize,
		(float)(originCell.x + cellsPerTile) * worldCellSize,
		(float)(originCell.y + cellsPerTile) * worldCellSize);

	logger::debug("[Skylighting] Streamed bent normal tile at cell {}, {}", originCell.x, originCell.y);
}

bool Skylighting::DispatchBentNormalSweep(Texture2D* accumTex, const int2& tileOriginAtlasPx)
{
	if (!accumTex || !bentNormalSweepCS || !bentNormalFinalizeCS || !bentNormalHullUAV || !HMapSRV)
		return false;

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	auto context = globals::d3d::context;
	const int tileSize = (int)accumTex->desc.Width;

	const float4 bounds = GetHeightMapBounds();
	const float2 worldPerTexel = float2(
		(bounds.z - bounds.x) / (float)bentNormalAtlasSize.x,
		(bounds.w - bounds.y) / (float)bentNormalAtlasSize.y);

	// Each azimuth adds its wedge to the accumulator, so it starts empty.
	const float clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearUnorderedAccessViewFloat(accumTex->uav.get(), clearValue);

	ID3D11ShaderResourceView* heightSRV = HMapSRV;
	context->CSSetShaderResources(0, 1, &heightSRV);
	context->CSSetShader(bentNormalSweepCS.get(), nullptr, 0);

	ID3D11UnorderedAccessView* uavs[2] = { accumTex->uav.get(), bentNormalHullUAV.get() };
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

		auto data = MakeCacheGenCB(float2((float)tileSize, (float)tileSize));
		data.SweepDir = float4(worldDir.x, worldDir.y, slope, majorStep);
		data.SweepParams = float4((float)firstLine, (float)lineCount, transpose ? 1.0f : 0.0f, stepWorldDist);
		data.SweepRect = float4((float)tileOriginAtlasPx.x, (float)tileOriginAtlasPx.y, (float)tileSize, 0.0f);
		cacheGenBuffer->Update(data);

		context->Dispatch((lineCount + 63) / 64, 1, 1);
	}

	// Resolve the accumulated integral in place
	auto data = MakeCacheGenCB(float2((float)tileSize, (float)tileSize));
	cacheGenBuffer->Update(data);

	ID3D11UnorderedAccessView* resolveUAVs[2] = { accumTex->uav.get(), nullptr };
	context->CSSetShader(bentNormalFinalizeCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 2, resolveUAVs, nullptr);
	context->Dispatch((tileSize + 7) / 8, (tileSize + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	return true;
}

bool Skylighting::StartBentNormalTiles()
{
	if (bentNormalTileGen)
		return false;

	if (cacheWorldspaceID.empty()) {
		logger::error("[Skylighting] No worldspace cache loaded, cannot generate bent normal tiles");
		return false;
	}

	if (!HMapSRV) {
		logger::error("[Skylighting] No height atlas loaded, build it before generating bent normal tiles");
		return false;
	}

	const int tileSize = settings.cacheAtlasTileSize;
	const int cellsPerTile = settings.cacheAtlasTileCells;
	if (tileSize <= 0 || cellsPerTile <= 0) {
		logger::error("[Skylighting] No atlas layout recorded, rebuild the height atlas first");
		return false;
	}

	// The atlas dimensions the height tiles were stitched into; the regions are relative to these.
	std::filesystem::path atlasPath;
	AtlasCellRange atlasRange;
	if (!FindAtlas(cacheWorldspaceID, "_H", atlasPath, atlasRange)) {
		logger::error("[Skylighting] No height atlas found, cannot generate bent normal tiles");
		return false;
	}

	DirectX::TexMetadata metadata;
	if (FAILED(DirectX::GetMetadataFromDDSFile(atlasPath.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
		logger::error("[Skylighting] Failed to read the height atlas, cannot generate bent normal tiles");
		return false;
	}
	bentNormalAtlasSize = int2((int)metadata.width, (int)metadata.height);

	// Mirror the set of height tiles exactly, so every height tile gets a bent normal partner.
	bentNormalTileQueue.clear();
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(cachePath, ec)) {
		const auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		HeightTileFile tile;
		if (ParseTileName(path, cacheWorldspaceID, "_H", tile) && tile.tileSize == (uint)tileSize && tile.cellsPerTile == cellsPerTile)
			bentNormalTileQueue.push_back(tile.originCell);
	}

	if (bentNormalTileQueue.empty()) {
		logger::error("[Skylighting] No height tiles matching the atlas layout, nothing to generate");
		return false;
	}

	std::ranges::sort(bentNormalTileQueue, [](const int2& a, const int2& b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});

	// One output tile, reused for the whole run. The compute shader writes normalised values, so
	// the float target converts cleanly to the 16 bit storage format on save.
	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)tileSize, (uint)tileSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	bentNormalTileTex = nullptr;
	try {
		bentNormalTileTex = eastl::make_unique<Texture2D>(desc, "Skylighting::BentNormalTile");
		bentNormalTileTex->CreateSRV(nullptr);
		bentNormalTileTex->CreateUAV(uavDesc);
	} catch (const std::exception& e) {
		logger::error("[Skylighting] Failed to create bent normal tile target: {}", e.what());
		bentNormalTileTex = nullptr;
		return false;
	}

	bentNormalSweepCS = nullptr;
	bentNormalFinalizeCS = nullptr;
	bentNormalSweepCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "SWEEP", "" } }, "cs_5_0")));
	bentNormalFinalizeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "SWEEP_FINALIZE", "" } }, "cs_5_0")));
	if (!bentNormalSweepCS || !bentNormalFinalizeCS) {
		logger::error("[Skylighting] Failed to compile the bent normal sweep shaders");
		StopBentNormalTiles();
		return false;
	}

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

		bentNormalHullBuffer = nullptr;
		bentNormalHullUAV = nullptr;
		if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, nullptr, bentNormalHullBuffer.put())) ||
			FAILED(globals::d3d::device->CreateUnorderedAccessView(bentNormalHullBuffer.get(), &uavBufferDesc, bentNormalHullUAV.put()))) {
			logger::error("[Skylighting] Failed to create the bent normal hull scratch buffer");
			StopBentNormalTiles();
			return false;
		}

		Util::SetResourceName(bentNormalHullBuffer.get(), "Skylighting::BentNormalHullStack");
		Util::SetResourceName(bentNormalHullUAV.get(), "Skylighting::BentNormalHullStack UAV");
	}

	bentNormalTileIndex = 0;
	bentNormalTileGen = true;

	logger::info("[Skylighting] Generating {0} bent normal tiles at {1}x{1} from a {2}x{3} atlas",
		bentNormalTileQueue.size(), tileSize, bentNormalAtlasSize.x, bentNormalAtlasSize.y);

	return true;
}

void Skylighting::StopBentNormalTiles()
{
	bentNormalTileGen = false;
	bentNormalTileQueue.clear();
	bentNormalTileIndex = 0;
	bentNormalTileTex = nullptr;
	bentNormalSweepCS = nullptr;
	bentNormalFinalizeCS = nullptr;
	bentNormalHullUAV = nullptr;
	bentNormalHullBuffer = nullptr;
}

void Skylighting::UpdateBentNormalTiles()
{
	if (!bentNormalTileGen)
		return;

	// One tile per frame; a whole set in a single call would sit far past the driver timeout.
	if (bentNormalTileIndex < bentNormalTileQueue.size()) {
		GenerateBentNormalTile(bentNormalTileQueue[bentNormalTileIndex]);
		bentNormalTileIndex++;
	}

	if (bentNormalTileIndex >= bentNormalTileQueue.size()) {
		logger::info("[Skylighting] Bent normal tiles complete: {} tiles", bentNormalTileQueue.size());
		StopBentNormalTiles();
	}
}

bool Skylighting::GenerateBentNormalTile(const int2& tileOriginCell)
{
	if (!bentNormalTileTex)
		return false;

	const int tileSize = settings.cacheAtlasTileSize;
	const int cellsPerTile = settings.cacheAtlasTileCells;
	if (tileSize <= 0 || cellsPerTile <= 0 || bentNormalAtlasSize.x <= 0 || bentNormalAtlasSize.y <= 0)
		return false;

	// The tile's north west corner in atlas texels; atlas rows run north first, hence the flip.
	const int tileColumn = (tileOriginCell.x - settings.cacheAtlasMinCellX) / cellsPerTile;
	const int maxOriginCellY = settings.cacheAtlasMinCellY + (settings.cacheAtlasTilesY - 1) * cellsPerTile;
	const int tileRow = (maxOriginCellY - tileOriginCell.y) / cellsPerTile;

	if (!DispatchBentNormalSweep(bentNormalTileTex.get(), int2(tileColumn * tileSize, tileRow * tileSize)))
		return false;

	DirectX::ScratchImage captured;
	HRESULT hr = DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, bentNormalTileTex->resource.get(), captured);
	if (FAILED(hr)) {
		logger::error("[Skylighting] Failed to capture bent normal tile {}, {}: {:X}", tileOriginCell.x, tileOriginCell.y, (uint32_t)hr);
		return false;
	}

	// Same storage convention as the height tiles: 16 bit unsigned, or raw float when that is off.
	// The shader already writes normal * 0.5 + 0.5 and AO, so everything is in range for UNORM.
	DirectX::ScratchImage converted;
	if (settings.cacheExport16Bit) {
		hr = DirectX::Convert(*captured.GetImages(), DXGI_FORMAT_R16G16B16A16_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
		if (FAILED(hr)) {
			logger::error("[Skylighting] Failed to convert bent normal tile {}, {}: {:X}", tileOriginCell.x, tileOriginCell.y, (uint32_t)hr);
			return false;
		}
	}

	const DirectX::Image* image = settings.cacheExport16Bit ? converted.GetImages() : captured.GetImages();
	auto path = MakeTilePath(cachePath, cacheWorldspaceID, "_BN", (uint)tileSize, cellsPerTile, tileOriginCell);

	if (!SaveMapDDS(*image, path))
		return false;

	logger::info("[Skylighting] Saved bent normal tile {}", path.string());
	return true;
}

bool Skylighting::AtlasTexelToCell(const int2& texel, int2& o_cell) const
{
	if (settings.cacheAtlasTileSize <= 0 || settings.cacheAtlasTileCells <= 0 || settings.cacheAtlasTilesY <= 0)
		return false;

	// The atlas is a uniform grid of cells, so the tile boundaries do not need to be walked.
	const int texelsPerCell = settings.cacheAtlasTileSize / settings.cacheAtlasTileCells;
	if (texelsPerCell <= 0)
		return false;

	// Top left origin: texel Y grows southwards, so it counts down from the northernmost cell.
	const int maxCellY = settings.cacheAtlasMinCellY + settings.cacheAtlasTilesY * settings.cacheAtlasTileCells - 1;

	o_cell = int2(settings.cacheAtlasMinCellX + FloorDiv(texel.x, texelsPerCell),
		maxCellY - FloorDiv(texel.y, texelsPerCell));
	return true;
}

// Move one cell at a time, casting a ray every worldRes units within the cell.
//
// The worldspace is covered by a grid of tileSize^2 tiles, each holding cellsPerTile^2 worldspace
// cells (1024 tile with 8x8 cells -> 128 texels/cell at 32 units per texel; 512 tile with 8x8 cells
// -> 64 texels/cell at 64 units per texel). Cells are visited tile by tile and the tile is saved the
// moment its last cell has been sampled, so an interrupted run only ever loses the tile in flight.
//
// Texture and cell progress must not update unless the position was updated correctly.
void Skylighting::GenerateHeightMap()
{
	static constexpr float CELL = worldCellSize;
	static constexpr int2 startCell = int2(-57, -43);  // same as dyndolod
	static constexpr int2 endCell = int2(61, 50);      // same as dyndolod, exclusive

	auto context = globals::d3d::context;
	auto tes = RE::TES::GetSingleton();
	auto player = RE::PlayerCharacter::GetSingleton();
	auto worldSpace = player ? player->GetWorldspace() : nullptr;

	if (!tes || !worldSpace) {
		logger::error("[Skylighting] tes or worldspace INVALID");
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
	static int failedCount = 0;

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
		failedCount = 0;
		settleFrames = 0;

		if (!EnsureHeightTileTexture(tileSize)) {
			MapGen = false;
			return;  // heightGenInit stays set so a retry re-runs the whole setup
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

		if (!beginTile()) {
			logger::error("[Skylighting] No height tiles to generate");
			MapGen = false;
			heightGenInit = true;
			return;
		}

		logger::info("[Skylighting] Generating {0} height cache: {1}x{1} tiles of {2}x{2} cells, {3} texels per cell, {4} units per texel, {5}",
			heightGenSingleTile ? "single tile" : "full", tileSize, cellsPerTile, texelsPerCell, worldRes,
			settings.cacheExport16Bit ? "16 bit unsigned" : "32 bit float");

		SetWorldPosition(currentCellXY, worldPositionSet);
		settleFrames = heightSettleFrames;
		heightGenInit = false;
		return;
	}

	if (!cacheOutputTexH) {
		logger::error("[Skylighting] cacheOutputTexH INVALID");
		MapGen = false;
		heightGenInit = true;
		return;
	}

	bool valid = IsPositionValid(worldPositionSet);
	failedCount = valid ? 0 : ++failedCount;
	if (!valid) {
		if (failedCount >= 10) {  // This should never happen but since its possible for the game to refuse an update we should handle it anyway.
			logger::error("[Skylighting] Sample position was unable to be updated");
			failedCount = 0;
		} else {
			//player->SetPosition(worldPositionSet, false);
			SetWorldPosition(currentCellXY, worldPositionSet);
		}
		return;
	}

	if (settleFrames > 0) {
		settleFrames--;
		return;
	}

	if (!test2) {
		// write heightmap

		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
		if (FAILED(hr) || !mapped.pData) {
			logger::error("[Skylighting] Map failed: {:x}", (uint32_t)hr);
			return;  // skip this frame, don't deref null
		}

		const int2 localCell = currentCellXY - currentTile * cellsPerTile;
		const float2 cellOrigin = float2((float)currentCellXY.x, (float)currentCellXY.y) * CELL;
		const float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), player->GetParentCell());

		for (int x = 0; x < texelsPerCell; ++x) {
			for (int y = 0; y < texelsPerCell; ++y) {
				float2 worldXY = cellOrigin + float2((float)x, (float)y) * worldRes;

				float landHeight;
				tes->GetLandHeight(RE::NiPoint3(worldXY.x, worldXY.y, 0), landHeight);

				float groundHeight = 1000;
				if (!test)
					groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 5000);

				groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

				int2 texCoord = localCell * texelsPerCell + int2(x, y);
				//texCoord.y = ((int)tileSize - 1) - texCoord.y;
				float* tex = (float*)((uint8_t*)mapped.pData + texCoord.y * mapped.RowPitch);
				tex[texCoord.x] = groundHeight;  // write to tex
			}
		}
		context->Unmap(cacheOutputTexH->resource.get(), 0);
	}

	cellsDone += 1;

	if (!advanceToNextCell()) {
		// Tile complete: flush it to disk before moving on.
		if (!test3)
			SaveHeightTile(currentTile * cellsPerTile, cellsPerTile);
		UpdateHeightPreview(currentTile * cellsPerTile);
		tilesDone += 1;

		if (heightGenSingleTile) {  // one-off, leaves the full run's progress alone
			logger::info("[Skylighting] Single height tile complete: {} cells", cellsDone);
			MapGen = false;
			heightGenInit = true;
			return;
		}

		if (!advanceToNextTile() || !beginTile()) {
			logger::info("[Skylighting] Height cache complete: {} tiles, {} cells", tilesDone, cellsDone);
			settings.cacheProgressX = startCell.x;
			settings.cacheProgressY = startCell.y;
			globals::state->Save();
			MapGen = false;
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

bool Skylighting::IsPositionValid(RE::NiPoint3 inputPosition)
{
	static constexpr float HALF_CELL = 2048.0f;
	static constexpr float CELL = worldCellSize;

	bool valid = false;
	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		auto diff = player->GetPosition() - inputPosition;
		valid = std::max(diff.x, diff.y) < CELL;
		logger::trace("diff: {}, {}", diff.x, diff.y);
		logger::trace("Pos: {}  :  InPos: {}", player->GetPosition(), inputPosition);
	}

	return valid;
}

void Skylighting::SetWorldPosition(const int2& currentCellXY, RE::NiPoint3& worldPos)
{
	static constexpr float CELL = worldCellSize;

	auto tes = RE::TES::GetSingleton();
	auto player = RE::PlayerCharacter::GetSingleton();

	//float2 worldXY = float2(minWorldCoords.x, minWorldCoords.y) + float2((float)currentCellXY.x, (float)currentCellXY.y) * CELL;
	float2 worldXY = float2((float)currentCellXY.x, (float)currentCellXY.y) * CELL;

	float landHeight;
	tes->GetLandHeight(RE::NiPoint3(worldXY.x, worldXY.y, 0), landHeight);
	logger::trace("land: {}", landHeight);

	float groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 15000);
	logger::trace("ground: {}", groundHeight);
	float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), player->GetParentCell());
	groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

	float3 sampleCoordsWS = float3(worldXY.x, worldXY.y, groundHeight);
	logger::trace("sampleCoordsWS: {}, {}, {}", sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z);

	worldPos = RE::NiPoint3(sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z + 1500.0f);  // place character in air to avoid crap happening
	player->SetPosition(worldPos, false);
}

float Skylighting::GetRayIntersectionHeight(float3 position, float RAY_OFFSET)
{
	//static constexpr float RAY_OFFSET = 20000.0f;
	static constexpr int MAX_ATTEMPTS = 10;

	static float prevZ = 0.0f;
	auto player = RE::PlayerCharacter::GetSingleton();
	auto cell = player->GetParentCell();
	auto bhkWorld = cell ? cell->GetbhkWorld() : nullptr;

	if (auto hkpWorld = bhkWorld ? cell->GetbhkWorld()->GetWorld1() : nullptr; hkpWorld) {
		float scale = RE::bhkWorld::GetWorldScale();
		float2 posScaledXY = float2(position.x * scale, position.y * scale);
		float currentZ = position.z + RAY_OFFSET;
		float endZ = position.z - RAY_OFFSET;

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

			if (!output.HasHit()) {
				logger::error("[Skylighting] Ray cast failed to find surface... continuing");
				return prevZ;
			}

			auto rootCollidable = output.rootCollidable;
			if (!rootCollidable) {
				logger::error("[Skylighting] Null Root collidable... continuing");
				return prevZ;
			}

			auto collisionObj = rootCollidable->GetCollisionLayer();

			if (!(collisionObj == RE::COL_LAYER::kTerrain || collisionObj == RE::COL_LAYER::kGround || collisionObj == RE::COL_LAYER::kStatic)) {
				float rayLength = currentZ - endZ;
				currentZ = currentZ - output.hitFraction * rayLength - (50.0f * scale);
				continue;
			}

			if (i + 1 == MAX_ATTEMPTS) {
				logger::error("[Skylighting] Ray cast had no valid hit; last recorded collision was: {} ... continuing", collisionObj);
				return prevZ;
			}

			float rayLength = currentZ - endZ;
			float hitZ = currentZ - output.hitFraction * rayLength;
			prevZ = hitZ;
			return hitZ;
		}
	}

	return prevZ;
}

float Skylighting::SampleHeightMap(float2 coords)
{
	static DirectX::ScratchImage image;
	static bool init = true;
	if (init) {
		std::filesystem::path atlasPath;
		AtlasCellRange atlasRange;
		if (FindAtlas(cacheWorldspaceID, "_H", atlasPath, atlasRange))
			DirectX::LoadFromDDSFile(atlasPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		init = false;
	}

	auto& cachedHeightmap = globals::features::terrainShadows.cachedHeightmap;
	float u = (coords.x - cachedHeightmap->pos0.x) / (cachedHeightmap->pos1.x - cachedHeightmap->pos0.x);
	float v = (coords.y - cachedHeightmap->pos0.y) / (cachedHeightmap->pos1.y - cachedHeightmap->pos0.y);
	//v = 1.0f - v;
	auto& img = *image.GetImages();
	int ix = std::clamp((int)(u * img.width), 0, (int)img.width - 1);
	int iy = std::clamp((int)(v * img.height), 0, (int)img.height - 1);
	auto row = reinterpret_cast<const float*>(img.pixels + iy * img.rowPitch);
	float normalizedHeight = row[ix];
	logger::trace("height: {}", normalizedHeight);

	return normalizedHeight;  //(normalizedHeight - 32767) * 8.0f;
}

#undef I18N_KEY_PREFIX
