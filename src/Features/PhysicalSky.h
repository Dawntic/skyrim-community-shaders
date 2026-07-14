#pragma once

struct PhysicalSky final : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	static PhysicalSky* GetSingleton()
	{
		static PhysicalSky singleton;
		return &singleton;
	}

	// Metadata
	inline std::string GetName() override { return "Physical Sky"; }
	std::string GetDisplayName() override { return T("feature.physical_sky.name", "Physical Sky"); }
	inline std::string GetShortName() override { return "PhysicalSky"; }
	inline std::string_view GetCategory() const override { return "Sky"; }
	inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.physical_sky.description", "Physically-based sky model for realistic sky gradients and other astronomical effects."),
			{
				T("feature.physical_sky.key_feature_1", "Physically-based atmosphere and aerial perspective."),
				T("feature.physical_sky.key_feature_2", "Procedural sun disk and celestial lighting controls."),
				T("feature.physical_sky.key_feature_3", "Worldspace whitelist and interior override support."),
				T("feature.physical_sky.key_feature_4", "Cloud relighting and silver lining controls."),
			}
		};
	}

	// Functionality
	inline std::string_view GetShaderDefineName() override { return "PHYSICAL_SKY"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	// Settings & UI
	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	void DrawSettings() override;
	void SettingsGeneral();
	void SettingsCelestials();
	void SettingsAtmosphere();
	void SettingsClouds();
	void SettingsDebug();

	// Resources
	void SetupResources() override;
	void ClearShaderCache() override;
	void CompileShaders();
	bool ShadersOK();

	// Draw
	void Reset() override;
	void EarlyPrepass() override;
	void ReflectionsPrepass() override;
	void Prepass() override;
	void GenerateLuts();
	void AccumShadow();
	inline void PostPostLoad() override { Hooks::Install(); }

	void CreateCloudResources();
	void RenderClouds();
	void CloudCompose();

	// Clouds

	bool par = false;

	float2 CLOUD_TEX_SIZE = float2(2560, 1440) * 0.5;
	ID3D11RasterizerState* rasterState = nullptr;
	ID3D11BlendState* additiveBlend = nullptr;

	static inline uint32_t bayerIndices4x4[16] = {
		0, 8, 2, 10,
		12, 4, 14, 6,
		3, 11, 1, 9,
		15, 7, 13, 5
	};

	// CB struct matching the shader
	struct alignas(16) CloudCB
	{
		float3 cameraPos;
		float groundRadius;
		float2 bayerPos;
		float atmTopRadius;
		float bottomRadius;

		float topRadius;
		float minDistance;
		float maxDistance;
		float coverage2;

		float coverage;
		float heightScale;
		float cloudType;
		float scroll;
	};

	// CB struct matching CloudDebugCB in CloudCommon.hlsli
	struct alignas(16) CloudDebugCB
	{
		uint debugSunTrMode;
		uint debugAmbientMode;
		float2 debugPad0;

		float3 debugColor;
		float sunGain;

		float ambientGain;
		float sunMsGain;
		float cloudTrMuMin;
		float cloudTrMuMax;

		float cloudTrRBot;
		float cloudTrRTop;
		float octaveAttenA;
		float octaveAttenB;

		float cloudScattering;
		float cloudExtinction;
		float2 debugPad1;
	};
	STATIC_ASSERT_ALIGNAS_16(CloudDebugCB);

	// Runtime-only lighting verification knobs (deliberately not serialized).
	struct CloudLightingSettings
	{
		uint debugSunTrMode = 0;   /**< 0 live | 1 force white | 2 force orange | 3 A/B global Tr LUT. */
		uint debugAmbientMode = 0; /**< 0 live | 1 DebugColor | 2 literal red | 3 red->blue height gradient. */
		float3 debugColor = { 1.f, 0.f, 1.f };
		float sunGain = 2.f;           /**< Debug gate for the direct sun term. 0 while validating ambient. */
		float ambientGain = 1.2f;      /**< Debug gate for the ambient term. */
		float sunMsGain = 1.8f;        /**< Flat gain on the sun path only (replaces CLOUD_MS_GAIN). */
		float octaveAttenA = .7f;      /**< Wrenninge octave extinction attenuation. */
		float octaveAttenB = .6f;      /**< Wrenninge octave energy attenuation. */
		bool showDebugOverlay = false; /**< Blit the sun-Tr LUT + ambient swatches into a screen corner. */
		float cloudScattering = 24.9f; /**< km^-1. Spectrally neutral droplets: albedo = scattering / extinction ~ 0.996. */
		float cloudExtinction = 25.f;  /**< km^-1. */
	};
	CloudLightingSettings cloudLighting;

	struct CloudSettings
	{
		//bool isEnabled = true;         /**< Is physically based volumetric clouds rendering enabled. */
		//bool renderShadows = true;     /**< Render cloud shadows to the shadow buffer. */
		float bottomRadius = 1.0f; /**< Stratus and cumulus clouds start height. (km) */
		float topRadius = 2.4f;    /**< Stratus and cumulus clouds end height. (km) */
		float minDistance = 0.0f;  /**< Clouds volume tracing offset in front of camera. (km) */
		float maxDistance = 30.0f; /**< Maximum clouds volume tracing distance. (km) */
		float coverage = 0.6f;     /**< Amount of cumulus clouds. (Clear or cloudy weather) */
		float heightScale = 0.95f; /**< Amount of cirrus clouds. (Clear or cloudy weather) */
		float cloudType = 0.8f;    /**< Temperature difference between layers. (Storm clouds) */
		float coverage2 = 0.0f;    /**< Custom current time value. (For a multiplayer sync) */
		float scroll = 0.0f;       /**< Noise z offset; drag/animate to evolve the cloud pattern. */
								   //bool noDelay = false;          /**< Make all computation in one frame. (Expensive!) */
	};
	CloudSettings cloudSettings;

	eastl::unique_ptr<Texture2D> cloudColorTex[2] = { nullptr, nullptr };
	eastl::unique_ptr<Texture2D> cloudDepthTex[2] = { nullptr, nullptr };

	eastl::unique_ptr<Texture2D> disoccTex = nullptr;

	ID3D11PixelShader* cloudShader = nullptr;
	ID3D11VertexShader* cloudVShader = nullptr;
	ID3D11PixelShader* cloudBlendShader = nullptr;
	ID3D11PixelShader* cloudDebugBlitShader = nullptr;

	ConstantBuffer* cloudBuffer = nullptr;
	ConstantBuffer* cloudDebugBuffer = nullptr;

	winrt::com_ptr<ID3D11ShaderResourceView> dataFieldsSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> vertProfileSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> noiseShapeSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> cirrusShapeSRV;

	winrt::com_ptr<ID3D11ShaderResourceView> cloudBaseSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> cloudDetailSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> weatherMapSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> curlNoiseSRV;

	bool overrideShader = false;

	////////////////////////////////////////////////// Feature Specific Data
	constexpr static uint16_t kTrLutW = 256;
	constexpr static uint16_t kTrLutH = 64;
	constexpr static uint16_t kMsLutW = 32;
	constexpr static uint16_t kMsLutH = 32;
	constexpr static uint16_t kSvLutW = 200;
	constexpr static uint16_t kSvLutH = 150;
	constexpr static uint16_t kApLutW = 32;
	constexpr static uint16_t kApLutH = 32;
	constexpr static uint16_t kApLutD = 32;
	constexpr static uint16_t kCloudTrLutW = 64;
	constexpr static uint16_t kCloudTrLutH = 32;

	struct WorldspaceInfo
	{
		float zBottom = -14500.f;
	};

	struct Settings
	{
		bool enabled = true;
		bool enableAllExteriorCells = false;
		bool forceEnableAllInteriorCells = false;
		bool overrideDirLight = true;
		bool lightSkyStatics = true;
		float skyStaticsBrightness = 1.0f;
		bool halfResApShadow = false;
		int tonemapper = 2;
		float vanillaMix = 0;
		float trMix = 1;
		float apLumMix = 1;
		float apTrMix = 1;

		float2 cloudShadowRemapRange = float2{ 0, 1.f };

		float3 sunlightColor = float3{ 1.0f, 0.97f, 0.95f } * 10.f;
		float3 masserColor = float3{ 1.0f, 0.6f, 0.6f } * 0.1f;
		float3 secundaColor = float3{ 0.8f, 1.0f, 1.0f } * 0.05f;

		bool proceduralSun = true;
		float sunDiskRad = DirectX::XMConvertToRadians(0.53f);

		std::map<std::string, WorldspaceInfo> worldspaceWhitelist = {
			{ "Tamriel", { -14500.f } },
			{ "WindhelmWorld", { -14500.f } },
			{ "RiftenWorld", { -14500.f } },
			{ "MarkarthWorld", { -14500.f } },
			{ "WhiterunWorld", { -14500.f } },
			{ "SolitudeWorld", { -14500.f } },
			{ "WhiterunDragonsreachWorld", { -14500.f } },
			{ "DLC01FalmerValley", { 3000.f } },
			{ "DLC2SolstheimWorld", { 256.f } }
		};
		float fallbackZBottom = 0.f;
		float3 groundAlbedo = { .2f, .2f, .2f };

		float planetRadius = 6.36e3f;      // in km
		float atmosphereRadius = 6.42e3f;  // in km

		float rayleighFalloff = 0.05f;                    //1 / 8.69645f;                    // in km^-1
		float3 rayleighScatter = { 4.0f, 12.0f, 29.0f };  //{ 6.6049f, 12.345f, 29.413f };  // in megameter^-1
		float aerosolFalloff = 0.7f;                      //1 / 1.2f;
		float aerosolPhaseG = 0.8f;
		float3 aerosolScatter = { 39.96f, 39.96f, 39.96f };
		float3 aerosolAbsorption = { 4.44f, 4.44f, 4.44f };
		float ozoneAltitude = 22.3499f + 35.66071f * .5f;  // in km
		float ozoneThickness = 35.66071f;
		float3 ozoneAbsorption = { 2.2911f, 1.5404f, 0 };

		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;
		float silverLiningSpread = 0.f;
	} settings;

	struct CbData
	{
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		int tonemapper;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG;  //
		float3 aerosolScatter;
		uint halfResApShadow;  //
		float3 aerosolAbsorption;

		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //

		// CLOUDS (VANILLA)
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;
		float silverLiningSpread;  //

		// SETTINGS
		uint lightSkyStatics;
		float skyStaticsBrightness;
		uint pad0[2];

		// CLOUD LUT WINDOW (LUTGEN 4/5)
		// mu is dimensionless; radii are planet-center-relative game units to
		// match rPlanet (the cloud raymarcher samples the same normalized axes
		// with its km-scale values).
		float cloudTrMuMin;
		float cloudTrMuMax;
		float cloudTrRBot;
		float cloudTrRTop;
	} cbData;
	STATIC_ASSERT_ALIGNAS_16(CbData);

	eastl::unique_ptr<Texture2D> texTrLut = nullptr;  // transmittance
	eastl::unique_ptr<Texture2D> texMsLut = nullptr;  // multiscattering
	eastl::unique_ptr<Texture2D> texSvLut = nullptr;  // sky view
	eastl::unique_ptr<Texture3D> texApLut = nullptr;  // aerial perspective
	eastl::unique_ptr<Texture2D> texApShadow = nullptr;
	eastl::unique_ptr<Texture2D> texCloudSunTr = nullptr;    // windowed cloud sun transmittance (LUTGEN 4)
	eastl::unique_ptr<Texture2D> texCloudAmbient = nullptr;  // cloud ambient endpoints, 2x1 (LUTGEN 5)

	winrt::com_ptr<ID3D11SamplerState> sampTr = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampSv = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampNoise = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csMsLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSvLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csApLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csCloudTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csCloudAmbLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccum = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccumHalfRes = nullptr;

	ID3D11SamplerState* originalPSSamplers[2] = { nullptr, nullptr };

	void ModifySky();
	void RestoreSamplers();
	struct Hooks
	{
		struct BSSkyShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSSkyShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct RenderSky
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::detour_thunk<RenderSky>(REL::RelocationID(107129, 107129));
			stl::write_vfunc<0x6, BSSkyShader_SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
			stl::write_vfunc<0x7, BSSkyShader_RestoreGeometry>(RE::VTABLE_BSSkyShader[0]);
		}
	};
};
