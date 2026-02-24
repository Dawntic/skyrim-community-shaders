#pragma once

struct ShadowmapMatrixFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Matrix Fix"; }
	bool SupportsVR() override { return false; }

	bool Install() override;

	static inline int constexpr maxCascades = 4;
	static inline uint cascadePxSize = 0;
	static inline int nCascades = 0;
	static inline int cascadeToRender = -1;
	static inline bool initialized = false;

	static inline float maxCascadeCoverageVS = 0;

	static inline float* gCascadeBlendDist = nullptr;
	//static inline float* gShadowDistance = nullptr;
	//static inline float* gInteriorShadowDistance = nullptr;

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

	static void BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera, const int index);
	static void SetPrimaryCullPlanes(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera);
	static void GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, const DirectX::XMMATRIX& viewProj);
	static DirectX::XMVECTOR QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees);
	static bool GeometryInsideShadowBound(RE::BSGeometry* geometry);

	static void BuildRootFrustum(Frustum& outputFrustum, const RE::NiFrustum& viewFrustum, const DirectX::XMMATRIX& rootWorld);
	static void SetCascadeSplit(CascadeBounds::Split& outputSplits, const RE::NiFrustum& viewFrustum);
	static void BuildLightFrustum(DirectX::XMMATRIX& outLightView, Frustum& outFrustum, const CascadeBounds::Split& cascadeSplits, const Frustum& rootFrustum, const DirectX::XMVECTOR& lightDirection);
	static void BuildCascadeBoundingSphere(CascadeBounds::Sphere& outSphere, const Frustum& lightFrustum);
	static void BuildCascadeAABB(CascadeBounds::AABB& outBoundingBox, const DirectX::XMVECTOR& lightCameraPos, const CascadeBounds::Sphere& sphere);
	static void BuildCascadeProjectionMatrices(DirectX::XMMATRIX& outProj, DirectX::XMMATRIX& outCullingProj, const CascadeBounds::AABB& boundingBox);
	static void DisableCullingPlanes(const RE::BSShadowDirectionalLight* light, const int index);

	struct CascadeData
	{
		DirectX::XMFLOAT3 translation;
		DirectX::XMFLOAT4X4 viewMatrix;
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT4X4 viewProjTex;
		RE::NiFrustumPlanes cullingPlanes;
		float endDepthNDC;
		float startDepthNDC;
		//float width;
		float _pad[2];
		//RE::NiFrustum frustum;
		//DirectX::XMFLOAT4X4 projMatrix;   //tmp
		//DirectX::XMFLOAT4X4 worldMatrix;  //tmp
	};
	static inline CascadeData cascadeData[maxCascades] = {};

	struct alignas(16) ShadowDataCB
	{
		DirectX::XMFLOAT4X4 lightViewProj;
		DirectX::XMFLOAT4X4 lightView;  //TMP
		DirectX::XMFLOAT4X4 shadowmapViewProjUV[4];
		float cascadeSplitEnds[4];
		float cascadeSplitStarts[4];
		//float cascadeWidth[4];
		uint numCascades;
		float _pad[3];
	};
	static inline ConstantBuffer* shadowCascadeFixCB = nullptr;

	static inline RE::NiPoint3 XMVectorToNiPoint3(DirectX::XMVECTOR vector)
	{
		return RE::NiPoint3(DirectX::XMVectorGetX(vector), DirectX::XMVectorGetY(vector), DirectX::XMVectorGetZ(vector));
	}

	static inline DirectX::XMVECTOR NiPoint3ToXMVector(RE::NiPoint3 point)
	{
		return DirectX::XMVectorSet(point.x, point.y, point.z, 1);
	}

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

	struct CreateVolumetricCascadeStencilTarget
	{
		static void thunk(RE::BSGraphics::Renderer* renderer, RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL stencil, RE::BSGraphics::DepthStencilTargetProperties* prop);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};