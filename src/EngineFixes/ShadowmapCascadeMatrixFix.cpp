#include "ShadowmapCascadeMatrixFix.h"

#include "../Features/GrassCollision.h"
#include "../Features/TerrainBlending.h"
#include "DirectXCollision.h"

//TODO:
// Add checks for VR
// Catch ini settings and override shadow settings
// Add UI?
// Support 4 cascades
// Split overlap
// Culling breaks at low texel size - high shadow rez
// Figure out how to handle shader flag issue

void ShadowmapMatrixFix::Install()
{
	// This sets up the cascade and culling cameras
	stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);

	//Render a cascade      -same hook as raster fix
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6));

	// This clears the current frustum - we use it to set a new view matrix and translation
	stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

	// Culls against the min near and max far plane of any cascade
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  //First call    need SE addr

	// Culls the individual cascade frustum
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //Second call

	// Fill VL shadows call
	REL::safe_fill(REL::RelocationID(101495, 108489).address() + REL::Relocate(0x30, 0x30), REL::NOP, 76);

	// This function creates the final frustum for the current cascade
	//stl::write_thunk_call<BSShadowDirectionalLight_CreateFrustum>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x23E5, 0x23E5));

	//stl::detour_thunk<BSShaderPropertySetFlags>(REL::RelocationID(98893, 105540));
	//stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
}
#pragma warning(push)
#pragma warning(disable: 4100 4456 4189)

void ShadowmapMatrixFix::GetMainFrustum(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;
}

void ShadowmapMatrixFix::BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	// Get root camera params
	XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);
	RE::NiFrustum& viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;
	XMMATRIX rootWorld = XMLoadFloat3x3(reinterpret_cast<const XMFLOAT3X3*>(&rootCamera.world.rotate.entry));

	// Discretize light dir to mitegate time scale variance
	XMVECTOR lightDirection = XMVector3Normalize(NiPoint3ToXMVector(light->GetShadowDirectionalLightRuntimeData().sunVector));
	lightDirection = QuantizeLightDirection(lightDirection, settings.lightUpdateAngle);

	// Build light view matrix - matching game format w/o translation
	XMVECTOR up = XMVectorSet(0, 1, 0, 0);
	XMVECTOR forward = lightDirection;  // Light -> eye
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
	up = XMVector3Cross(right, forward);

	XMMATRIX lightWorld = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));

	if (settings.updateView) {
		XMStoreFloat4x4(&cascadeData[cascadeToRender].worldMatrix, lightWorld);
	} else {
		lightWorld = XMLoadFloat4x4(&cascadeData[cascadeToRender].worldMatrix);
	}

	XMMATRIX lightView = XMMatrixTranspose(lightWorld);  // ViewRot == InvWorld == TrspWorld

	// Build frustums
	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), rootWorld),

		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), rootWorld),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), rootWorld),
	};

	// Get dir light params
	//auto& tmp_split = light->GetShadowDirectionalLightRuntimeData().endSplitDistances;
	float cascadeSplits[4] = { (float)settings.splits[0], (float)settings.splits[1], (float)settings.splits[2], (float)settings.splits[3] };
	cascadeSplitViewDist[0] = cascadeSplits[0];
	cascadeSplitViewDist[1] = cascadeSplits[1];
	cascadeSplitViewDist[2] = cascadeSplits[2];
	cascadeSplitViewDist[3] = cascadeSplits[3];

	for (int i = 0; i < 4; i++)
		cascadeData[cascadeToRender].splitEndDepth = ViewDepthToNDC(cascadeSplits[i], viewFrustum);  // Make static //

	float split_near = (cascadeToRender == 0) ? 0.0f : LinearStep(viewFrustum.fNear, viewFrustum.fFar, cascadeSplits[cascadeToRender - 1]);  // These lin steps can be static //
	float split_far = LinearStep(viewFrustum.fNear, viewFrustum.fFar, cascadeSplits[cascadeToRender]);

	XMVECTOR lightFrustum[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_near), lightView),  // TR - near
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_far), lightView),   // TR - far
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_near), lightView),  // TL - near
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_far), lightView),   // TL - far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_near), lightView),  // BR - near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_far), lightView),   // BR - far
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_near), lightView),  // BL - near
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_far), lightView),   // BL - far
	};

	// Build bounding sphere
	XMVECTOR center = XMVectorZero();
	for (int j = 0; j < 8; ++j) {
		center = XMVectorAdd(center, lightFrustum[j]);
	}
	center = center / 8.0f;

	float radius = 0;
	for (int j = 0; j < 8; ++j) {
		radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(lightFrustum[j], center))));
	}

	// Build AABB from sphere
	XMVECTOR vRadius = XMVectorReplicate(radius);
	XMVECTOR cornerMin = XMVectorSubtract(center, vRadius);
	XMVECTOR cornerMax = XMVectorAdd(center, vRadius);

	// Add trans vec
	XMVECTOR lightCameraPos = XMVector3Transform(rootCameraPos, lightView);
	cornerMin = XMVectorAdd(cornerMin, lightCameraPos);
	cornerMax = XMVectorAdd(cornerMax, lightCameraPos);

	// Snap texel grid
	XMVECTOR extent = XMVectorReplicate(2.0f * radius);
	XMVECTOR texelSize = extent / float(cascadePxSize);
	cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
	cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;

	float3 clipMin = cornerMin;  // left, bottom, near
	float3 clipMax = cornerMax;  // right, top, far

	// Extend depth
	float centerZ = float3((clipMin + clipMax) * 0.5f).z;
	float halfExtentZ = abs(centerZ - clipMin.z);
	halfExtentZ *= settings.multiplerRange;  // The lower this is the more spread the shadow map depth values are so higher means less precision

	clipMin.z = centerZ - halfExtentZ;
	clipMax.z = centerZ + halfExtentZ;

	auto lightProj = XMMatrixOrthographicOffCenterLH(clipMin.x, clipMax.x, clipMin.y, clipMax.y, clipMin.z, clipMax.z);

	if (settings.updateProj) {
		XMStoreFloat4x4(&cascadeData[cascadeToRender].projMatrix, lightProj);
	} else {
		lightProj = XMLoadFloat4x4(&cascadeData[cascadeToRender].projMatrix);
	}

	auto viewProj = XMMatrixMultiply(lightView, lightProj);
	XMStoreFloat4x4(&cascadeData[cascadeToRender].viewProj, XMMatrixTranspose(viewProj));

	// Build shadow sampling matrix
	XMMATRIX texProj = XMMATRIX(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMStoreFloat4x4(&cascadeData[cascadeToRender].viewProjTex, XMMatrixTranspose(XMMatrixMultiply(viewProj, texProj)));

	// Set translation for geometry to transform against
	cascadeData[cascadeToRender].translation = rootCameraPos;

	// Build culling planes
	GetCullPlanesFromVPMatrix(cascadeData[cascadeToRender].cullingPlanes, viewProj);
}

