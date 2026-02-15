#include "ShadowmapCascadeMatrixFix.h"

#include "../Features/GrassCollision.h"
#include "../Features/TerrainBlending.h"
#include "DirectXCollision.h"

//TODO:
// Add checks for VR
// Catch ini settings and override shadow settings
// Add UI?
// // Add native split calculations
// Split overlap
// Build culling matrices

//ISSUES:
// Culling breaks at low texel size - high shadow rez  OR small split distances  - i think increasing Z range mult fixes this
// Shader flags still causing some blinking

void ShadowmapMatrixFix::Install()
{
	// Sets up the cascade and culling cameras
	stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);

	//Render a cascade
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6));

	// Clear the current frustum - we use it to set a new view matrix and translation
	stl::write_thunk_call<BSShadowDirectionalLight_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

	// Culls against min near and max far plane of any cascade
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59, 0xC59));  //First call    need SE addr

	// Culls individual cascade frustum
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

//////////////////////////////////////////////
void ShadowmapMatrixFix::BuildRootFrustum(Frustum& outputFrustum, const RE::NiFrustum& viewFrustum, const DirectX::XMMATRIX& rootWorld)
{
	using namespace DirectX;

	outputFrustum.corner[0] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fFar), rootWorld);
	outputFrustum.corner[1] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fFar), rootWorld);
	outputFrustum.corner[2] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fFar), rootWorld);
	outputFrustum.corner[3] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fFar), rootWorld);

	outputFrustum.corner[4] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fRight, 0), viewFrustum.fNear), rootWorld);
	outputFrustum.corner[5] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fTop, viewFrustum.fLeft, 0), viewFrustum.fNear), rootWorld);
	outputFrustum.corner[6] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fRight, 0), viewFrustum.fNear), rootWorld);
	outputFrustum.corner[7] = XMVector3Transform(XMVectorScale(XMVectorSet(1, viewFrustum.fBottom, viewFrustum.fLeft, 0), viewFrustum.fNear), rootWorld);
}

// Call this only once since all can be static assuming no UI
void ShadowmapMatrixFix::SetCascadeSplit(CascadeBounds::EndSplits& outputSplits, const RE::NiFrustum& viewFrustum)
{
	auto& settings = globals::features::terrainBlending;

	float cascadeSplits[4] = { (float)settings.splits[0], (float)settings.splits[1], (float)settings.splits[2], (float)settings.splits[3] };

	for (int i = 0; i < (int)nCascades; i++) {
		outputSplits.SplitVS[i] = cascadeSplits[i];
		outputSplits.SplitNDC[i] = ViewDepthToNDC(cascadeSplits[i], viewFrustum);
		outputSplits.SplitLin[i] = LinearStep(viewFrustum.fNear, viewFrustum.fFar, cascadeSplits[i]);
	}

	maxCascadeCoverageVS = outputSplits.SplitVS[nCascades - 1];
}

void ShadowmapMatrixFix::BuildLightFrustum(DirectX::XMMATRIX& outLightView, Frustum& outFrustum, const CascadeBounds::EndSplits& cascadeSplits, const Frustum& rootFrustum, const DirectX::XMVECTOR& lightDirection)
{
	using namespace DirectX;

	// Build light view matrix - matching game format w/o translation
	XMVECTOR up = XMVectorSet(0, 1, 0, 0);
	XMVECTOR forward = lightDirection;  // Light -> eye
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
	up = XMVector3Cross(right, forward);

	XMMATRIX lightWorld = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));
	const XMMATRIX lightView = XMMatrixTranspose(lightWorld);  // ViewRot == InvWorld == TrspWorld
	outLightView = lightView;

	float nearSplit = (cascadeToRender == 0) ? 0.0f : cascadeSplits.SplitLin[cascadeToRender - 1];
	float FarSplit = cascadeSplits.SplitLin[cascadeToRender];

	outFrustum.corner[0] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[4], rootFrustum.corner[0], nearSplit), lightView);  // TR - near
	outFrustum.corner[1] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[4], rootFrustum.corner[0], FarSplit), lightView);   // TR - far
	outFrustum.corner[2] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[5], rootFrustum.corner[1], nearSplit), lightView);  // TL - near
	outFrustum.corner[3] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[5], rootFrustum.corner[1], FarSplit), lightView);   // TL - far
	outFrustum.corner[4] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[6], rootFrustum.corner[2], nearSplit), lightView);  // BR - near
	outFrustum.corner[5] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[6], rootFrustum.corner[2], FarSplit), lightView);   // BR - far
	outFrustum.corner[6] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[7], rootFrustum.corner[3], nearSplit), lightView);  // BL - near
	outFrustum.corner[7] = XMVector3Transform(XMVectorLerp(rootFrustum.corner[7], rootFrustum.corner[3], FarSplit), lightView);   // BL - far
}

