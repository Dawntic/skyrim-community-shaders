#include "ShadowmapCascadeMatrixFix.h"
#include "../State.h"

#include "../ShaderCache.h"

#include "../Features/TerrainBlending.h"
/*
Fixes:
fix view proj variance
fix sky sync support
fix cascade mip bias
fix inter cascade blending
fix wind factoring
fix proj FOV factoring
fix map sampling fp precision
fix geometry model fp precision
fix cpu geometry translation
fix bounding aspect ratio
fix light altitude cap
fix light dir update variance
disable depth clipping
add support for 4 cascades
add time sliced rendering
*/

//TODO:
// Catch ini settings and override shadow settings
// Find SE addresses
// Only render first 2 VL maps
// Default 1024 cascades
// Deferred renderer should use my buffer
// Only render VL cascdes during 2nd frame

//ISSUES:
// culling breaks when setting very fig cascade distance and disabling culling doesn't fix it
// Distance things still flicker
// Do i Cap the altitude or find a way to slow down the light updating
// Do i remove wind from bones?
// Do VL Cascades render regardless of VL? they need to now for shadows

//RE-CHECK:
// Need more offset - cascade is wasting lots of room
// Sky sync compat
// Inteirors
// Test in base game

// Tested at 4k with 4 cascades - everything renders fine including VL maps(time slicing works)

bool ShadowmapMatrixFix::Install()
{
	// Sets up the cascade and culling cameras
	stl::write_vfunc<0x10, BSShadowDirectionalLight_SetFrameCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);

	//Render a cascade
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6));
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderVolumetricCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0x6F, 0x6F));

	// Clear the current frustum - we use it to set a new view matrix and translation
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_SetCameraRuntimeData2>(REL::RelocationID(108496, 108496).address() + REL::Relocate(0x1918, 0x1918));

	// Culls against min near and max far plane of any cascade
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0xC59, 0xC59));  //First call    need SE addr

	// Culls individual cascade frustum
	stl::write_thunk_call<BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond>(REL::RelocationID(101499, 108496).address() + REL::Relocate(0x1B12, 0x1C02, 0x1C82));  //Second call

	// Set limit on VL shadow cascade min size
	stl::write_thunk_call<CreateVolumetricCascadeStencilTarget>(REL::RelocationID(107175, 107175).address() + REL::Relocate(0x9DC, 0x9DC));

	// Fill VL shadows call
	//REL::safe_fill(REL::RelocationID(101495, 108489).address() + REL::Relocate(0x30, 0x30), REL::NOP, 76);

	// Need to use these somewhere
	//gShadowDistance = reinterpret_cast<float*>(REL::RelocationID(528314, 415263).address());
	//gInteriorShadowDistance = reinterpret_cast<float*>(REL::RelocationID(513755, 391724).address());

	stl::write_vfunc<0x2A, Test>(RE::VTABLE_BSLightingShaderProperty[0]);

	gCascadeBlendDist = reinterpret_cast<float*>(REL::RelocationID(513805, 391863).address());

	return true;
}
#pragma warning(push)
#pragma warning(disable: 4100 4456 4189)

