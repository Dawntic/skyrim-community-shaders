#include "TexGen.h"

#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "State.h"
#include "Utils/D3D.h"

#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "feature.texgen."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TexGen::Settings,
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
	cacheBentNormalAtlasScale,
	dynDOLODPath)

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
		logger::error("[TexGen] Failed to save {}: {:X}", path.string(), (uint32_t)hr);
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

static bool ParseAtlasName(const std::filesystem::path& path, const std::string& worldspaceID, const std::string& mapTag, TexGen::AtlasCellRange& o_range)
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

		TexGen::AtlasCellRange range;
		if (!ParseAtlasName(path, worldspaceID, mapTag, range))
			continue;

		if (std::filesystem::remove(path, ec))
			logger::info("[TexGen] Replaced previous atlas {}", path.string());
	}
}

// Encode a height in game units into the xLODGen 16 bit unsigned representation:
// zero height is 32767, one step is 8 game units, so height = (encoded - 32767) * 8.
static uint16_t EncodeHeight16(float height)
{
	float encoded = std::round(height / TexGen::heightExportScale) + TexGen::heightExportOffset;
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

struct HeightTileFile
{
	std::filesystem::path path;
	int2 originCell;
	int cellsPerTile;
	uint tileSize;
};

// Matches the names GetTilePath writes: "<Worldspace><mapTag><tileSize>.<cellsPerTile>.<originX>.<originY>".
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

std::string TexGen::GetCurrentWorldspaceID()
{
	auto tes = RE::TES::GetSingleton();
	auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	while (worldspace && worldspace->parentWorld)
		worldspace = worldspace->parentWorld;

	return worldspace ? std::string(worldspace->GetFormEditorID()) : std::string();
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
	// Skylighting owns the loaded copies of everything generated here, so it has to pick the new
	// files up. Reloading happens on its next frame rather than inline, which keeps generation free
	// of any knowledge of how the maps are bound.
	if (globals::features::skylighting.loaded)
		globals::features::skylighting.InvalidateCacheMaps();
}

void TexGen::EnsureCacheGenBuffer()
{
	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());
}

//////////////////////////////////////////////////////////////////////////////////
//// Cache layout helpers
//////////////////////////////////////////////////////////////////////////////////

int2 TexGen::WorldToCell(float a_worldX, float a_worldY)
{
	return int2((int)std::floor(a_worldX / worldCellSize), (int)std::floor(a_worldY / worldCellSize));
}

int2 TexGen::GetTileOriginCell(const int2& a_cell, int a_cellsPerTile)
{
	if (a_cellsPerTile <= 0)
		return a_cell;

	return int2(FloorDiv(a_cell.x, a_cellsPerTile), FloorDiv(a_cell.y, a_cellsPerTile)) * a_cellsPerTile;
}

std::filesystem::path TexGen::GetTilePath(const std::string& a_worldspaceID, const std::string& a_mapTag, uint a_tileSize, int a_cellsPerTile, const int2& a_originCell)
{
	return cachePath / fmt::format("{}{}{}.{}.{}.{}.dds", a_worldspaceID, a_mapTag, a_tileSize, a_cellsPerTile, a_originCell.x, a_originCell.y);
}

bool TexGen::FindAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, std::filesystem::path& o_path, AtlasCellRange& o_range) const
{
	if (a_worldspaceID.empty())
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
		if (!ParseAtlasName(path, a_worldspaceID, a_mapTag, range))
			continue;

		if (found)  // a rebuild removes the old one, so more than one means the folder was edited by hand
			logger::warn("[TexGen] Multiple {}{} atlases present, using {}", a_worldspaceID, a_mapTag, path.string());

		o_path = path;
		o_range = range;
		found = true;
	}

	return found;
}

float4 TexGen::GetHeightMapBounds() const
{
	// The cell range the atlas on disk actually covers, read from its file name.
	if (heightAtlasRange.valid)
		return heightAtlasRange.WorldBounds();

	// Legacy full worldspace map: cells -57,-43 to 62,51, the range the old generator covered.
	return float4(-233472.0f, -176128.0f, 253952.0f, 208896.0f);
}

float4 TexGen::GetBentNormalAtlasBounds() const
{
	if (bentNormalAtlasRange.valid)
		return bentNormalAtlasRange.WorldBounds();

	return GetHeightMapBounds();  // the bent normal tiles mirror the height tiles
}

TexGen::CacheGenCBStruct TexGen::MakeCacheGenCB(const float2& outputSize, const float4& regionOffsetScale) const
{
	CacheGenCBStruct data;
	data.TexParams = float4(outputSize.x, outputSize.y, heightMapOffset, heightMapScale);
	data.RegionOffsetScale = regionOffsetScale;
	data.GridBounds = GetHeightMapBounds();
	return data;
}