// Step 1: Update cascade camera matrices, cull planes etc.
bool ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera::thunk(RE::BSShadowDirectionalLight* light, RE::NiCamera& inputCamera)
{
	if (!initialized && light) {
		cascadePxSize = RE::GetINISetting("iShadowMapResolution:Display")->data.u;
		nCascades = RE::GetINISetting("iNumSplits:Display")->data.u;
		if (!shadowCascadeFixCB)
			shadowCascadeFixCB = new ConstantBuffer(ConstantBufferDesc<ShadowDataCB>());

		initialized = true;
	}

	// -1 by default
	cascadeToRender = ++cascadeToRender < (int)nCascades ? cascadeToRender : 0;

	// Build the cascade we want to render this frame
	BuildShadowCascade(light, inputCamera);

	//GetMainFrustum(light, inputCamera);

	// Run game func to init and update the frame camera with the newly calculated cascade data
	bool funcReturn = func(light, inputCamera);

	// Stop other cascades from being rendered this frame
	// Note other methods to defer the accumulator dispatch cause recursion deadlocks
	for (int i = 0; i < (int)nCascades; i++) {
		if (i != cascadeToRender) {  //Cascade to render must be updated before this
			if (light) {
				if (auto cullingProcess = light->GetRuntimeData().shadowmapDescriptors[i].cullingProcess) {
					cullingProcess->customCullPlanes.cullingPlanes[0].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[1].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[2].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[3].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[4].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[5].constant = 0;
				}
			}
		}
	}

	return funcReturn;
}

// Subsequent call propagates local changes to the rest of the camera
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetCameraRuntimeData2::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	static uint counter = 0;

	cascadeCamera->local.rotate = RE::NiMatrix3();
	cascadeCamera->local.translate = XMVectorToNiPoint3(cascadeData[counter].translation);

	//cascadeCamera->GetRuntimeData2().minNearPlaneDist = -FLT_MAX;
	//cascadeCamera->GetRuntimeData2().maxFarNearRatio = -FLT_MIN;

	counter = ++counter < nCascades ? counter : 0;

	func(cascadeCamera, frustum);
}

