#pragma once

struct Skylighting : Feature
{
private:
	static constexpr std::string_view MOD_ID = "139352";

public:
	virtual bool SupportsVR() override { return true; };

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

	struct Settings
	{
		float MaxZenith = 3.1415926f / 2.f;  // 90 deg
		float MinDiffuseVisibility = 0.1f;
		float MinSpecularVisibility = 0.1f;

		int2 cacheProgress = int2();
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
	winrt::com_ptr<ID3D11ShaderResourceView> stbn_vec3_2Dx1D_128x128x64;

	// misc parameters
	uint probeArrayDims[3] = { 256, 256, 128 };
	float occlusionDistance = 4096.f * 2.5f;  // 5 ugrids

	// cached variables
	bool queuedResetSkylighting = true;
	bool inOcclusion = false;
	REX::W32::XMFLOAT4X4 OcclusionTransform;
	float4 OcclusionDir;
	uint frameCount = 0;

	// caching system
	static constexpr uint DEPTH_CUBE_SIZE = 128;

	eastl::unique_ptr<Texture2D> depthCubemap = nullptr;
	eastl::unique_ptr<Texture2D> bentNormalMap = nullptr;

	ID3D11ComputeShader* bentNormalComputeShader = nullptr;

	struct alignas(16) CacheGenCBStruct
	{
		int2 BentNormalWritePx;
		int2 BentNormalTexSize;
		float4 CubemapParams;  // Dimension, 1.0 / Dimension,  Dimension^2, Dimension^2 * valid sides
		int CubeMapWriteFace;
		float _pad[3];
	};
	ConstantBuffer* cacheGenBuffer = nullptr;

	struct alignas(16) AlphaRefCBStruct
	{
		float AlphaTestRefRS;
		float _pad[3];
	};
	ConstantBuffer* clipRefOverrideBuffer = nullptr;

	struct INIConfig
	{
		std::pair<int*, int> frameClamp;
		std::pair<bool*, bool> frameLock;
		std::pair<bool*, bool> interval;
		std::pair<bool*, bool> borderLock;
		std::pair<float*, float> maxTime;
	};
	INIConfig cachedINIValues;

	bool buildingCache = true;
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";

	void TryLoadCacheProgress(std::string name, int2& outProgress);
	void CreateCachingResources(int2 totalCells);
	void GenerateWorldspaceCache();
	void SetWorldPosition(const int2& cellPos, const RE::NiPoint2 minXY, RE::NiPoint3& posSet);
	void BackupCacheProgress(int2 currentCellXY, std::string name);
	float GetRayIntersectionHeight(float3 pos);
	bool IsPositionValid(RE::NiPoint3 pos);
	void GenerateVisibilityCubemap();
	void GenerateBentNormal();
	void FinishCaching();

	float3 sampleCoordsWS = float3();
	int cubemapSide = 0;
	//

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
