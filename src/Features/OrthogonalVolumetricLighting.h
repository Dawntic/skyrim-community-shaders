#pragma once

#include "../Deferred.h"
#include "Feature.h"
#include "State.h"
#include "TerrainShadows.h"
#include "Util.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>

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

	virtual inline void PostPostLoad() override { Hooks::Install(); }
	virtual void SetupResources() override;
	void CompileShaders();

	virtual void EarlyPrepass();
	virtual void Prepass();

	//// Bent Normal Resources /////////////////////////////////////////////////////////////
	void IterateWorldFullDepth();
	void RenderMainDepth();
	bool UpdateCubemapCapture();
	void CopyDepthBufferToCubemap(int face);
	void GenerateBentNormalMap();

	void LoadHeightmap();
	void DisableCellPortals();
	bool IsPositionValid();
	float GetRayIntersectionHeight(float3 pos);
	float SampleHeightMap(int2 coords);

	static constexpr uint CUBE_SIZE = 512;
	static constexpr int2 BENT_NORMAL_SIZE = int2(1104, 768);

	//static constexpr float4 MapBounds = float4(-230000, 160000, 230000, -160000); // completed with float4(-200000, 130000, 230000, -180000);

	static constexpr int2 GRID_BOUND_TL = int2(-230000, 160000);
	static constexpr int2 GRID_BOUND_BR = int2(230000, -160000);

	struct IterationData
	{
		// Start xy - top left tex, near solitude
		// Finish xy - bottom right, near riften
		const int2 START = GRID_BOUND_TL;
		const int2 END = GRID_BOUND_BR;
		const int2 STEP = (END - START) / BENT_NORMAL_SIZE;

		const int TILE_SIZE = 10;
		const int2 TILE_TOTAL = BENT_NORMAL_SIZE + int2(TILE_SIZE, TILE_SIZE) - int2(1, 1) / int2(TILE_SIZE, TILE_SIZE);
		int2 local = int2(0, 0);
		int2 tile = int2(0, 0);
		int wave = 0;
	};
	IterationData cData;

	void TryLoadCacheProgress();
	void BackupCacheProgress();
	static inline const std::wstring bentNormalPath = L"Data\\Shaders\\OrthogonalVolumetricLighting\\BentNormalBackup\\bentNormalMap.dds";

	float3 coordsWS = float3(0, 0, 0);
	int2 coordsPX;

	int BUFFER_FRAMES = 0;

	bool runIterateWorld = false;
	bool disablePipeline = false;  //set after cubemaps
	bool disablePipelineUI = false;
	bool renderingMainDepth = false;
	bool reflectionCubeRender = false;

	struct Pass
	{
		RE::BSRenderPass* a_pass;
		uint32_t a_technique;
		bool a_alphaTest;
		uint32_t a_renderFlags;
	};
	std::vector<Pass> depthPasses;

	struct alignas(16) CacheGenCBStruct
	{
		int2 BentNormalWritePx;
		int2 BentNormalTexSize;
		float4 CubemapParams;
	};
	ConstantBuffer* cacheGenBuffer = nullptr;

	D3D11_VIEWPORT viewport = {};

	ID3D11ComputeShader* generateBentNormalCS = nullptr;
	ID3D11ComputeShader* copyDepthCS = nullptr;

	ID3D11Texture2D* mainDepthTex = nullptr;
	ID3D11DepthStencilView* mainDepthDSV = nullptr;
	ID3D11ShaderResourceView* mainDepthSRV = nullptr;

	eastl::unique_ptr<Texture2D> depthCubemap = nullptr;
	eastl::unique_ptr<Texture2D> bentNormalTex = nullptr;

	DirectX::ScratchImage heightMapTex;
	TerrainShadows::HeightMapMetadata* cachedHeightmap;
	/////////////////////////////////////////////////////////////////////////////////////

	//// Probe Grid Updating ////////////////////////////////////////////////////////////

	void UpdateSparseProbeGrid();

	static constexpr uint PROBE_ARRAY_SIZE = 128;

	struct alignas(16) GridUpdateCBStruct
	{
		DirectX::XMFLOAT4X4 InvViewProj;
		int2 GridTexSize;  // Shared //
		float2 InvGridTexSize;
		float2 GridMinCorner;  // Shared //
		float2 GridMaxCorner;  // Shared //
		float2 InvGridSpan;    // Shared //
		uint toggleLighting;
		uint toggleTrees;
		uint toggleGrass;
		uint toggleDeferred;
		uint toggleEffect;
		float _pad[1];
	};
	ConstantBuffer* gridUpdateBuffer = nullptr;

	GridUpdateCBStruct GetCommonBufferData();

	eastl::unique_ptr<Texture2D> probeGridArray = nullptr;

	ID3D11ComputeShader* updateSparseGridCS = nullptr;

	/////////////////////////////////////////////////////////////////////////////////////

	eastl::unique_ptr<Texture2D> placementMap = nullptr;
	bool test = true;

	uint frameCounter = 0;

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	struct Settings
	{
		uint bentNormalCacheProgress = 0;
		uint toggleLighting = true;
		uint toggleTrees = true;
		uint toggleGrass = true;
		uint toggleDeferred = true;
		uint toggleEffect = true;
		uint toggleAll = false;
	};
	Settings settings;

	virtual inline DirectX::XMFLOAT4A VectorToXMFloat(float4& value) { return DirectX::XMFLOAT4A(value.x, value.y, value.z, value.w); }
	virtual inline float LinearStep(float edge0, float edge1, float x) { return std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f); }

	struct Hooks
	{
		struct GetRenderPassArray
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSShaderProperty*, RE::BSGeometry*, std::uint32_t, RE::BSShaderAccumulator*);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderDepth
		{
			static void thunk(bool a1, bool a2)
			{
				auto& ovl = globals::features::orthogonalVolumetricLighting;

				ovl.disablePipeline = false;
				ovl.renderingMainDepth = true;
				func(a1, a2);
			};
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Only capture and render first dispatch since second is skinned and it fucks up the frame buffer
		struct BSShaderAccumulator_FinishAccumulatingDispatch
		{
			static void thunk(RE::BSGraphics::BSShaderAccumulator* shaderAccumulator, uint32_t renderFlags)
			{
				func(shaderAccumulator, renderFlags);

				auto& ovl = globals::features::orthogonalVolumetricLighting;

				if (ovl.renderingMainDepth) {
					ovl.renderingMainDepth = false;
					ovl.RenderMainDepth();
					ovl.disablePipeline = true;
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
			{
				auto& ovl = globals::features::orthogonalVolumetricLighting;

				if (ovl.renderingMainDepth) {
					ovl.depthPasses.push_back({ a_pass, a_technique, a_alphaTest, a_renderFlags });
				}

				// Don't render effects or reflections or game depth passes
				if (ovl.disablePipelineUI && (ovl.reflectionCubeRender || ovl.renderingMainDepth))
					return;

				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

#pragma warning(push)
#pragma warning(disable: 4100)
		struct BSCubeMapCamera_RenderCubemap
		{
			static void thunk(RE::NiAVObject* camera, int a2, bool a3, bool a4, bool a5)
			{
				auto& ovl = globals::features::orthogonalVolumetricLighting;
				ovl.reflectionCubeRender = true;
				func(camera, a2, a3, a4, a5);
				ovl.reflectionCubeRender = false;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
#pragma warning(pop)
		static void Install()
		{
			logger::info("[Lens Effects] Installed hooks");

			stl::write_vfunc<0x2A, Hooks::GetRenderPassArray>(RE::VTABLE_BSLightingShaderProperty[0]);
			stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));

			stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulatingDispatch>(RE::VTABLE_BSShaderAccumulator[0]);
			stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

			stl::write_vfunc<0x35, BSCubeMapCamera_RenderCubemap>(RE::VTABLE_BSCubeMapCamera[0]);
		}
	};
};
