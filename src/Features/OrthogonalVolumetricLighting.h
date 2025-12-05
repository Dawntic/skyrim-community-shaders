#pragma once
#include "../Deferred.h"
#include "Feature.h"
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
	uint lightCascades = 2;
	void SetupCascadeTextures();
	bool patchCascade = false;
	bool patchCulling = false;

	float lightUpdateAngle = 0.02f;
	float cascadeSplit0 = 1000.0f;
	float cascadeSplit1 = 3500.0f;

	//void(__fastcall* BuildPlaneFromPointsCallBack)(RE::NiPlane&, RE::NiPoint3&, RE::NiPoint3&, RE::NiPoint3&) = nullptr;
	//void(__fastcall* SetPlaneFromPointAndNormal)(RE::NiPlane&, RE::NiPoint3&, RE::NiPoint3&) = nullptr;
	//void BuildPlaneFromPoints(RE::NiPlane& planeOut, const RE::NiPoint3& p1, const RE::NiPoint3& p2, const RE::NiPoint3& p3);

	virtual inline void DataLoaded() override
	{
		RE::GetINISetting("bLensFlare:Imagespace")->data.b = true;
		CSM_Size = RE::GetINISetting("iShadowMapResolution:Display")->data.u;
		EVSM_Size = CSM_Size / 4;

		SetupCascadeTextures();
	}

	virtual inline void PostPostLoad() override { Hooks::Install(); }
	virtual void SetupResources() override;
	virtual void CompileShaders();

	virtual void CheckOverride();
	virtual void LookupShader(int desc);
	virtual void PerFrameUpdate();
	virtual int UpdateMatrixCache();
	virtual bool CheckFrameBuffer();
	virtual void SetupShadowCascade();
	virtual REX::W32::XMFLOAT4X4 GetCascadeMatrix(REX::W32::XMFLOAT4X4& lightTransform);
	void UpdateShadowLightMatrices();
	DirectX::XMVECTOR QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees);
	//virtual void BuildCloudShadowMatrix();
	//void ExtractFrustumPlanes(RE::NiFrustumPlanes& outPlanes, const DirectX::XMMATRIX& viewProj);
	//DirectX::XMMATRIX GameViewProj;
	//DirectX::XMMATRIX GameViewProjTransed;
	void BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& camera);
	virtual void LogMatrix(std::string desc, DirectX::XMMATRIX inMatrix);
	virtual void LogVector(std::string desc, DirectX::XMVECTOR vec);
	//virtual DirectX::XMFLOAT4X4 ConvertTransMatrix(DirectX::XMFLOAT4X4& m);
	struct FrustumCorners
	{
		float3 corners[8];
	};
	//void BuildCascadeCameraCullingPlanes(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners, uint32_t splitCornerIndices[8],
	//	uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);

	struct CascadeData
	{
		float3 worldCorners[8];
		float3 cascadeTranslation;
		DirectX::XMFLOAT4X4 viewRotation;
		RE::NiFrustum frustum;
		DirectX::XMFLOAT4X4 viewProj;
	};
	CascadeData cascadeData[2];
	int cascadeIt = 0;

	virtual void SetupBypass();
	virtual void SetupScatteringVolume();
	virtual void SetupFilterPass();
	virtual void SetupSliceMarch();
	virtual void SetupApplyPass();
	//virtual void SetupCloudShadowMap();
	virtual void SetupEVSM();
	virtual void SetupShadowVolume();
	virtual void SetupMediaVolume();
	virtual void SetupPerlinNoise();
	virtual void SetupEVSMBlur();
	virtual void DrawFogMap();
	//virtual void SetupCloudESM();
	virtual void RenderShadowMap();
	virtual void RenderShadowMapDebug();

	D3D11_VIEWPORT viewPort[4];
	ID3D11RasterizerState* Rasterizer = nullptr;
	Microsoft::WRL::ComPtr<ID3D11Buffer> FrameBuffer[2];
	ConstantBuffer* VolumeCB = nullptr;
	ConstantBuffer* SettingsCB = nullptr;
	ConstantBuffer* ESMCBuffer = nullptr;
	ID3D11BlendState* AddBlend = nullptr;
	ID3D11SamplerState* LinearSampler = nullptr;
	ID3D11SamplerState* LinearMinSampler = nullptr;
	ID3D11SamplerState* PointSampler = nullptr;
	ID3D11SamplerState* AnisoLinear = nullptr;
	ID3D11SamplerState* AnisoWrapLinear = nullptr;

	//// Volumes ////////////
	ID3D11ComputeShader* GenerateScatteringVolumeCS = nullptr;
	ID3D11ComputeShader* FilterVolumeCS = nullptr;
	ID3D11ComputeShader* SliceMarchCS = nullptr;
	ID3D11ComputeShader* GenerateMediaVolumeCS = nullptr;

	ID3D11Texture3D* MediaVolume[2] = {};
	ID3D11UnorderedAccessView* MediaVolumeUAV[2] = {};
	ID3D11ShaderResourceView* MediaVolumeSRV[2] = {};

	ID3D11Texture3D* ScatteringVolume = nullptr;
	ID3D11UnorderedAccessView* ScatteringVolumeUAV = nullptr;
	ID3D11ShaderResourceView* ScatteringVolumeSRV = nullptr;

	ID3D11Texture3D* IntergrationVolume = nullptr;
	ID3D11UnorderedAccessView* IntergrationVolumeUAV = nullptr;
	ID3D11ShaderResourceView* IntergrationVolumeSRV = nullptr;

	ID3D11Texture3D* FilteringVolume[2] = {};
	ID3D11UnorderedAccessView* FilterVolumeUAV[2] = {};
	ID3D11ShaderResourceView* FilterVolumeSRV[2] = {};

	//// Shadows /////////////
	ID3D11ComputeShader* GenerateEVSMCS = nullptr;
	ID3D11ComputeShader* BlurEVSMCS = nullptr;
	ID3D11ComputeShader* GenerateShadowVolumeCS = nullptr;
	ID3D11ComputeShader* CloudShadowCS = nullptr;
	ID3D11VertexShader* CloudShadowVS = nullptr;
	ID3D11PixelShader* CloudShadowPS = nullptr;

	ID3D11Texture2D* CascadeTex = nullptr;
	ID3D11ShaderResourceView* CascadeSRV = nullptr;
	ID3D11DepthStencilView* CascadeDSV[4] = {};

	ID3D11Texture2D* CloudShadowTexture = nullptr;
	ID3D11RenderTargetView* CloudShadowRTV = nullptr;
	ID3D11ShaderResourceView* CloudShadowSRV = nullptr;

	ID3D11Texture2D* CloudShadowESMTexture = nullptr;
	ID3D11ShaderResourceView* CloudShadowESMSRV = nullptr;
	ID3D11UnorderedAccessView* CloudShadowESMUAV = nullptr;

	ID3D11Texture2D* EVSMTexture = nullptr;
	ID3D11UnorderedAccessView* EVSMUAV = nullptr;
	ID3D11ShaderResourceView* EVSMSRV = nullptr;

	ID3D11Texture2D* EVSMBlurTexture = nullptr;
	ID3D11UnorderedAccessView* EVSMBlurUAV = nullptr;
	ID3D11ShaderResourceView* EVSMBlurSRV = nullptr;

	ID3D11Texture3D* ShadowVolume[2] = {};
	ID3D11UnorderedAccessView* ShadowVolumeUAV[2] = {};
	ID3D11ShaderResourceView* ShadowVolumeSRV[2] = {};

	////
	ID3D11VertexShader* ShadowMapVS = nullptr;
	ID3D11PixelShader* ShadowMapPS = nullptr;
	ID3D11ComputeShader* ShadowMapDebugCS = nullptr;

	ID3D11Texture2D* ShadowVarianceDebugTex = nullptr;
	ID3D11UnorderedAccessView* ShadowVarianceDebugUAV = nullptr;
	ID3D11ShaderResourceView* ShadowVarianceDebugSRV = nullptr;

	//// Util /////////////
	ID3D11ComputeShader* GeneratePerlinCS = nullptr;
	ID3D11ComputeShader* DrawFogMapCS = nullptr;
	ID3D11VertexShader* BypassVertexShader = nullptr;

	ID3D11Texture3D* PerlinTex = nullptr;
	ID3D11UnorderedAccessView* PerlinUAV = nullptr;
	ID3D11ShaderResourceView* PerlinSRV = nullptr;

	ID3D11Texture2D* WorldMapTexture = nullptr;
	ID3D11ShaderResourceView* staticWorldMapSRV = nullptr;

	ID3D11Texture2D* UIFogMapTexture = nullptr;
	ID3D11ShaderResourceView* UIFogMapSRV = nullptr;
	ID3D11UnorderedAccessView* UIFogMapUAV = nullptr;

	ID3D11Texture2D* FogMapTexture = nullptr;
	ID3D11ShaderResourceView* FogMapSRV = nullptr;
	ID3D11UnorderedAccessView* FogMapUAV = nullptr;

	//// Misc /////////
	ID3D11PixelShader* ApplyVolumePS = nullptr;
	ID3D11PixelShader* OutputPS = nullptr;
	ID3D11ShaderResourceView* STBNoiseSRV = nullptr;

	ID3D11Texture2D* OutputTexture = nullptr;
	ID3D11ShaderResourceView* OutputSRV = nullptr;
	ID3D11RenderTargetView* OutputRTV = nullptr;

	bool swapShadowMapShader = false;
	bool CSMFinished = false;
	bool resetVariance = false;
	uint varianceFrames = 32;

	float4 frustumNearFar;
	float3 eyePositionWS;
	float4 cameraData;
	REX::W32::XMFLOAT4X4 directionalShadowCascadeMatrices[4] = {};

	struct LocalShadowLightTransform
	{
		REX::W32::XMFLOAT4X4 matrix = {};
		uint shadowmapIndex = 0;
		uint _pad[3] = {};
	};
	LocalShadowLightTransform localShadowLightMatrices[8];

	//REX::W32::XMFLOAT4X4 localShadowLightMatrices[16] = {};
	float4 shadowCascadeEndSplit = float4(0, 0, 0, 0);

	static constexpr float4 volumeDimensions = float4(240, 136, 68, 0);
	static constexpr float4 noiseDimensions = float4(64, 64, 32, 0);
	float2 fogMapSize;
	DirectX::XMFLOAT4X4 fogMapViewProj = DirectX::XMFLOAT4X4(
		1.19175, 0.00, 0.00, 0.00,
		0.00, 2.11867, 0.00073, 0.00,
		0.00, 0.00035, -1.00036, -128.04633,
		0.00, 0.00035, -1.00, 0.00);

	float2 screenSize;
	uint frameCounter = 0;
	bool overrideShader = false;
	bool swapOutputRT = false;

	int PrevMatrixIdx;
	bool FilterVolParity;
	bool shadowVolParity;
	bool mediaVolParity;

	uintptr_t* skyrim_FlareData = nullptr;
	uint32_t* skyrim_RunFlarePtr = nullptr;
	//RE::NiPoint3* skyrim_SunPosition = nullptr;
	inline static RE::NiPoint3* skyrim_SunPosition = nullptr;
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

	float2 CloudESM_Size = float2(2560, 1440);
	DirectX::XMFLOAT4X4 cloudShadowLSViewProj;
	DirectX::XMFLOAT4X4 cloudShadowLSViewProjInverse;
	float4 cloudOrigin = float4(1, 1, 1, 1);

	struct Settings
	{
		float extinction = 1;
		float anisotropy = 0.85;
		uint esmExponent = 2;
		float albedo = 1.0;

		float color_saturation = 1.0;

		float globalFogDensity = 0;
		float globalFogStartHeight = 0;
		float globalFogFalloffHeight = 0;

		//uint VarienceFrameIndex;
		//uint RunVarienceMapping;
		//uint DebugCascadeSplit;

		float blendOpp = false;

		float _pad[3];
		float4 fogMapData;
		float4 UIfogMapParams;
	};
	Settings settings;

	struct alignas(16) VolumeBuffer
	{
		REX::W32::XMFLOAT4X4 directionalShadowCascadeMatrices[4];
		LocalShadowLightTransform localShadowLightMatrices[8];
		DirectX::XMFLOAT4X4 fogMapMatrix;

		float4 shadowCascadeEndSplit;
		float4 frustumNearFar;
		float4 CameraWSPos;
		float4 cameraData;

		float4 EVSMData;
		float4 heightMapParams;
		float4 heightMapZRange;

		float4 VolumeSize;
		float4 NoiseSize;

		uint frameCounter;
		uint boardCondition;

		float _pad[2];
	};
	virtual VolumeBuffer UpdateVolumeBuffer();

	struct alignas(16) SettingsBuffer
	{
		Settings cbsettings;
	};
	virtual SettingsBuffer UpdateSettingsBuffer();

	struct Shaders
	{
		enum Enum
		{
			Bypass = 0,
			ShadowEVSM = 1,
			ShadowEVSMBlur = 2,
			ShadowVolume = 5,
			ScatterVolume = 6,
			IntergrationVolume = 7,
			FilterVolume = 8,
			Apply = 9,
			Output = 10,
			RenderCloudMap = 11,
			MediaVolume = 12,
			Perlin = 13,
			CloudESM = 14,
			RenderShadowMap = 15,
			RenderShadowMapDebug = 16
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

		struct BSSkyShader_GetRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSGeometry* a_geometry, std::uint32_t a_arg2, RE::BSShaderAccumulator* a_accumulator);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SetShadowMapCount
		{
			static void thunk(RE::BSShadowLight* light, uint64_t numLights);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowDirectionalLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* light, void* unk);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowDirectionalLight_SetFrameCamera
		{
			static bool thunk(RE::BSShadowDirectionalLight* a_light, RE::NiCamera& a_camera);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes
		{
			static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond
		{
			struct FrustumSplit
			{
				RE::NiPoint3 nearFace[4];
				RE::NiPoint3 farFace[4];
			};

			static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowDirectionalLight_SetCameraRuntimeData2
		{
			static void thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowDirectionalLight_CreateFrustum
		{
			static void thunk(RE::NiFrustum& frustum, float left, float right, float top, float farP, float bottom, float nearP, bool ortho);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			logger::info("[Lens Effects] Installed hooks");

			stl::detour_thunk<LensFlare_CheckResources>(REL::RelocationID(25772, 26327));
			stl::write_thunk_call<LensFlareVisibility_CheckRenderCondition>(REL::RelocationID(100274, 106988).address() + REL::Relocate(0x188, 0x195));
			stl::write_thunk_call<LensFlare_CheckRenderCondition>(REL::RelocationID(100281, 106995).address() + REL::Relocate(0x14, 0x16));
			stl::write_thunk_call<LensFlare_AssignTexture>(REL::RelocationID(100280, 106994).address() + REL::Relocate(0x4B, 0x4B));

			stl::write_vfunc<0x1, BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>>(RE::VTABLE_BSImagespaceShaderLensFlare[3]);
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);

			stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);
			stl::write_thunk_call<BSShadowDirectionalLight_CreateFrustum>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x23E5, 0x23E5));
			stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

			stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));           //First call
			stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //Second call
		}
	};
};

//stl::write_vfunc<0x2A, BSSkyShader_GetRenderPasses>(RE::VTABLE_BSSkyShaderProperty[0]);
//stl::detour_thunk<SetShadowMapCount>(REL::RelocationID(107599, 107599));
//stl::write_vfunc<0xA, BSShadowDirectionalLight_RenderShadowmaps>(RE::VTABLE_BSShadowDirectionalLight[0]);
//stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));                                   //set rotation and translation
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //override corners
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  ///FIRST ---------------------