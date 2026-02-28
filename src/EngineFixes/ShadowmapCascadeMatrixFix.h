#pragma once

struct ShadowmapMatrixFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Matrix Fix"; }
	bool SupportsVR() override { return false; }

	bool Install() override;

	static inline int constexpr maxCascades = 4;
	static inline bool initialized = false;
	static inline uint cascadePxSize = 0;
	static inline int nCascades = 0;

	static inline float maxCascadeCoverageVS = 0;
	static inline float* gCascadeBlendDist = nullptr;

	static inline bool renderingCascades = false;
	static inline bool renderingVLCascades = false;

	static inline std::vector<RE::BSGeometry*> VLGeometry = {};

	static inline void PrintSetShaderFlags(uint64_t flag)
	{
		if (flag == 0) {
			logger::info("No shader flags set");
			return;
		}

		logger::info("Set shader flags (0x{:X}):", flag);

		struct FlagInfo
		{
			uint64_t value;
			const char* name;
		};

		static const FlagInfo flagNames[] = {
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kSpecular), "kSpecular" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned), "kSkinned" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTempRefraction), "kTempRefraction" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha), "kVertexAlpha" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteColor), "kGrayscaleToPaletteColor" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kGrayscaleToPaletteAlpha), "kGrayscaleToPaletteAlpha" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kFalloff), "kFalloff" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kEnvMap), "kEnvMap" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kReceiveShadows), "kReceiveShadows" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows), "kCastShadows" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kFace), "kFace" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kParallax), "kParallax" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals), "kModelSpaceNormals" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kNonProjectiveShadows), "kNonProjectiveShadows" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape), "kMultiTextureLandscape" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction), "kRefraction" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kRefractionFalloff), "kRefractionFalloff" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kEyeReflect), "kEyeReflect" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kHairTint), "kHairTint" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kScreendoorAlphaFade), "kScreendoorAlphaFade" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kLocalMapClear), "kLocalMapClear" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint), "kFaceGenRGBTint" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit), "kOwnEmit" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kProjectedUV), "kProjectedUV" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMultipleTextures), "kMultipleTextures" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kRemappableTextures), "kRemappableTextures" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kDecal), "kDecal" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal), "kDynamicDecal" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kParallaxOcclusion), "kParallaxOcclusion" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance), "kExternalEmittance" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kSoftEffect), "kSoftEffect" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferTest), "kZBufferTest" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite), "kZBufferWrite" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape), "kLODLandscape" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kLODObjects), "kLODObjects" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kNoFade), "kNoFade" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided), "kTwoSided" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors), "kVertexColors" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kGlowMap), "kGlowMap" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask), "kAssumeShadowmask" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kCharacterLighting), "kCharacterLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMultiIndexSnow), "kMultiIndexSnow" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kVertexLighting), "kVertexLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale), "kUniformScale" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kFitSlope), "kFitSlope" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kBillboard), "kBillboard" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kNoLODLandBlend), "kNoLODLandBlend" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kEnvmapLightFade), "kEnvmapLightFade" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kWireframe), "kWireframe" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kWeaponBlood), "kWeaponBlood" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kHideOnLocalMap), "kHideOnLocalMap" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kPremultAlpha), "kPremultAlpha" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kCloudLOD), "kCloudLOD" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kAnisotropicLighting), "kAnisotropicLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kNoTransparencyMultiSample), "kNoTransparencyMultiSample" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMenuScreen), "kMenuScreen" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kMultiLayerParallax), "kMultiLayerParallax" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kSoftLighting), "kSoftLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kRimLighting), "kRimLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kBackLighting), "kBackLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kSnow), "kSnow" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim), "kTreeAnim" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting), "kEffectLighting" },
			{ static_cast<uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kHDLODObjects), "kHDLODObjects" }
		};

		bool foundAny = false;
		for (const auto& flagInfo : flagNames) {
			if (flag & flagInfo.value) {
				logger::info("  - {}", flagInfo.name);
				foundAny = true;
			}
		}

		if (!foundAny) {
			logger::info("  - [Unknown/Invalid flags]");
		}
	}

	// Bitmask which cascades we render each frame
	// Slicing across more than 2 frames causes some motion lag for dragons
	static constexpr uint8_t cascadeMasks[][2] = {
		{}, {},            // There should always be more than 1 cascade
		{ 0b01, 0b10 },    // 2 cascades == frame0=[0], frame1=[1]
		{ 0b11, 0b100 },   // 3 cascades == frame0=[0,1], frame1=[2]
		{ 0b1001, 0b110 }  // 4 cascades == frame0=[0,3], frame1=[1,2]
	};
	static inline uint8_t activeCascades = 0;

	struct Frustum
	{
		DirectX::XMVECTOR corner[8];
	};

	struct CascadeBounds
	{
		struct Split
		{
			float splitVS[4];
			float endSplitNDC[4];
			float startSplitNDC[4];
			float splitLin[4];
		};
		Split splitDist;

		struct Sphere
		{
			DirectX::XMVECTOR center;
			float radius;
		};
		Sphere boundingSphere;

		struct AABB
		{
			float3 cornerMin;
			float3 cornerMax;
		};
		AABB boundingBox;
	};

	static inline Frustum primaryCullFrustum = {};
	static inline RE::NiFrustumPlanes primaryCullPlanes = RE::NiFrustumPlanes();

	static inline RE::NiFrustumPlanes backupPlanes[maxCascades] = {};

	static void BuildShadowCascadeCameraInput(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera, const int index);
	static void SetupPrimaryCullPlanes(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera);
	static void GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, const DirectX::XMMATRIX& viewProj);
	static DirectX::XMVECTOR QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees);
	static bool GeometryInsideShadowBound(RE::BSGeometry* geometry);

	static void BuildRootFrustum(Frustum& outputFrustum, const RE::NiFrustum& viewFrustum, const DirectX::XMMATRIX& rootWorld);
	static void SetCascadeSplit(CascadeBounds::Split& outputSplits, const RE::NiFrustum& viewFrustum);
	static void BuildLightFrustum(DirectX::XMMATRIX& outLightView, Frustum& outFrustum, const CascadeBounds::Split& cascadeSplits, const Frustum& rootFrustum, const DirectX::XMVECTOR& lightDirection, const int cascadeIndex);
	static void BuildCascadeBoundingSphere(CascadeBounds::Sphere& outSphere, const Frustum& lightFrustum);
	static void BuildCascadeAABB(CascadeBounds::AABB& outBoundingBox, const DirectX::XMVECTOR& lightCameraPos, const CascadeBounds::Sphere& sphere);
	static void BuildCascadeProjectionMatrices(DirectX::XMMATRIX& outProj, DirectX::XMMATRIX& outCullingProj, const CascadeBounds::AABB& boundingBox);
	static void DisableCullingPlanes(const RE::BSShadowDirectionalLight* light, const int index);
	static void EnableCullingPlanes(const RE::BSShadowDirectionalLight* light, const int index);

	struct CascadeData
	{
		DirectX::XMFLOAT3 translation;
		DirectX::XMFLOAT4X4 viewMatrix;
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT4X4 viewProjTex;
		RE::NiFrustumPlanes cullingPlanes;
		float endDepthNDC;
		float startDepthNDC;
		float _pad[2];
	};
	static inline CascadeData cascadeData[maxCascades] = {};

	struct alignas(16) ShadowDataCB
	{
		DirectX::XMFLOAT4X4 lightViewProj;
		DirectX::XMFLOAT4X4 shadowmapViewProjUV[4];
		float cascadeSplitEnds[4];
		float cascadeSplitStarts[4];
		uint numCascades;
		float _pad[3];
	};
	static inline ConstantBuffer* shadowCascadeFixCB = nullptr;

	static inline void LogMatrix(std::string desc, DirectX::XMMATRIX inMatrix)
	{
		DirectX::XMFLOAT4X4 matrix;
		XMStoreFloat4x4(&matrix, inMatrix);
		logger::info("{} row 1: {}, {}, {}, {}", desc, matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3]);
		logger::info("{} row 2: {}, {}, {}, {}", desc, matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3]);
		logger::info("{} row 3: {}, {}, {}, {}", desc, matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3]);
		logger::info("{} row 4: {}, {}, {}, {}", desc, matrix.m[3][0], matrix.m[3][1], matrix.m[3][2], matrix.m[3][3]);
	}
	static inline void LogMatrix(std::string desc, DirectX::XMFLOAT4X4 matrix)
	{
		logger::info("{} row 1: {}, {}, {}, {}", desc, matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3]);
		logger::info("{} row 2: {}, {}, {}, {}", desc, matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3]);
		logger::info("{} row 3: {}, {}, {}, {}", desc, matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3]);
		logger::info("{} row 4: {}, {}, {}, {}", desc, matrix.m[3][0], matrix.m[3][1], matrix.m[3][2], matrix.m[3][3]);
	}

	static inline void LogMatrix(const std::string& desc, const float matrix[4][4])
	{
		logger::info("{} row 1: {}, {}, {}, {}", desc,
			matrix[0][0], matrix[0][1], matrix[0][2], matrix[0][3]);

		logger::info("{} row 2: {}, {}, {}, {}", desc,
			matrix[1][0], matrix[1][1], matrix[1][2], matrix[1][3]);

		logger::info("{} row 3: {}, {}, {}, {}", desc,
			matrix[2][0], matrix[2][1], matrix[2][2], matrix[2][3]);

		logger::info("{} row 4: {}, {}, {}, {}", desc,
			matrix[3][0], matrix[3][1], matrix[3][2], matrix[3][3]);
	}

	static inline void LogVector(std::string desc, DirectX::XMVECTOR vec)
	{
		logger::info("{}: {}, {}, {}, {}", desc, DirectX::XMVectorGetX(vec), DirectX::XMVectorGetY(vec), DirectX::XMVectorGetZ(vec), DirectX::XMVectorGetW(vec));
	}

	static inline RE::NiPoint3 XMVectorToNiPoint3(DirectX::XMVECTOR vector)
	{
		return RE::NiPoint3(DirectX::XMVectorGetX(vector), DirectX::XMVectorGetY(vector), DirectX::XMVectorGetZ(vector));
	}

	static inline DirectX::XMVECTOR NiPoint3ToXMVector(RE::NiPoint3 point)
	{
		return DirectX::XMVectorSet(point.x, point.y, point.z, 1);
	}

	struct CreateVolumetricCascadeStencilTarget
	{
		static void thunk(RE::BSGraphics::Renderer* renderer, RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL stencil, RE::BSGraphics::DepthStencilTargetProperties* prop);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_SetFrameCamera
	{
		static bool thunk(RE::BSShadowDirectionalLight* a_light, RE::NiCamera& a_camera);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_SetFrameCamera_SetCameraRuntimeData2
	{
		static void thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct FrustumSplit
	{
		RE::NiPoint3 corners[8];
	};

	struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes
	{
		static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond
	{
		static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& arg1, uint32_t* arg2, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderVolumetricCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& arg1, uint32_t* arg2, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Test
	{
		static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSShaderProperty*, RE::BSGeometry*, std::uint32_t, RE::BSShaderAccumulator*);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
