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
	//winrt::com_ptr<ID3D11ShaderResourceView> stbn_vec3_2Dx1D_128x128x64;

	// sparse grid
	static constexpr int2 sparseGridSize = int2(1024, 1024);  //int2(119 * 4, 94 * 4);

	eastl::unique_ptr<Texture2D> texSparseProbeArray = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> updateSparseGridCS = nullptr;

	// misc parameters
	uint probeArrayDims[3] = { 256, 256, 128 };
	float occlusionDistance = 4096.f * 2.5f;  // 5 ugrids

	bool queuedResetSkylighting = true;
	bool inOcclusion = false;
	REX::W32::XMFLOAT4X4 OcclusionTransform;
	float4 OcclusionDir;
	uint frameCount = 0;

	// Sparse grid

	void GetCachedWorldspaces();
	bool LoadWorldspaceCache();

	bool worldHasCache = false;
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";

	//eastl::unique_ptr<Texture2D> bentNormalMap = nullptr;
	ID3D11ShaderResourceView* BNMapSRV = nullptr;
	std::unordered_set<std::string> worldSpaceCachedMapList;
	std::string currentLoadedWorldspaceID = "";

	ID3D11ShaderResourceView* COMapSRV = nullptr;
	ID3D11ShaderResourceView* CO2MapSRV = nullptr;
	ID3D11ShaderResourceView* AMapSRV = nullptr;
	ID3D11ShaderResourceView* NMapSRV = nullptr;

	std::filesystem::path lodPath = L"C:\\Skyrim Modding Utilities\\DynDOLOD\\xLODGen\\Output\\textures\\terrain\\tamriel";
	void BuildAtlas(const std::filesystem::path& outputDir, std::string mapTag);

	//// Cache gen resources ////
	static constexpr uint depthCubeSize = 128;
	static constexpr float CACHE_SAMPLES_PER_CELL = 2;

	static constexpr uint COMapSize = 1024;
	float HeightMapOffset = 32767;  // from xlodgen
	float HeightMapScale = 8.0;     // from xlodgen

	eastl::unique_ptr<Texture2D> depthCubemap = nullptr;
	std::array<ID3D11DepthStencilView*, 6> depthCubemapDSVs{};

	eastl::unique_ptr<Texture2D> stagingDepthTex = nullptr;
	DirectX::ScratchImage stagingHeightMapTex;

	eastl::unique_ptr<Texture2D> cacheOutputTexBN = nullptr;
	ID3D11ComputeShader* BNComputeShader = nullptr;

	struct alignas(16) CacheGenCBStruct
	{
		float4 CubemapParams;  // dimension, 1.0 / dimension,  dimension^2, dimension^2 * valid_cube_sides
		int2 BentNormalWritePx;
		float _pad[2];
	};
	ConstantBuffer* cacheGenBuffer = nullptr;

	struct alignas(16) AlphaRefCBStruct
	{
		float AlphaTestRefRS;
		float _pad[3];
	};
	ConstantBuffer* clipRefOverrideBuffer = nullptr;

	void SetInitalState(RE::NiPoint3& initalPos);
	void CreateCachingResources();
	bool CreateUniqueCachingResources(int2 totalCells);
	void GenerateWorldspaceCache();
	void GenerateVisibilityCubemap();
	void GenerateBentNormal(int2 currentCellID);
	void GenerateCardinalOcclusion();
	float SampleHeightMap(float2 coords);
	void FinishCaching(std::string worldName);

	RE::NiPoint3 cachedActorPosition;
	float cachedActorFOV;

	bool buildingCache = false;
	float3 sampleCoordsWS = float3();
	int cubemapSide = 0;
	int cellCount = 0;  //tmp

	bool override = false;  //tmp
	float3 coords = float3();

	void ResetSkylighting();

	std::chrono::time_point<std::chrono::system_clock> lastUpdateTimer = std::chrono::system_clock::now();

	//////////////////////////////////////////////////////////////////////////////////

	// Build occBasis0/1 (uploaded to OcclusionParams[1768]/[1784]).
	//   occVal = max(dot(occ0, basis0), dot(occ1, basis1))
	// occ0 = cardinal horizons (+X,+Y,-X,-Y), occ1 = diagonal (+X+Y,-X+Y,-X-Y,+X-Y).
	// Channel order MUST match the bake's CARD/DIAG arrays.
	// lightDir = world direction TO the light (e.g. -sunForward), +Z up.

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
