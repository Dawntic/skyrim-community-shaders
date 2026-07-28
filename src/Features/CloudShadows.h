#pragma once

#include "Buffer.h"

struct CloudShadows : Feature
{
private:
	static constexpr std::string_view MOD_ID = "139185";

public:
	static constexpr int kMaxCloudLayers = 32;

	/** Longest mip chain the volumetric shadow map builds. Past ~64x64 the map no
	 *  longer resolves individual clouds, so further levels only cost dispatches. */
	static constexpr uint32_t kMaxVolumetricMips = 7;
	/** Must match kMinLightCos in VolumetricCloudShadowMapCS.hlsl. */
	static constexpr float kMinLightCos = 0.02f;
	/** Light zenith cosine above which the shadow map is at full strength; below it
	 *  the contribution ramps down to nothing at kMinLightCos. */
	static constexpr float kHorizonFadeCos = 0.10f;
	/** Width of the ramp back to "unshadowed" at the edge of the mapped square, in uv. */
	static constexpr float kVolumetricBorderFade = 0.04f;

	/** User-facing, serialized settings. */
	struct Settings
	{
		float Opacity = 0.8f;
		/** Project the Physical Sky volumetric cloud layer into a Beer-Lambert
		 *  shadow map instead of reading the vanilla cloud-plane cubemap. */
		bool EnableVolumetricShadowMap = true;
		/** Edge length of the shadow map, in texels. */
		uint32_t VolumetricResolution = 512;
		/** Half-size of the square the map covers around the camera, in km. */
		float VolumetricRangeKm = 30.0f;
		/** Raymarch steps through the cloud layer per texel. */
		uint32_t VolumetricSteps = 32;
		/** Extra softness on top of the distance-derived mip. */
		float VolumetricMipBias = 0.0f;
	};

	/** Layout must match SharedData::CloudShadowsSettings in SharedData.hlsli. */
	struct alignas(16) BufferData
	{
		float Opacity = 0.8f;
		uint32_t VolumetricEnabled = 0;
		float2 VolCenter = {};

		float2 VolLightDirXY = {};
		float VolRcpLightZ = 0.f;
		float VolRcpExtent = 0.f;

		float VolLayerBottomZ = 0.f;
		float VolLayerTopZ = 0.f;
		float VolMipScale = 0.f;
		float VolMaxMip = 0.f;

		float VolMipBias = 0.f;
		float VolRcpBorderFade = 0.f;
		float2 pad0 = {};
	};
	STATIC_ASSERT_ALIGNAS_16(BufferData);

	/** Per-dispatch constants for VolumetricCloudShadowMapCS.hlsl. */
	struct alignas(16) VolumetricShadowCB
	{
		float3 lightDir;
		float rcpStepCount;

		float2 mapCenterKm;
		float mapExtentKm;
		uint32_t stepCount;

		uint32_t dstDim[2];
		float maxPathKm;
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(VolumetricShadowCB);

	Settings settings;

	virtual inline std::string GetName() override { return "Cloud Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.cloud_shadows.name", "Cloud Shadows"); }
	virtual inline std::string GetShortName() override { return "CloudShadows"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	virtual inline std::string_view GetShaderDefineName() override { return "CLOUD_SHADOWS"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.cloud_shadows.description", "Adds realistic cloud shadows that move across the landscape, creating dynamic lighting changes as clouds pass overhead, enhancing atmospheric immersion."),
			{ T("feature.cloud_shadows.key_feature_1", "Dynamic cloud shadow projection on terrain and objects"),
				T("feature.cloud_shadows.key_feature_2", "Configurable shadow opacity for artistic control"),
				T("feature.cloud_shadows.key_feature_3", "Real-time shadow movement synchronized with cloud motion"),
				T("feature.cloud_shadows.key_feature_4", "Cubemap-based shadow calculation for accurate projection"),
				T("feature.cloud_shadows.key_feature_5", "Beer-Lambert shadow map for Physical Sky volumetric clouds") } };
	};

	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	bool overrideSky = false;
	void SkyShaderHacks();

	Texture2D* texCloudShadowLayers[kMaxCloudLayers] = {};
	ID3D11RenderTargetView* cloudShadowLayerRTVs[kMaxCloudLayers][6] = {};
	Texture2D* texCubemapCloudOccCopy = nullptr;
	Texture2D* texSelfShadowCopy = nullptr;

	UINT cubemapMipLevels = 1;
	int currentLayerForDraw = 0;

	uint32_t renderedLayersMask[6] = {};
	uint32_t globalRenderedMask = 0;
	int previouslyRenderedSide = -1;

	ID3D11BlendState* cloudShadowBlendState = nullptr;

	////////////////////////////////////////////////// Volumetric (Physical Sky) shadow map

	/** Beer-Lambert transmittance of the volumetric cloud layer, mip-mapped. */
	eastl::unique_ptr<Texture2D> texVolumetricShadow = nullptr;
	/** Ping-pong target for the separable blur over mip 0. */
	eastl::unique_ptr<Texture2D> texVolumetricShadowBlur = nullptr;

	/** Full-chain SRV; this is what gets bound for the sampling side. */
	winrt::com_ptr<ID3D11ShaderResourceView> volumetricShadowSrv = nullptr;
	/** Single-mip views, needed because the blur and downsample passes read one
	 *  level while writing another of the same resource. */
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> volumetricShadowMipSrvs;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> volumetricShadowMipUavs;

	winrt::com_ptr<ID3D11ComputeShader> csVolumetricTrace = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolumetricBlurH = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolumetricBlurV = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csVolumetricDownsample = nullptr;

	std::unique_ptr<ConstantBuffer> volumetricShadowBuffer = nullptr;

	uint32_t volumetricResolution = 0;
	uint32_t volumetricMipLevels = 0;

	/** Sampling-side parameters for this frame, mirrored into shared data. */
	BufferData volumetricParams{};
	bool volumetricActive = false;
	/** Ramps the shadow contribution down as the light approaches the horizon. */
	float volumetricHorizonFade = 0.f;
	/** Generation-side geometry, in the km-scale space the cloud raymarcher uses. */
	float2 volumetricCenterKm = {};
	float volumetricExtentKm = 0.f;
	/** Normalized direction toward the directional light, pointing away from the ground. */
	float3 volumetricLightDir = {};

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileShaders();
	void CreateVolumetricResources();

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	BufferData GetCommonBufferData();

	void CheckResourcesSide(int side);
	void PropagateToCompletion(int side);
	int FindCloudLayer(RE::BSRenderPass* Pass);
	void ModifySky(RE::BSRenderPass* Pass);

	void UpdateVolumetricParams();
	void RenderVolumetricShadowMap();
	void BindVolumetricShadowMap();

	virtual void ReflectionsPrepass() override;
	virtual void EarlyPrepass() override;

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSSkyShader_SetupMaterial
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);
			logger::info("[Cloud Shadows] Installed hooks");
		}
	};
};
