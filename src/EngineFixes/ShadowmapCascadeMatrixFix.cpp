#include "ShadowmapCascadeMatrixFix.h"

#include "../Features/GrassCollision.h"
#include "../Features/TerrainBlending.h"
#include "DirectXCollision.h"

//TODO:
// Catch ini settings and override shadow settings
// Add UI?
// Support 4 cascades
// Split overlap

void ShadowmapMatrixFix::Install()
{
	// This sets up the cascade and culling cameras
	stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);

	//Render a cascade            -same hook as raster fix
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6));

	// This clears the current frustum - we use it to set a new view matrix and translation
	stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

	// This function creates the final frustum for the current cascade
	stl::write_thunk_call<BSShadowDirectionalLight_CreateFrustum>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x23E5, 0x23E5));

	// Culls against the min near and max far plane of any cascade
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  //First call    need SE addr

	// Culls the individual cascade frustum
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //Second call

	// Fill VL shadows call
	REL::safe_fill(REL::RelocationID(101495, 108489).address() + REL::Relocate(0x30, 0x30), REL::NOP, 76);

	//stl::detour_thunk<BSShaderPropertySetFlags>(REL::RelocationID(98893, 105540));

	//stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
}
#pragma warning(push)
#pragma warning(disable: 4100 4456 4189)

void ShadowmapMatrixFix::BSShaderPropertySetFlags::thunk(RE::BSShaderProperty* prop, RE::BSShaderProperty::EShaderPropertyFlag8 a_flag, bool a_set)
{
	logger::info("Test");
	//if (a_flag == RE::BSShaderProperty::EShaderPropertyFlag8::kReceiveShadows)

	if (renderShadowmaps && a_flag == RE::BSShaderProperty::EShaderPropertyFlag8::kReceiveShadows) {
		if (a_set)
			logger::info("Fuck dis: true");
		else {
			logger::info("Fuck dis: false");
		}
	}

	if (a_flag == RE::BSShaderProperty::EShaderPropertyFlag8::kReceiveShadows) {
		if (a_set)
			logger::info("Fuck dis: true 2");
		else {
			logger::info("Fuck dis: false 2");
		}
	}

	func(prop, a_flag, a_set);
}

//static XMVECTOR PrevlightDirection = lightDirection;
//static bool update = true;
//if (XMVector3NotEqual(lightDirection, PrevlightDirection)) {
//	PrevlightDirection = lightDirection;  //update light dir
//	update = true;
//} else {
//	update = false;
//}

void ShadowmapMatrixFix::GetMainFrustum(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	// Get dir light params
	auto& tmp_split = light->GetShadowDirectionalLightRuntimeData().endSplitDistances;
	float cascadeSplits[3] = { tmp_split[0], tmp_split[1], tmp_split[2] };

	// Due to the game time scale creating high temporal variance in the view matrix we mitegate this by quantizing light direction to discrete angular steps
	XMVECTOR lightDirection = XMVector3Normalize(NiPoint3ToXMVector(light->GetShadowDirectionalLightRuntimeData().sunVector));
	lightDirection = QuantizeLightDirection(lightDirection, settings.lightUpdateAngle);

	// Get root camera params
	RE::NiFrustum& viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;
	XMMATRIX worldRotMat = XMLoadFloat3x3(reinterpret_cast<const XMFLOAT3X3*>(&rootCamera.world.rotate.entry));
	XMMATRIX viewRotMat = XMMatrixTranspose(worldRotMat);
	XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);

	// Build light view matrix
	XMVECTOR up = XMVectorSet(0, 1, 0, 0);
	XMVECTOR forward = lightDirection;                                 // Direction light travels
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));  // Matches game format
	up = XMVector3Cross(right, forward);

	// Adding translation breaks texel snapping
	XMMATRIX lightWorld = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));
	XMMATRIX lightView = XMMatrixTranspose(lightWorld);  // ViewRot == InvWorldRot == TrspWorld

	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),

		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),
	};

	float split_near = viewFrustum.fNear / viewFrustum.fFar;
	float split_far = cascadeSplits[nCascades - 1] / viewFrustum.fFar;

	XMVECTOR lightFrustum[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_near), lightView),  // TR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_far), lightView),   // TR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_near), lightView),  // TL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_far), lightView),   // TL split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_near), lightView),  // BR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_far), lightView),   // BR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_near), lightView),  // BL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_far), lightView),   // BL split_far
	};

	// Build bounding sphere
	XMVECTOR center = XMVectorZero();
	float radius = 0;

	for (int j = 0; j < 8; ++j) {
		center = XMVectorAdd(center, lightFrustum[j]);
	}
	center = center / 8.0f;

	for (int j = 0; j < 8; ++j) {
		radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(lightFrustum[j], center))));
	}

	// Build AABB from sphere
	XMVECTOR vRadius = XMVectorReplicate(radius);
	XMVECTOR cornerMin = XMVectorSubtract(center, vRadius);
	XMVECTOR cornerMax = XMVectorAdd(center, vRadius);

	// Snap cascade to texel grid
	const XMVECTOR extent = XMVectorSubtract(cornerMax, cornerMin);
	const XMVECTOR texelSize = extent / float(cascadePxSize);
	cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
	cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;

	// Extend depth range
	center = (cornerMin + cornerMax) * 0.5f;
	float centerZ = XMVectorGetZ(center);
	float halfExtentZ = abs(centerZ - XMVectorGetZ(cornerMin));

	float ExtentZ = halfExtentZ * globals::features::terrainBlending.multiplerRange;  // The lower this is the more spread the shadow map depth values are so higher means less precision
	cornerMin = XMVectorSetZ(cornerMin, centerZ - ExtentZ);
	cornerMax = XMVectorSetZ(cornerMax, centerZ + ExtentZ);

	// Build view projection matrix
	float3 clipMin = cornerMin;  // left, bottom, near
	float3 clipMax = cornerMax;  // right, top, far
	auto lightProj = XMMatrixOrthographicOffCenterLH(clipMin.x, clipMax.x, clipMin.y, clipMax.y, clipMin.z, clipMax.z);
	auto viewProj = XMMatrixMultiply(lightView, lightProj);

	// Build culling planes
	GetCullPlanesFromVPMatrix(maxExtentCullPlanes, viewProj, rootCameraPos);
}

