#pragma once

// This overrides the shadow cascade rasterizers to fix issues with peter panning and self shadowing
struct ShadowmapRasterizerFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Rasterizer Fix"; }
	bool SupportsVR() override { return true; }

	bool Install() override;

	using RasterStateArray = ID3D11RasterizerState* [2][3][12][2];

	static void CloneRasterStates(RasterStateArray* inputArray, int cascade);
	static void CloneRasterStatesInterior(RasterStateArray* inputArray, int cascade);

	static constexpr uint maxCascades = 4;
	static inline uint numCascades = 0;

	static inline RasterStateArray* gRasterStates = nullptr;
	static inline RasterStateArray backupGameRasterStates = {};
	static inline RasterStateArray shadowmapRasterStates[maxCascades] = {};
	static inline RasterStateArray shadowmapInteriorRasterStates = {};

	static constexpr int firstCascadeDepthBias = 160;
	static constexpr float firstCascadeDepthBiasClamp = 1.0f;
	static constexpr float firstCascadeSlopeScaleBias = 3.2f;

	static constexpr int secondCascadeDepthBias = 100;
	static constexpr float secondCascadeDepthBiasClamp = 1.0f;
	static constexpr float secondCascadeSlopeScaleBias = 3.8f;

	static constexpr int thirdCascadeDepthBias = 100;
	static constexpr float thirdCascadeDepthBiasClamp = 1.0f;
	static constexpr float thirdCascadeSlopeScaleBias = 3.8f;

	static constexpr int fourthCascadeDepthBias = 100;
	static constexpr float fourthCascadeDepthBiasClamp = 1.0f;
	static constexpr float fourthCascadeSlopeScaleBias = 3.8f;

	static constexpr int interiorCascadeDepthBias = 150;
	static constexpr float interiorCascadeDepthBiasClamp = 1.0f;
	static constexpr float interiorCascadeSlopeScaleBias = 3.0f;

	struct ShadowMapRasterizerDescriptor
	{
		int rasterDepthBias;
		float rasterDepthBiasClamp;
		float rasterSlopeScaleBias;
		bool depthClipEnable = false;
		bool rasterCulling = true;
	};
	static void GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor desc);

	static inline ShadowMapRasterizerDescriptor cascadeDescriptors[maxCascades + 1] = {
		{ firstCascadeDepthBias, firstCascadeDepthBiasClamp, firstCascadeSlopeScaleBias },
		{ secondCascadeDepthBias, secondCascadeDepthBiasClamp, secondCascadeSlopeScaleBias },
		{ thirdCascadeDepthBias, thirdCascadeDepthBiasClamp, thirdCascadeSlopeScaleBias },
		{ fourthCascadeDepthBias, fourthCascadeDepthBiasClamp, fourthCascadeSlopeScaleBias },
		{ interiorCascadeDepthBias, interiorCascadeDepthBiasClamp, interiorCascadeSlopeScaleBias }
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, void* arg1, void* arg2, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Reload();
};
