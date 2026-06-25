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

		float3 PosOffset;  // cell origin in camera model space
		uint _pad0;
		uint ArrayOrigin[3];  // xyz: array origin, w: max accum frames
		uint _pad1;
		int ValidMargin[4];

		int2 GridTexSize;
		int2 _pad3;

		float4 GridBounds;
		float2 InvGridTexSize;
		float2 GridMinWorldCorner;

		float2 InvGridSpan;
		float _pad4[2];

		uint HasCache;
		uint toggleLighting;
		uint toggleTrees;
		uint toggleGrass;

		uint toggleDeferred;
		uint toggleEffect;
		uint _pad[2];

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint _pad2[2];

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
	static constexpr int2 sparseGridSize = int2(1024, 1024);  //int2(119 * 4, 94 * 4);
	eastl::unique_ptr<Texture2D> texSparseProbeArray = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> updateSparseGridCS = nullptr;

	void GetCachedWorldspaces();
	bool LoadWorldspaceCache();

	bool worldHasCache = false;
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";
	std::unordered_set<std::string> worldSpaceCachedMapList;
	std::string cacheWorldspaceID = "";

	ID3D11ShaderResourceView* BNMapSRV = nullptr;
	ID3D11ShaderResourceView* COMapSRV = nullptr;
	ID3D11ShaderResourceView* CO2MapSRV = nullptr;
	ID3D11ShaderResourceView* AMapSRV = nullptr;
	ID3D11ShaderResourceView* NMapSRV = nullptr;

	//// Cache gen resources ////
	static constexpr uint COMapSize = 1024;
	static constexpr uint BNMapSize = 1024;
	float HeightMapOffset = 32767;  // from xlodgen
	float HeightMapScale = 8.0;     // from xlodgen

	std::filesystem::path lodPath = L"C:\\Skyrim Modding Utilities\\DynDOLOD\\xLODGen\\Output\\textures\\terrain\\tamriel";
	void BuildAtlas(const std::filesystem::path& outputDir, std::string mapTag);
	void GenerateBentNormalMap();
	void GenerateCardinalOcclusionMap();

	struct alignas(16) CacheGenCBStruct
	{
		float4 TexParams;  // dimension, 1.0 / dimension,  dimension^2, dimension^2 * valid_cube_sides
		float _pad[2];
	};
	ConstantBuffer* cacheGenBuffer = nullptr;

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