RE::BSShaderProperty::RenderPassArray* ShadowmapMatrixFix::Test::thunk(RE::BSShaderProperty* prop, RE::BSGeometry* geometry, std::uint32_t flags, RE::BSShaderAccumulator* accumulator)
{
	RE::BSShaderProperty::RenderPassArray* renderPasses = func(prop, geometry, flags, accumulator);
	//if (renderPasses == nullptr) {
	//	return renderPasses;
	//}

	//using enum SIE::ShaderCache::LightingShaderTechniques;
	//using enum RE::BSShaderProperty::EShaderPropertyFlag8;
	using enum RE::BSShaderProperty::EShaderPropertyFlag;

	//accumulator->GetRuntimeData()->currentActive = false; // Does nothing

	//prop->SetFlags(kCastShadows, false);

	//BGSDefaultObjectManager::GetSingleton()->GetObject()

	if (renderingVLCascades) {
		//logger::info("Volumetric RenderPassArray:  flags: {}   prop: {:X}   geometry: {:X}", flags, (uintptr_t)prop, (uintptr_t)geometry);
		//if(geometry)
		//	VLGeometry.push_back(geometry);
	}

	if (renderingCascades) {
		logger::info("Cascade RenderPassArray:  flags: {}   prop: {:X}   geometry: {:X}", flags, (uintptr_t)prop, (uintptr_t)geometry);

		if (geometry) {
			if (RE::TESObjectREFR* refData = geometry->GetUserData()) {
				if (refData->loadedData) {
					if (auto refFlags = refData->loadedData->flags) {
						if ((refFlags & RE::TESObjectREFR::RecordFlags::kGround) != 0) {
							logger::info("kGround Found");
						}

						if ((refFlags & RE::TESObjectREFR::RecordFlags::kIsGroundPiece) != 0) {
							logger::info("kIsGroundPiece   Geom Name: {}  flags: {}", geometry->name.c_str(), prop->flags.underlying());
						}

						if (refData->QIsLODLandObject())
							logger::info("Found Land");
					}
				}
			}
		}
		//geometry->GetUserData()->Is(RE::FormType::Land);

		//(GetFormFlags() & RecordFlags::kInitialized) != 0;

		//logger::info("FormType: {}", RE::FormTypeToString(geometry->GetUserData()->GetFormType()));

		if (prop->flags.any(kParallaxOcclusion)) {
			logger::info("Has kParallaxOcclusion:  {}", geometry->name.c_str());
		}

		if (prop->flags.any(kFitSlope)) {
			logger::info("Has kFitSlope:  {}", geometry->name.c_str());
		}

		PrintSetShaderFlags(prop->flags.underlying());
		logger::info("Name: {}", geometry->name.c_str());
		//logger::info("Flags: {}", prop->flags.underlying());

		//if (const auto feature = prop->GetBaseMaterial()->GetFeature();
		auto& settings = globals::features::terrainBlending;
		if (settings.test) {
			geometry->CullGeometry(true);
			geometry->CullNode(true);
		}

		if (prop->flags.any(kMultiTextureLandscape, kLODLandscape, kNoLODLandBlend, kMultiIndexSnow)) {
			if (prop->flags.none(kSpecular, kVertexAlpha, kDecal, kDynamicDecal, kSoftLighting, kTreeAnim)) {
				logger::info("Culling:  {}    flags: {}", geometry->name.c_str(), prop->flags.underlying());
				geometry->CullGeometry(true);
				geometry->CullNode(true);

				//if(geometry->GetCollisionLayer() == RE::COL_LAYER::kTerrain || geometry->GetCollisionLayer() == RE::COL_LAYER::kGround){
				//geometry->CullGeometry(true);
				//geometry->CullNode(true);
				//geometry->AsFadeNode()->GetFlags()
				//logger::info("Culling:  {}", geometry->name.c_str());
			}
		}

		//logger::info("Coll Layer: {}", CollisionLayerToString(geometry->GetCollisionLayer()));

		//	for(auto& geom : VLGeometry){
		//	if(geometry == geom){
		//logger::info("Culling Match:  {}", geometry->name.c_str());
		//geometry->CullGeometry(true);
		//geometry->CullNode(true);
		//}
		//}

		//return nullptr; // only effects culling in world renders - color buffer

		//if (prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape, RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows)) {
		//	logger::info("MultiTextureLandscape");
		//prop->DoClearRenderPasses(); //Effects the whole pipeline not just shadows
		//currentPass->ClearRenderPass();  //Deadlocks
		//	return nullptr;
		//}

		/*
		RE::BSRenderPass* currentPass = renderPasses->head;
		while (currentPass != nullptr) {
			constexpr uint32_t LightingTechniqueStart = 0x4800002D;
			auto lightingTechnique = currentPass->passEnum - LightingTechniqueStart;

			//logger::info("PassEnum: {} : {:X}     lightingTechnique: {} : {:X}", currentPass->passEnum, currentPass->passEnum, lightingTechnique, lightingTechnique);
		//	if (currentPass->passEnum == 49222 || currentPass->passEnum == 49254) {
			////	logger::info("Found Enum: {:X}", currentPass->passEnum);
			//	currentPass->ClearRenderPass();  //Deadlocks
			//}



			//if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
			//	logger::info("MultiTextureLandscape");
			//	currentPass->ClearRenderPass();  //Deadlocks
			//}

			if (currentPass->shader->shaderType != RE::BSShader::Type::Lighting) {
				logger::info("Not lighting pass  type: {}", currentPass->shader->shaderType.underlying());
				//prop->SetFlags(kCastShadows, false);
				//prop->SetFlags(kAssumeShadowmask, false);
				//prop->SetFlags(kReceiveShadows, false);
				//prop->SetFlags(kZBufferWrite, false);
			}


			if (currentPass->shader->shaderType == RE::BSShader::Type::Lighting) {
				constexpr uint32_t LightingTechniqueStart = 0x4800002D;
				auto lightingTechnique = currentPass->passEnum - LightingTechniqueStart;
				auto lightingFlags = lightingTechnique & ~(~0u << 24);
				auto lightingType = static_cast<SIE::ShaderCache::LightingShaderTechniques>((lightingTechnique >> 24) & 0x3F);
				lightingFlags &= ~0b111000u;

				//if (lightingType == MTLand || lightingType == MTLandLODBlend || prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {

				// Seems to stop almost everything
					lightingFlags &= ~(static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ShadowDir) | static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::DefShadow));
					prop->SetFlags(kCastShadows, false);
					prop->SetFlags(kAssumeShadowmask, false);
					prop->SetFlags(kReceiveShadows, false);
					prop->SetFlags(kZBufferWrite, false);
				//}

				//prop->InvalidateMaterial(); //Bad clib Addr



				//if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kReceiveShadows)) {

				lightingTechnique = (static_cast<uint32_t>(lightingType) << 24) | lightingFlags;
				currentPass->passEnum = lightingTechnique + LightingTechniqueStart;

			//}

			currentPass = currentPass->next;
		}
		*/
	}

	return renderPasses;
}

