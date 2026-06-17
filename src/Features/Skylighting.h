#pragma once

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#include "../Deferred.h"
#include "ShaderCache.h"
#include "State.h"

#include "Features/TerrainShadows.h"

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
		float2 InvGridTexSize;
		float2 GridMinWorldCorner;
		float2 InvGridSpan;
		uint HasCache;
		uint toggleLighting;
		uint toggleTrees;
		uint toggleGrass;
		uint toggleDeferred;
		uint toggleEffect;
		float _pad[2];

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint _pad2[2];
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
	static constexpr int2 sparseGridSize = int2(128, 128);

	eastl::unique_ptr<Texture2D> texSparseProbeArray = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> updateSparseGridCS = nullptr;

	// misc parameters
	uint probeArrayDims[3] = { 256, 256, 128 };
	float occlusionDistance = 4096.f * 2.5f;  // 5 ugrids

	// cached variables
	bool queuedResetSkylighting = true;
	bool inOcclusion = false;
	REX::W32::XMFLOAT4X4 OcclusionTransform;
	float4 OcclusionDir;
	uint frameCount = 0;

	RE::NiPoint3 cachedActorPosition;
	float cachedActorFOV;
	eastl::unique_ptr<Texture2D> bentNormalMap = nullptr;
	std::unordered_set<std::string> bentNormalMaps;
	std::string currentBentNormalMap = "";
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";

	void GetCachedWorldspaces();
	bool LoadWorldspaceBentNormalMap();
	bool worldHasCache = false;

	// cache gen resources
	static constexpr uint depthCubeSize = 128;

	eastl::unique_ptr<Texture2D> depthCubemap = nullptr;
	eastl::unique_ptr<Texture2D> bentNormalCacheTex = nullptr;
	std::array<ID3D11DepthStencilView*, 6> depthCubemapDSVs{};

	eastl::unique_ptr<Texture2D> stagingDepthTex = nullptr;
	DirectX::ScratchImage stagingHeightMapTex;

	ID3D11ComputeShader* bentNormalComputeShader = nullptr;

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
	float SampleHeightMap(float2 coords);
	void FinishCaching(std::string worldName);

	bool buildingCache = false;
	float3 sampleCoordsWS = float3();
	int cubemapSide = 0;
	int cellCount = 0;  //tmp

	bool override = false;  //tmp
	float3 coords = float3();

	void ResetSkylighting();

	std::chrono::time_point<std::chrono::system_clock> lastUpdateTimer = std::chrono::system_clock::now();

	//////////////////////////////////////////////////////////////////////////////////

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
