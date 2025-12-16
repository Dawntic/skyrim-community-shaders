#pragma once
#include "../Deferred.h"
#include "Feature.h"
#include "IBL.h"
#include "LightLimitFix.h"
#include "Skylighting.h"
#include "State.h"
#include "TerrainShadows.h"
#include "Util.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <REX/W32/COMPTR.h>
#include <cmath>
#include <vector>

struct OrthogonalVolumetricLighting : Feature
{
	static OrthogonalVolumetricLighting* GetSingleton()
	{
		static OrthogonalVolumetricLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Orthogonal Volumetric Lighting"; }
	virtual inline std::string GetShortName() override { return "OrthogonalVolumetricLighting"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "OVL"; }
	virtual std::string_view GetCategory() const override { return "Display"; }
	virtual inline bool SupportsVR() override { return false; };  //

	uint CSM_Size;
	uint EVSM_Size;
	uint nCascades = 2;
	void SetupPostLoadResources();

	virtual inline void DataLoaded() override
	{
		RE::GetINISetting("bLensFlare:Imagespace")->data.b = true;
		CSM_Size = RE::GetINISetting("iShadowMapResolution:Display")->data.u;
		EVSM_Size = CSM_Size / 4;
		SetupPostLoadResources();
	}

	virtual inline void PostPostLoad() override { Hooks::Install(); }
	virtual void SetupResources() override;
	void CompileShaders();

	void CheckOverride();
	void LookupShader(int desc);
	void UpdateAndSetupResources();
	void SetupApplyPassResources();
	void VLightingRenderChain();

	void UpdateShadowBuffer();
	void UpdateFroxelBuffer();
	void UpdateGeneralBuffers();

	void RenderEVSM();
	void RenderEVSMBlur();
	void GenerateShadowVolume();
	void GenerateMediaVolume();
	void GenerateScatteringVolume();
	void RunIntergrationPass();
	void SetupApplyPass();
	void SetupBypass();
	void DrawFogMap();
	void SetupPerlinNoise();
	//void SetupFilterPass();

	ConstantBuffer* shadowDataCB = nullptr;
	ConstantBuffer* froxelGridCB = nullptr;
	ConstantBuffer* volumeCB = nullptr;
	ConstantBuffer* settingsCB = nullptr;

	ID3D11SamplerState* linearSampler = nullptr;
	ID3D11SamplerState* pointSampler = nullptr;
	ID3D11SamplerState* anisoLinear = nullptr;
	ID3D11SamplerState* anisoWrapLinear = nullptr;

	ID3D11BlendState* outVolumetricsBlendState = nullptr;

	//// Volumes ////////////
	ID3D11ComputeShader* generateMediaVolumeCS = nullptr;
	ID3D11ComputeShader* generateScatteringVolumeCS = nullptr;
	ID3D11ComputeShader* intergrationSliceMarchCS = nullptr;

	ID3D11Texture3D* mediaVolume[2] = {};
	ID3D11UnorderedAccessView* mediaVolumeUAV[2] = {};
	ID3D11ShaderResourceView* mediaVolumeSRV[2] = {};

	ID3D11Texture3D* scatteringVolume = nullptr;
	ID3D11UnorderedAccessView* scatteringVolumeUAV = nullptr;
	ID3D11ShaderResourceView* scatteringVolumeSRV = nullptr;

	ID3D11Texture3D* intergrationVolume = nullptr;
	ID3D11UnorderedAccessView* intergrationVolumeUAV = nullptr;
	ID3D11ShaderResourceView* intergrationVolumeSRV = nullptr;

	//ID3D11ComputeShader* FilterVolumeCS = nullptr;
	//ID3D11Texture3D* FilteringVolume[2] = {};
	//ID3D11UnorderedAccessView* FilterVolumeUAV[2] = {};
	//ID3D11ShaderResourceView* FilterVolumeSRV[2] = {};

	//// Shadows /////////////
	ID3D11ComputeShader* EVSMComputeShader = nullptr;
	ID3D11ComputeShader* blurEVSMComputeShader = nullptr;
	ID3D11ComputeShader* generateShadowVolumeCS = nullptr;

	ID3D11Texture2D* expVarianceMapTex = nullptr;
	ID3D11UnorderedAccessView* expVarianceMapUAV = nullptr;
	ID3D11ShaderResourceView* expVarianceMapSRV = nullptr;

	ID3D11Texture2D* EVSMBlurTexture = nullptr;
	ID3D11UnorderedAccessView* EVSMBlurUAV = nullptr;
	ID3D11ShaderResourceView* EVSMBlurSRV = nullptr;

	ID3D11Texture3D* shadowVolume[2] = {};
	ID3D11UnorderedAccessView* shadowVolumeUAV[2] = {};
	ID3D11ShaderResourceView* shadowVolumeSRV[2] = {};

	//// Util /////////////
	ID3D11ComputeShader* generatePerlinCS = nullptr;
	ID3D11ComputeShader* drawFogMapCS = nullptr;
	ID3D11VertexShader* bypassVS = nullptr;

	ID3D11Texture3D* perlinTex = nullptr;
	ID3D11UnorderedAccessView* perlinUAV = nullptr;
	ID3D11ShaderResourceView* perlinSRV = nullptr;

	ID3D11Texture2D* worldMapTexture = nullptr;
	ID3D11ShaderResourceView* staticWorldMapSRV = nullptr;

	ID3D11Texture2D* UIFogMapTexture = nullptr;
	ID3D11ShaderResourceView* UIFogMapSRV = nullptr;
	ID3D11UnorderedAccessView* UIFogMapUAV = nullptr;

	ID3D11Texture2D* fogMapTexture = nullptr;
	ID3D11ShaderResourceView* fogMapSRV = nullptr;
	ID3D11UnorderedAccessView* fogMapUAV = nullptr;

	//// Misc /////////
	ID3D11PixelShader* applyVolumetricLightingPS = nullptr;
	ID3D11PixelShader* outputPS = nullptr;
	ID3D11ShaderResourceView* blueNoiseSRV = nullptr;

	ID3D11Texture2D* outputTexture = nullptr;
	ID3D11ShaderResourceView* outputSRV = nullptr;
	ID3D11RenderTargetView* outputRTV = nullptr;

	//// Frustum and Shadow Data //////////
	static constexpr float4 volumeDimensions = float4(240, 136, 68, 0);
	static constexpr float4 noiseDimensions = float4(64, 64, 32, 0);

	float2 screenSize;
	bool overrideShader = false;
	bool swapOutputRT = false;
	uint frameCounter = 0;
	uint currentVolumeIdx = 0;
	uint historyVolumeIdx = 1;

	bool updateLightDir = true;
	float4 lightDir = float4(0, 0, 0, 0);

	struct LocalShadowLightTransform
	{
		REX::W32::XMFLOAT4X4 matrix = {};
		uint shadowmapIndex = 0;
		uint _pad[3] = {};
	};
	LocalShadowLightTransform localShadowLightMatrices[4];
	REX::W32::XMFLOAT4X4 directionalShadowCascadeMatrices[4] = {};
	float4 shadowCascadeEndSplit = float4(0, 0, 0, 0);
	float4 frustumNearFar;
	float3 eyePositionWS;

	float distributionLambda = 1.6f;
	float nearPlane = 15.0f;
	float farPlane = 353840.0f;

	float2 fogMapSize;
	DirectX::XMFLOAT4X4 fogMapViewProj = DirectX::XMFLOAT4X4(
		1.19175, 0.00, 0.00, 0.00,
		0.00, 2.11867, 0.00073, 0.00,
		0.00, 0.00035, -1.00036, -128.04633,
		0.00, 0.00035, -1.00, 0.00);

	uintptr_t* skyrim_FlareData = nullptr;
	uint32_t* skyrim_RunFlarePtr = nullptr;
	inline static RE::NiPoint3* skyrim_SunPosition = nullptr;
	//RE::NiPoint3* skyrim_SunPosition = nullptr;
	//RE::BSShadowLight* shadowLight;

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	//float localFogDensity = 0.1f;
	//float4 fogMapColor;
	//float globalFogStartHeight = 0.0f;
	//float globalFogFalloffHeight = 0.0f;
	float brushRadius = 24.0f;
	float brushFeather = 0.5;
	float fogErase = false;

	inline REX::W32::XMFLOAT4X4 GetCascadeMatrix(REX::W32::XMFLOAT4X4& lightMatrix)
	{
		float4 pos = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0);
		float4 transform = mul(pos, lightMatrix);

		REX::W32::XMFLOAT4X4 matrix = lightMatrix;
		matrix.m[3][0] = transform.x;
		matrix.m[3][1] = transform.y;
		matrix.m[3][2] = transform.z;
		matrix.m[3][3] = transform.w;

		REX::W32::XMFLOAT4X4 outMatrix = matrix;
		transpose(matrix, outMatrix);

		return outMatrix;
	}