void ShadowmapMatrixFix::BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	// Get dir light params
	auto& tmp_split = light->GetShadowDirectionalLightRuntimeData().endSplitDistances;
	float cascadeSplits[3] = { tmp_split[0], tmp_split[1], tmp_split[2] };

	// Due to the game time scale creating high temporal variance in the view matrix we mitegate this by quantizing light direction to discrete angular steps
	XMVECTOR lightDirection = XMVector3Normalize(NiPoint3ToXMVector(light->GetShadowDirectionalLightRuntimeData().sunVector));
	lightDirection = QuantizeLightDirection(lightDirection, settings.lightUpdateAngle);

	// Get root camera params
	RE::NiFrustum& viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;
	XMMATRIX worldRotMat = XMLoadFloat3x3(reinterpret_cast<const XMFLOAT3X3*>(&rootCamera.world.rotate.entry));
	XMMATRIX viewRotMat = XMMatrixTranspose(worldRotMat);
	XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);

	// Build light view matrix
	XMVECTOR up = XMVectorSet(0, 1, 0, 0);
	XMVECTOR forward = lightDirection;                                 // Direction light travels
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));  // Matches game format
	up = XMVector3Cross(right, forward);

	// Adding translation breaks texel snapping
	XMMATRIX lightWorld = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));
	XMMATRIX lightView = XMMatrixTranspose(lightWorld);  // ViewRot == InvWorldRot == TrspWorld

	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),

		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),
	};

	// light frustum focus point must be relative to player look direction
	//for (int cascade = 0; cascade < int(nCascades); ++cascade) {
	int cascade = cascadeToRender;

	float split_near = (cascade == 0) ? viewFrustum.fNear / viewFrustum.fFar : cascadeSplits[cascade - 1] / viewFrustum.fFar;
	float split_far = cascadeSplits[cascade] / viewFrustum.fFar;

	XMVECTOR lightFrustum[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_near), lightView),  // TR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], split_far), lightView),   // TR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_near), lightView),  // TL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], split_far), lightView),   // TL split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_near), lightView),  // BR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], split_far), lightView),   // BR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_near), lightView),  // BL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], split_far), lightView),   // BL split_far
	};

	// Build bounding sphere
	XMVECTOR center = XMVectorZero();
	float radius = 0;

	for (int j = 0; j < 8; ++j) {
		center = XMVectorAdd(center, lightFrustum[j]);
	}
	center = center / 8.0f;

	for (int j = 0; j < 8; ++j) {
		radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(lightFrustum[j], center))));
	}

	// Build AABB from sphere
	XMVECTOR vRadius = XMVectorReplicate(radius);
	XMVECTOR cornerMin = XMVectorSubtract(center, vRadius);
	XMVECTOR cornerMax = XMVectorAdd(center, vRadius);

	// Snap cascade to texel grid
	const XMVECTOR extent = XMVectorSubtract(cornerMax, cornerMin);
	const XMVECTOR texelSize = extent / float(cascadePxSize);
	cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
	cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;

	// Extend depth range
	center = (cornerMin + cornerMax) * 0.5f;
	float centerZ = XMVectorGetZ(center);
	float halfExtentZ = abs(centerZ - XMVectorGetZ(cornerMin));

	float ExtentZ = halfExtentZ * globals::features::terrainBlending.multiplerRange;  // The lower this is the more spread the shadow map depth values are so higher means less precision
	cornerMin = XMVectorSetZ(cornerMin, centerZ - ExtentZ);
	cornerMax = XMVectorSetZ(cornerMax, centerZ + ExtentZ);

	// Build view projection matrix
	float3 clipMin = cornerMin;  // left, bottom, near
	float3 clipMax = cornerMax;  // right, top, far
	cascadeData[cascade].frustum.fLeft = clipMin.x;
	cascadeData[cascade].frustum.fRight = clipMax.x;
	cascadeData[cascade].frustum.fBottom = clipMin.y;
	cascadeData[cascade].frustum.fTop = clipMax.y;
	cascadeData[cascade].frustum.fNear = clipMin.z;
	cascadeData[cascade].frustum.fFar = clipMax.z;
	auto lightProj = XMMatrixOrthographicOffCenterLH(clipMin.x, clipMax.x, clipMin.y, clipMax.y, clipMin.z, clipMax.z);
	auto viewProj = XMMatrixMultiply(lightView, lightProj);

	// Needed when we go back to game cbuffer
	XMStoreFloat4x4(&cascadeData[cascade].viewMatrix, XMMatrixTranspose(lightView));

	XMStoreFloat4x4(&cascadeData[cascade].viewProj, XMMatrixTranspose(viewProj));

	// Build shadow sampling matrix
	XMMATRIX texProj = XMMATRIX(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f);

	XMStoreFloat4x4(&cascadeData[cascade].viewProjTex, XMMatrixTranspose(XMMatrixMultiply(viewProj, texProj)));

	// Transform camera to light space and snap to grid
	//XMVECTOR snappedTranslation = XMVector3Transform(rootCameraPos, lightView);
	//snappedTranslation = XMVectorFloor(snappedTranslation / texelSize) * texelSize;

	XMVECTOR cameraLS = XMVector3Transform(rootCameraPos, lightView);
	XMVECTOR texelSizeVec = XMVectorReplicate(float(settings.testScale));  //XMVectorReplicate((split_far * 2.0f) / float(cascadePxSize));
	XMVECTOR snappedCameraLS = XMVectorFloor(cameraLS / texelSizeVec) * texelSizeVec;
	XMVECTOR snappedCameraPosWorld = XMVector3Transform(snappedCameraLS, lightWorld);

	// We need to set a translation for geometry to transform against
	cascadeData[cascade].translation = snappedCameraPosWorld;  //XMVector3Transform(snappedTranslation, lightWorld);

	//logger::info("texel size: {}", XMVectorGetX(texelSize));

	// Build culling planes
	GetCullPlanesFromVPMatrix(cascadeData[cascade].cullingPlanes, viewProj, rootCameraPos);
	//}

	// Maybe i should add camera world position into the corners?
	// Store previous frame's viewProj (static or member variable)
	//static XMMATRIX prevViewProj = cascadeData[cascade].viewProj;
	// Inside cascade loop, after computing center from bounding sphere:
	// Project center using PREVIOUS frame's viewProj
	//XMVECTOR projectedCenter = XMVector3TransformCoord(center, prevViewProj[cascade]); //is using center corect?
	// Snap in clip space (ortho clip space is [-1,1] or depends on your projection)
	//float clipSpaceTexelSize = texelSize; //2.0f / cascadePxSize;     // 2.0 because clip space spans 2 units
	//float x = floor(XMVectorGetX(projectedCenter) / clipSpaceTexelSize) * clipSpaceTexelSize;
	//float y = floor(XMVectorGetY(projectedCenter) / clipSpaceTexelSize) * clipSpaceTexelSize;
	//float z = XMVectorGetZ(projectedCenter);  // don't snap Z
	// Transform back using inverse of PREVIOUS frame's viewProj
	//XMMATRIX invPrevViewProj = XMMatrixInverse(nullptr, prevViewProj[cascade]);
	//XMVECTOR snappedCenter = XMVector3Transform(XMVectorSet(x, y, z, 0), invPrevViewProj);
}

// Step 1: Update cascade camera matrices, cull planes etc.
bool ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera::thunk(RE::BSShadowDirectionalLight* light, RE::NiCamera& inputCamera)
{
	static bool init = true;
	if (init && light) {
		cascadePxSize = RE::GetINISetting("iShadowMapResolution:Display")->data.u;
		nCascades = RE::GetINISetting("iNumSplits:Display")->data.u;
		if (!shadowDataCB)
			shadowDataCB = new ConstantBuffer(ConstantBufferDesc<ShadowDataCB>());

		init = false;
	}

	// -1 by default
	cascadeToRender = ++cascadeToRender < (int)nCascades ? cascadeToRender : 0;

	newFrame = true;

	// Build the cascade we want to render this frame
	BuildShadowCascade(light, inputCamera);

	GetMainFrustum(light, inputCamera);

	// Run game func to init and update the frame camera with the newly calculated cascade data
	bool funcReturn = func(light, inputCamera);

	// Stop other cascades from being rendered this frame
	// Note other methods to defer the accumulator dispatch cause recursion deadlocks in batch rendering
	for (int i = 0; i < (int)nCascades; i++) {
		if (i != cascadeToRender) {  //Cascade to render must be updated before this
			if (light) {
				if (auto cullingProcess = light->GetRuntimeData().shadowmapDescriptors[i].cullingProcess) {
					cullingProcess->customCullPlanes.cullingPlanes[0].constant = 0;  //RE::NiFrustumPlanes();  Can we just set active culling planes to none?
					cullingProcess->customCullPlanes.cullingPlanes[1].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[2].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[3].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[4].constant = 0;
					cullingProcess->customCullPlanes.cullingPlanes[5].constant = 0;  //CHANGED
																					 //cullingProcess->customCullPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
				}
			}
		}
	}

	renderShadowmaps = true;

	return funcReturn;
}

// For some reason the viewProj built in SetFrameCamera() is recalulated from the camera data when updating the frame buffer
// So we update the camera with the relevent data here. The translation is also used in geometry matrices
// Subsequent call propagates local changes to the rest of the camera
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetCameraRuntimeData2::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	static uint counter = 0;

	// It makes sense if you don't think about it
	DirectX::XMFLOAT4X4 matrix = cascadeData[counter].viewMatrix;
	auto row0Mat = RE::NiPoint3(matrix._13, matrix._12, matrix._11);
	auto row1Mat = RE::NiPoint3(matrix._23, matrix._22, matrix._21);
	auto row2Mat = RE::NiPoint3(matrix._33, matrix._32, matrix._31);

	RE::NiMatrix3 rotation = RE::NiMatrix3(row0Mat, row1Mat, row2Mat);
	rotation.Transpose();

	cascadeCamera->local.rotate = rotation;
	cascadeCamera->local.translate = XMVectorToNiPoint3(cascadeData[counter].translation);

	cascadeCamera->GetRuntimeData2().minNearPlaneDist = -FLT_MAX;
	cascadeCamera->GetRuntimeData2().maxFarNearRatio = -FLT_MIN;

	counter = ++counter < nCascades ? counter : 0;

	func(cascadeCamera, frustum);
}

// Override the final cascade frustum
void ShadowmapMatrixFix::BSShadowDirectionalLight_CreateFrustum::thunk(RE::NiFrustum& frustum, float left, float right, float top, float bottom, float nearP, float farP, bool ortho)
{
	static uint counter = 0;

	auto frust = cascadeData[counter].frustum;
	func(frustum, frust.fLeft, frust.fRight, frust.fTop, frust.fBottom, frust.fNear, frust.fFar, ortho);

	counter = ++counter < nCascades ? counter : 0;
}

void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(
	RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners, uint32_t splitCornerIndices[8],
	uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPosA, uint32_t cornerOffsetIndex)
{
	if (!globals::features::terrainBlending.disableCulling)
		outPlanes = maxExtentCullPlanes;
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

// During world rendering count how many passes have receive shadows set
// Compare when cascadeToRender == 0 and compare to when 1
#include "../State.h"
void ShadowmapMatrixFix::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	static int countCascadeOneOnly = 0;
	static int countCascadeTwoOnly = 0;
	static int framesPassed = 0;

	a_pass->shaderProperty->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kReceiveShadows, 1);

	if (globals::features::terrainBlending.test) {  //Triggered on button press
		if (!renderShadowmaps) {                    // only track when we are not rendering the cascade
			if (newFrame) {
				newFrame = false;
				if (framesPassed == 2) {  //
					logger::info("countCascadeOneOnly: {}", countCascadeOneOnly);
					logger::info("countCascadeTwoOnly: {}", countCascadeTwoOnly);
					globals::features::terrainBlending.test = false;
					countCascadeOneOnly = 0;
					countCascadeTwoOnly = 0;
					framesPassed = 0;
				}
				framesPassed += 1;
			}

			const auto flags = a_pass->shaderProperty->flags;
			if (flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kReceiveShadows, RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask, RE::BSShaderProperty::EShaderPropertyFlag::kNonProjectiveShadows)) {
				if (cascadeToRender == 0) {
					countCascadeOneOnly++;
				} else {
					countCascadeTwoOnly++;
				}
			}
		}
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

