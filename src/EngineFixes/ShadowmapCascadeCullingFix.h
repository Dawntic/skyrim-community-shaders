#pragma once

struct ShadowmapCascadeCullingFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Culling Fix"; }

	void Install() override;

private:
	inline static float* gfSplitOverlap = nullptr;

	struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes
	{
		struct FrustumSplit
		{
			RE::NiPoint3 nearFace[4];
			RE::NiPoint3 farFace[4];
		};

		static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

/*
	struct CascadeData
	{
		float3 worldCorners[8];
		float3 cascadeTranslation;
		DirectX::XMFLOAT4X4 viewRotation;
		RE::NiFrustum frustum;
		DirectX::XMFLOAT4X4 viewProj;
	};
	CascadeData cascadeData[2];
	int cascadeIt = 0;


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

struct BSShadowDirectionalLight_SetFrameCamera
{
	static bool thunk(RE::BSShadowDirectionalLight* a_light, RE::NiCamera& a_camera);
	static inline REL::Relocation<decltype(thunk)> func;
};

struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes
{
	static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
	static inline REL::Relocation<decltype(thunk)> func;
};
struct BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond
{
	struct FrustumSplit
	{
		RE::NiPoint3 nearFace[4];
		RE::NiPoint3 farFace[4];
	};

	static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex);
	static inline REL::Relocation<decltype(thunk)> func;
};

struct BSShadowDirectionalLight_SetCameraRuntimeData2
{
	static void thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum);
	static inline REL::Relocation<decltype(thunk)> func;
};

struct BSShadowDirectionalLight_CreateFrustum
{
	static void thunk(RE::NiFrustum& frustum, float left, float right, float top, float farP, float bottom, float nearP, bool ortho);
	static inline REL::Relocation<decltype(thunk)> func;
};





stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);
stl::write_thunk_call<BSShadowDirectionalLight_CreateFrustum>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x23E5, 0x23E5));
stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));           //First call
stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //Second call
}




//stl::write_vfunc<0x2A, BSSkyShader_GetRenderPasses>(RE::VTABLE_BSSkyShaderProperty[0]);
//stl::detour_thunk<SetShadowMapCount>(REL::RelocationID(107599, 107599));
//stl::write_vfunc<0xA, BSShadowDirectionalLight_RenderShadowmaps>(RE::VTABLE_BSShadowDirectionalLight[0]);
//stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));                                   //set rotation and translation
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //override corners
//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  ///FIRST ---------------------
*/