bool TexGen::ResolveHeightAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild)
{
	heightAtlasRange = {};
	EnsureHeightAtlas(a_worldspaceID, a_forceRebuild);  // stitch the generated tiles together if it is missing

	if (!FindAtlas(a_worldspaceID, "_H", o_path, heightAtlasRange))
		return false;

	// The decode the cache gen shaders need: a 16 bit atlas samples as UNORM, so undo the
	// normalisation as well as the xLODGen offset/scale. A raw float atlas is already in game units.
	// A zero based atlas has had its offset folded out already, and reads relative to
	// settings.cacheAtlasMinHeight rather than absolute world Z.
	DirectX::TexMetadata metadata;
	if (SUCCEEDED(DirectX::GetMetadataFromDDSFile(o_path.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
		bool is16Bit = metadata.format == DXGI_FORMAT_R16_UNORM;
		heightMapOffset = (is16Bit && !settings.cacheAtlasZeroBase) ? heightExportOffset / 65535.0f : 0.0f;
		heightMapScale = is16Bit ? heightExportScale * 65535.0f : 1.0f;
	}

	return true;
}

bool TexGen::ResolveBentNormalAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild)
{
	bentNormalAtlasRange = {};
	EnsureBentNormalAtlas(a_worldspaceID, a_forceRebuild);  // stitch the generated tiles together if it is missing

	return FindAtlas(a_worldspaceID, "_BN", o_path, bentNormalAtlasRange);
}

//////////////////////////////////////////////////////////////////////////////////
//// GPU generators
//////////////////////////////////////////////////////////////////////////////////

bool TexGen::GenerateNormalMap()
{
	UpdateWorldspaceID();  // callable straight from a consumer's load path, before Prepass has run

	if (!heightMapSRV) {
		logger::error("[TexGen] No height map loaded, skipping normal map generation");
		return false;
	}

	if (worldspaceID.empty()) {
		logger::error("[TexGen] No worldspace known, skipping normal map generation");
		return false;
	}

	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexN = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> NComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)BNMapSize.x, (uint)BNMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexN = eastl::make_unique<Texture2D>(desc, "TexGen::NormalMap");
	cacheOutputTexN->CreateSRV(nullptr);
	cacheOutputTexN->CreateUAV(uavDesc);

	NComputeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "NORMALS", "" } }, "cs_5_0")));
	if (!NComputeShader) {
		logger::error("[TexGen] Failed to compile the normal map shader");
		return false;
	}

	EnsureCacheGenBuffer();

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav = cacheOutputTexN->uav.get();
	context->CSSetShader(NComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto heightSRV = heightMapSRV;
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
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	// Save output
	auto outputPath = cachePath / (worldspaceID + "_N.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexN->resource.get(), ouputImage));
	if (!SaveMapDDS(*ouputImage.GetImages(), outputPath))
		return false;

	NotifyCacheMapsChanged();
	return true;
}

bool TexGen::DispatchBentNormals(ID3D11ComputeShader* a_computeShader, Texture2D* a_outputTex, const float4& regionOffsetScale)
{
	if (!a_computeShader || !a_outputTex)
		return false;

	if (!heightMapSRV) {
		logger::error("[TexGen] No height map loaded, cannot generate bent normals");
		return false;
	}

	EnsureCacheGenBuffer();

	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav = a_outputTex->uav.get();
	context->CSSetShader(a_computeShader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto heightSRV = heightMapSRV;
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	auto data = MakeCacheGenCB(float2((float)a_outputTex->desc.Width, (float)a_outputTex->desc.Height), regionOffsetScale);
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->Dispatch((a_outputTex->desc.Width + 7) / 8, (a_outputTex->desc.Height + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	return true;
}

bool TexGen::GenerateBentNormalMap()
{
	UpdateWorldspaceID();

	if (worldspaceID.empty()) {
		logger::error("[TexGen] No worldspace known, skipping bent normal generation");
		return false;
	}

	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexBN = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> BNComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, (uint)BNMapSize.x, (uint)BNMapSize.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexBN = eastl::make_unique<Texture2D>(desc, "TexGen::BentNormalMap");
	cacheOutputTexBN->CreateSRV(nullptr);
	cacheOutputTexBN->CreateUAV(uavDesc);

	BNComputeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "BENT_NORMALS", "" } }, "cs_5_0")));

	// Generate map over the whole height map. Never save an output the dispatch did not write:
	// that would overwrite a good map on disk with the uninitialised target.
	if (!DispatchBentNormals(BNComputeShader.get(), cacheOutputTexBN.get(), float4(0.0f, 0.0f, 1.0f, 1.0f))) {
		logger::error("[TexGen] Bent normal generation did not run, nothing written");
		return false;
	}

	// Save output. This pass covers whatever the height map covers, so it is named with that range
	// and is found by the same lookup as a stitched atlas.
	const float4 bounds = GetHeightMapBounds();
	const int2 minCell = int2((int)std::floor(bounds.x / worldCellSize), (int)std::floor(bounds.y / worldCellSize));
	const int2 maxCell = int2((int)std::floor(bounds.z / worldCellSize) - 1, (int)std::floor(bounds.w / worldCellSize) - 1);
	auto outputPath = MakeAtlasPath(cachePath, worldspaceID, "_BN", minCell, maxCell);

	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexBN->resource.get(), ouputImage));

	RemoveExistingAtlases(cachePath, worldspaceID, "_BN");
	if (!SaveMapDDS(*ouputImage.GetImages(), outputPath))
		return false;

	bentNormalAtlasRange.minCell = minCell;
	bentNormalAtlasRange.maxCell = maxCell;
	bentNormalAtlasRange.valid = true;

	NotifyCacheMapsChanged();
	return true;
}