// Step 2: Render a cascade, update cbuffer for shadowmask pass
void ShadowmapMatrixFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& desc, uint32_t* arg2, uint32_t flags)
{
	static bool init = true;

	// Update cascade buffer
	ShadowDataCB data{};
	data.lightViewProj = cascadeData[cascadeToRender].viewProj;  // Dont need this anymore
	data.shadowmapViewProj[0] = cascadeData[0].viewProjTex;
	data.shadowmapViewProj[1] = cascadeData[1].viewProjTex;
	data.shadowmapViewProj[2] = cascadeData[2].viewProjTex;
	data.shadowmapViewProj[3] = cascadeData[3].viewProjTex;
	shadowDataCB->Update(data);

	//LogMatrix("Tex", cascadeData[0].viewProjTex);
	//LogMatrix("Tex 1", cascadeData[1].viewProjTex);
	//LogMatrix("Tex 2", cascadeData[2].viewProjTex);
	//LogMatrix("Tex 3", cascadeData[3].viewProjTex);

	ID3D11Buffer* buffer = shadowDataCB->CB();
	globals::d3d::context->VSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->PSSetShaderResources(27, 1, &cascadeSRV);

	// Disable wind for now // TEMP
	if (auto manager = RE::BSTreeManager::GetSingleton()) {
		manager->windMagnitude = 0.0f;
	}

	//desc.shaderAccumulator->GetRuntimeData()->batchRenderer->

	// Only clear the RT of cascade we are rendering this frame
	desc.clearRenderTarget = desc.shadowmapIndex == (uint)cascadeToRender;

	//if (!init)
	//	desc.clearRenderTarget = false;

	// I think we need to call the engine functions and just bypass or patch out the clear RT section

	//Doesn't work cuz we need stencil state, raster state, viewport etc.
	//if (!init && desc.shadowmapIndex == (uint)cascadeToRender) {
	//logger::info("Test Call");

	// Clear all 8 possible RT slots + DSV
	//ID3D11RenderTargetView* nullRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	//globals::d3d::context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRTVs, nullptr);

	//auto& DSV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[desc.shadowmapIndex];
	//globals::d3d::context->ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

	//globals::d3d::context->ClearDepthStencilView(cascadeDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

	//if (cascadeDSV) {
	//	logger::info("DSV");
	//} else {
	//	logger::info("NO DSV");
	//}

	//globals::d3d::context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRTVs, nullptr);

	//globals::d3d::context->OMSetRenderTargets(0, nullptr, cascadeDSV);
	//	globals::d3d::context->OMSetDepthStencilState(clonedDepthStencilState, currentStencilRef);

	//globals::d3d::context->RSSetState(clonedRasterState);
	//globals::d3d::context->RSSetViewports(1, &clonedViewport);
	//}

	//	if (!init && desc.shadowmapIndex != (uint)cascadeToRender) {
	//	auto& SMTex = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].texture;
	//globals::d3d::context->CopyResource(cascadeTex, SMTex);
	//	globals::d3d::context->CopySubresourceRegion(cascadeTex, D3D11CalcSubresource(0, desc.shadowmapIndex, 1), 0, 0, 0, SMTex, D3D11CalcSubresource(0, desc.shadowmapIndex, 1), nullptr);
	//}

	func(light, desc, arg2, flags);

	if (desc.shadowmapIndex == nCascades - 1) {
		renderShadowmaps = false;
	}

	/*
	if (desc.shadowmapIndex != (uint)cascadeToRender) {
		if (auto cullingProcess = desc.cullingProcess) {
			cullingProcess->customCullPlanes.cullingPlanes[0] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[0];
			cullingProcess->customCullPlanes.cullingPlanes[1] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[1];
			cullingProcess->customCullPlanes.cullingPlanes[2] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[2];
			cullingProcess->customCullPlanes.cullingPlanes[3] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[3];
			cullingProcess->customCullPlanes.cullingPlanes[4] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[4];
			cullingProcess->customCullPlanes.cullingPlanes[5] = cascadeData[desc.shadowmapIndex].cullingPlanes.cullingPlanes[5];
		}
	}


	if (light && init && desc.shadowmapIndex == 0) {
		auto context = globals::d3d::context;
		auto device = globals::d3d::device;

		// Get currently bound states
		ID3D11DepthStencilState* currentDepthStencilState = nullptr;
		D3D11_VIEWPORT currentViewport = {};
		UINT numViewports = 1;

		// Fetch current states
		context->RSGetState(&currentRasterState);
		context->OMGetDepthStencilState(&currentDepthStencilState, &currentStencilRef);
		context->RSGetViewports(&numViewports, &currentViewport);

		// Clone rasterizer state
		if (currentRasterState) {
			D3D11_RASTERIZER_DESC rasterDesc = {};
			currentRasterState->GetDesc(&rasterDesc);
			device->CreateRasterizerState(&rasterDesc, &clonedRasterState);
			//currentRasterState->Release();  // Release the fetched reference
		}

		// Clone depth-stencil state
		if (currentDepthStencilState) {
			D3D11_DEPTH_STENCIL_DESC depthDesc = {};
			currentDepthStencilState->GetDesc(&depthDesc);
			device->CreateDepthStencilState(&depthDesc, &clonedDepthStencilState);
			currentDepthStencilState->Release();  // Release the fetched reference
		}

		// Clone viewport (just copy the struct)
		clonedViewport = currentViewport;


		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = 1024;
		texDesc.Height = 1024;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 2;
		texDesc.Format = DXGI_FORMAT_R16_TYPELESS;  // Test 32 bit
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = 0;
		device->CreateTexture2D(&texDesc, nullptr, &cascadeTex);

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
		dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = 0;
		dsvDesc.Texture2DArray.ArraySize = 1;
		dsvDesc.Flags = 0;
		device->CreateDepthStencilView(cascadeTex, &dsvDesc, &cascadeDSV);

		// SRV to read both slices in shader
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R16_UNORM;  // Match depth format
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.ArraySize = 2;  // Both slices
		device->CreateShaderResourceView(cascadeTex, &srvDesc, &cascadeSRV);


		init = false;
	}
	*/

	// If this is the final cascade
	//if (desc.shadowmapIndex == 1) {
	//	if (auto cullingProcess = desc.cullingProcess) {
	//cullingProcess->customCullPlanes.cullingPlanes[0] = cascadeData[1].cullingPlanes.cullingPlanes[0];
	//cullingProcess->customCullPlanes.cullingPlanes[1] = cascadeData[1].cullingPlanes.cullingPlanes[1];
	//cullingProcess->customCullPlanes.cullingPlanes[2] = cascadeData[1].cullingPlanes.cullingPlanes[2];
	//cullingProcess->customCullPlanes.cullingPlanes[3] = cascadeData[1].cullingPlanes.cullingPlanes[3];
	//cullingProcess->customCullPlanes.cullingPlanes[4] = cascadeData[1].cullingPlanes.cullingPlanes[4];
	//cullingProcess->customCullPlanes.cullingPlanes[5] = cascadeData[1].cullingPlanes.cullingPlanes[5];
	//logger::info("Replace planes, const: {}", cullingProcess->customCullPlanes.cullingPlanes[0].constant);
	//cullingProcess->customCullPlanes.activePlanes = cascadeData[1].cullingPlanes.activePlanes;
	//	}
	//}

	//cullingProcess->customCullPlanes.cullingPlanes[0].constant = 0;  //RE::NiFrustumPlanes();  Can we just set active culling planes to none?
	//cullingProcess->customCullPlanes.cullingPlanes[1].constant = 0;
	//cullingProcess->customCullPlanes.cullingPlanes[2].constant = 0;
	//cullingProcess->customCullPlanes.cullingPlanes[3].constant = 0;
	//cullingProcess->customCullPlanes.cullingPlanes[4].constant = 0;
	//cullingProcess->customCullPlanes.cullingPlanes[5].constant = 0;
	//cullingProcess->customCullPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
	//}

	//}
	//}
}

