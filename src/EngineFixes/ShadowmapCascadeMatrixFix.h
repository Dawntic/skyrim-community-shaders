#pragma once

struct ShadowmapMatrixFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Matrix Fix"; }
	void Install() override;

	static inline int constexpr maxCascades = 4;
	static inline uint cascadePxSize = 0;
	static inline uint nCascades = 0;
	static inline int cascadeToRender = -1;

	static void BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCameraNew);
	static void GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, DirectX::XMMATRIX viewProj, DirectX::XMVECTOR translate);
	static DirectX::XMVECTOR QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees);

	static void GetMainFrustum(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera);

	static inline ID3D11RasterizerState* currentRasterState = nullptr;
	static inline ID3D11RasterizerState* clonedRasterState = nullptr;
	static inline ID3D11DepthStencilState* clonedDepthStencilState = nullptr;
	static inline UINT currentStencilRef = 0;
	static inline D3D11_VIEWPORT clonedViewport;

	//static inline ID3D11Texture2D* cascadeTexv2 = nullptr;
	static inline ID3D11Texture2D* cascadeTex = nullptr;
	static inline ID3D11RenderTargetView* cascadeRT = nullptr;
	static inline ID3D11DepthStencilView* cascadeDSV = nullptr;
	static inline ID3D11ShaderResourceView* cascadeSRV = nullptr;

	static inline bool renderShadowmaps = false;
	static inline bool newFrame = false;

	struct CascadeData
	{
		DirectX::XMVECTOR translation;
		DirectX::XMFLOAT4X4 viewMatrix;
		DirectX::XMFLOAT4X4 viewProj;
		DirectX::XMFLOAT4X4 viewProjTex;
		RE::NiFrustum frustum;
		RE::NiFrustumPlanes cullingPlanes;
	};
	static inline CascadeData cascadeData[maxCascades] = {};

	static inline RE::NiFrustumPlanes maxExtentCullPlanes = RE::NiFrustumPlanes();

	struct alignas(16) ShadowDataCB
	{
		DirectX::XMFLOAT4X4 lightViewProj;
		DirectX::XMFLOAT4X4 shadowmapViewProj[4];
	};
	static inline ConstantBuffer* shadowDataCB = nullptr;

	static inline RE::NiPoint3 XMVectorToNiPoint3(DirectX::XMVECTOR vector)
	{
		using namespace DirectX;
		return RE::NiPoint3(XMVectorGetX(vector), XMVectorGetY(vector), XMVectorGetZ(vector));
	}

	static inline DirectX::XMVECTOR NiPoint3ToXMVector(RE::NiPoint3 point)
	{
		using namespace DirectX;
		return XMVectorSet(point.x, point.y, point.z, 1);
	}

	static inline DirectX::XMVECTOR XMVectorReverse(const DirectX::XMVECTOR& vec)
	{
		using namespace DirectX;
		return XMVectorSet(XMVectorGetW(vec), XMVectorGetZ(vec), XMVectorGetY(vec), XMVectorGetX(vec));
	}

	static inline float LinearStep(float edge0, float edge1, float x) { return std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f); }

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

	struct BSShadowDirectionalLight_SetCameraRuntimeData2
	{
		static void thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct BSShadowDirectionalLight_SetCameraRuntimeData2Test
	{
		static void thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct BSShadowDirectionalLight_CreateFrustum
	{
		static void thunk(RE::NiFrustum& frustum, float left, float right, float top, float farP, float bottom, float nearP, bool ortho);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_Mul
	{
		static bool thunk(RE::NiCamera& camera, RE::NiPoint3& cornerPosition, float& outX, float& outY, float& outZ, float thresh);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct BSShadowDirectionalLight_Mul_Precascade
	{
		static bool thunk(RE::NiCamera& camera, float3& cornerPosition, float& outX, float& outY, float& outZ, float thresh);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct FrustumSplit
	{
		//RE::NiPoint3 nearFace[4];
		//RE::NiPoint3 farFace[4];
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

	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& arg1, uint32_t* arg2, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetShadowMapCount
	{
		static void thunk(RE::BSShadowLight* light, uint64_t numLights);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_RenderShadowmaps
	{
		static void thunk(RE::BSShadowLight* light, void* a2);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShadowDirectionalLight_TestFunc
	{
		static RE::BSShadowLight* thunk(void* arg1, uint32_t arg2);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct AccumulateShadowmap
	{
		static void thunk(RE::NiCamera* camera, RE::NiAccumulator* accumulator, uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSShaderPropertySetFlags
	{
		static void thunk(RE::BSShaderProperty* prop, RE::BSShaderProperty::EShaderPropertyFlag8 a_flag, bool a_set);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

/*



	//void(__fastcall* BuildPlaneFromPointsCallBack)(RE::NiPlane&, RE::NiPoint3&, RE::NiPoint3&, RE::NiPoint3&) = nullptr;
	//void(__fastcall* SetPlaneFromPointAndNormal)(RE::NiPlane&, RE::NiPoint3&, RE::NiPoint3&) = nullptr;
	//void BuildPlaneFromPoints(RE::NiPlane& planeOut, const RE::NiPoint3& p1, const RE::NiPoint3& p2, const RE::NiPoint3& p3);
	struct FrustumCorners
	{
		float3 corners[8];
	};
	//void BuildCascadeCameraCullingPlanes(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners, uint32_t splitCornerIndices[8],
	//	uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);

	DirectX::XMVECTOR QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees);
	//virtual void BuildCloudShadowMatrix();
	//void ExtractFrustumPlanes(RE::NiFrustumPlanes& outPlanes, const DirectX::XMMATRIX& viewProj);
	//DirectX::XMMATRIX GameViewProj;
	//DirectX::XMMATRIX GameViewProjTransed;
	void BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& camera);
	virtual void LogMatrix(std::string desc, DirectX::XMMATRIX inMatrix);
	virtual void LogVector(std::string desc, DirectX::XMVECTOR vec);
	//virtual DirectX::XMFLOAT4X4 ConvertTransMatrix(DirectX::XMFLOAT4X4& m);



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








stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);



}




//stl::write_vfunc<0x2A, BSSkyShader_GetRenderPasses>(RE::VTABLE_BSSkyShaderProperty[0]);
//stl::detour_thunk<SetShadowMapCount>(REL::RelocationID(107599, 107599));
//stl::write_vfunc<0xA, BSShadowDirectionalLight_RenderShadowmaps>(RE::VTABLE_BSShadowDirectionalLight[0]);
//stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));                                   //set rotation and translation
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //override corners
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  ///FIRST ---------------------
*/