bool TexGen::GenerateCardinalOcclusionMap()
{
	UpdateWorldspaceID();

	if (!heightMapSRV) {
		logger::error("[TexGen] No height map loaded, skipping cardinal occlusion generation");
		return false;
	}

	if (worldspaceID.empty()) {
		logger::error("[TexGen] No worldspace known, skipping cardinal occlusion generation");
		return false;
	}

	// Setup resources. Six outputs, all the same shape: cardinal, diagonal and the second set the
	// shader writes for the wider horizon search.
	static constexpr std::array<const char*, 6> outputTags = { "_CO", "_CO2", "_DO", "_DO2", "_DOB", "_DO2B" };

	std::array<eastl::unique_ptr<Texture2D>, 6> outputs;
	winrt::com_ptr<ID3D11ComputeShader> COComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, COMapSize, COMapSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	for (size_t i = 0; i < outputs.size(); ++i) {
		const auto name = std::format("TexGen::Occlusion{}", outputTags[i]);
		outputs[i] = eastl::make_unique<Texture2D>(desc, name.c_str());
		outputs[i]->CreateSRV(nullptr);
		outputs[i]->CreateUAV(uavDesc);
	}

	COComputeShader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "CARDINALS", "" } }, "cs_5_0")));
	if (!COComputeShader) {
		logger::error("[TexGen] Failed to compile the occlusion shader");
		return false;
	}

	EnsureCacheGenBuffer();

	// Generate maps
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav[6];
	for (size_t i = 0; i < outputs.size(); ++i)
		uav[i] = outputs[i]->uav.get();

	context->CSSetShader(COComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uav), uav, nullptr);

	auto heightSRV = heightMapSRV;
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
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	// Save output
	bool saved = true;
	DirectX::ScratchImage ouputImage;
	for (size_t i = 0; i < outputs.size(); ++i) {
		DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, outputs[i]->resource.get(), ouputImage));
		saved = SaveMapDDS(*ouputImage.GetImages(), cachePath / (worldspaceID + outputTags[i] + ".dds")) && saved;
	}

	if (saved)
		NotifyCacheMapsChanged();

	return saved;
}

//////////////////////////////////////////////////////////////////////////////////
//// xLODGen albedo atlas
//////////////////////////////////////////////////////////////////////////////////

struct TileInfo
{
	int cellX, cellY;
	DirectX::ScratchImage image;
};

static bool ParseLODTile(const std::filesystem::path& path, int& cellX, int& cellY)
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