void ShadowmapMatrixFix::GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, DirectX::XMMATRIX viewProj, DirectX::XMVECTOR translation)
{
	using namespace DirectX;

	viewProj = XMMatrixTranspose(viewProj);

	XMVECTOR planes[6];
	planes[0] = XMVectorAdd(viewProj.r[3], viewProj.r[0]);       // Left:   w + x >= 0
	planes[1] = XMVectorSubtract(viewProj.r[3], viewProj.r[0]);  // Right:  w - x >= 0
	planes[2] = XMVectorAdd(viewProj.r[3], viewProj.r[1]);       // Bottom: w + y >= 0
	planes[3] = XMVectorSubtract(viewProj.r[3], viewProj.r[1]);  // Top:    w - y >= 0
	planes[4] = viewProj.r[2];                                   // Near:   z >= 0     (DirectX NDC)
	planes[5] = XMVectorSubtract(viewProj.r[3], viewProj.r[2]);  // Far:    w - z >= 0

	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);

	for (uint32_t i = 0; i < 6; i++) {
		float len = XMVectorGetX(XMVector3Length(planes[i]));
		if (len > 1e-6f) {
			float invLen = 1.0f / len;

			// After unitize: n dot p + d >= 0 (inside)
			// NiPlane wants: n dot p = -d
			XMVECTOR normal = XMVectorScale(planes[i], invLen);
			auto constant = -XMVectorGetW(planes[i]) * invLen;
			constant += XMVectorGetX(XMVector3Dot(normal, translation));

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

/*
//both cascades use: 6,2,0,1,5,7
//This is used to create the viewProj that is probs used using the mul function above
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetCameraRuntimeData2Test::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	if (!globals::features::grassCollision.test) {
		auto matrix = cascadeData[cascadeIt].rotation;  //DirectX::XMMatrixTranspose(cascadeData[cascadeIt].rotation);
		auto col1 = XMVectorToNiPoint3(matrix.r[0]);
		auto col2 = XMVectorToNiPoint3(matrix.r[1]);
		auto col3 = XMVectorToNiPoint3(matrix.r[2]);
		RE::NiMatrix3 rotation = RE::NiMatrix3(col3, col2, col1);  //reverse order...

		if (!globals::features::grassCollision.test3)
			cascadeCamera->local.rotate = rotation;  //Breaks culling when we use this

		//no impact i guess
		cascadeCamera->local.translate = XMVectorToNiPoint3(cascadeData[cascadeIt].translation);

		cascadeCamera->GetRuntimeData2().viewFrustum.fNear = 0.0f;
		cascadeCamera->GetRuntimeData2().viewFrustum.fFar = 1.0f;
	}

	func(cascadeCamera, frustum);
}

bool ShadowmapMatrixFix::BSShadowDirectionalLight_Mul_Precascade::thunk(RE::NiCamera& camera, float3& cornerPosition, float& outX, float& outY, float& outZ, float thresh)
{
	static int counter = 0;

	float3 MyCornerPosition = cornerPosition;
	if (!globals::features::grassCollision.test2) {
		MyCornerPosition = float3(DirectX::XMVectorGetX(cullCorners[counter]), DirectX::XMVectorGetY(cullCorners[counter]), DirectX::XMVectorGetZ(cullCorners[counter]));
		//	outX = 1;
		//	outY = 1;
		//	outZ = 1;
	}

	bool retValue = func(camera, MyCornerPosition, outX, outY, outZ, thresh);

	//using namespace DirectX;
	//XMVECTOR value = cullCorners[counter];
	//value = XMVector3Transform()
	//Transfrom by game cull cam matrix?
	//outX = DirectX::XMVectorGetX(lightCullCorners[counter]);
	//outY = DirectX::XMVectorGetY(lightCullCorners[counter]);
	//outZ = DirectX::XMVectorGetZ(lightCullCorners[counter]);
	//camera.GetRuntimeData().worldToCam
	//if (globals::features::grassCollision.settings.EnableGrassCollision) {
	//if (globals::features::grassCollision.test) {
	//}
	//}

	counter = ++counter < 8 ? counter : 0;

	return retValue;
}
*/

/*
static inline void BuildCascadeCameraCullingPlanes(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners,
uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightToEye, RE::NiPoint3& rootCameraPosition, uint32_t cornerOffsetIndex)
{
	auto float3ToNiPoint3 = [](const float3& vec) -> RE::NiPoint3 {
		return RE::NiPoint3(vec.x, vec.y, vec.z);
	};

	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
	if (numSplitCornerIndices != 0) {
		float storedEdgeLengths[5] = {};
		uint32_t minSlotIndex = 0;

		for (uint i = 0; i < numSplitCornerIndices; i++) {
			RE::NiPoint3 currCorner = float3ToNiPoint3(frustumCorners.corners[splitCornerIndices[i]]);
			RE::NiPoint3 nextCorner = float3ToNiPoint3(frustumCorners.corners[splitCornerIndices[(i + 1) % numSplitCornerIndices]]);
			float edgeLength = (currCorner - nextCorner).Length();

			uint32_t targetSlot = i;
			if (i > 4) {  // If we've filled all 5 slots, check if this edge is longer than the minimum
				if (edgeLength <= storedEdgeLengths[minSlotIndex])
					continue;  // This edge isn't long enough, skip plane construction

				targetSlot = minSlotIndex;
				storedEdgeLengths[targetSlot] = edgeLength;

				minSlotIndex = 0;
				for (int j = 0; j < 5; j++)  // Find new minimum slot
					if (storedEdgeLengths[j] < storedEdgeLengths[minSlotIndex])
						minSlotIndex = j;
			} else {  // Update minimum slot index if this new edge is smaller
				storedEdgeLengths[i] = edgeLength;
				if (edgeLength < storedEdgeLengths[minSlotIndex])
					minSlotIndex = i;
			}

			currCorner += rootCameraPosition;
			nextCorner += rootCameraPosition;
			RE::NiPoint3 currCornerExtruded = currCorner - lightToEye;

			RE::NiPlane planeOut;
			BuildPlaneFromPoints(planeOut, currCorner, nextCorner, currCornerExtruded);

			outPlanes.cullingPlanes[targetSlot] = planeOut;
			outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << targetSlot));
		}
	}

	RE::NiPlane plane;
	plane.normal = -lightToEye;
	//plane.constant = plane.normal.Dot(float3ToNiPoint3(frustumCorners.corners[cornerOffsetIndex]) + rootCameraPosition);
	plane.constant = plane.normal.Dot(float3ToNiPoint3(frustumCorners.corners[cornerOffsetIndex]));

	uint32_t capSlot = (numSplitCornerIndices < 6) ? numSplitCornerIndices : 5;

	outPlanes.cullingPlanes[capSlot] = plane;
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << capSlot));
}





// The input frustumCorners are built like this, where the viewFrustum(unitized) and viewRotMat(3x3 rotation) is that of the main player camera. maxFar is the furthest distance any of the cascades cover and minNear is the closest distance.
XMVECTOR CullTest[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), maxFar), viewRotMat),     //TR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), maxFar), viewRotMat),      //TL Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), maxFar), viewRotMat),  //BR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), maxFar), viewRotMat),   //BL Far

		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), minNear), viewRotMat),     //TR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), minNear), viewRotMat),      //TL Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), minNear), viewRotMat),  //BR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), minNear), viewRotMat),   //BL Near
	};

The splitCornerIndices, numSplitCornerIndices and cornerOffsetIndex all change depending on viewing direction. numSplitCornerIndices is normally either 5 or 6 it seems.


static inline void BuildPlaneFromPoints(RE::NiPlane& planeOut, const RE::NiPoint3& p1, const RE::NiPoint3& p2, const RE::NiPoint3& p3)
{
	RE::NiPoint3 edge1 = p2 - p1;
	RE::NiPoint3 edge2 = p3 - p2;

	planeOut.normal = edge1.UnitCross(edge2);
	planeOut.constant = planeOut.normal.Dot(p1);  // Note positive dot, unlike XMPlaneFromPoints()
}

static inline void BuildCascadeCameraCullingPlanes(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners,
uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightToEye, RE::NiPoint3& rootCameraPosition, uint32_t cornerOffsetIndex)
{
	auto float3ToNiPoint3 = [](const float3& vec) -> RE::NiPoint3 {
		return RE::NiPoint3(vec.x, vec.y, vec.z);
	};

	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
	if (numSplitCornerIndices != 0) {
		float storedEdgeLengths[5] = {};
		uint32_t minSlotIndex = 0;

		for (uint i = 0; i < numSplitCornerIndices; i++) {
			RE::NiPoint3 currCorner = float3ToNiPoint3(frustumCorners.corners[splitCornerIndices[i]]);
			RE::NiPoint3 nextCorner = float3ToNiPoint3(frustumCorners.corners[splitCornerIndices[(i + 1) % numSplitCornerIndices]]);
			float edgeLength = (currCorner - nextCorner).Length();

			uint32_t targetSlot = i;
			if (i > 4) {  // If we've filled all 5 slots, check if this edge is longer than the minimum
				if (edgeLength <= storedEdgeLengths[minSlotIndex])
					continue;  // This edge isn't long enough, skip plane construction

				targetSlot = minSlotIndex;
				storedEdgeLengths[targetSlot] = edgeLength;

				minSlotIndex = 0;
				for (int j = 0; j < 5; j++)  // Find new minimum slot
					if (storedEdgeLengths[j] < storedEdgeLengths[minSlotIndex])
						minSlotIndex = j;
			} else {  // Update minimum slot index if this new edge is smaller
				storedEdgeLengths[i] = edgeLength;
				if (edgeLength < storedEdgeLengths[minSlotIndex])
					minSlotIndex = i;
			}

			currCorner += rootCameraPosition;
			nextCorner += rootCameraPosition;
			RE::NiPoint3 currCornerExtruded = currCorner - lightToEye;

			RE::NiPlane planeOut;
			BuildPlaneFromPoints(planeOut, currCorner, nextCorner, currCornerExtruded);

			outPlanes.cullingPlanes[targetSlot] = planeOut;
			outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << targetSlot));
		}
	}

	RE::NiPlane plane;
	plane.normal = -lightToEye;
	plane.constant = plane.normal.Dot(float3ToNiPoint3(frustumCorners.corners[cornerOffsetIndex]) + rootCameraPosition);

	uint32_t capSlot = (numSplitCornerIndices < 6) ? numSplitCornerIndices : 5;

	outPlanes.cullingPlanes[capSlot] = plane;
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << capSlot));
}


 	// Due to the game time scale the cascade grid carries high natural temporal variance.
	// One way to mitegate this is to quantize light direction to discrete angle steps.
	// Once we accept the scaling will still cause some shadow creep, we can leavage(make the best of) this to squash the variance period by synchronising it with the update of the projection matrix and view translation.  //Novel fix

	// Main issues:
	// Games natural cascade center varies +- 500
	// Must encode translation in camera to avoid breaking culling(?)

	// Getting rid of the downstream translational dependance is probably a good idea.
	// What happens if we zero out player camera until cascade is finished rendering
	// OR just set cascade trans to -player pos

	// Minimal fix:
	// quantize light dir
	// sync update of matrices


DirectX::XMVECTOR OrthogonalVolumetricLighting::QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees)  //0.05 - 0.1 works well
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

void OrthogonalVolumetricLighting::BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCameraNew)
{
	using namespace DirectX;

	const float farPlane = Util::GetCameraData().x;
	const float nearPlane = Util::GetCameraData().y;

	auto tmp = Util::GetEyePosition(0);
	XMVECTOR rootCameraPos = XMVectorSet(tmp.x, tmp.y, tmp.z, 1.0);

	XMMATRIX rootViewProj = {};
	XMMATRIX rootInvViewProj = {};
	auto rootCamera = RE::Main::WorldRootCamera();  //correct
	if (rootCamera) {
		XMFLOAT4X4 tmp2{};
		auto& worldToCam = rootCamera->GetRuntimeData().worldToCam;
		std::memcpy(&tmp2, &worldToCam, sizeof(tmp2));
		rootViewProj = XMMatrixTranspose(XMLoadFloat4x4(&tmp2));
		rootViewProj.r[3] = XMVector4Transform(rootCameraPos, rootViewProj);
		rootInvViewProj = XMMatrixInverse(nullptr, rootViewProj);
	}

	float cascadeSplits[2] = { 1000, 3500 };
	auto splits = light->GetShadowDirectionalLightRuntimeData().endSplitDistances;
	cascadeSplits[0] = splits[0];
	cascadeSplits[1] = splits[1];

	int nearFaces[4] = { 0, 2, 4, 6 };
	int farFaces[4] = { 1, 3, 5, 7 };

	// Due to the game time scale the cascade grid carries high natural temporal variance.
	// One way to mitegate this is to quantize light direction to discrete angle steps.
	// Once we accept the scaling will still cause some shadow creep, we can leavage(make the best of) this to squash the variance period by synchronising it with the update of the projection matrix and view translation.  //Novel fix

	// Main issues:
	// Games natural cascade center varies +- 500
	// Must encode translation in camera to avoid breaking culling(?)

	// Getting rid of the downstream translational dependance is probably a good idea.
	// What happens if we zero out player camera until cascade is finished rendering
	// OR just set cascade trans to -player pos

	// Minimal fix:
	// quantize light dir
	// sync update of matrices

	auto lightDir = *skyrim_SunPosition;
	XMVECTOR lightDirection = XMVectorNegate(XMVector3Normalize(XMVectorSet(lightDir.x, lightDir.y, lightDir.z, 0)));
	//auto test = light->GetShadowDirectionalLightRuntimeData().lightDirection;
	//lightDirection = XMVectorSet(test.x, test.y, test.z, 0);
	lightDirection = QuantizeLightDirection(lightDirection, lightUpdateAngle);

	static XMVECTOR PrevlightDirection = lightDirection;
	static bool updateProj = true;
	if (XMVector3NotEqual(lightDirection, PrevlightDirection)) {
		PrevlightDirection = lightDirection;  //update light dir
		updateProj = true;
	} else {
		updateProj = false;
	}

	if (patchCascade) {
		light->GetShadowDirectionalLightRuntimeData().lightDirectionUpdateTimer = 1;
		light->GetShadowDirectionalLightRuntimeData().previousLightDirection = light->GetShadowDirectionalLightRuntimeData().lightDirection;
		light->GetShadowDirectionalLightRuntimeData().lightDirection = { XMVectorGetX(lightDirection), XMVectorGetY(lightDirection), XMVectorGetZ(lightDirection) };
	}

	XMVECTOR frustum_corners[] = {
		XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), rootInvViewProj),  // near
		XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), rootInvViewProj),  // far
		XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), rootInvViewProj),   // near
		XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), rootInvViewProj),   // far
		XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), rootInvViewProj),   // near
		XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), rootInvViewProj),   // far
		XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), rootInvViewProj),    // near
		XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), rootInvViewProj),    // far
	};

	for (int cascade = 0; cascade < int(nCascades); ++cascade) {
		const float split_near = (cascade == 0) ? 0 : LinearStep(nearPlane, farPlane, cascadeSplits[cascade - 1]);
		const float split_far = LinearStep(nearPlane, farPlane, cascadeSplits[cascade]);

		XMVECTOR up = XMVectorSet(0, 1, 0, 0);
		XMVECTOR forward = lightDirection;
		XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
		up = XMVector3Cross(right, forward);

		const XMVECTOR corners[] = {
			XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near),
			XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far),
			XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near),
			XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far),
			XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near),
			XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far),
			XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near),
			XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far)
		};

		XMVECTOR localTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));
		XMMATRIX localRotation = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));

		XMMATRIX viewMatrix = XMMatrixInverse(nullptr, XMMATRIX(localRotation.r[0], localRotation.r[1], localRotation.r[2], localTranslation));
		XMMATRIX baseProj = XMMatrixOrthographicOffCenterLH(0.0, 1.0, 0.0, 1.0, 0.0, 1.0);
		XMMATRIX cascadeViewProj = XMMatrixMultiply(viewMatrix, baseProj);

		XMVECTOR cornerMin = XMVectorReplicate(1e6f);
		XMVECTOR cornerMax = XMVectorNegate(cornerMin);
		for (int i = 0; i < 8; i++) {
			XMVECTOR cameraCornerPos = XMVectorAdd(rootCameraPos, corners[i]);
			XMVECTOR CascadeCameraPosition = XMVector3TransformCoord(cameraCornerPos, cascadeViewProj);
			CascadeCameraPosition = XMVectorMultiply(CascadeCameraPosition, XMVectorSet(0.5, 0.5, 1.0, 1.0));

			cornerMin = XMVectorMin(cornerMin, CascadeCameraPosition);
			cornerMax = XMVectorMax(cornerMax, CascadeCameraPosition);
		}
		cornerMin = XMVectorSetZ(cornerMin, XMVectorGetZ(cornerMin) - 600);
		cornerMax = XMVectorSetZ(cornerMax, XMVectorGetZ(cornerMax) + 600);

		for (int i = 0; i < 8; i++) {
			cascadeData[cascade].worldCorners[i] = corners[i];
		}

		if (updateProj) {
			XMStoreFloat4x4(&cascadeData[cascade].viewRotation, XMMatrixTranspose(localRotation));
			cascadeData[cascade].cascadeTranslation = localTranslation;

			float3 clipMin = cornerMin;  //left, bottom, near
			float3 clipMax = cornerMax;  //right, top, far
			cascadeData[cascade].frustum.fLeft = clipMin.x;
			cascadeData[cascade].frustum.fRight = clipMax.x;
			cascadeData[cascade].frustum.fTop = clipMax.y;
			cascadeData[cascade].frustum.fBottom = clipMin.y;
			cascadeData[cascade].frustum.fNear = clipMin.z;
			cascadeData[cascade].frustum.fFar = clipMax.z;
		}

		XMVECTOR corners[] = {
		XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near),
		XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far),
		XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near),
		XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far),
		XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near),
		XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far),
		XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near),
		XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far)
	};






	inline void CreateDirLightShadowCams(const LightComponent& light, CameraComponent camera, SHCAM* shcams, size_t shcam_count, const wi::rectpacker::Rect& shadow_rect, const Sphere* dedicated_shadows = nullptr, size_t dedicated_shadow_count = 0)
	{
		const XMMATRIX lightRotation = XMMatrixRotationQuaternion(XMLoadFloat4(&light.rotation));
		const XMVECTOR to = XMVector3TransformNormal(XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f), lightRotation);
		const XMVECTOR up = XMVector3TransformNormal(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), lightRotation);
		const XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), to, up);  // important to not move (zero out eye vector) the light view matrix itself because texel snapping must be done on projection matrix!
		const float farPlane = camera.zFarP;

		// Unproject main frustum corners into world space (notice the reversed Z projection!):
		const XMMATRIX unproj = camera.GetInvViewProjection();
		const XMVECTOR frustum_corners[] = {
			XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), unproj),  // near
			XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), unproj),  // far
			XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), unproj),   // near
			XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), unproj),   // far
			XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), unproj),   // near
			XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), unproj),   // far
			XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), unproj),    // near
			XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), unproj),    // far
		};

		// Compute shadow cameras:
		for (int cascade = 0; cascade < shcam_count; ++cascade) {
			const bool dedicated_cascade = cascade < dedicated_shadow_count;

			XMVECTOR center = XMVectorZero();
			float radius = 0;

			if (dedicated_cascade) {
				// Dedicated shadow sphere is used as-is:
				const Sphere& sphere = dedicated_shadows[cascade];
				center = XMVector3Transform(XMLoadFloat3(&sphere.center), lightView);
				radius = sphere.radius;
			} else {
				// Compute cascade bounds in light-view-space from the main frustum corners:
				const float split_near = cascade == 0 ? 0 : light.cascade_distances[cascade - 1 - dedicated_shadow_count] / farPlane;
				const float split_far = light.cascade_distances[cascade - dedicated_shadow_count] / farPlane;
				const XMVECTOR corners[] = {
					XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near), lightView),
					XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far), lightView),
				};

				// Compute cascade bounding sphere center:
				for (int j = 0; j < arraysize(corners); ++j) {
					center = XMVectorAdd(center, corners[j]);
				}
				center = center / float(arraysize(corners));

				// Compute cascade bounding sphere radius:
				for (int j = 0; j < arraysize(corners); ++j) {
					radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(corners[j], center))));
				}
			}
			// Fit AABB onto bounding sphere:
			XMVECTOR vRadius = XMVectorReplicate(radius);
			XMVECTOR vMin = XMVectorSubtract(center, vRadius);
			XMVECTOR vMax = XMVectorAdd(center, vRadius);

			// Snap cascade to texel grid:
			const XMVECTOR extent = XMVectorSubtract(vMax, vMin);
			const XMVECTOR texelSize = extent / float(shadow_rect.w);
			vMin = XMVectorFloor(vMin / texelSize) * texelSize;
			vMax = XMVectorFloor(vMax / texelSize) * texelSize;
			center = (vMin + vMax) * 0.5f;

			XMFLOAT3 _center;
			XMStoreFloat3(&_center, center);

			// clipping extrusion for projection:
			//	Tight Z distribution for precision (16-bit unorm especially) but allowing some extra room for cascade blending in Z
			{
				XMFLOAT3 _min;
				XMFLOAT3 _max;
				XMStoreFloat3(&_min, vMin);
				XMStoreFloat3(&_max, vMax);
				float ext = abs(_center.z - _min.z);
				ext *= 4;
				_min.z = _center.z - ext;
				_max.z = _center.z + ext;

				const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _max.z, _min.z);
				shcams[cascade].view_projection = XMMatrixMultiply(lightView, lightProjection);
			}

			// culling extrusion for frustum:
			//	This only affects the frustum, which is for frustum culling draw call selection
			//	It is coarser to allow far away casters to be drawn. Far away casters can be outside real projection, and their depth will be clamped (depth clip is off)
			{
				XMFLOAT3 _min;
				XMFLOAT3 _max;
				XMStoreFloat3(&_min, vMin);
				XMStoreFloat3(&_max, vMax);
				float ext = abs(_center.z - _min.z);
				ext = std::max(ext, std::min(2000.0f, farPlane) * 0.5f);
				_min.z = _center.z - ext;
				_max.z = _center.z + ext;

				// For the frustum, it is extended in Z for culling
				const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _max.z, _min.z);  // notice reversed Z!
				shcams[cascade].frustum.Create(XMMatrixMultiply(lightView, lightProjection));
			}
		}
	}











	/*
// Unproject main frustum corners into world space (notice the reversed Z projection!):
const XMMATRIX unproj = camera.GetInvViewProjection();
const XMVECTOR frustum_corners[] = {
	XMVector3TransformCoord(XMVectorSet(-1, -1, 1, 1), unproj),  // near
	XMVector3TransformCoord(XMVectorSet(-1, -1, 0, 1), unproj),  // far
	XMVector3TransformCoord(XMVectorSet(-1, 1, 1, 1), unproj),   // near
	XMVector3TransformCoord(XMVectorSet(-1, 1, 0, 1), unproj),   // far
	XMVector3TransformCoord(XMVectorSet(1, -1, 1, 1), unproj),   // near
	XMVector3TransformCoord(XMVectorSet(1, -1, 0, 1), unproj),   // far
	XMVector3TransformCoord(XMVectorSet(1, 1, 1, 1), unproj),    // near
	XMVector3TransformCoord(XMVectorSet(1, 1, 0, 1), unproj),    // far
};

// Compute shadow cameras:
for (int cascade = 0; cascade < shcam_count; ++cascade) {
	const bool dedicated_cascade = cascade < dedicated_shadow_count;

	XMVECTOR center = XMVectorZero();
	float radius = 0;

	if (dedicated_cascade) {
		// Dedicated shadow sphere is used as-is:
		const Sphere& sphere = dedicated_shadows[cascade];
		center = XMVector3Transform(XMLoadFloat3(&sphere.center), lightView);
		radius = sphere.radius;
	} else {
		// Compute cascade bounds in light-view-space from the main frustum corners:
		const float split_near = cascade == 0 ? 0 : light.cascade_distances[cascade - 1 - dedicated_shadow_count] / farPlane;
		const float split_far = light.cascade_distances[cascade - dedicated_shadow_count] / farPlane;
		const XMVECTOR corners[] = {
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near), lightView),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far), lightView),
		};

XMVECTOR corners[] = {
	XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),                         // near-top-right
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(viewFrustum.fNear), XMVectorSet(1, viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),  // near-top-left
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, -viewFrustum.fTop, viewFrustum.fRight, 0)), worldRot),   // near-bottom-right
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, -viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),  // near-bottom-left
	//cloned
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0)), worldRot),    // near-top-right
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),   // near-top-left
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, -viewFrustum.fTop, viewFrustum.fRight, 0)), worldRot),   // near-bottom-right
	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(nearPlane), XMVectorSet(1, -viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),  // near-bottom-left
};
	//XMVECTOR newCorners[] = {
	//	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(split_far), XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0)), worldRot),    // near-top-right
	//	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(split_far), XMVectorSet(1, viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),   // near-top-left
	//	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(split_far), XMVectorSet(1, -viewFrustum.fTop, viewFrustum.fRight, 0)), worldRot),   // near-bottom-right
	//	XMVector3Transform(XMVectorMultiply(XMVectorReplicate(split_far), XMVectorSet(1, -viewFrustum.fTop, -viewFrustum.fRight, 0)), worldRot),  // near-bottom-left
	//};

	//move last cascade far planes to current near planes
	//for (int i = 0; i < 4; i++) {
	//	corners[i] = corners[i + 4];
	//}

	//move new corners to far plane
	//for (int i = 0; i < 4; i++) {
	//	corners[i + 4] = newCorners[i];
	//}


			//for (int i = 0; i < 8; i++) {
		//	cascadeData[cascade].casCullCorners[i] = lightFrustum[i];
		//}


		 	XMVECTOR lightFrustum2[] = {
			XMVectorLerp(rootFrustum[0], rootFrustum[1], split_near),
			XMVectorLerp(rootFrustum[0], rootFrustum[1], split_far),
			XMVectorLerp(rootFrustum[2], rootFrustum[3], split_near),
			XMVectorLerp(rootFrustum[2], rootFrustum[3], split_far),
			XMVectorLerp(rootFrustum[4], rootFrustum[5], split_near),
			XMVectorLerp(rootFrustum[4], rootFrustum[5], split_far),
			XMVectorLerp(rootFrustum[6], rootFrustum[7], split_near),
			XMVectorLerp(rootFrustum[6], rootFrustum[7], split_far),
		};
		for (int j = 0; j < 8; ++j) {
			LogVector("Light frustum", lightFrustum2[j]);
		}

	//if (globals::features::terrainBlending.disableCulling)
//func(dirLight, outPlanes, NewfrustumCorners, NewSplitCornerIndices, numSplitCornerIndices, NewLightDir, NewcameraPos, NewcornerOffsetIndex);

//logger::info("Test 2");
//auto& OVL = globals::features::orthogonalVolumetricLighting;

//auto float3ToNiPoint3 = [](const float3& vec) -> RE::NiPoint3 {
//	return RE::NiPoint3(vec.x, vec.y, vec.z);
//};
//auto NiPoint3ToFloat3 = [](const RE::NiPoint3& vec) -> float3 {
//	return float3(vec.x, vec.y, vec.z);
//};

//int SecondCullOrder[8] = { 6, 2, 4, 0, 7, 3, 5, 1 };
//if (OVL.patchCascade && OVL.patchCulling) {
//	for (int i = 0; i < 4; i++) {
//		frustumCorners.nearFace[i] = float3ToNiPoint3(OVL.cascadeData[OVL.cascadeIt].worldCorners[SecondCullOrder[i]]);
//	}
//	for (int i = 0; i < 4; i++) {
//		frustumCorners.farFace[i] = float3ToNiPoint3(OVL.cascadeData[OVL.cascadeIt].worldCorners[SecondCullOrder[i + 4]]);
//	}
//}

//if (!globals::features::grassCollision.settings.EnableGrassCollision)
//	func(dirLight, outPlanes, frustumCorners, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);








	//for (int i=0; i<8; i++){
	//	logger::info("plane: {}, {}, {}", outPlanes.corners[i].x, outPlanes.corners[i].y, outPlanes.corners[i].z);
	//}
	//int firstCullOrder[8] = { 7, 3, 5, 1, 6, 2, 4, 0 };
	//if (OVL.patchCascade && OVL.patchCulling) {
	//	for (int i = 0; i < 8; i++)                                                                                                                         // Set frustum corners as the far planes of the final cascade and the near plane of the 0th cascade
	//		frustumCorners.corners[i] = (i < 4) ? OVL.cascadeData[1].worldCorners[firstCullOrder[i]] : OVL.cascadeData[0].worldCorners[firstCullOrder[i]];  //dirLight->shadowMapCount - 1
	//}
	//if (!globals::features::grassCollision.settings.EnableGrassCollision)
	//	func(dirLight, outPlanes, frustumCorners, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);










	/*
	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),	//TR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), worldRot),		//TR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), worldRot),		//TL Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), worldRot),		//TL Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), worldRot), //BR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), worldRot),	//BR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), worldRot),	//BL Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), worldRot)	//BL Far
	};
	LogMatrix("World rotation", worldRot);

	for (int j = 0; j < 8; ++j)
		LogVector("Root frustum", rootFrustum[j]);
*/

/*
	XMVECTOR cullLightFrustum[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], minNear), lightView),	//TR Near   0   1
		XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], maxFar), lightView),	//TR Far    1   2
		XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], minNear), lightView),	//TL Near	2
		XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], maxFar), lightView),	//TL Far	3   3
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], minNear), lightView),	//BR Near	4	6
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], maxFar), lightView),	//BR Far	5   5
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], minNear), lightView),	//BL Near	6
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], maxFar), lightView),	//BL Far	7   4
	};
	for (int i = 0; i < 8; i++) {
		cullCorners[i] = cullLightFrustum[i];
		LogVector("Culling light frustum", cullLightFrustum[i]);
	}

	//std::swap(viewRot.r[0], viewRot.r[2]);  // This gives the view matrix as seen by shaders
	//worldRot = XMMatrixTranspose(viewRot);


		// Perimeter trace
	// Pick one near corner, go to its corresponding far corner,
	// trace around the far plane, come back to adjacent near corner
	//uint maxCFRTraceOrder[6] = {0, 1, 3, 7, 5, 4};
	//memcpy(&cullIndices, &maxCFRTraceOrder, sizeof(maxCFRTraceOrder));

		//scrap
	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),  //1
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),   //3
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), viewRotMat),  //5
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), viewRotMat),    //7


		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),     //0
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),      //2
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), viewRotMat),  //4
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), viewRotMat),   //6

	};
*/

/*

		//XMVECTOR moveDir = XMVectorSubtract(rootCameraPos, prevPos);
		//moveDir = SafeNormalize(moveDir);

		//XMVECTOR cross = SafeNormalize(XMVector3Cross(lightDirection, moveDir));  //need to guard 0

		XMVECTOR quantizer = XMVectorScale(lightDirection, (float)globals::features::terrainBlending.testScale);  // Works when moving perpendiular to light direction

		XMVECTOR quantizerTest = XMVectorScale(XMVector3Transform(lightDirection, XMMatrixRotationZ(XM_PIDIV2)), (float)globals::features::terrainBlending.testScale);

		//XMVECTOR perpendicular = ;
		//XMVECTOR quantizerTest = XMVectorScale(cross, (float)globals::features::terrainBlending.testScale);

		//cascadeData[cascade].cascadeTranslation = XMVectorFloor(rootCameraPos / quantizer) * quantizer;

		if (globals::features::grassCollision.test) {
			//cascadeData[cascade].cascadeTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(cascadeSplits[cascade])));
			cascadeData[cascade].cascadeTranslation = XMVectorFloor(rootCameraPos / (float)globals::features::terrainBlending.testScale) * (float)globals::features::terrainBlending.testScale;
		}

		if (globals::features::grassCollision.test2) {
			cascadeData[cascade].cascadeTranslation = XMVectorFloor(rootCameraPos / quantizer) * quantizer;  // Best so far
			//quantizer = XMVectorScale(XMVectorNegate(lightDirection), (float)globals::features::terrainBlending.testScale);
			//cascadeData[cascade].cascadeTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));
		}

		if (globals::features::grassCollision.test3) {
				cascadeData[cascade].cascadeTranslation = XMVectorFloor(rootCameraPos / quantizerTest) * quantizerTest;
			//cascadeData[cascade].cascadeTranslation = XMVectorAdd(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));
		}

		if (globals::features::grassCollision.test && globals::features::grassCollision.test2) {
				cascadeData[cascade].cascadeTranslation = XMVectorFloor(rootCameraPos / XMVectorNegate(quantizerTest)) * XMVectorNegate(quantizerTest); //at quantize 4
		}

		// Clamp player position to the one specific
		// Always quantize player position towards a position in a given direction(dir of sun?)

		// What about geom world uses player camera +or- (cascadeEndSplit * sunDir)  thats what is way before?
		//localTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));











/*
	// This should be left = subtract : right = add ?
	XMVECTOR left = XMVectorAdd(viewProj.r[3], viewProj.r[0]);
	XMVECTOR right = XMVectorSubtract(viewProj.r[3] , viewProj.r[0]);

	XMVECTOR bottom = XMVectorAdd(viewProj.r[3], viewProj.r[1]);
	XMVECTOR top = XMVectorSubtract(viewProj.r[3], viewProj.r[1]);

	XMVECTOR nearP = XMVectorAdd(viewProj.r[3], viewProj.r[2]);
	XMVECTOR farP = XMVectorSubtract(viewProj.r[3], viewProj.r[2]);

	left = XMVector4Normalize(XMVectorSetW(left, -XMVectorGetW(left)));
	right = XMVector4Normalize(XMVectorSetW(right, -XMVectorGetW(right)));
	bottom = XMVector4Normalize(XMVectorSetW(bottom, -XMVectorGetW(bottom)));
	top = XMVector4Normalize(XMVectorSetW(top, -XMVectorGetW(top)));
	nearP = XMVector4Normalize(XMVectorSetW(nearP, -XMVectorGetW(nearP)));
	farP = XMVector4Normalize(XMVectorSetW(farP, -XMVectorGetW(farP)));

	outPlanes.cullingPlanes[0].normal = XMVectorToNiPoint3(left);
	outPlanes.cullingPlanes[1].normal = XMVectorToNiPoint3(right);
	outPlanes.cullingPlanes[2].normal = XMVectorToNiPoint3(bottom);
	outPlanes.cullingPlanes[3].normal = XMVectorToNiPoint3(top);
	outPlanes.cullingPlanes[4].normal = XMVectorToNiPoint3(nearP);
	outPlanes.cullingPlanes[5].normal = XMVectorToNiPoint3(farP);

	outPlanes.cullingPlanes[0].constant = XMVectorGetW(left);
	outPlanes.cullingPlanes[1].constant = XMVectorGetW(right);
	outPlanes.cullingPlanes[2].constant = XMVectorGetW(bottom);
	outPlanes.cullingPlanes[3].constant = XMVectorGetW(top);
	outPlanes.cullingPlanes[4].constant = XMVectorGetW(nearP);
	outPlanes.cullingPlanes[5].constant = XMVectorGetW(farP);

	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 0));
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 1));
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 2));
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 3));
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 4));
	outPlanes.activePlanes.set(static_cast<RE::NiFrustumPlanes::ActivePlane>(1 << 5));

		LogVector("left", left);
	LogVector("right", right);
	LogVector("bottom", bottom);
	LogVector("top", top);
	LogVector("near", nearP);
	LogVector("far", farP);
*/

/*
void ShadowmapMatrixFix::BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	XMFLOAT3X3 tmpRotMat{};
	std::memcpy(&tmpRotMat, &rootCamera.world.rotate.entry, sizeof(tmpRotMat));
	XMMATRIX worldRot = XMMatrixTranspose(XMLoadFloat3x3(&tmpRotMat));

	auto tmpTrans = rootCamera.world.translate;
	XMVECTOR rootCameraPos = XMVectorSet(tmpTrans.x, tmpTrans.y, tmpTrans.z, 1.0);

	auto& viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;

	XMVECTOR rootFrustum[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, -viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, -viewFrustum.fRight, 0), viewFrustum.fFar), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, -viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, -viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, -viewFrustum.fTop, -viewFrustum.fRight, 0), viewFrustum.fNear), worldRot),
		XMVector3Transform(XMVectorScale(XMVectorSet(1, -viewFrustum.fTop, -viewFrustum.fRight, 0), viewFrustum.fFar), worldRot)
	};

	LogMatrix("World rotation", worldRot);
	for (int j = 0; j < 8; ++j)
		LogVector("Root frustum", rootFrustum[j]);

	auto tmpLightDir = light->GetShadowDirectionalLightRuntimeData().lightDirection;
	XMVECTOR lightDirection = XMVector3Normalize(XMVectorSet(tmpLightDir.x, tmpLightDir.y, tmpLightDir.z, 0));
	LogVector("lightDirection", lightDirection);

	auto& tmpSplits = light->GetShadowDirectionalLightRuntimeData().endSplitDistances;
	float cascadeSplits[3] = { tmpSplits[0], tmpSplits[1], tmpSplits[2] };

	static XMVECTOR prevPos = rootCameraPos;

	for (int cascade = 0; cascade < int(nCascades); ++cascade) {
		float split_near = (cascade == 0) ? viewFrustum.fNear / viewFrustum.fFar : cascadeSplits[cascade - 1] / viewFrustum.fFar;
		float split_far = cascadeSplits[cascade] / viewFrustum.fFar;

		XMVECTOR up = XMVectorSet(0, 1, 0, 0);
		XMVECTOR forward = lightDirection;  // Direction light travels
		XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
		up = XMVector3Cross(right, forward);

		XMMATRIX lightView = XMMatrixTranspose(XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1)));  // ViewRot == InvWorld == TrspWorld

		XMVECTOR lightFrustum[] = {
			XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], split_near), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], split_far), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], split_near), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], split_far), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], split_near), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], split_far), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], split_near), lightView),
			XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], split_far), lightView),
		};

		XMVECTOR center = XMVectorZero();
		float radius = 0;

		for (int j = 0; j < 8; ++j) {
			center = XMVectorAdd(center, lightFrustum[j]);
		}
		center = center / 8.0f;

		for (int j = 0; j < 8; ++j) {
			radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(lightFrustum[j], center))));
		}

		XMVECTOR vRadius = XMVectorReplicate(radius);
		XMVECTOR cornerMin = XMVectorSubtract(center, vRadius);
		XMVECTOR cornerMax = XMVectorAdd(center, vRadius);

		// Snap cascade to texel grid
		const XMVECTOR extent = XMVectorSubtract(cornerMax, cornerMin);
		const XMVECTOR texelSize = extent / float(cascadePxSize);
		cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
		cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;

		// Extend depth range
		center = (cornerMin + cornerMax) * 0.5f;
		float centerZ = XMVectorGetZ(center);
		float halfExtentZ = abs(centerZ - XMVectorGetZ(cornerMin));

		float ExtentZ = halfExtentZ * globals::features::terrainBlending.multiplerRange;  // The lower this is the more spread the shadow map depth values are so higher means less precision
		cornerMin = XMVectorSetZ(cornerMin, centerZ - ExtentZ);
		cornerMax = XMVectorSetZ(cornerMax, centerZ + ExtentZ);

		LogVector("corner left, bottom, near", XMVectorSetW(cornerMin, 0));
		LogVector("corner right, top, far", XMVectorSetW(cornerMax, 0));

		float3 clipMin = cornerMin;  //left, bottom, near
		float3 clipMax = cornerMax;  //right, top, far
		cascadeData[cascade].frustum.fLeft = clipMin.x;
		cascadeData[cascade].frustum.fRight = clipMax.x;
		cascadeData[cascade].frustum.fBottom = clipMin.y;
		cascadeData[cascade].frustum.fTop = clipMax.y;
		cascadeData[cascade].frustum.fNear = clipMin.z;
		cascadeData[cascade].frustum.fFar = clipMax.z;

		cascadeData[cascade].rotation = lightView;

		cascadeData[cascade].cascadeTranslation = rootCameraPos;

		auto proj = XMMatrixOrthographicOffCenterLH(clipMin.x, clipMax.x, clipMin.y, clipMax.y, clipMin.z, clipMax.z);
		auto viewProj = XMMatrixMultiply(lightView, proj);

		if (globals::features::terrainBlending.update)
			XMStoreFloat4x4(&cascadeData[cascade].viewProj, XMMatrixTranspose(viewProj));

		XMMATRIX texProj = XMMATRIX(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);

		if (globals::features::terrainBlending.update)
			XMStoreFloat4x4(&shadowMatrix[cascade], XMMatrixTranspose(XMMatrixMultiply(viewProj, texProj)));

	}
}







	for (int i = 0; i < 8; i++) {
		lightCullCorners[i] = XMVector3Transform(CullTest[i], lightView);
		LogVector("light Culling frustum test", lightCullCorners[i]);
	}

	//uint cullIndices[8] = {1, 3, 5, 7, 0, 2, 4, 6};
	maxFar = maxFar / viewFrustum.fFar;
	minNear = minNear / viewFrustum.fFar;

	// Need to re-test
	XMVECTOR lightFrustumCulling[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], maxFar), lightView),     // TR (near->far), split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], maxFar), lightView),     // TL (near->far), split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], maxFar), lightView),     // BR (near->far), split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], maxFar), lightView),	 // BL (near->far), split_far

		XMVector3Transform(XMVectorLerp(rootFrustum[0], rootFrustum[1], minNear), lightView),     // TR (near->far), split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[2], rootFrustum[3], minNear), lightView),     // TL (near->far), split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[5], minNear), lightView),     // BR (near->far), split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[7], minNear), lightView),	  // BL (near->far), split_near
	};

	//Doesn't work
	maxFar = cascadeSplits[1];
	minNear = viewFrustum.fNear;
	XMVECTOR CullTestTest[] = {
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), maxFar), lightView),      //TR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), maxFar), lightView),       //TL Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), maxFar), lightView),  //BR Far
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), maxFar), lightView),   //BL Far

		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), minNear), lightView),      //TR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), minNear), lightView),       //TL Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), minNear), lightView),  //BR Near
		XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), minNear), lightView),   //BL Near
	};

	// Doesnt work
	maxFar = cascadeSplits[1] / viewFrustum.fFar * 4;
	minNear = viewFrustum.fNear / viewFrustum.fFar;
	XMVECTOR Cully[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], maxFar), lightView),   // TR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], maxFar), lightView),   // TL split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], maxFar), lightView),   // BR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], maxFar), lightView),   // BL split_far

		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], minNear), lightView),  // TR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], minNear), lightView),  // TL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], minNear), lightView),  // BR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], minNear), lightView),  // BL split_near
	};


	maxFar = cascadeSplits[1] / viewFrustum.fFar;
	minNear = viewFrustum.fNear / viewFrustum.fFar;
	XMVECTOR Cully2[] = {
		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], maxFar), lightView),   // TR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], maxFar), lightView),   // TL split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], maxFar), lightView),   // BR split_far
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], maxFar), lightView),   // BL split_far

		XMVector3Transform(XMVectorLerp(rootFrustum[4], rootFrustum[0], minNear), lightView),     // TR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[5], rootFrustum[1], minNear), lightView),     // TL split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[6], rootFrustum[2], minNear), lightView),     // BR split_near
		XMVector3Transform(XMVectorLerp(rootFrustum[7], rootFrustum[3], minNear), lightView),	  // BL split_near

	};
	for (int i = 0; i < 8; i++) {
		LogVector("Culling frustum test V3", Cully2[i]);
	}



		/*
	using namespace DirectX;

	auto XMVectorToNiPoint3 = [](const XMVECTOR& vec) -> RE::NiPoint3 {
		return RE::NiPoint3(XMVectorGetX(vec), XMVectorGetY(vec), XMVectorGetZ(vec));
	};

	auto NewLightDir = XMVectorToNiPoint3(lightDirection);
	//RE::NiPoint3 NewcameraPos = cameraPos; //CHANGED

	FrustumSplit NewfrustumCorners = {};
	for (int i = 0; i < 8; i++) {
		NewfrustumCorners.corners[i] = XMVectorToNiPoint3(cullCorners[i]);
		//logger::info("Indice: {}", splitCornerIndices[0])
	}

	//if (!globals::features::terrainBlending.disableCulling)
	//	func(dirLight, outPlanes, NewfrustumCorners, splitCornerIndices, numSplitCornerIndices, NewLightDir, cameraPosA, cornerOffsetIndex);
	*/

//auto view = cascadeData[cascadeIt].viewMatrix;
//auto row0Mat = RE::NiPoint3(matrix._13, matrix._12, matrix._11);
//auto row1Mat = RE::NiPoint3(matrix._23, matrix._22, matrix._21);
//auto row2Mat = RE::NiPoint3(matrix._33, matrix._32, matrix._31);
//std::swap(row0Mat.y, row1Mat.z);
//std::swap(row1Mat.x, row2Mat.y);
//RE::NiMatrix3 rotation = RE::NiMatrix3(row0Mat, row1Mat, row2Mat);
//auto rotation = cascadeData[cascadeIt].rotation; // maybe transpose
//std::memcpy(&cascadeCamera->local.rotate, &rotation, sizeof(rotation));

//auto matrix = cascadeData[cascadeIt].rotation;  //DirectX::XMMatrixTranspose(cascadeData[cascadeIt].rotation);
//auto col1 = XMVectorToNiPoint3(matrix.r[0]);
//auto col2 = XMVectorToNiPoint3(matrix.r[1]);
//auto col3 = XMVectorToNiPoint3(matrix.r[2]);
//RE::NiMatrix3 rotation = RE::NiMatrix3(col3, col2, col1);
//
//
//cornerOffsetIndex = 2,6   far, far
//numSplitCornerIndices = 5,6
//light dir = :XMFLOAT3 <-0.0, -0.0, -1.0> ???
//cornerOffsetIndex corner = DirectX::XMFLOAT3 <3841.2878, -3536.936, -1887.974>
//4 5 1 2 0

// convex polytope

//const float farPlane = Util::GetCameraData().x;
//float nearPlane = Util::GetCameraData().y;
// One main issue i think, is that the game was transforming geometry world translations with a cascade camera position not with player camera position

/*
	if (globals::features::grassCollision.test2) {
		if (light) {
			logger::info("shadowmapIndex: {}", desc.shadowmapIndex);
			if (desc.shadowmapIndex == 1 && !globals::features::grassCollision.test3) { //This is the path taken by default. Once this eval becomes false(by toggling test3), the next time it becomes true again it will freeze the game permenently
				logger::info("Passed shadowmapIndex: {}", desc.shadowmapIndex);
				func(light, desc, arg2, flags);
			}
			else if (desc.shadowmapIndex == 0 && globals::features::grassCollision.test3) { // Can trigger this fine just can't change test3 back to false
				logger::info("Passed shadowmapIndex: {}", desc.shadowmapIndex);
				func(light, desc, arg2, flags);
			}
		}
	}
	else {
		if (light) {
			logger::info("shadowmapIndex: {}", desc.shadowmapIndex);
			func(light, desc, arg2, flags);
		}
	}

	cascadeIt = ++cascadeIt < nCascades ? cascadeIt : 0;

	*/

//static uint cascadeID = 0;
//if (cascadeIt == 0) {  // If first call of the frame
//	if (cascadeToRender < 2) {
//		if (light) {
//			if(auto* newDesc = &light->GetRuntimeData().shadowmapDescriptors[cascadeToRender]) //using cascadeIt works fine  -  is it when going from 0 to 1 in next frame? it only crashes when changing descriptors
//				func(light, newDesc, arg2, flags);
//		}

//	}
//	cascadeID = ++cascadeID < nCascades ? cascadeID : 0;
//}

//else {
// For each accumulator we don't dispatch, we do
//*arg2 = *arg2 + 1;
//}

//desc->clearRenderTarget = false; //This causes the wrong RT to be assigned?

//if (globals::features::grassCollision.test) {
//	*arg2 = *arg2 + 1;
//}
//logger::info("Test: {}", *arg2);
//logger::info("render cascade");

//auto accum = globals::game::smState->shadowSceneNode[0]->GetRuntimeData().shadowLightsAccum.size();
//logger::info("accum size cas: {}", accum);

//if (cascadeIt == 0)
//	func(light, desc, arg2, flags);

//accum = globals::game::smState->shadowSceneNode[0]->GetRuntimeData().shadowLightsAccum.size();
//logger::info("accum size cas after: {}", accum);

/*
	bool weRan = false;
	if (light) {
		//light->shadowMapCount = 1;  // Only needs to be run once
		logger::info("cascadeIt: {}", cascadeIt);
		logger::info("array size: {}", light->GetRuntimeData().shadowmapDescriptors.size());
		if (globals::features::grassCollision.test2){
			if (auto* newDesc = &light->GetRuntimeData().shadowmapDescriptors[cascadeIt]) { //0 works
				//desc->shadowmapIndex = cascadeIt;

				func(light, newDesc, arg2, flags);
				weRan = true;
			}
		}
	}
	if (!weRan)
		func(light, desc, arg2, flags);

// Returns ShadowLight*
RE::BSShadowLight* ShadowmapMatrixFix::BSShadowDirectionalLight_TestFunc::thunk(void* arg1, uint32_t arg2)
{
	//arg2 = arg2 + 1;
	//arg2 = 2;  //IF this works then try return on entry of render cascade
	return nullptr;  //func(arg1, arg2);
}
void ShadowmapMatrixFix::SetShadowMapCount::thunk(RE::BSShadowLight* light, uint64_t numShadowMaps)
{
	if (light->IsDirectionalLight())
		numShadowMaps = 4;

	func(light, numShadowMaps);
}

// Need to set game cascades to 1 so it only runs once per frame.
// Set the render target index manually
//We can set a bogus cull frustum so nothing gets rendered but still need to stop stencil being cleared
void ShadowmapMatrixFix::BSShadowDirectionalLight_RenderShadowmaps::thunk(RE::BSShadowLight* light, void* a2)
{


	func(light, a2);
}








	//stl::write_thunk_call<BSShadowDirectionalLight_TestFunc>(REL::RelocationID(107133, 107133).address() + REL::Relocate(0x1BC, 0x1BC, 0x1BC));
	//gSunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());
	//stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2Test>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x9C8, 0x9C8)); //ADDED
	//stl::write_thunk_call<BSShadowDirectionalLight_Mul_Precascade>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0xAFF, 0xAFF));  //no cascade
	//stl::write_thunk_call<BSShadowDirectionalLight_Mul>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1A10, 0x1A10));  //cascade
	//stl::detour_thunk<SetShadowMapCount>(REL::RelocationID(107599, 107599));
	//stl::write_vfunc<0xA, BSShadowDirectionalLight_RenderShadowmaps>(RE::VTABLE_BSShadowDirectionalLight[0]);
	//stl::write_thunk_call<AccumulateShadowmap>(REL::RelocationID(107604, 107604).address() + REL::Relocate(0x167, 0x167, 0x167));
*/