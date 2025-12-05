#include "ShadowmapCascadeCullingFix.h"

void ShadowmapCascadeCullingFix::Install()
{
	gfSplitOverlap = reinterpret_cast<float*>(REL::RelocationID(513805, 391863).address());

	//stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));
}

void ShadowmapCascadeCullingFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	func(dirLight, outPlanes, frustumSplit, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);

	// This fix pulls the far face corners back towards the near face corners by double fSplitOverlap to provide an effective overlap of 1 * fSplitOverlap in each direction.
	// This corrects the vanilla behaviour which sets nearFace = farFace for the next cascade camera, where nearFace already includes +fSplitOverlap
	// which incorrectly pushes out the culling for the next cascade camera causing shadow gaps even at the default fSplitOverlap of 100.
	// This newly calculated farFace is not immediately used but will be copied into the nearFace for the next cascade camera and provide effective overlap.

	const float splitOverlap = *gfSplitOverlap * 2.0f;

	for (uint32_t i = 0; i < 4; ++i) {
		auto& nearCorner = frustumSplit.nearFace[i];
		auto& farCorner = frustumSplit.farFace[i];
		auto dir = farCorner - nearCorner;
		dir.Unitize();

		farCorner -= dir * splitOverlap;
	}
}

/*
* //BuildPlaneFromPointsCallBack = reinterpret_cast<decltype(BuildPlaneFromPointsCallBack)>(REL::RelocationID(70804, 70804).address());
	//SetPlaneFromPointAndNormal = reinterpret_cast<decltype(SetPlaneFromPointAndNormal)>(REL::RelocationID(70803, 70803).address());

void OrthogonalVolumetricLighting::Hooks::SetShadowMapCount::thunk(RE::BSShadowLight* light, uint64_t numCascades)
{
	if (light->IsDirectionalLight())
		numCascades = 4;
	logger::info("Set shadow map count");
	func(light, numCascades);
}

void OrthogonalVolumetricLighting::LogMatrix(std::string desc, DirectX::XMMATRIX inMatrix)
{
	DirectX::XMFLOAT4X4 matrix;
	XMStoreFloat4x4(&matrix, XMMatrixTranspose(inMatrix));  //convert to column vector to match shaders
	logger::info("{} row 1: {}, {}, {}, {}", desc, matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3]);
	logger::info("{} row 2: {}, {}, {}, {}", desc, matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3]);
	logger::info("{} row 3: {}, {}, {}, {}", desc, matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3]);
	logger::info("{} row 4: {}, {}, {}, {}", desc, matrix.m[3][0], matrix.m[3][1], matrix.m[3][2], matrix.m[3][3]);
}

void OrthogonalVolumetricLighting::LogVector(std::string desc, DirectX::XMVECTOR vec)
{
	logger::info("{}: {}, {}, {}, {}", desc, DirectX::XMVectorGetX(vec), DirectX::XMVectorGetY(vec), DirectX::XMVectorGetZ(vec), DirectX::XMVectorGetW(vec));
}

void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_RenderShadowmaps::thunk(RE::BSShadowLight* light, void* unk)
{
	//auto& lens = globals::features::orthogonalVolumetricLighting;

	//lens.CSMFinished = false;

	//if (lens.swapShadowMapShader) {
	//	lens.CSMFinished = false;
	//	lens.overrideShader = true;
	//	lens.shaderdesc = Shaders::RenderShadowMap;
	//}

	func(light, unk);
}

#pragma warning(push)
#pragma warning(disable: 4100 4456)

bool OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetFrameCamera::thunk(RE::BSShadowDirectionalLight* light, RE::NiCamera& inputCamera)
{
	auto& lens = globals::features::orthogonalVolumetricLighting;
	bool returnValue = true;

	lens.BuildShadowCascade(light, inputCamera);

	returnValue = func(light, inputCamera);

	return returnValue;
}

//2,6,7,5,1,0
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumCorners& frustumCorners, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	using namespace DirectX;
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	int firstCullOrder[8] = { 7, 3, 5, 1, 6, 2, 4, 0 };
	if (OVL.patchCascade && OVL.patchCulling) {
		for (int i = 0; i < 8; i++)                                                                                                                         // Set frustum corners as the far planes of the final cascade and the near plane of the 0th cascade
			frustumCorners.corners[i] = (i < 4) ? OVL.cascadeData[1].worldCorners[firstCullOrder[i]] : OVL.cascadeData[0].worldCorners[firstCullOrder[i]];  //dirLight->shadowMapCount - 1
	}

	func(dirLight, outPlanes, frustumCorners, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);
}

struct FrustumSplit
{
	RE::NiPoint3 nearFace[4];
	RE::NiPoint3 farFace[4];
};
//both cascades use: 6,2,0,1,5,7
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond::thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	using namespace DirectX;
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	auto float3ToNiPoint3 = [](const float3& vec) -> RE::NiPoint3 {
		return RE::NiPoint3(vec.x, vec.y, vec.z);
	};
	auto NiPoint3ToFloat3 = [](const RE::NiPoint3& vec) -> float3 {
		return float3(vec.x, vec.y, vec.z);
	};

	int SecondCullOrder[8] = { 6, 2, 4, 0, 7, 3, 5, 1 };
	if (OVL.patchCascade && OVL.patchCulling) {
		for (int i = 0; i < 4; i++) {
			frustumCorners.nearFace[i] = float3ToNiPoint3(OVL.cascadeData[OVL.cascadeIt].worldCorners[SecondCullOrder[i]]);
		}
		for (int i = 0; i < 4; i++) {
			frustumCorners.farFace[i] = float3ToNiPoint3(OVL.cascadeData[OVL.cascadeIt].worldCorners[SecondCullOrder[i + 4]]);
		}
	}

	func(dirLight, outPlanes, frustumCorners, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);
}

// Subsequent call propagates local changes to the rest of the camera
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetCameraRuntimeData2::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	if (OVL.patchCascade) {
		auto matrix = OVL.cascadeData[OVL.cascadeIt].viewRotation;
		auto row0Mat = RE::NiPoint3(matrix._13, matrix._12, matrix._11);
		auto row1Mat = RE::NiPoint3(matrix._23, matrix._22, matrix._21);
		auto row2Mat = RE::NiPoint3(matrix._33, matrix._32, matrix._31);
		RE::NiMatrix3 rotation = RE::NiMatrix3(row0Mat, row1Mat, row2Mat);

		cascadeCamera->local.rotate = rotation;
		cascadeCamera->local.translate = RE::NiPoint3(OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.x, OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.y, OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.z);
	}

	func(cascadeCamera, frustum);
}

// Override the final cascade frustum input
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_CreateFrustum::thunk(RE::NiFrustum& frustum, float left, float right, float top, float bottom, float nearP, float farP, bool ortho)
{
	auto& OVL = globals::features::orthogonalVolumetricLighting;
	auto frust = OVL.cascadeData[OVL.cascadeIt].frustum;

	if (OVL.patchCascade)
		func(frustum, frust.fLeft, frust.fRight, frust.fTop, frust.fBottom, frust.fNear, frust.fFar, ortho);
	else {
		func(frustum, left, right, top, bottom, nearP, farP, ortho);
	}

	//if (OVL.cascadeIt == 0) {
	//OVL.LogVector("frustum(left, right, top, bottom)", DirectX::XMVectorSet(frustum.fLeft, frustum.fRight, frustum.fTop, frustum.fBottom));
	//OVL.LogVector("frustum(near, far)", DirectX::XMVectorSet(frustum.fNear, frustum.fFar, 0, 0));
	//}

	OVL.cascadeIt = (++OVL.cascadeIt < 2) ? OVL.cascadeIt : 0;
}

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

	for (int cascade = 0; cascade < int(lightCascades); ++cascade) {
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
	}
}
*/