// needs to support all input texture sizes
bool TexGen::BuildLODAtlas(const std::filesystem::path& a_outputPath, const std::string& a_mapTag)
{
	std::vector<TileInfo> tiles;

	using namespace DirectX;

	const std::filesystem::path lodPath = settings.dynDOLODPath;
	std::error_code ec;
	if (!std::filesystem::exists(lodPath, ec)) {
		logger::error("[TexGen] xLODGen output folder {} not found, cannot build {}", lodPath.string(), a_outputPath.filename().string());
		return false;
	}

	for (auto& entry : std::filesystem::directory_iterator(lodPath, ec)) {
		auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		std::string stem = path.stem().string();
		if (stem.rfind("tamriel.32.", 0) != 0)
			continue;

		if (stem[stem.size() - 2] == '_' && (a_mapTag.empty() || !stem.ends_with(a_mapTag)))
			continue;

		if (a_mapTag.empty())
			if (!stem.ends_with(a_mapTag)) {
				logger::info("[TexGen] Found Tag");
				continue;
			}

		TileInfo ti;
		if (!ParseLODTile(path, ti.cellX, ti.cellY))
			continue;

		HRESULT hr = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, nullptr, ti.image);
		if (FAILED(hr)) {
			logger::error("[TexGen] Failed load: {}", stem);
			return false;
		}

		// Convert to RGBA32 for uniform blitting
		auto img = ti.image.GetImage(0, 0, 0);
		if (img && img->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			ScratchImage converted;
			hr = Convert(*img, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
			if (FAILED(hr)) {
				logger::error("[TexGen] Failed Convert: {}", stem);
				return false;
			}
			ti.image = std::move(converted);
		}

		tiles.push_back(std::move(ti));
	}

	logger::info("[TexGen] LOD tile count: {}", tiles.size());
	if (tiles.empty())
		return false;

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

	logger::info("[TexGen] LOD atlas size: {}, {}", atlasW, atlasH);

	ScratchImage atlas;
	HRESULT hr = atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, atlasW, atlasH, 1, 1);
	if (FAILED(hr)) {
		logger::error("[TexGen] Failed to allocate the {}x{} LOD atlas: {:X}", atlasW, atlasH, (uint32_t)hr);
		return false;
	}

	// Zero-fill
	const Image* atlasImg = atlas.GetImage(0, 0, 0);
	memset(atlasImg->pixels, 0, atlasImg->slicePitch);

	for (auto& t : tiles) {
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

	if (!SaveMapDDS(*atlasImg, a_outputPath))
		return false;

	logger::info("[TexGen] Built LOD atlas {}", a_outputPath.string());

	NotifyCacheMapsChanged();
	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// Tile stitching
//////////////////////////////////////////////////////////////////////////////////

bool TexGen::StitchTileAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, const float4& unormFill, const float4& floatFill, TileAtlasResult& o_result, float scale)
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
		logger::error("[TexGen] Failed to read {} tile metadata: {:X}", a_mapTag, (uint32_t)hr);
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
		logger::info("[TexGen] Scaling {} tiles {} -> {} texels ({:.4f} of original)", a_mapTag, tileSize, outTileSize, (float)outTileSize / (float)tileSize);

	if (atlasWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || atlasHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
		logger::warn("[TexGen] {} atlas is {}x{}, larger than the {} texel D3D11 limit; the file will be written but cannot be loaded as a texture",
			a_mapTag, atlasWidth, atlasHeight, (uint)D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);

	logger::info("[TexGen] Stitching {0} atlas: {1}x{2} texels, {3} MB", a_mapTag, atlasWidth, atlasHeight,
		(atlasWidth * atlasHeight * bytesPerTexel) / (1024 * 1024));

	ScratchImage atlas;
	hr = atlas.Initialize2D(format, atlasWidth, atlasHeight, 1, 1);
	if (FAILED(hr)) {
		logger::error("[TexGen] Failed to allocate {}x{} atlas: {:X}", atlasWidth, atlasHeight, (uint32_t)hr);
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
			logger::warn("[TexGen] Skipping unreadable tile {}: {:X}", tile.path.string(), (uint32_t)hr);
			continue;
		}

		const Image* src = tileImage.GetImages();
		if (!src || src->width != tileSize || src->height != tileSize || src->format != format) {
			logger::warn("[TexGen] Skipping tile {}, does not match the atlas layout", tile.path.string());
			continue;
		}

		ScratchImage scaledImage;
		if (outTileSize != tileSize) {
			hr = Resize(*src, outTileSize, outTileSize, TEX_FILTER_DEFAULT, scaledImage);
			if (FAILED(hr)) {
				logger::error("[TexGen] Failed to scale tile {}: {:X}", tile.path.string(), (uint32_t)hr);
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
		logger::error("[TexGen] No usable {}{} tiles, atlas not written", a_worldspaceID, a_mapTag);
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

bool TexGen::EnsureHeightAtlas(const std::string& a_worldspaceID, bool a_forceRebuild)
{
	using namespace DirectX;

	if (a_worldspaceID.empty())
		return false;

	std::filesystem::path existingPath;
	if (!a_forceRebuild && FindAtlas(a_worldspaceID, "_H", existingPath, heightAtlasRange))
		return true;

	// Gaps between tiles read as zero height, matching how the tiles clear unsampled texels.
	TileAtlasResult result;
	if (!StitchTileAtlas(a_worldspaceID, "_H", float4(heightExportOffset / 65535.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f), result))
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

		logger::info("[TexGen] Height atlas biased to zero base, 0.0 is {} game units", settings.cacheAtlasMinHeight);
	}
	globals::state->Save();

	// The name carries the inclusive cell range the atlas covers, so its extent is always readable
	// from the file itself rather than from whatever the settings happen to hold.
	const int2 maxCell = maxOrigin + int2(cellsPerTile - 1, cellsPerTile - 1);
	auto atlasPath = MakeAtlasPath(cachePath, a_worldspaceID, "_H", minOrigin, maxCell);

	RemoveExistingAtlases(cachePath, a_worldspaceID, "_H");

	if (!SaveMapDDS(*atlasImage, atlasPath))
		return false;

	heightAtlasRange.minCell = minOrigin;
	heightAtlasRange.maxCell = maxCell;
	heightAtlasRange.valid = true;

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

	std::filesystem::path existingPath;
	if (!a_forceRebuild && FindAtlas(a_worldspaceID, "_BN", existingPath, bentNormalAtlasRange))
		return true;

	// Gaps read as an unoccluded surface: normal straight up, encoded, and full visibility.
	TileAtlasResult result;
	if (!StitchTileAtlas(a_worldspaceID, "_BN", float4(0.5f, 0.5f, 1.0f, 1.0f), float4(0.5f, 0.5f, 1.0f, 1.0f), result, settings.cacheBentNormalAtlasScale))
		return false;

	const Image* atlasImage = result.image.GetImages();

	const int2 minCell = result.minOriginCell;
	const int2 maxCell = result.maxOriginCell + int2(result.cellsPerTile - 1, result.cellsPerTile - 1);
	auto atlasPath = MakeAtlasPath(cachePath, a_worldspaceID, "_BN", minCell, maxCell);

	RemoveExistingAtlases(cachePath, a_worldspaceID, "_BN");

	if (!SaveMapDDS(*atlasImage, atlasPath))
		return false;

	bentNormalAtlasRange.minCell = minCell;
	bentNormalAtlasRange.maxCell = maxCell;
	bentNormalAtlasRange.valid = true;

	logger::info("[TexGen] Built bent normal atlas {}: {}x{}, cells {},{} to {},{}",
		atlasPath.string(), atlasImage->width, atlasImage->height,
		minCell.x, minCell.y, maxCell.x, maxCell.y);

	return true;
}

bool TexGen::AtlasTexelToCell(const int2& a_texel, int2& o_cell) const
{
	if (settings.cacheAtlasTileSize <= 0 || settings.cacheAtlasTileCells <= 0 || settings.cacheAtlasTilesY <= 0)
		return false;

	// The atlas is a uniform grid of cells, so the tile boundaries do not need to be walked.
	const int texelsPerCell = settings.cacheAtlasTileSize / settings.cacheAtlasTileCells;
	if (texelsPerCell <= 0)
		return false;

	// Top left origin: texel Y grows southwards, so it counts down from the northernmost cell.
	const int maxCellY = settings.cacheAtlasMinCellY + settings.cacheAtlasTilesY * settings.cacheAtlasTileCells - 1;

	o_cell = int2(settings.cacheAtlasMinCellX + FloorDiv(a_texel.x, texelsPerCell),
		maxCellY - FloorDiv(a_texel.y, texelsPerCell));
	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// Height cache tiles
//////////////////////////////////////////////////////////////////////////////////

bool TexGen::EnsureHeightTileTexture(uint a_tileSize)
{
	if (cacheOutputTexH && cacheOutputTexH->desc.Width == a_tileSize && cacheOutputTexH->desc.Height == a_tileSize)
		return true;

	// Sampling keeps full float precision; the encode to 16 bit happens on save.
	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32_FLOAT, a_tileSize, a_tileSize, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ);

	cacheOutputTexH = nullptr;
	try {
		cacheOutputTexH = eastl::make_unique<Texture2D>(desc, "TexGen::HeightCacheTile");
	} catch (const std::exception& e) {
		logger::error("[TexGen] Failed to create {0}x{0} height tile: {1}", a_tileSize, e.what());
		return false;
	}

	return cacheOutputTexH != nullptr;
}

void TexGen::ClearHeightTile()
{
	if (!cacheOutputTexH)
		return;

	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;

	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_WRITE, 0, &mapped);
	if (FAILED(hr) || !mapped.pData) {
		logger::error("[TexGen] Height tile clear failed to map: {:X}", (uint32_t)hr);
		return;
	}

	// Texels belonging to cells outside the worldspace are never sampled and stay at zero height.
	for (uint y = 0; y < tileSize; ++y)
		memset((uint8_t*)mapped.pData + y * mapped.RowPitch, 0, tileSize * sizeof(float));

	context->Unmap(cacheOutputTexH->resource.get(), 0);
}

bool TexGen::SaveHeightTile(const int2& a_tileOriginCell, int a_cellsPerTile)
{
	if (!cacheOutputTexH)
		return false;

	auto context = globals::d3d::context;
	const uint tileSize = cacheOutputTexH->desc.Width;
	const bool export16Bit = settings.cacheExport16Bit;

	auto tileWorldspaceID = worldspaceID.empty() ? std::string("Unknown") : worldspaceID;
	auto path = GetTilePath(tileWorldspaceID, "_H", tileSize, a_cellsPerTile, a_tileOriginCell);

	DirectX::ScratchImage outputImage;
	HRESULT hr = outputImage.Initialize2D(HeightStorageFormat(export16Bit), tileSize, tileSize, 1, 1);
	if (FAILED(hr)) {
		logger::error("[TexGen] Failed to allocate height tile image: {:X}", (uint32_t)hr);
		return false;
	}

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr) || !mapped.pData) {
		logger::error("[TexGen] Failed to map height tile for save: {:X}", (uint32_t)hr);
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

	logger::info("[TexGen] Saved height tile {}", path.string());
	return true;
}