/*
{
	logger::info("Flags: {},   Prop Ptr: {:X},   Accum Ptr: {:X}", flags, (uintptr_t)prop, (uintptr_t)accumulator);

	//
	if(renderingCascades){
		if(prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)){
			logger::info("kMultiTexture");
			lightingFlags |= static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ShadowDir) | static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::DefShadow);
		}


	}


	if(prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask)){
		logger::info("assume shadowmask");
	}

	return func(prop, geometry, flags, accumulator);
}
*/

// Ideally the VL cascade will be remvoed in the future
void ShadowmapMatrixFix::CreateVolumetricCascadeStencilTarget::thunk(RE::BSGraphics::Renderer* renderer, RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL stencil, RE::BSGraphics::DepthStencilTargetProperties* prop)
{
	//if (prop->width < 128) {
	//	prop->width = 128;
	//	prop->height = 128;
	//}

	prop->width = prop->height = std::max(prop->height, 128u);

	func(renderer, stencil, prop);
};

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
void ShadowmapMatrixFix::SetCascadeSplit(CascadeBounds::Split& outputSplits, const RE::NiFrustum& viewFrustum)
{
	auto& settings = globals::features::terrainBlending;

	auto LinearStep = [](float edge0, float edge1, float x) { return std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f); };

	auto ViewDepthToNDC = [](float depth, RE::NiFrustum frustum) { return (frustum.fFar * (depth - frustum.fNear) / (depth * (frustum.fFar - frustum.fNear))); };

	float cascadeSplits[4] = { (float)settings.splits[0], (float)settings.splits[1], (float)settings.splits[2], (float)settings.splits[3] };

	float BLEND_AREA = *gCascadeBlendDist * 2.0f;

	for (int i = 0; i < nCascades; i++) {
		outputSplits.splitVS[i] = cascadeSplits[i];
		outputSplits.splitLin[i] = LinearStep(viewFrustum.fNear, viewFrustum.fFar, cascadeSplits[i]);
		outputSplits.endSplitNDC[i] = ViewDepthToNDC(cascadeSplits[i], viewFrustum);
		float blendedVS = (i > 0) ? cascadeSplits[i - 1] - BLEND_AREA : viewFrustum.fNear;
		outputSplits.startSplitNDC[i] = ViewDepthToNDC(blendedVS, viewFrustum);
	}

	maxCascadeCoverageVS = outputSplits.splitVS[nCascades - 1] + 2000;  //Change name
}