void ShadowmapMatrixFix::BuildCascadeBoundingSphere(CascadeBounds::Sphere& outSphere, const Frustum& lightFrustum)
{
	using namespace DirectX;

	XMVECTOR center = XMVectorZero();
	for (int j = 0; j < 8; ++j) {
		center = XMVectorAdd(center, lightFrustum.corner[j]);
	}
	outSphere.center = center / 8.0f;

	float radius = 0;
	for (int j = 0; j < 8; ++j) {
		radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(lightFrustum.corner[j], outSphere.center))));
	}
	outSphere.radius = radius;
}

void ShadowmapMatrixFix::BuildCascadeAABB(CascadeBounds::AABB& outBoundingBox, const DirectX::XMVECTOR& lightCameraPos, const CascadeBounds::Sphere& sphere)
{
	using namespace DirectX;

	// Build AABB from sphere
	XMVECTOR vRadius = XMVectorReplicate(sphere.radius);
	XMVECTOR cornerMin = XMVectorSubtract(sphere.center, vRadius);
	XMVECTOR cornerMax = XMVectorAdd(sphere.center, vRadius);

	// Add trans vec - adding here avoids extra variance
	cornerMin = XMVectorAdd(cornerMin, lightCameraPos);
	cornerMax = XMVectorAdd(cornerMax, lightCameraPos);

	// Snap texel grid
	XMVECTOR extent = XMVectorReplicate(2.0f * sphere.radius);
	XMVECTOR texelSize = extent / float(cascadePxSize);
	cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
	cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;

	outBoundingBox.cornerMin = cornerMin;  // left, bottom, near
	outBoundingBox.cornerMax = cornerMax;  // right, top, far
}

void ShadowmapMatrixFix::BuildCascadeProjectionMatrices(DirectX::XMMATRIX& outProj, DirectX::XMMATRIX& outCullProj, const CascadeBounds::AABB& boundingBox)
{
	auto& settings = globals::features::terrainBlending;

	float centerZ = float3((boundingBox.cornerMin + boundingBox.cornerMax) * 0.5f).z;
	float halfExtentZ = abs(centerZ - boundingBox.cornerMin.z);

	// Build main proj frustum
	{
		// Adjust depth range for better precision - depth clipping is disabled
		float extent = halfExtentZ * settings.multiplerRange;  // The lower this is the more spread the shadow map depth values are so higher means less precision

		float adjustedMin = centerZ - extent;
		float adjustedMax = centerZ + extent;

		outProj = DirectX::XMMatrixOrthographicOffCenterLH(boundingBox.cornerMin.x, boundingBox.cornerMax.x, boundingBox.cornerMin.y, boundingBox.cornerMax.y, adjustedMin, adjustedMax);
	}

	// Build culling frustum
	{
		// Cap min extent for small cascades to avoid issues
		float extent = std::max(halfExtentZ, settings.minExtent);  //1600 seems okay

		float adjustedMin = centerZ - extent;
		float adjustedMax = centerZ + extent;

		outCullProj = DirectX::XMMatrixOrthographicOffCenterLH(boundingBox.cornerMin.x, boundingBox.cornerMax.x, boundingBox.cornerMin.y, boundingBox.cornerMax.y, adjustedMin, adjustedMax);
	}
}

/*

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
*/