void TexGen::UpdateHeightPreview(const int2& a_tileOriginCell)
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
			heightPreviewTex = eastl::make_unique<Texture2D>(desc, "TexGen::HeightTilePreview");
			heightPreviewTex->CreateSRV(srvDesc);
		} catch (const std::exception& e) {
			logger::error("[TexGen] Failed to create height tile preview: {}", e.what());
			heightPreviewTex = nullptr;
			return;
		}
	}

	D3D11_MAPPED_SUBRESOURCE src;
	HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ, 0, &src);
	if (FAILED(hr) || !src.pData) {
		logger::error("[TexGen] Failed to map height tile for preview: {:X}", (uint32_t)hr);
		return;
	}

	D3D11_MAPPED_SUBRESOURCE dst;
	hr = context->Map(heightPreviewTex->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst);
	if (FAILED(hr) || !dst.pData) {
		logger::error("[TexGen] Failed to map height preview: {:X}", (uint32_t)hr);
		context->Unmap(cacheOutputTexH->resource.get(), 0);
		return;
	}

	for (uint y = 0; y < tileSize; ++y) {
		auto srcRow = (const float*)((const uint8_t*)src.pData + y * src.RowPitch);
		ConvertHeightRow(srcRow, (uint8_t*)dst.pData + y * dst.RowPitch, tileSize, export16Bit);
	}

	context->Unmap(heightPreviewTex->resource.get(), 0);
	context->Unmap(cacheOutputTexH->resource.get(), 0);

	heightPreviewOrigin = a_tileOriginCell;
	heightPreviewValid = true;
}

// Move one cell at a time, casting a ray every worldRes units within the cell.
//
// The worldspace is covered by a grid of tileSize^2 tiles, each holding cellsPerTile^2 worldspace
// cells (1024 tile with 8x8 cells -> 128 texels/cell at 32 units per texel; 512 tile with 8x8 cells
// -> 64 texels/cell at 64 units per texel). Cells are visited tile by tile and the tile is saved the
// moment its last cell has been sampled, so an interrupted run only ever loses the tile in flight.
//
// Texture and cell progress must not update unless the position was updated correctly.
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
			heightGenRunning = false;
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
			logger::error("[TexGen] No height tiles to generate");
			heightGenRunning = false;
			heightGenInit = true;
			return;
		}

		logger::info("[TexGen] Generating {0} height cache: {1}x{1} tiles of {2}x{2} cells, {3} texels per cell, {4} units per texel, {5}",
			heightGenSingleTile ? "single tile" : "full", tileSize, cellsPerTile, texelsPerCell, worldRes,
			settings.cacheExport16Bit ? "16 bit unsigned" : "32 bit float");

		SetWorldPosition(currentCellXY, worldPositionSet);
		settleFrames = heightSettleFrames;
		heightGenInit = false;
		return;
	}

	if (!cacheOutputTexH) {
		logger::error("[TexGen] cacheOutputTexH INVALID");
		heightGenRunning = false;
		heightGenInit = true;
		return;
	}

	bool valid = IsPositionValid(worldPositionSet);
	failedCount = valid ? 0 : ++failedCount;
	if (!valid) {
		if (failedCount >= 10) {  // This should never happen but since its possible for the game to refuse an update we should handle it anyway.
			logger::error("[TexGen] Sample position was unable to be updated");
			failedCount = 0;
		} else {
			SetWorldPosition(currentCellXY, worldPositionSet);
		}
		return;
	}

	if (settleFrames > 0) {
		settleFrames--;
		return;
	}

	if (!skipTileWrite) {
		// write heightmap

		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = context->Map(cacheOutputTexH->resource.get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
		if (FAILED(hr) || !mapped.pData) {
			logger::error("[TexGen] Map failed: {:x}", (uint32_t)hr);
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
				if (!skipRaycast)
					groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 5000);

				groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

				int2 texCoord = localCell * texelsPerCell + int2(x, y);
				float* tex = (float*)((uint8_t*)mapped.pData + texCoord.y * mapped.RowPitch);
				tex[texCoord.x] = groundHeight;  // write to tex
			}
		}
		context->Unmap(cacheOutputTexH->resource.get(), 0);
	}

	cellsDone += 1;

	if (!advanceToNextCell()) {
		// Tile complete: flush it to disk before moving on.
		if (!skipTileSave)
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
			settings.cacheProgressX = startCell.x;
			settings.cacheProgressY = startCell.y;
			globals::state->Save();
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

bool TexGen::IsPositionValid(RE::NiPoint3 a_inputPosition)
{
	static constexpr float CELL = worldCellSize;

	bool valid = false;
	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		auto diff = player->GetPosition() - a_inputPosition;
		valid = std::max(diff.x, diff.y) < CELL;
		logger::trace("diff: {}, {}", diff.x, diff.y);
		logger::trace("Pos: {}  :  InPos: {}", player->GetPosition(), a_inputPosition);
	}

	return valid;
}