void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(
	RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners, uint32_t splitCornerIndices[8],
	uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	//if (!globals::features::terrainBlending.disableCulling)
	//	outPlanes = maxExtentCullPlanes;
}

void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond::thunk(
	RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners,
	uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPosA, uint32_t cornerOffsetIndex)
{
	static uint counter = 0;

	if (!globals::features::terrainBlending.disableCulling)  //Cant disable now because time slicing
		outPlanes = cascadeData[counter].cullingPlanes;

	counter = ++counter < nCascades ? counter : 0;
}

// Step 2: Render a cascade, update cbuffer for shadowmask pass
void ShadowmapMatrixFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& desc, uint32_t* arg2, uint32_t flags)
{
	// Update cascade buffer
	ShadowDataCB data{};
	data.lightViewProj = cascadeData[cascadeToRender].viewProj;
	for (int i = 0; i < (int)nCascades; i++) {
		data.shadowmapViewProj[i] = cascadeData[i].viewProjTex;
		data.cascadeSplitEnds[i] = cascadeData[i].splitEndDepth;
	}
	data.numCascades = nCascades;
	shadowCascadeFixCB->Update(data);

	ID3D11Buffer* buffer = shadowCascadeFixCB->CB();
	globals::d3d::context->VSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);

	// Disable wind for now // TEMP
	if (auto manager = RE::BSTreeManager::GetSingleton()) {
		manager->windMagnitude = 0.0f;
	}

	// Only clear RT of cascade we are rendering this frame
	desc.clearRenderTarget = desc.shadowmapIndex == (uint)cascadeToRender;

	func(light, desc, arg2, flags);
}

//// UTIL ///////////////////////////////////////
void ShadowmapMatrixFix::GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, DirectX::XMMATRIX viewProj)
{
	using namespace DirectX;

	viewProj = XMMatrixTranspose(viewProj);

	XMVECTOR planes[6];
	planes[0] = XMVectorAdd(viewProj.r[3], viewProj.r[0]);
	planes[1] = XMVectorSubtract(viewProj.r[3], viewProj.r[0]);
	planes[2] = XMVectorAdd(viewProj.r[3], viewProj.r[1]);
	planes[3] = XMVectorSubtract(viewProj.r[3], viewProj.r[1]);
	planes[4] = viewProj.r[2];
	planes[5] = XMVectorSubtract(viewProj.r[3], viewProj.r[2]);

	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);

	for (uint32_t i = 0; i < 6; i++) {
		float len = XMVectorGetX(XMVector3Length(planes[i]));
		if (len > 1e-6f) {
			float invLen = 1.0f / len;

			// After unitize n dot p + d >= 0 (inside)
			// NiPlane wants n dot p = -d
			XMVECTOR normal = XMVectorScale(planes[i], invLen);
			auto constant = -XMVectorGetW(planes[i]) * invLen;

			outPlanes.cullingPlanes[i].normal = XMVectorToNiPoint3(normal);
			outPlanes.cullingPlanes[i].constant = constant;

			outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << i));
		}
	}
}

DirectX::XMVECTOR ShadowmapMatrixFix::QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees)  //0.05 - 0.1 works well
{
	using namespace DirectX;

	float stepRadians = XMConvertToRadians(stepDegrees);

	//Calculate azimuth and elevation
	float azimuth = atan2f(XMVectorGetX(lightDir), XMVectorGetZ(lightDir));
	float elevation = asinf(XMVectorGetY(lightDir));

	//Quantize angles to nearest step
	azimuth = roundf(azimuth / stepRadians) * stepRadians;
	elevation = roundf(elevation / stepRadians) * stepRadians;

	//Convert back to cart coords
	float cosElev = cosf(elevation);
	XMVECTOR quantized = XMVectorSet(sinf(azimuth) * cosElev, sinf(elevation), cosf(azimuth) * cosElev, 0.0f);

	return XMVector3Normalize(quantized);
}

bool ShadowmapMatrixFix::GeometryInsideShadowBound(RE::BSGeometry* geometry)
{
	auto pos = geometry->worldBound.center - RE::Main::WorldRootCamera()->world.translate;
	float dist = pos.Length() - geometry->worldBound.radius;
	float maxShadowDist = cascadeSplitViewDist[maxCascades - 1];
	return dist < maxShadowDist;
}