	struct Settings
	{
		float extinction = 0.1;
		float anisotropy = 0.1;
		float localLightsAnisotropy = 0.80;
		float localLightsMultiplier = 2.0;
		float4 scatteringRatio = float4(1.0, 1.0, 1.0, 1.0);

		float globalFogDensity = 0.3;
		float globalFogFalloffHeight = 500;
		float globalFogStartHeight = -4000;

		float skyAmbientContribution = 1.0;
		float sceneAmbientContribution = 1.0;

		uint esmExponent = 8;
		float color_saturation = 0.25;
		float preExposure = 1.0;

		uint useHistory = true;
		float disocclutionThreshold = 0.05;
		float distanceFadeIn = 1.0;

		float blendOpp = false;
		//float _pad[3];

		float4 fogMapData;
		float4 UIfogMapParams;
	};
	Settings settings;

	struct alignas(16) ShadowDataCB
	{
		REX::W32::XMFLOAT4X4 directionalShadowCascadeMatrices[4];
		LocalShadowLightTransform localShadowLightMatrices[4];
		float4 shadowCascadeEndSplit;
		float4 EVSMData;
	};

	struct alignas(16) FroxelGridCB
	{
		Matrix cameraView;
		Matrix cameraProj;
		Matrix cameraViewInverse;
		Matrix cameraProjInverse;
		Matrix prevCameraViewProj;
		Matrix cameraViewProjInverse;

