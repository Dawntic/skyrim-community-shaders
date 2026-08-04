#pragma once

#include "Buffer.h"
#include <filesystem>

struct TerrainShadows : public Feature
{
public:
	virtual inline std::string GetName() override { return "Terrain Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.terrain_shadows.name", "Terrain Shadows"); }
	virtual inline std::string GetShortName() override { return "TerrainShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.terrain_shadows.description", "Adds realistic shadow casting from terrain features using heightmap data to create accurate terrain shadows that enhance depth perception and visual realism."),
			{ T("feature.terrain_shadows.key_feature_1", "Heightmap-based terrain shadow calculation"),
				T("feature.terrain_shadows.key_feature_2", "Dynamic shadow updates based on sun position"),
				T("feature.terrain_shadows.key_feature_3", "Support for custom heightmap files"),
				T("feature.terrain_shadows.key_feature_4", "Real-time shadow preprocessing and computation"),
				T("feature.terrain_shadows.key_feature_5", "Integration with existing shadow systems") } };
	};

	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	struct Settings
	{
		bool EnableTerrainShadow = true;

		// Hierarchical traversal of the shadow height field (see TerrainShadowsTraversal.hlsli).
		// The chain is only built when this is on, and consumers fall back to point sampling
		// when ShadowMipLevels is reported as 0.
		bool EnableMinMaxMip = true;
		uint TraversalStartLevel = 4;
		uint TraversalMaxIterations = 64;
	} settings;

	// Validation state for the traversal debug view. Deliberately not serialised: this is a
	// diagnostic overlay and should never survive a restart.
	struct DebugSettings
	{
		bool EnableTraversalDebug = false;
		int TraversalDebugMode = 0;
		uint ReferenceSteps = 30;
		float Sigma = 0.f;
		float DifferenceGain = 10.f;
		float MaxRayLength = 40000.f;
	} debugSettings;

	bool needPrecompute = false;
	uint shadowUpdateIdx = 0;
	// Only true once a full shadow-map sweep has been reduced into the chain; consumers read
	// this through PerFrame::ShadowMipLevels.
	bool mipChainBuilt = false;

	struct HeightMapMetadata
	{
		std::wstring dir;
		std::string filename;
		std::string worldspace;
		float3 pos0, pos1;  // left-top-z=0 vs right-bottom-z=1
		float2 zRange;
	};
	std::unordered_map<std::string, HeightMapMetadata> heightmaps;
	HeightMapMetadata* cachedHeightmap;

	struct ShadowUpdateCB
	{
		float2 LightPxDir;   // direction on which light descends, from one pixel to next via dda
		float2 LightDeltaZ;  // per LightUVDir, upper penumbra and lower, should be negative
		uint StartPxCoord;
		float2 PxSize;
		uint pad0[1];
		float2 PosRange;
		float2 ZRange;
	} shadowUpdateCBData;
	static_assert(sizeof(ShadowUpdateCB) % 16 == 0);
	std::unique_ptr<ConstantBuffer> shadowUpdateCB = nullptr;

	struct MinMaxMipCB
	{
		uint SrcDim[2];
		uint DstDim[2];
	} minMaxMipCBData;
	static_assert(sizeof(MinMaxMipCB) % 16 == 0);
	std::unique_ptr<ConstantBuffer> minMaxMipCB = nullptr;

	struct alignas(16) TraversalDebugCB
	{
		float2 BufferDim;
		float2 RcpBufferDim;

		uint DebugMode;
		uint ReferenceSteps;
		float Sigma;
		float DifferenceGain;

		float MaxRayLength;
		uint MaxIterationsForDisplay;
		float2 pad0;
	} traversalDebugCBData;
	STATIC_ASSERT_ALIGNAS_16(TraversalDebugCB);
	std::unique_ptr<ConstantBuffer> traversalDebugCB = nullptr;

	struct alignas(16) PerFrame
	{
		uint EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
		uint TraversalStartLevel;
		uint TraversalMaxIterations;
		uint ShadowMipLevels;  // 0 when the min/max chain is unavailable, which disables traversal in shaders
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	PerFrame GetCommonBufferData();

	winrt::com_ptr<ID3D11ComputeShader> shadowUpdateProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> minMaxMipLevel0Program = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> minMaxMipLevelNProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> traversalDebugProgram = nullptr;

	std::unique_ptr<Texture2D> texHeightMap = nullptr;
	std::unique_ptr<Texture2D> texShadowHeight = nullptr;
	std::unique_ptr<Texture2D> texShadowMinMaxMip = nullptr;
	std::unique_ptr<Texture2D> texTraversalDebug = nullptr;

	// One view per level: the chain is reduced level by level, reading L-1 and writing L.
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> minMaxMipLevelSRVs;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> minMaxMipLevelUAVs;

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;

	bool IsHeightMapReady();
	uint GetShadowMipLevels() const;

	virtual void SetupResources() override;
	void ParseHeightmapPath(std::filesystem::path p, bool xlodgen_style);
	void CompileComputeShaders();

	virtual void DrawSettings() override;
	void DrawTraversalSettings();
	void DrawDebugSettings();

	virtual void EarlyPrepass() override;
	void LoadHeightmap();
	void Precompute();
	void UpdateShadow();
	void BuildMinMaxMip();
	void BindShadowResources();
	void UnbindShadowResources();

	virtual void Prepass() override;
	void DrawTraversalDebug();

	virtual void ReflectionsPrepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual inline void RestoreDefaultSettings() override { settings = {}; }
	virtual void ClearShaderCache() override;
	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };
};