void TexGen::SetWorldPosition(const int2& a_currentCellXY, RE::NiPoint3& o_worldPos)
{
	static constexpr float CELL = worldCellSize;

	auto tes = RE::TES::GetSingleton();
	auto player = RE::PlayerCharacter::GetSingleton();

	float2 worldXY = float2((float)a_currentCellXY.x, (float)a_currentCellXY.y) * CELL;

	float landHeight;
	tes->GetLandHeight(RE::NiPoint3(worldXY.x, worldXY.y, 0), landHeight);
	logger::trace("land: {}", landHeight);

	float groundHeight = GetRayIntersectionHeight(float3(worldXY.x, worldXY.y, landHeight), 15000);
	logger::trace("ground: {}", groundHeight);
	float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), player->GetParentCell());
	groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

	float3 sampleCoordsWS = float3(worldXY.x, worldXY.y, groundHeight);
	logger::trace("sampleCoordsWS: {}, {}, {}", sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z);

	o_worldPos = RE::NiPoint3(sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z + 1500.0f);  // place character in air to avoid crap happening
	player->SetPosition(o_worldPos, false);
}

float TexGen::GetRayIntersectionHeight(float3 a_position, float a_rayOffset)
{
	static constexpr int MAX_ATTEMPTS = 10;

	static float prevZ = 0.0f;
	auto player = RE::PlayerCharacter::GetSingleton();
	auto cell = player->GetParentCell();
	auto bhkWorld = cell ? cell->GetbhkWorld() : nullptr;

	if (auto hkpWorld = bhkWorld ? cell->GetbhkWorld()->GetWorld1() : nullptr; hkpWorld) {
		float scale = RE::bhkWorld::GetWorldScale();
		float2 posScaledXY = float2(a_position.x * scale, a_position.y * scale);
		float currentZ = a_position.z + a_rayOffset;
		float endZ = a_position.z - a_rayOffset;

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
				logger::error("[TexGen] Ray cast failed to find surface... continuing");
				return prevZ;
			}

			auto rootCollidable = output.rootCollidable;
			if (!rootCollidable) {
				logger::error("[TexGen] Null Root collidable... continuing");
				return prevZ;
			}

			auto collisionObj = rootCollidable->GetCollisionLayer();

			if (!(collisionObj == RE::COL_LAYER::kTerrain || collisionObj == RE::COL_LAYER::kGround || collisionObj == RE::COL_LAYER::kStatic)) {
				float rayLength = currentZ - endZ;
				currentZ = currentZ - output.hitFraction * rayLength - (50.0f * scale);
				continue;
			}

			if (i + 1 == MAX_ATTEMPTS) {
				logger::error("[TexGen] Ray cast had no valid hit; last recorded collision was: {} ... continuing", collisionObj);
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

float TexGen::SampleHeightMap(float2 a_coords)
{
	static DirectX::ScratchImage image;
	static bool init = true;
	if (init) {
		std::filesystem::path atlasPath;
		AtlasCellRange atlasRange;
		if (FindAtlas(worldspaceID, "_H", atlasPath, atlasRange))
			DirectX::LoadFromDDSFile(atlasPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		init = false;
	}

	auto& cachedHeightmap = globals::features::terrainShadows.cachedHeightmap;
	float u = (a_coords.x - cachedHeightmap->pos0.x) / (cachedHeightmap->pos1.x - cachedHeightmap->pos0.x);
	float v = (a_coords.y - cachedHeightmap->pos0.y) / (cachedHeightmap->pos1.y - cachedHeightmap->pos0.y);
	auto& img = *image.GetImages();
	int ix = std::clamp((int)(u * img.width), 0, (int)img.width - 1);
	int iy = std::clamp((int)(v * img.height), 0, (int)img.height - 1);
	auto row = reinterpret_cast<const float*>(img.pixels + iy * img.rowPitch);
	float normalizedHeight = row[ix];
	logger::trace("height: {}", normalizedHeight);

	return normalizedHeight;
}

//////////////////////////////////////////////////////////////////////////////////
//// LOD bent normal tiles
//////////////////////////////////////////////////////////////////////////////////

bool TexGen::DispatchBentNormalSweep(Texture2D* a_accumTex, const int2& tileOriginAtlasPx)
{
	if (!a_accumTex || !bentNormalSweepCS || !bentNormalFinalizeCS || !bentNormalHullUAV || !heightMapSRV)
		return false;

	EnsureCacheGenBuffer();

	auto context = globals::d3d::context;
	const int tileSize = (int)a_accumTex->desc.Width;

	const float4 bounds = GetHeightMapBounds();
	const float2 worldPerTexel = float2(
		(bounds.z - bounds.x) / (float)bentNormalAtlasSize.x,
		(bounds.w - bounds.y) / (float)bentNormalAtlasSize.y);

	// Each azimuth adds its wedge to the accumulator, so it starts empty.
	const float clearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearUnorderedAccessViewFloat(a_accumTex->uav.get(), clearValue);

	ID3D11ShaderResourceView* heightSRV = heightMapSRV;
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

	ID3D11UnorderedAccessView* resolveUAVs[2] = { a_accumTex->uav.get(), nullptr };
	context->CSSetShader(bentNormalFinalizeCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 2, resolveUAVs, nullptr);
	context->Dispatch((tileSize + 7) / 8, (tileSize + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);

	return true;
}

bool TexGen::StartBentNormalTiles()
{
	if (bentNormalTileGen)
		return false;

	UpdateWorldspaceID();

	if (worldspaceID.empty()) {
		logger::error("[TexGen] No worldspace known, cannot generate bent normal tiles");
		return false;
	}

	if (!heightMapSRV) {
		logger::error("[TexGen] No height atlas loaded, build it before generating bent normal tiles");
		return false;
	}

	const int tileSize = settings.cacheAtlasTileSize;
	const int cellsPerTile = settings.cacheAtlasTileCells;
	if (tileSize <= 0 || cellsPerTile <= 0) {
		logger::error("[TexGen] No atlas layout recorded, rebuild the height atlas first");
		return false;
	}

	// The atlas dimensions the height tiles were stitched into; the regions are relative to these.
	std::filesystem::path atlasPath;
	AtlasCellRange atlasRange;
	if (!FindAtlas(worldspaceID, "_H", atlasPath, atlasRange)) {
		logger::error("[TexGen] No height atlas found, cannot generate bent normal tiles");
		return false;
	}

	DirectX::TexMetadata metadata;
	if (FAILED(DirectX::GetMetadataFromDDSFile(atlasPath.c_str(), DirectX::DDS_FLAGS_NONE, metadata))) {
		logger::error("[TexGen] Failed to read the height atlas, cannot generate bent normal tiles");
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
		if (ParseTileName(path, worldspaceID, "_H", tile) && tile.tileSize == (uint)tileSize && tile.cellsPerTile == cellsPerTile)
			bentNormalTileQueue.push_back(tile.originCell);
	}

	if (bentNormalTileQueue.empty()) {
		logger::error("[TexGen] No height tiles matching the atlas layout, nothing to generate");
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
		bentNormalTileTex = eastl::make_unique<Texture2D>(desc, "TexGen::BentNormalTile");
		bentNormalTileTex->CreateSRV(nullptr);
		bentNormalTileTex->CreateUAV(uavDesc);
	} catch (const std::exception& e) {
		logger::error("[TexGen] Failed to create bent normal tile target: {}", e.what());
		bentNormalTileTex = nullptr;
		return false;
	}

	bentNormalSweepCS = nullptr;
	bentNormalFinalizeCS = nullptr;
	bentNormalSweepCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "SWEEP", "" } }, "cs_5_0")));
	bentNormalFinalizeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "SWEEP_FINALIZE", "" } }, "cs_5_0")));
	if (!bentNormalSweepCS || !bentNormalFinalizeCS) {
		logger::error("[TexGen] Failed to compile the bent normal sweep shaders");
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
			logger::error("[TexGen] Failed to create the bent normal hull scratch buffer");
			StopBentNormalTiles();
			return false;
		}

		Util::SetResourceName(bentNormalHullBuffer.get(), "TexGen::BentNormalHullStack");
		Util::SetResourceName(bentNormalHullUAV.get(), "TexGen::BentNormalHullStack UAV");
	}

	bentNormalTileIndex = 0;
	bentNormalTileGen = true;

	logger::info("[TexGen] Generating {0} bent normal tiles at {1}x{1} from a {2}x{3} atlas",
		bentNormalTileQueue.size(), tileSize, bentNormalAtlasSize.x, bentNormalAtlasSize.y);

	return true;
}