		float4 cameraPosition;
		float4 cameraData;
		float4 volumeSize;
		float4 frustumNearFar;

		float4 lightDirection;
		float4 frameparams;
		uint lightClusterGridSize[4];
	};

	struct alignas(16) VolumeBuffer
	{
		DirectX::XMFLOAT4X4 fogMapMatrix;
		float4 heightMapParams;
		float4 heightMapZRange;
		float4 NoiseSize;
		//uint frameCounter;
		//uint boardCondition;
		//float _pad[3];
	};

	struct alignas(16) SettingsBuffer
	{
		Settings cbsettings;
	};

	struct Shaders
	{
		enum Enum
		{
			Bypass = 0,
			RenderVL = 1,
			Apply = 2
		};
	};
	Shaders::Enum shaderdesc;

	virtual inline DirectX::XMFLOAT4A VectorToXMFloat(float4& value) { return DirectX::XMFLOAT4A(value.x, value.y, value.z, value.w); }
	virtual inline float LinearStep(float edge0, float edge1, float x) { return std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f); }

	static inline float4 mul(const float4& v, const REX::W32::XMFLOAT4X4& M)
	{
		return {
			v.x * M.m[0][0] + v.y * M.m[1][0] + v.z * M.m[2][0] + v.w * M.m[3][0],
			v.x * M.m[0][1] + v.y * M.m[1][1] + v.z * M.m[2][1] + v.w * M.m[3][1],
			v.x * M.m[0][2] + v.y * M.m[1][2] + v.z * M.m[2][2] + v.w * M.m[3][2],
			v.x * M.m[0][3] + v.y * M.m[1][3] + v.z * M.m[2][3] + v.w * M.m[3][3]
		};
	}

	static inline void transpose(const REX::W32::XMFLOAT4X4& in, REX::W32::XMFLOAT4X4& out)
	{
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				out.m[c][r] = in.m[r][c];
	}

	struct Setup  //expanded version of BGS lens flare system
	{
		class LF_PassData
		{
		private:
			uint64_t shaderdesc;
			uint64_t active;
			uint64_t numpasses;
			uint64_t weatherpass;
			uint64_t uncondpass;
			uint64_t pad[3] = {};
			uint64_t* enginerefs_ptr = nullptr;

			std::array<uint64_t, 4> enginerefs{ 1, 8, 0, 0 };

		public:
			LF_PassData(uint64_t desc, uint64_t active, uint64_t numpasses, uint64_t weather, uint64_t nocond) :
				shaderdesc(desc), active(active), numpasses(numpasses), weatherpass(weather), uncondpass(nocond)
			{
				enginerefs_ptr = enginerefs.data();
			}

			int passesdone = 0;

			Shaders::Enum GetDesc() const { return (Shaders::Enum)shaderdesc; }
			int PassesLeft() const { return (int)numpasses - (int)passesdone; }

			void Toggle(bool value) { active = (uint64_t)value; }
			bool IsActive() const { return (bool)active; }
			bool IsWeatherShader() const { return (bool)weatherpass; }

			void CheckRefs()
			{
				enginerefs = { 1, 8, 0, 0 };
				enginerefs_ptr = enginerefs.data();
			}
		};

		class LF_RenderData
		{
		private:
			uint64_t head = 0x3F800000;
			std::unique_ptr<LF_PassData>* passarray_ptr = nullptr;
			uint64_t _pad[1] = {};
			uint64_t passcount = 0;
			uint64_t pad[2] = {};

			std::vector<std::unique_ptr<LF_PassData>> Passes;
			std::vector<std::unique_ptr<LF_PassData>> PassList;  //engine loops via passarray_ptr and renders for each
			std::vector<std::unique_ptr<LF_PassData>> NoSunPassList;
			size_t currentEffect = 0;

		public:
			LF_RenderData()
			{
				PassList.reserve(100);
				NoSunPassList.reserve(100);
			}

			struct Type
			{
				bool weather_pass = false;
				bool uncond_pass = false;
			};

			void SetupPass(int desc, bool active, int passes, Type type = {})
			{
				Passes.push_back(std::make_unique<LF_PassData>(desc, active, passes, type.weather_pass, type.uncond_pass));

				for (auto i = 0; i < passes; i++) {
					PassList.push_back(std::make_unique<LF_PassData>(*Passes.back()));
					if (type.weather_pass || type.uncond_pass)
						NoSunPassList.push_back(std::make_unique<LF_PassData>(*Passes.back()));
				}
			}

			void SetupRenderData()
			{
				passcount = PassList.size();
				passarray_ptr = PassList.data();
			}

			Shaders::Enum UpdateCurrentEffect()
			{
				if (Passes[currentEffect]->PassesLeft() == 0)
					if (currentEffect + 1 < Passes.size())
						currentEffect++;

				Passes[currentEffect]->passesdone++;
				return (Passes[currentEffect]->IsActive()) ? Passes[currentEffect]->GetDesc() : Shaders::Bypass;
			}

			void ResetEffects(bool sunVisible)
			{
				currentEffect = 0;
				for (auto& passn : Passes) passn->passesdone = 0;
				passarray_ptr = (sunVisible) ? PassList.data() : NoSunPassList.data();
				passcount = (sunVisible) ? PassList.size() : NoSunPassList.size();
			}

			LF_PassData& GetEffect(int desc)
			{
				for (auto& passn : Passes) {
					if (passn->GetDesc() == desc)
						return *passn.get();
				}
				throw std::out_of_range("");
			}

			LF_PassData& GetCurrentEffect()
			{
				return *Passes[currentEffect].get();
			}

			void CheckRefData()
			{
				for (auto& passn : PassList) {
					passn->CheckRefs();
				}
			}
		};
	};
	Setup::LF_RenderData* renderdata = nullptr;

	struct Hooks
	{
		struct LensFlare_CheckResources  //override main init/integ
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LensFlareVisibility_CheckRenderCondition
		{
			static void thunk(RE::NiCamera* camera, void* unk);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LensFlare_CheckRenderCondition  //setup buffers etc, if sun on screen, return true (override for non sun FX)
		{
#pragma warning(push)
#pragma warning(disable: 4189)
			static bool thunk(void* shader, RE::NiCamera* camera, uint64_t unk)
			{
				bool result = func(shader, camera, unk);
				return true;
			}
#pragma warning(pop)
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <RE::ImageSpaceManager::ImageSpaceEffectEnum EffectType>
		struct BSImagespaceShader_Render
		{
			static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LensFlare_AssignTexture  //remove lensflare texture init/assignment
		{
			static void thunk(void* previous, uint64_t current)
			{
				current = 0;
				func(previous = &current, current);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSSkyShader_SetupMaterial  //override sun glare, fetch sun scale, append occlusion LUT
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		//struct BSSkyShader_GetRenderPasses
		//{
		//	static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSGeometry* a_geometry, std::uint32_t a_arg2, RE::BSShaderAccumulator* a_accumulator);
		//	static inline REL::Relocation<decltype(thunk)> func;
		//};

		static void Install()
		{
			logger::info("[Lens Effects] Installed hooks");

			stl::detour_thunk<LensFlare_CheckResources>(REL::RelocationID(25772, 26327));
			stl::write_thunk_call<LensFlareVisibility_CheckRenderCondition>(REL::RelocationID(100274, 106988).address() + REL::Relocate(0x188, 0x195));
			stl::write_thunk_call<LensFlare_CheckRenderCondition>(REL::RelocationID(100281, 106995).address() + REL::Relocate(0x14, 0x16));
			stl::write_thunk_call<LensFlare_AssignTexture>(REL::RelocationID(100280, 106994).address() + REL::Relocate(0x4B, 0x4B));

			stl::write_vfunc<0x1, BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>>(RE::VTABLE_BSImagespaceShaderLensFlare[3]);
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);
		}
	};
};