void ShadowmapMatrixFix::BuildShadowCascade(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	// Get root camera params
	const XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);
	const RE::NiFrustum& viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;
	const XMMATRIX worldRotation = XMLoadFloat3x3(reinterpret_cast<const XMFLOAT3X3*>(&rootCamera.world.rotate.entry));

	// Discretize light dir to mitigate variance from time scale
	XMVECTOR lightDirection = XMVector3Normalize(NiPoint3ToXMVector(light->GetShadowDirectionalLightRuntimeData().sunVector));
	lightDirection = QuantizeLightDirection(lightDirection, settings.lightUpdateAngle);

	static CascadeBounds cascadeBoundData;

	SetCascadeSplit(cascadeBoundData.endSplits, viewFrustum);

	// Add this back once split settings are finalized
	//static bool setupSplits = true;
	//if (setupSplits) {
	//	SetCascadeSplit(cascadeBoundData.endSplits, viewFrustum);
	//	setupSplits = false;
	//}

	// Build frustums, view matrix
	Frustum rootFrustum;
	BuildRootFrustum(rootFrustum, viewFrustum, worldRotation);

	XMMATRIX lightView = {};
	Frustum lightFrustum;
	BuildLightFrustum(lightView, lightFrustum, cascadeBoundData.endSplits, rootFrustum, lightDirection);

	// Build bounding objects
	BuildCascadeBoundingSphere(cascadeBoundData.boundingSphere, lightFrustum);

	const XMVECTOR lightCameraPos = XMVector3Transform(rootCameraPos, lightView);

	BuildCascadeAABB(cascadeBoundData.boundingBox, lightCameraPos, cascadeBoundData.boundingSphere);

	// Build view projection transforms
	XMMATRIX lightProj = {};
	XMMATRIX cullingProj = {};
	BuildCascadeProjectionMatrices(lightProj, cullingProj, cascadeBoundData.boundingBox);

	const XMMATRIX viewProj = XMMatrixMultiply(lightView, lightProj);
	XMStoreFloat4x4(&cascadeData[cascadeToRender].viewProj, XMMatrixTranspose(viewProj));

	// Relative translation added to shader MS position to avoid dot prod precision loss - DO NOT fuck with
	const XMMATRIX texProj = XMMatrixMultiply(XMMatrixScaling(0.5f, -0.5f, 1.0f), XMMatrixTranslation(0.5f, 0.5f, 0.0f));
	XMStoreFloat4x4(&cascadeData[cascadeToRender].viewProjTex, XMMatrixTranspose(XMMatrixMultiply(viewProj, texProj)));

	// Set translation for geometry to transform against
	cascadeData[cascadeToRender].translation = rootCameraPos;

	cascadeData[cascadeToRender].splitEndDepthNDC = cascadeBoundData.endSplits.SplitNDC[cascadeToRender];

	// Build culling planes
	const XMMATRIX cullingViewProj = XMMatrixMultiply(lightView, cullingProj);
	GetCullPlanesFromVPMatrix(cascadeData[cascadeToRender].cullingPlanes, cullingViewProj);
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

	if (!initialized) {
		return func(light, inputCamera);
	}

	// Build the cascade we want to render this frame
	cascadeToRender = ++cascadeToRender < (int)nCascades ? cascadeToRender : 0;

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
	if (!initialized) {
		return func(light, desc, arg2, flags);
	}

	// Update cascade buffer
	ShadowDataCB data{};
	if (desc.shadowmapIndex == 0) {  // Only update buffer once per frame
		data.lightViewProj = cascadeData[cascadeToRender].viewProj;
		for (int i = 0; i < (int)nCascades; i++) {
			data.shadowmapViewProj[i] = cascadeData[i].viewProjTex;
			data.cascadeSplitEnds[i] = cascadeData[i].splitEndDepthNDC;
		}
		data.numCascades = nCascades;

		shadowCascadeFixCB->Update(data);

		ID3D11Buffer* buffer = shadowCascadeFixCB->CB();
		globals::d3d::context->VSSetConstantBuffers(7, 1, &buffer);
		globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
	}

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
	return dist < maxCascadeCoverageVS;
}