void TexGen::StopBentNormalTiles()
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

void TexGen::UpdateBentNormalTiles()
{
	if (!bentNormalTileGen)
		return;

	// One tile per frame; a whole set in a single call would sit far past the driver timeout.
	if (bentNormalTileIndex < bentNormalTileQueue.size()) {
		GenerateBentNormalTile(bentNormalTileQueue[bentNormalTileIndex]);
		bentNormalTileIndex++;
	}

	if (bentNormalTileIndex >= bentNormalTileQueue.size()) {
		logger::info("[TexGen] Bent normal tiles complete: {} tiles", bentNormalTileQueue.size());
		StopBentNormalTiles();

		// The atlas is stitched from these tiles, so it is stale until it is rebuilt.
		NotifyCacheMapsChanged();
	}
}

bool TexGen::GenerateBentNormalTile(const int2& a_tileOriginCell)
{
	if (!bentNormalTileTex)
		return false;

	const int tileSize = settings.cacheAtlasTileSize;
	const int cellsPerTile = settings.cacheAtlasTileCells;
	if (tileSize <= 0 || cellsPerTile <= 0 || bentNormalAtlasSize.x <= 0 || bentNormalAtlasSize.y <= 0)
		return false;

	// The tile's north west corner in atlas texels; atlas rows run north first, hence the flip.
	const int tileColumn = (a_tileOriginCell.x - settings.cacheAtlasMinCellX) / cellsPerTile;
	const int maxOriginCellY = settings.cacheAtlasMinCellY + (settings.cacheAtlasTilesY - 1) * cellsPerTile;
	const int tileRow = (maxOriginCellY - a_tileOriginCell.y) / cellsPerTile;

	if (!DispatchBentNormalSweep(bentNormalTileTex.get(), int2(tileColumn * tileSize, tileRow * tileSize)))
		return false;

	DirectX::ScratchImage captured;
	HRESULT hr = DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, bentNormalTileTex->resource.get(), captured);
	if (FAILED(hr)) {
		logger::error("[TexGen] Failed to capture bent normal tile {}, {}: {:X}", a_tileOriginCell.x, a_tileOriginCell.y, (uint32_t)hr);
		return false;
	}

	// Same storage convention as the height tiles: 16 bit unsigned, or raw float when that is off.
	// The shader already writes normal * 0.5 + 0.5 and AO, so everything is in range for UNORM.
	DirectX::ScratchImage converted;
	if (settings.cacheExport16Bit) {
		hr = DirectX::Convert(*captured.GetImages(), DXGI_FORMAT_R16G16B16A16_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
		if (FAILED(hr)) {
			logger::error("[TexGen] Failed to convert bent normal tile {}, {}: {:X}", a_tileOriginCell.x, a_tileOriginCell.y, (uint32_t)hr);
			return false;
		}
	}

	const DirectX::Image* image = settings.cacheExport16Bit ? converted.GetImages() : captured.GetImages();
	auto path = GetTilePath(worldspaceID, "_BN", (uint)tileSize, cellsPerTile, a_tileOriginCell);

	if (!SaveMapDDS(*image, path))
		return false;

	logger::info("[TexGen] Saved bent normal tile {}", path.string());
	return true;
}