void ShadowmapMatrixFix::BuildLightFrustum(DirectX::XMMATRIX& outLightView, Frustum& outFrustum, const CascadeBounds::Split& cascadeSplits, const Frustum& rootFrustum, const DirectX::XMVECTOR& lightDirection, const int cascadeIndex)
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

	float nearSplit = (cascadeIndex == 0) ? 0.0f : cascadeSplits.splitLin[cascadeIndex - 1];
	float FarSplit = cascadeSplits.splitLin[cascadeIndex];

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

	// Add trans vec here to avoid variance
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

	//constexpr float RANGE_EXTENSION = 2.0f;
	//constexpr float MIN_CULL_EXTENT = 2600.0f;
	float RANGE_MULT = settings.multiplerRange;
	float MIN_CULL_EXTENT = settings.minExtent;

	float centerZ = float3((boundingBox.cornerMin + boundingBox.cornerMax) * 0.5f).z;
	float halfExtentZ = abs(centerZ - boundingBox.cornerMin.z);

	// Build main proj frustum
	{
		// Adjust depth range for better precision - depth clipping is disabled
		float extent = halfExtentZ * RANGE_MULT;  // The lower this is the more spread the shadow map depth values are so higher means less precision

		float adjustedMin = centerZ - extent;
		float adjustedMax = centerZ + extent;

		//logger::info("adjustedMin: {}   adjustedMax: {}", adjustedMin, adjustedMax);

		outProj = DirectX::XMMatrixOrthographicOffCenterLH(boundingBox.cornerMin.x, boundingBox.cornerMax.x, boundingBox.cornerMin.y, boundingBox.cornerMax.y, adjustedMin, adjustedMax);
	}

	// Build culling frustum
	{
		// Cap min extent to avoid issues with small cascades
		// If we dont care about moutains having broken culling then mult could be way lower - check for terrain shadows before choosing?
		float extent = std::max(halfExtentZ * RANGE_MULT * settings.MultTwo, MIN_CULL_EXTENT) + settings.testVar;  //MIN_CULL_EXTENT = 5000 @ 2.0 range mult?  more like MultTwo == 4, testVar == 0 @ 3.0 range mult

		float adjustedMin = centerZ - extent;
		float adjustedMax = centerZ + extent;

		//logger::info("Culling  adjustedMin: {}   adjustedMax: {}", adjustedMin, adjustedMax);

		outCullProj = DirectX::XMMatrixOrthographicOffCenterLH(boundingBox.cornerMin.x, boundingBox.cornerMax.x, boundingBox.cornerMin.y, boundingBox.cornerMax.y, adjustedMin, adjustedMax);
	}
}

