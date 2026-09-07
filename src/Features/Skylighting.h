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
		float HorizonBand = 0.15f;
		float HorizonBias = 0.0f;

		uint toggleLighting = true;
		uint toggleTrees = true;
		uint toggleGrass = true;
		uint toggleDeferred = true;
		uint toggleEffect = true;
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

		float4 BentNormalTileBounds;   // xy: min, zw: max world XY the streamed tile covers
		float4 BentNormalAtlasBounds;  // xy: min, zw: max world XY the atlas covers
		uint HasBentNormalTile;
		uint HasBentNormalAtlas;
		float HorizonBand;
		float HorizonBias;
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

	bool updateTerrainLighting = true;
	bool runSparse = true;  /////////////

	// Sparse grid
	static constexpr int2 sparseGridSize = int2(119 * 2, 94 * 2);  //int2(1024, 1024);  //int2(256, 256);  //int2(119 * 4, 94 * 4);
	eastl::unique_ptr<Texture2D> texSparseProbeArray = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> updateSparseGridCS = nullptr;

	static constexpr int2 terrainMapSize = int2(1024, 1024);  //int2(256, 256);
	eastl::unique_ptr<Texture2D> terrainLightingTex = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> terrainRelightCS = nullptr;

	void GetCachedWorldspaces();
	bool LoadWorldspaceCache();

	/// @brief Force every cache map to be reloaded from disk on the next frame.
	/// Called by TexGen after it has regenerated one of them.
	void InvalidateCacheMaps() { cacheWorldspaceID.clear(); }

	void UpdateTerrainLighting();

	bool worldHasCache = false;
	std::unordered_set<std::string> worldSpaceCachedMapList;
	std::string cacheWorldspaceID = "";

	ID3D11ShaderResourceView* BNMapSRV = nullptr;
	ID3D11ShaderResourceView* COMapSRV = nullptr;
	ID3D11ShaderResourceView* CO2MapSRV = nullptr;
	ID3D11ShaderResourceView* DOMapSRV = nullptr;
	ID3D11ShaderResourceView* DO2MapSRV = nullptr;
	ID3D11ShaderResourceView* AMapSRV = nullptr;
	ID3D11ShaderResourceView* NMapSRV = nullptr;
	ID3D11ShaderResourceView* HMapSRV = nullptr;

	//// Bent normal tile streaming ////
	// Exactly one bent normal tile is resident at a time: the one covering the player. Coverage
	// therefore ends at the tile edge rather than at a fixed radius, which is a deliberate trade
	// for keeping a single texture in memory. Tiles themselves are baked by TexGen.

	ID3D11ShaderResourceView* BNTileSRV = nullptr;  // streamed tile, separate from the BN atlas
	int2 bnTileOriginCell = int2(0, 0);             // origin cell of the tile last attempted
	bool bnTileOriginValid = false;                 // whether bnTileOriginCell has been set
	float4 bnTileWorldBounds = float4(0, 0, 0, 0);  // minX, minY, maxX, maxY the loaded tile covers

	/// @brief Keep the tile under the player resident, swapping it when a tile boundary is crossed.
	void UpdateBentNormalTileStream();
	/// @brief Drop the resident tile and forget which one it was.
	void ReleaseBentNormalTileStream();

	/// @brief (Re)load a cache map, releasing whatever the view held. False if it is missing or unreadable.
	bool LoadCacheMap(const std::filesystem::path& a_path, ID3D11ShaderResourceView** a_srv);

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