//////////////////////////////////////////////////////////////////////////////////
//// Settings UI
//////////////////////////////////////////////////////////////////////////////////

void TexGen::DrawSettings()
{
	ImGui::TextWrapped(
		"Bakes the terrain LOD map cache into %s.\n"
		"Generation teleports the player across the worldspace and takes a long time; the features "
		"that render with these maps load them from disk and do not need this feature at runtime.",
		cachePath.string().c_str());

	ImGui::Separator();

	ImGui::Text("Worldspace: %s", worldspaceID.empty() ? "N/A" : worldspaceID.c_str());

	ImGui::InputText("DynDOLOD Worldspace Directory", &settings.dynDOLODPath);
	ImGui::TextWrapped("Select the DynDOLOD terrain-texture folder for the worldspace you want to generate textures for.");

	ImGui::BeginDisabled(settings.dynDOLODPath.empty());
	if (ImGui::Button("Generate albedo atlas")) {
		auto outputPath = cachePath / ((worldspaceID.empty() ? std::string("Tamriel") : worldspaceID) + "_A.dds");
		BuildLODAtlas(outputPath, "");
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Stitches the xLODGen LOD tiles in\n%s\ninto one albedo atlas.", settings.dynDOLODPath.c_str());

	if (ImGui::Button("Generate card Occl"))
		GenerateCardinalOcclusionMap();

	if (ImGui::Button("Generate bent normal"))
		GenerateBentNormalMap();

	if (ImGui::Button("Generate Normal"))
		GenerateNormalMap();

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
		GetHeightTileSize(), texelsPerCell, worldCellSize / (float)texelsPerCell,
		settings.cacheExport16Bit ? "16 bit unsigned" : "32 bit float");

	if (ImGui::Button(heightGenRunning ? "Stop Height Generation" : "Generate Height Map")) {
		heightGenRunning = !heightGenRunning;
		heightGenSingleTile = false;
		heightGenInit = true;  // stopping discards the in-flight tile; restart on its boundary
	}

	ImGui::SameLine();

	ImGui::BeginDisabled(heightGenRunning);
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

	ImGui::Checkbox("Zero Base Atlas", &settings.cacheAtlasZeroBase);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Bias the atlas so its lowest point is 0.0 instead of storing absolute heights.\nTakes effect on the next atlas build.");

	if (settings.cacheAtlasZeroBase && settings.cacheAtlasMinHeight != 0.0f)
		ImGui::Text("Last atlas: 0.0 is %.0f game units", settings.cacheAtlasMinHeight);

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

	ImGui::BeginDisabled(heightGenRunning || (!bentNormalTileGen && (worldspaceID.empty() || !heightMapSRV)));
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

	if (bentNormalAtlasRange.valid)
		ImGui::BulletText("BN atlas cells %d,%d to %d,%d", bentNormalAtlasRange.minCell.x, bentNormalAtlasRange.minCell.y, bentNormalAtlasRange.maxCell.x, bentNormalAtlasRange.maxCell.y);

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

	ImGui::BeginDisabled(heightGenRunning);
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
	if (!AtlasTexelToCell(int2(texelCoords[0], texelCoords[1]), texelCell)) {
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
}

#undef I18N_KEY_PREFIX