// Gribb-Hartmann extraction
void ShadowmapMatrixFix::GetCullPlanesFromVPMatrix(RE::NiFrustumPlanes& outPlanes, const DirectX::XMMATRIX& viewProj)
{
	using namespace DirectX;

	XMMATRIX viewProjCol = XMMatrixTranspose(viewProj);

	XMVECTOR planes[6];
	planes[0] = XMVectorAdd(viewProjCol.r[3], viewProjCol.r[0]);
	planes[1] = XMVectorSubtract(viewProjCol.r[3], viewProjCol.r[0]);
	planes[2] = XMVectorAdd(viewProjCol.r[3], viewProjCol.r[1]);
	planes[3] = XMVectorSubtract(viewProjCol.r[3], viewProjCol.r[1]);
	planes[4] = viewProjCol.r[2];
	planes[5] = XMVectorSubtract(viewProjCol.r[3], viewProjCol.r[2]);

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

//if cascade == 0 then run this - far plane is only 1 frame out of sync
// Builds culling frustum
void ShadowmapMatrixFix::SetupPrimaryCullPlanes(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	const XMMATRIX lightView = XMMatrixTranspose(XMLoadFloat4x4(&cascadeData[0].viewMatrix));
	const XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);
	const XMVECTOR lightCameraPos = XMVector3Transform(rootCameraPos, lightView);

	// Build bounding objects
	CascadeBounds::Sphere boundingSphere;
	BuildCascadeBoundingSphere(boundingSphere, primaryCullFrustum);

	CascadeBounds::AABB boundingBox;
	BuildCascadeAABB(boundingBox, lightCameraPos, boundingSphere);

	// Build projection transforms
	XMMATRIX lightProj = {};  // Not used
	XMMATRIX cullingProj = {};
	BuildCascadeProjectionMatrices(lightProj, cullingProj, boundingBox);

	const XMMATRIX cullingViewProj = XMMatrixMultiply(lightView, cullingProj);

	// Build culling planes
	GetCullPlanesFromVPMatrix(primaryCullPlanes, cullingViewProj);
}

