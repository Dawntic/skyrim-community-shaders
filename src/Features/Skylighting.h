#pragma once

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#include "../Deferred.h"
#include "ShaderCache.h"
#include "State.h"

#include "Features/TerrainShadows.h"
#include "PhysicalSky.h"

struct Skylighting : Feature
{
private:
	static constexpr std::string_view MOD_ID = "139352";

public:
	virtual inline std::string GetName() override { return "Skylighting"; }
	virtual inline std::string GetShortName() override { return "Skylighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "SKYLIGHTING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Simulates realistic ambient lighting by calculating sky occlusion and directional lighting, providing more accurate and natural illumination in outdoor environments.",
			{ "Sky occlusion calculation for ambient lighting",
				"Directional skylighting based on environment geometry",
				"Enhanced ambient lighting for outdoor scenes",
				"Support for varying sky illumination intensities",
				"Integration with existing lighting systems" }
		};
	}
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void Prepass() override;

	virtual void PostPostLoad() override;

	//////////////////////////////////////////////////////////////////////////////////

	void UpdateDenseProbeGrid();
	void UpdateSparseProbeGrid();

	struct Settings
	{
		float MaxZenith = 3.1415926f / 2.f;  // 90 deg
		float MinDiffuseVisibility = 0.1f;
		float MinSpecularVisibility = 0.1f;
		float SkyInfluence = 4.0f;
		float EnvInfluence = 1.0f;

		uint toggleLighting = true;
		uint toggleTrees = true;
		uint toggleGrass = true;
		uint toggleDeferred = true;
		uint toggleEffect = true;

		int cacheProgressX = -57;
		int cacheProgressY = -43;
		int cacheTileCells = 8;            // worldspace cells per height tile edge (4 or 8)
		int cacheTileSize = 1024;          // texels per height tile edge (512 or 1024)
		bool cacheExport16Bit = true;      // export tiles as xLODGen-style 16 bit unsigned instead of raw float
		bool cacheAtlasZeroBase = false;   // bias the atlas so its lowest point sits at 0.0
		float cacheAtlasMinHeight = 0.0f;  // game unit height the last biased atlas stores as 0.0

		// Layout of the last built atlas, so texel coordinates can be mapped back to cells.
		int cacheAtlasMinCellX = 0;  // origin cell of the atlas' lower left tile
		int cacheAtlasMinCellY = 0;
		int cacheAtlasTileSize = 0;   // texels per tile edge, 0 if no atlas has been built
		int cacheAtlasTileCells = 0;  // cells per tile edge
		int cacheAtlasTilesX = 0;     // tile columns
		int cacheAtlasTilesY = 0;     // tile rows, needed to flip texel Y into cell space
	} settings;

	struct SkylightingCB
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float4 OcclusionDir;

		float4 PosOffset;     // cell origin in camera model space
		uint ArrayOrigin[4];  // xyz: array origin, w: max accum frames
		int ValidMargin[4];

		int2 GridTexSize;
		int2 EnvRadianceTexSize;

		float4 GridBounds;
		float2 InvGridTexSize;
		float2 InvEnvRadianceTexSize;
		float2 GridMinWorldCorner;
		float2 InvGridSpan;

		uint HasCache;
		uint toggleLighting;
		uint toggleTrees;
		uint toggleGrass;

		uint toggleDeferred;
		uint toggleEffect;
		uint _pad[2];

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		float SkyInfluence;
		float EnvInfluence;

		float4 Basis0;
		float4 Basis1;
	};
	static_assert(sizeof(SkylightingCB) % 16 == 0);

	SkylightingCB GetCommonBufferData(bool a_inWorld);

	winrt::com_ptr<ID3D11SamplerState> comparisonSampler = nullptr;

	Texture2D* texOcclusion = nullptr;
	Texture3D* texProbeArray = nullptr;
	Texture3D* texAccumFramesArray = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> probeUpdateCompute = nullptr;

	// misc parameters
	uint probeArrayDims[3] = { 256, 256, 128 };
	float occlusionDistance = 4096.f * 2.5f;  // 5 ugrids

	bool queuedResetSkylighting = true;
	bool inOcclusion = false;
	REX::W32::XMFLOAT4X4 OcclusionTransform;
	float4 OcclusionDir;
	uint frameCount = 0;

	// Sparse grid
	static constexpr int2 sparseGridSize = int2(256, 256);  //int2(119 * 4, 94 * 4);
	eastl::unique_ptr<Texture2D> texSparseProbeArray = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> updateSparseGridCS = nullptr;

	static constexpr int2 terrainMapSize = int2(256, 256);
	eastl::unique_ptr<Texture2D> terrainLightingTex = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> terrainRelightCS = nullptr;

	void GetCachedWorldspaces();
	bool LoadWorldspaceCache();

	void UpdateTerrainLighting();

	bool runSparse = true;  /////////////

	bool worldHasCache = false;
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";
	std::unordered_set<std::string> worldSpaceCachedMapList;
	std::string cacheWorldspaceID = "";

	ID3D11ShaderResourceView* BNMapSRV = nullptr;
	ID3D11ShaderResourceView* COMapSRV = nullptr;
	ID3D11ShaderResourceView* CO2MapSRV = nullptr;
	ID3D11ShaderResourceView* DOMapSRV = nullptr;
	ID3D11ShaderResourceView* DO2MapSRV = nullptr;
	ID3D11ShaderResourceView* AMapSRV = nullptr;
	ID3D11ShaderResourceView* NMapSRV = nullptr;
	//ID3D11ShaderResourceView* NSMapSRV = nullptr;
	ID3D11ShaderResourceView* HMapSRV = nullptr;
	ID3D11ShaderResourceView* DOMapSRVB = nullptr;
	ID3D11ShaderResourceView* DO2MapSRVB = nullptr;

	//// Cache gen resources ////
	static constexpr uint COMapSize = 1024;
	static constexpr int2 BNMapSize = int2(3808, 3008);
	float HeightMapOffset = 0;  //32767;  // from xlodgen
	float HeightMapScale = 1;   //8.0;     // from xlodgen

	std::filesystem::path lodPath = L"C:\\Skyrim Modding Utilities\\DynDOLOD\\xLODGen\\Output\\textures\\terrain\\tamriel";
	void BuildAtlas(const std::filesystem::path& outputDir, std::string mapTag);
	void GenerateBentNormalMap();
	void GenerateCardinalOcclusionMap();
	//void GenerateNormalStepMap();
	void GenerateNormalMap();
	bool MapGen = false;
	bool heightGenInit = true;              // set to restart the height run from the stored tile boundary
	bool heightGenSingleTile = false;       // generate only the tile covering heightGenTargetCell, then stop
	int2 heightGenTargetCell = int2(0, 0);  // cell whose tile a single tile run generates
	int cellsDone = 0;
	int tilesDone = 0;
	bool test = false;
	bool test2 = false;
	bool test3 = false;

	//// Height cache tiles ////
	// The height cache is written out as a grid of fixed-size tiles instead of one huge
	// worldspace-sized texture. Each tile covers cacheTileCells x cacheTileCells worldspace
	// cells and is saved to disk as soon as its last cell has been sampled.
	static constexpr float worldCellSize = 4096.0f;  // world units per worldspace cell edge
	static inline int heightSettleFrames = 20;       // 60 // frames to let terrain stream in after a teleport
	static inline eastl::unique_ptr<Texture2D> cacheOutputTexH = nullptr;

	// xLODGen-compatible export encoding: 16 bit unsigned, zero height stored as 32767, one step
	// per 8 game units. Decode with height = (encoded - heightExportOffset) * heightExportScale.
	static constexpr float heightExportOffset = 32767.0f;
	static constexpr float heightExportScale = 8.0f;

	/// @brief Cells per tile edge, sanitised to a value the tile size divides evenly (4 or 8).
	int GetHeightTileCells() const { return settings.cacheTileCells == 4 ? 4 : 8; }
	/// @brief Texels per tile edge, sanitised to a supported size (512 or 1024).
	uint GetHeightTileSize() const { return settings.cacheTileSize == 512 ? 512u : 1024u; }
	/// @brief (Re)create the staging tile at the given edge size. Returns false on failure.
	bool EnsureHeightTileTexture(uint tileSize);
	/// @brief Zero the staging tile so cells that fall outside the worldspace stay at zero height.
	void ClearHeightTile();
	/// @brief Write the staging tile to "<Worldspace>_H<tileSize>.<cellsPerTile>.<originX>.<originY>.dds".
	bool SaveHeightTile(const int2& tileOriginCell, int cellsPerTile);
	/// @brief Copy the staging tile into a viewable texture holding exactly what gets written to disk.
	void UpdateHeightPreview(const int2& tileOriginCell);
	/// @brief A set of tiles stitched into one north up image, with where each tile landed.
	struct TileAtlasResult
	{
		DirectX::ScratchImage image;
		std::vector<std::pair<int2, std::string>> placements;  // atlas texel origin -> source file
		int2 minOriginCell = int2(0, 0);
		int2 maxOriginCell = int2(0, 0);
		int2 tileCounts = int2(0, 0);
		uint tileSize = 0;
		int cellsPerTile = 0;
	};

	/// @brief Stitch "<Worldspace><mapTag><tileSize>.<cells>.<x>.<y>.dds" tiles into one image.
	/// @param unormFill Value gaps between tiles take in a UNORM format, normalised to [0, 1].
	/// @param floatFill Value gaps between tiles take in a float format.
	bool StitchTileAtlas(const std::string& worldspaceID, const std::string& mapTag, const float4& unormFill, const float4& floatFill, TileAtlasResult& o_result);

	/// @brief Ensure "<Worldspace>_H.dds" exists, stitching it from the generated height tiles if not.
	/// @param forceRebuild Rebuild even when the atlas is already on disk.
	/// @return True if the atlas exists once the call returns.
	bool EnsureHeightAtlas(const std::string& worldspaceID, bool forceRebuild = false);
	/// @brief Ensure "<Worldspace>_BN.dds" exists, stitching it from the generated bent normal tiles.
	bool EnsureBentNormalAtlas(const std::string& worldspaceID, bool forceRebuild = false);
	/// @brief Map an atlas texel to the worldspace cell it covers, using the last built atlas layout.
	/// @return False if no atlas has been built, leaving o_cell untouched.
	bool AtlasTexelToCell(const int2& texel, int2& o_cell) const;

	//// LOD bent normal tiles ////
	// One bent normal tile per height tile, matching their dimensions, format and naming, built
	// from the stitched height atlas so rays still see terrain beyond the tile they belong to.

	/// @brief Run the per texel bent normal march over a UV region of the height atlas.
	/// @param regionOffsetScale xy uv offset, zw uv scale. (0, 0, 1, 1) covers the whole atlas.
	bool DispatchBentNormals(ID3D11ComputeShader* computeShader, Texture2D* outputTex, const float4& regionOffsetScale);
	/// @brief Line sweep bent normals for one tile: one dispatch per azimuth, then a resolve pass.
	/// @param tileOriginAtlasPx North west corner of the tile in atlas texels.
	bool DispatchBentNormalSweep(Texture2D* accumTex, const int2& tileOriginAtlasPx);
	static constexpr int bentNormalAzimuths = 64;  // must match NUM_AZIMUTH in the shader
	static constexpr int bentNormalHullCapacity = 1024;
	/// @brief Queue a bent normal tile for every height tile on disk. Tiles are processed one per frame.
	bool StartBentNormalTiles();
	/// @brief Process the next queued bent normal tile, if any.
	void UpdateBentNormalTiles();
	/// @brief Generate and save the bent normal tile whose grid starts at the given cell.
	bool GenerateBentNormalTile(const int2& tileOriginCell);
	void StopBentNormalTiles();

	bool bentNormalTileGen = false;
	size_t bentNormalTileIndex = 0;
	std::vector<int2> bentNormalTileQueue;
	int2 bentNormalAtlasSize = int2(0, 0);
	eastl::unique_ptr<Texture2D> bentNormalTileTex = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalSweepCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalFinalizeCS = nullptr;
	winrt::com_ptr<ID3D11Buffer> bentNormalHullBuffer = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> bentNormalHullUAV = nullptr;

	eastl::unique_ptr<Texture2D> heightPreviewTex = nullptr;
	int2 heightPreviewOrigin = int2(0, 0);
	bool heightPreviewValid = false;

	float GetRayIntersectionHeight(float3 position, float f);
	void SetWorldPosition(const int2& currentCellXY, RE::NiPoint3& worldPos);
	void GenerateHeightMap();
	bool IsPositionValid(RE::NiPoint3 inputPosition);
	ID3D11ShaderResourceView* tmpTex = nullptr;
	float SampleHeightMap(float2 coords);

	struct alignas(16) CacheGenCBStruct
	{
		float4 TexParams;
		float4 RegionOffsetScale = float4(0.0f, 0.0f, 1.0f, 1.0f);  // height map sub rect to process
		float4 GridBounds;                                          // world xy min/max of the height map
		float4 SweepDir;                                            // xy: world dir, z: slope, w: major step
		float4 SweepParams;                                         // x: first line, y: line count, z: transpose, w: units per step
		float4 SweepRect;                                           // xy: tile origin in atlas texels, z: tile size
	};
	ConstantBuffer* cacheGenBuffer = nullptr;

	/// @brief World bounds (minX, minY, maxX, maxY) of the cells the height map covers.
	float4 GetHeightMapBounds() const;
	/// @brief Fill the cache gen constants shared by every generator.
	CacheGenCBStruct MakeCacheGenCB(const float2& outputSize, const float4& regionOffsetScale = float4(0.0f, 0.0f, 1.0f, 1.0f)) const;

	void ResetSkylighting();
	std::chrono::time_point<std::chrono::system_clock> lastUpdateTimer = std::chrono::system_clock::now();

	//////////////////////////////////////////////////////////////////////////////////

	const float CARD[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
	const float DIAG[4][2] = { { 0.70710678f, 0.70710678f }, { -0.70710678f, 0.70710678f }, { -0.70710678f, -0.70710678f }, { 0.70710678f, -0.70710678f } };

	float4 BuildHorizonBasis(const float (&dirs)[4][2], float ax, float ay, float sharpness)
	{
		float w[4], sum = 0.f;
		for (int k = 0; k < 4; ++k) {
			float d = std::max(0.f, ax * dirs[k][0] + ay * dirs[k][1]);  // project azimuth onto bin
			d = std::pow(d, sharpness);                                  // 1 = linear cosine blend
			w[k] = d;
			sum += d;
		}
		float inv = sum > 1e-5f ? 1.f / sum : 0.f;  // 0 weights when sun is overhead -> occVal 0 -> lit
		return float4{ w[0] * inv, w[1] * inv, w[2] * inv, w[3] * inv };
	}

	void BuildOcclusionBasis(float3 lightDir, float4& basis0, float4& basis1, float sharpness = 1.f)
	{
		float hl = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y);  // azimuth length
		float ax = hl > 1e-5f ? lightDir.x / hl : 0.f;
		float ay = hl > 1e-5f ? lightDir.y / hl : 0.f;
		basis0 = BuildHorizonBasis(CARD, ax, ay, sharpness);
		basis1 = BuildHorizonBasis(DIAG, ax, ay, sharpness);
	}

	struct BSParticleShaderRainEmitter
	{
		void* vftable_BSParticleShaderRainEmitter_0;
		char _pad_8[4056];
	};

	enum class ShaderTechnique
	{
		// Sky
		SkySunOcclude = 0x2,

		// Grass
		GrassNoAlphaDirOnlyFlatLit = 0x3,
		GrassNoAlphaDirOnlyFlatLitSlope = 0x5,
		GrassNoAlphaDirOnlyVertLitSlope = 0x6,
		GrassNoAlphaDirOnlyFlatLitBillboard = 0x13,
		GrassNoAlphaDirOnlyFlatLitSlopeBillboard = 0x14,

		// Utility
		UtilityGeneralStart = 0x2B,

		// Effect
		EffectGeneralStart = 0x4000002C,

		// Lighting
		LightingGeneralStart = 0x4800002D,

		// DistantTree
		DistantTreeDistantTreeBlock = 0x5C00002E,
		DistantTreeDepth = 0x5C00002F,

		// Grass
		GrassDirOnlyFlatLit = 0x5C000030,
		GrassDirOnlyFlatLitSlope = 0x5C000032,
		GrassDirOnlyVertLitSlope = 0x5C000033,
		GrassDirOnlyFlatLitBillboard = 0x5C000040,
		GrassDirOnlyFlatLitSlopeBillboard = 0x5C000041,
		GrassRenderDepth = 0x5C00005C,

		// Sky
		SkySky = 0x5C00005E,
		SkyMoonAndStarsMask = 0x5C00005F,
		SkyStars = 0x5C000060,
		SkyTexture = 0x5C000061,
		SkyClouds = 0x5C000062,
		SkyCloudsLerp = 0x5C000063,
		SkyCloudsFade = 0x5C000064,

		// Particle
		ParticleParticles = 0x5C000065,
		ParticleParticlesGryColorAlpha = 0x5C000066,
		ParticleParticlesGryColor = 0x5C000067,
		ParticleParticlesGryAlpha = 0x5C000068,
		ParticleEnvCubeSnow = 0x5C000069,
		ParticleEnvCubeRain = 0x5C00006A,

		// Water
		WaterSimple = 0x5C00006B,
		WaterSimpleVc = 0x5C00006C,
		WaterStencil = 0x5C00006D,
		WaterStencilVc = 0x5C00006E,
		WaterDisplacementStencil = 0x5C00006F,
		WaterDisplacementStencilVc = 0x5C000070,
		WaterGeneralStart = 0x5C000071,

		// Sky
		SkySunGlare = 0x5C006072,

		// BloodSplater
		BloodSplaterFlare = 0x5C006073,
		BloodSplaterSplatter = 0x5C006074,
	};

	// Hooks
	struct BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t renderMode, RE::BSGraphics::BSShaderAccumulator* accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void RenderOcclusion();

	struct NiCamera_SetMatrix  // not needed?
	{
		static void thunk(RE::NiCamera* camera, void* unk);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetViewport
	{
		static void thunk(RE::BSGraphics::Renderer* renderer, uint32_t arg1, uint32_t arg2, uint32_t arg3);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_Precipitation_RenderOcclusion
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetViewFrustum
	{
		static void thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetViewFrustumVR
	{
		static void thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Event handler
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;

			if (!ui) {
				logger::error("UI event source not found");
				return false;
			}

			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());

			return true;
		}
	};
};
