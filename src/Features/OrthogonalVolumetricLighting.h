#pragma once
#include "Feature.h"
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
	//virtual inline std::string_view GetShaderDefineName() override { return "LENS_EFFECTS"; }
	//virtual std::string_view GetCategory() const override { return "Post Process"; }
	virtual inline bool SupportsVR() override { return false; };  //

	virtual inline void PostPostLoad() override { Hooks::Install(); }
	virtual void SetupResources() override;
	virtual void CompileShaders();

	virtual void CheckOverride();
	virtual void LookupShader(int desc);

	virtual void SetupDownSampleExpo();
	virtual void SetupMinify();
	virtual void SetupShadowVolume();
	virtual void SetupScatteringVolume();
	virtual void SetupSliceMarch();
	virtual void SetupApplyVolume();
	virtual void SetupOutput();

	virtual void UpdateFrustum();
	virtual int UpdateMatrixCache();
	virtual void Override();

	virtual bool CheckFrameBuffer();

	virtual void SetupHorizontalFilter();
	virtual void SetupVerticalFilter();

	D3D11_VIEWPORT viewPort[4];
	ConstantBuffer* ESMCBuffer = nullptr;
	ID3D11SamplerState* LinearSampler = nullptr;
	ID3D11SamplerState* PointSampler = nullptr;
	ID3D11SamplerState* DepthSampler = nullptr;

	ID3D11VertexShader* BypassVertexShader = nullptr;
	ID3D11PixelShader* DownSamplePS = nullptr;
	ID3D11PixelShader* MinifyPS = nullptr;
	ID3D11PixelShader* FilterPS = nullptr;

	ID3D11Texture2D* ExponentiateTex = nullptr;
	ID3D11Texture2D* MinifyTex = nullptr;
	ID3D11Texture2D* HorizontalTex = nullptr;
	ID3D11Texture2D* ESMTexture = nullptr;

	ID3D11ShaderResourceView* ExponentiateSRV = nullptr;
	ID3D11ShaderResourceView* MinifySRV = nullptr;
	ID3D11ShaderResourceView* HorizontalSRV = nullptr;
	ID3D11ShaderResourceView* ESM_SRV = nullptr;

	ID3D11RenderTargetView* ExponentiateRTV = nullptr;
	ID3D11RenderTargetView* MinifyRTV = nullptr;
	ID3D11RenderTargetView* HorizontalRTV = nullptr;
	ID3D11RenderTargetView* ESM_RTV = nullptr;

	float2 CSM_Size = float2(4096.0f, 4096.0f);
	float2 DownSampleExpo_AtlasSize = float2(1024.0f, 2048.0f);
	float DownSampleExpo_TileSize = 1024.0f;
	float2 ESM_AtlasSize = DownSampleExpo_AtlasSize;  //float2(256.0f, 512.0f); ///////////
	float ESM_TileSize = 256.0f;

	uint AtlasBorderPx = 2;  //
	uint ESM_EXP = 60;
	uint ESM_Scale = 65000;

	Microsoft::WRL::ComPtr<ID3D11Buffer> PrevFrameBuffer[2];
	ConstantBuffer* ShadowVolumeBuffer = nullptr;
	ID3D11BlendState* AddBlend = nullptr;

	ID3D11ComputeShader* GenerateShadowVolumeCS = nullptr;
	ID3D11ComputeShader* GenerateScatteringVolumeCS = nullptr;
	ID3D11ComputeShader* SliceMarchCS = nullptr;
	ID3D11PixelShader* ApplyVolumePS = nullptr;
	ID3D11PixelShader* OutputPS = nullptr;

	ID3D11Texture3D* ShadowVolume = nullptr;
	ID3D11Texture3D* PrevShadowVolume = nullptr;
	ID3D11Texture3D* ScatteringVolume = nullptr;
	ID3D11Texture3D* IntergrationVolume = nullptr;

	ID3D11UnorderedAccessView* ShadowVolumeUAV = nullptr;
	ID3D11UnorderedAccessView* PrevShadowVolumeUAV = nullptr;
	ID3D11UnorderedAccessView* ScatteringVolumeUAV = nullptr;
	ID3D11UnorderedAccessView* IntergrationVolumeUAV = nullptr;

	ID3D11ShaderResourceView* STBNoiseSRV = nullptr;
	ID3D11ShaderResourceView* ShadowVolumeSRV = nullptr;
	ID3D11ShaderResourceView* PrevShadowVolumeSRV = nullptr;
	ID3D11ShaderResourceView* ScatteringVolumeSRV = nullptr;
	ID3D11ShaderResourceView* IntergrationVolumeSRV = nullptr;

	ID3D11ShaderResourceView* InvRepartitionSRV = nullptr;
	ID3D11ShaderResourceView* RepartitionSRV = nullptr;

	ID3D11Texture2D* OutputTexture = nullptr;
	ID3D11ShaderResourceView* OutputSRV = nullptr;
	ID3D11RenderTargetView* OutputRTV = nullptr;

	//float4 volumeDimensions = float4(160, 88, 64, 0);
	float4 volumeDimensions = float4(320, 192, 90, 0);
	float4 noiseDimensions = float4(64, 64, 32, 0);

	float4 FrustumNearFar = float4(0.1f, 200.0f, 1.0f / 0.1f, volumeDimensions.z / std::log2(200.0f / 0.1f));
	float4 frustum[4];
	float4 cameraPosition;
	Matrix WorldFromUVZ;

	float4 PrevCameraData[2];
	int PrevMatrixIdx;

	//float AmbientTerm = 1.0; //
	float CellJitterValue = 0.55;  //
	float RayJitterValue = 0.28;   //

	bool overrideCalled = false;
	bool overrideShader = false;
	uint pass = 1;
	uint FrameIdx = 0;  //max?
	float2 screenSize;

	int overrideNum = 0;

	uintptr_t* skyrim_FlareData = nullptr;
	uint32_t* skyrim_RunFlarePtr = nullptr;

	void(__fastcall* LFApply_func)(RE::NiCamera*, void*, uint64_t) = nullptr;

	RE::NiCamera* BGSCamera = nullptr;
	void* BGSShader = nullptr;

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	struct Settings
	{
		float test = 1.0f;
	};
	Settings settings;

	struct alignas(16) ESMBuffer
	{
		uint slice;
		uint KernalWidth;
		uint ESM_EXP;
		uint ESM_Scale;
		float2 srcSize;
		float2 InvSrcSize;
		float2 dstSize;
		float _pad[2];
	};
	virtual ESMBuffer UpdateESMBuffer(uint slice);

	struct alignas(16) ShadowVolBuffer
	{
		Matrix frustum;
		float4 FrustumNearFar;
		float4 CameraPosition;
		float4 VolumeSize;
		float4 NoiseSize;
		float4 PrevCameraData;
		float2 ShadowAtlasSize;
		float CellJitterValue;
		float RayJitterValue;
		uint Frame;
		uint ESM_Scale;
		uint ESM_EXP;
		float _pad[1];
	};
	virtual ShadowVolBuffer UpdateShadowBuffer();

	virtual inline DirectX::XMFLOAT4A VectorToXMFloat(float4& value) { return DirectX::XMFLOAT4A(value.x, value.y, value.z, value.w); }
	virtual inline float LinearStep(float edge0, float edge1, float x) { return std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f); }

	struct Shaders
	{
		enum Enum
		{
			Bypass = 0,
			ExpDownSample = 1,
			Minify = 2,
			VerFilter = 3,
			HorFilter = 4,
			ShadowVolume = 5,
			ScatterVolume = 6,
			IntergrationVolume = 7,
			Apply = 8,
			Output = 9
		};
	};
	Shaders::Enum shaderdesc;

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

		static void Install()
		{
			logger::info("[Lens Effects] Installed hooks");

			stl::detour_thunk<LensFlare_CheckResources>(REL::RelocationID(25772, 26327));
			stl::write_thunk_call<LensFlareVisibility_CheckRenderCondition>(REL::RelocationID(100274, 106988).address() + REL::Relocate(0x188, 0x195));
			stl::write_thunk_call<LensFlare_CheckRenderCondition>(REL::RelocationID(100281, 106995).address() + REL::Relocate(0x14, 0x16));
			stl::write_thunk_call<LensFlare_AssignTexture>(REL::RelocationID(100280, 106994).address() + REL::Relocate(0x4B, 0x4B));

			stl::write_vfunc<0x1, BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>>(RE::VTABLE_BSImagespaceShaderLensFlare[3]);
		}
	};
};