#include "../Features/SkySync.h"
void ShadowmapMatrixFix::BuildShadowCascadeCameraInput(RE::BSShadowDirectionalLight* light, RE::NiCamera& rootCamera, const int cascadeIndex)
{
	using namespace DirectX;

	auto& settings = globals::features::terrainBlending;

	// Get root camera params
	const XMVECTOR rootCameraPos = NiPoint3ToXMVector(rootCamera.world.translate);
	RE::NiFrustum viewFrustum = rootCamera.GetRuntimeData2().viewFrustum;
	const XMMATRIX worldRotation = XMMatrixTranspose(XMLoadFloat3x3(reinterpret_cast<const XMFLOAT3X3*>(&rootCamera.world.rotate.entry)));

	//Re-calculate because root frustum FOV is too dynamic
	float& cameraFOVDeg = (*(float*)(REL::RelocationID(513786, 388785).address()));
	float hFOVRad = cameraFOVDeg * (XM_PI / 180.0f);
	float unitHalfWidth = tan(hFOVRad / 2);
	float unitHalfHeight = unitHalfWidth / (globals::state->screenSize.x / globals::state->screenSize.y);

	//float vFOVRad = 2.0f * atan(unitHalfHeight);

	viewFrustum.fRight = unitHalfWidth;
	viewFrustum.fLeft = -unitHalfWidth;
	viewFrustum.fTop = unitHalfHeight;
	viewFrustum.fBottom = -unitHalfHeight;

	// Discretize light dir to mitigate variance from time scale
	XMVECTOR lightDirection = XMVector3Normalize(NiPoint3ToXMVector(light->GetShadowDirectionalLightRuntimeData().sunVector));
	lightDirection = QuantizeLightDirection(lightDirection, settings.lightUpdateAngle);

	//if (settings.test)
	//	lightDirection = NiPoint3ToXMVector(globals::features::skySync.GetSunDirectionWithAltitudeLimit(settings.lightMinAngle));

	static CascadeBounds cascadeBoundData;

	SetCascadeSplit(cascadeBoundData.splitDist, viewFrustum);

	// Add this back once split settings are finalized
	//static bool setupSplits = true;
	//if (setupSplits) {
	//	SetCascadeSplit(cascadeBoundData.splitDist, viewFrustum);
	//	setupSplits = false;
	//}

	if (settings.test2) {
		float frustumSize = viewFrustum.fTop;

		viewFrustum.fRight = frustumSize;
		viewFrustum.fLeft = -frustumSize;
	}

	// Build frustums, view matrix
	Frustum rootFrustum;
	BuildRootFrustum(rootFrustum, viewFrustum, worldRotation);

	XMMATRIX lightView = {};
	Frustum lightFrustum;
	BuildLightFrustum(lightView, lightFrustum, cascadeBoundData.splitDist, rootFrustum, lightDirection, cascadeIndex);
	//LogMatrix("view", lightView);

	// Set min/max cascade extent corners for first culling round
	static constexpr int nearFarIndices[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };  // near corners -> far corners
	if (cascadeIndex == 0 || cascadeIndex == nCascades - 1) {
		for (int i = 0; i < 4; i++) {
			int index = (cascadeIndex == 0) ? nearFarIndices[i] : nearFarIndices[i + 4];
			//cascadeBoundData.cullExtent.frustum.corner[index] = lightFrustum.corner[index];
			primaryCullFrustum.corner[index] = lightFrustum.corner[index];
		}
	}

	// Build bounding objects
	BuildCascadeBoundingSphere(cascadeBoundData.boundingSphere, lightFrustum);
	//logger::info("bounding sphere radius: {}", cascadeBoundData.boundingSphere.radius);
	//LogVector("bounding sphere center", cascadeBoundData.boundingSphere.center);

	const XMVECTOR lightCameraPos = XMVector3Transform(rootCameraPos, lightView);

	BuildCascadeAABB(cascadeBoundData.boundingBox, lightCameraPos, cascadeBoundData.boundingSphere);

	//logger::info("bounding box min: {}, {}, {}", cascadeBoundData.boundingBox.cornerMin.x, cascadeBoundData.boundingBox.cornerMin.y, cascadeBoundData.boundingBox.cornerMin.z);
	//logger::info("bounding box max: {}, {}, {}", cascadeBoundData.boundingBox.cornerMax.x, cascadeBoundData.boundingBox.cornerMax.y, cascadeBoundData.boundingBox.cornerMax.z);

	// Build view projection transforms
	XMMATRIX lightProj = {};
	XMMATRIX cullingProj = {};
	BuildCascadeProjectionMatrices(lightProj, cullingProj, cascadeBoundData.boundingBox);
	//LogMatrix("proj", lightProj);

	const XMMATRIX viewProj = XMMatrixMultiply(lightView, lightProj);
	XMStoreFloat4x4(&cascadeData[cascadeIndex].viewProj, XMMatrixTranspose(viewProj));
	//LogMatrix("viewProj", viewProj);

	// Relative translation added to shader MS position to avoid dot prod precision loss - probably don't fuck with
	const XMMATRIX texProj = XMMatrixMultiply(XMMatrixScaling(0.5f, -0.5f, 1.0f), XMMatrixTranslation(0.5f, 0.5f, 0.0f));
	XMStoreFloat4x4(&cascadeData[cascadeIndex].viewProjTex, XMMatrixTranspose(XMMatrixMultiply(viewProj, texProj)));

	// Set translation for geometry to transform against
	XMStoreFloat3(&cascadeData[cascadeIndex].translation, rootCameraPos);

	XMStoreFloat4x4(&cascadeData[cascadeIndex].viewMatrix, XMMatrixTranspose(lightView));

	cascadeData[cascadeIndex].endDepthNDC = cascadeBoundData.splitDist.endSplitNDC[cascadeIndex];
	cascadeData[cascadeIndex].startDepthNDC = cascadeBoundData.splitDist.startSplitNDC[cascadeIndex];

	// Build culling planes
	const XMMATRIX cullingViewProj = XMMatrixMultiply(lightView, cullingProj);
	GetCullPlanesFromVPMatrix(cascadeData[cascadeIndex].cullingPlanes, cullingViewProj);
}

// Stop other cascades from being rendered this frame - other methods to defer the accumulator dispatch cause recursion deadlocks
void ShadowmapMatrixFix::DisableCullingPlanes(const RE::BSShadowDirectionalLight* light, const int cascadeIndex)
{
	if (auto cullingProcess = light->GetRuntimeData().shadowmapDescriptors[cascadeIndex].cullingProcess) {
		backupPlanes[cascadeIndex] = cullingProcess->customCullPlanes;
		for (int j = 0; j < 6; j++)
			cullingProcess->customCullPlanes.cullingPlanes[j].constant = 0;
	}
}

void ShadowmapMatrixFix::EnableCullingPlanes(const RE::BSShadowDirectionalLight* light, const int cascadeIndex)
{
	if (auto cullingProcess = light->GetRuntimeData().shadowmapDescriptors[cascadeIndex].cullingProcess) {
		for (int j = 0; j < 6; j++) {
			cullingProcess->customCullPlanes.cullingPlanes[j].constant = backupPlanes[cascadeIndex].cullingPlanes[j].constant;
			//logger::info("plane: {}", cullingProcess->customCullPlanes.cullingPlanes[j].constant);
		}
	}
}

// Update cascade camera matrices, cull planes etc.
// Runs once per frame
bool ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera::thunk(RE::BSShadowDirectionalLight* light, RE::NiCamera& inputCamera)
{
	static int frameCounter = 0;

	if (!initialized) {
		cascadePxSize = RE::GetINISetting("iShadowMapResolution:Display")->data.u;
		nCascades = RE::GetINISetting("iNumSplits:Display")->data.u;
		shadowCascadeFixCB = new ConstantBuffer(ConstantBufferDesc<ShadowDataCB>());
		initialized = true;
	}

	activeCascades = cascadeMasks[nCascades][frameCounter];

	globals::game::smState->shadowSceneNode[0]->GetRuntimeData().windMagnitude = 0.0f;

	auto& accum = light->GetRuntimeData().shadowmapDescriptors[0].shaderAccumulator;
	//logger::info("Cascade Accum Ptr: {:X}", (uintptr_t)accum.get());

	logger::info("Frame Camera");

	VLGeometry.clear();

	auto& cullProcessArray = light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray;
	for (auto& process : cullProcessArray) {
		logger::info("culling Process");  //How many?
	}
	//Build cascades we're rendering this frame
	for (int i = 0; i < nCascades; i++) {
		if (activeCascades & (1 << i))
			BuildShadowCascadeCameraInput(light, inputCamera, i);
	}

	// Only set for cascade 0 since result is the same
	if (frameCounter == 0)
		SetupPrimaryCullPlanes(light, inputCamera);

	// Run game func to init and update frame camera with new params
	bool ret = func(light, inputCamera);

	// Disable cascades we're not rendering this frame
	for (int i = 0; i < nCascades; i++) {
		if (!(activeCascades & (1 << i)))
			DisableCullingPlanes(light, i);
	}

	frameCounter = ++frameCounter < 2 ? frameCounter : 0;

	return ret;
}

// Subsequent call propagates local changes to the rest of the camera
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_SetCameraRuntimeData2::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	static int counter = 0;

	cascadeCamera->local.rotate = RE::NiMatrix3();
	cascadeCamera->local.translate = RE::NiPoint3(cascadeData[counter].translation.x, cascadeData[counter].translation.y, cascadeData[counter].translation.z);

	counter = ++counter < nCascades ? counter : 0;

	func(cascadeCamera, frustum);
}

// Defines culling planes of the closest and furthest cascade
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(
	RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners, uint32_t splitCornerIndices[8],
	uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	if (!globals::features::terrainBlending.disableCulling)
		outPlanes = primaryCullPlanes;
}

// Called per cascade
void ShadowmapMatrixFix::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanesSecond::thunk(
	RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumCorners,
	uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPosA, uint32_t cornerOffsetIndex)
{
	static int counter = 0;

	if (!globals::features::terrainBlending.disableCulling)  //Cant disable now because time slicing
		outPlanes = cascadeData[counter].cullingPlanes;

	counter = ++counter < nCascades ? counter : 0;
}

// Update buffer before VL cascades since everything is the same
void ShadowmapMatrixFix::BSShadowDirectionalLight_RenderShadowmaps_RenderVolumetricCascade::thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& desc, uint32_t* unk, uint32_t flags)
{
	static int pass = 0;

	if (!initialized) {
		return func(light, desc, unk, flags);
	}
	globals::game::smState->shadowSceneNode[0]->GetRuntimeData().windMagnitude = 0.0f;
	//Update cascade buffer
	ShadowDataCB data{};
	data.lightViewProj = cascadeData[pass].viewProj;
	for (int i = 0; i < nCascades; i++) {
		data.shadowmapViewProjUV[i] = cascadeData[i].viewProjTex;
		data.cascadeSplitEnds[i] = cascadeData[i].endDepthNDC;
		data.cascadeSplitStarts[i] = cascadeData[i].startDepthNDC;
	}
	data.numCascades = nCascades;
	shadowCascadeFixCB->Update(data);

	ID3D11Buffer* buffer = shadowCascadeFixCB->CB();
	globals::d3d::context->VSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->CSSetConstantBuffers(7, 1, &buffer);

	if (pass == 0) {
		renderingVLCascades = true;
		logger::info("Start VL Cascades");
	} else {
		logger::info("VL Cascades");
	}
	//TMP
	auto& normalRoughness = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED].SRV;
	globals::d3d::context->PSSetShaderResources(27, 1, &normalRoughness);

	desc.clearRenderTarget = activeCascades & (1 << desc.shadowmapIndex);

	//Only render first 2 VL cascades
	if (pass < 2)
		func(light, desc, unk, flags);

	pass = ++pass < nCascades ? pass : 0;

	if (pass == 0) {
		renderingVLCascades = false;
		logger::info("End VL Cascades");
	}
}

// Render main cascades - called after VL cascades
void ShadowmapMatrixFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, RE::BSShadowLight::ShadowmapDescriptor& desc, uint32_t* unk, uint32_t flags)
{
	if (!initialized) {
		return func(light, desc, unk, flags);
	}
	globals::game::smState->shadowSceneNode[0]->GetRuntimeData().windMagnitude = 0.0f;
	static int pass = 0;

	ShadowDataCB data{};
	data.lightViewProj = cascadeData[pass].viewProj;
	for (int i = 0; i < nCascades; i++) {
		data.shadowmapViewProjUV[i] = cascadeData[i].viewProjTex;
		data.cascadeSplitEnds[i] = cascadeData[i].endDepthNDC;
		data.cascadeSplitStarts[i] = cascadeData[i].startDepthNDC;
	}
	data.numCascades = nCascades;
	shadowCascadeFixCB->Update(data);

	if (pass == 0) {
		renderingCascades = true;
		logger::info("Start Cascades");
	} else {
		logger::info("Cascades");
	}

	ID3D11Buffer* buffer = shadowCascadeFixCB->CB();
	globals::d3d::context->VSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
	globals::d3d::context->CSSetConstantBuffers(7, 1, &buffer);

	func(light, desc, unk, flags);
	globals::game::smState->shadowSceneNode[0]->GetRuntimeData().windMagnitude = 0.0f;
	// Needed because VL shadow maps use the same descriptors...
	desc.clearRenderTarget = true;

	pass = ++pass < nCascades ? pass : 0;

	if (pass == 0) {
		renderingCascades = false;
		logger::info("End Cascades");
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

// This is used in True PBR during the render pass list generation
// It updates the flags for geometry retreiving shadows
// The flag is set wrong during pre culling when rendering more than 2 cascades
// and when the most distant cascade isn't rendered in the same frame
bool ShadowmapMatrixFix::GeometryInsideShadowBound(RE::BSGeometry* geometry)
{
	auto pos = geometry->worldBound.center - RE::Main::WorldRootCamera()->world.translate;
	float dist = pos.Length() - (geometry->worldBound.radius);
	return dist < maxCascadeCoverageVS;
}
