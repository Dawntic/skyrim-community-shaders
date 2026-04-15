#include "Skylighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Skylighting::Settings,
	MaxZenith,
	MinDiffuseVisibility,
	MinSpecularVisibility)

void Skylighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void Skylighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Skylighting::RestoreDefaultSettings()
{
	settings = {};
}

void Skylighting::ResetSkylighting()
{
	auto context = globals::d3d::context;
	UINT clr[1] = { 0 };
	context->ClearUnorderedAccessViewUint(texAccumFramesArray->uav.get(), clr);
	queuedResetSkylighting = false;
}

void Skylighting::DrawSettings()
{
	ImGui::Text("Minimum visibility values. Diffuse darkens objects. Specular removes the sky from reflections.");
	ImGui::SliderFloat("Diffuse Min Visibility", &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat("Specular Min Visibility", &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");

	ImGui::Separator();

	if (ImGui::Button("Generate Worldspace Cache"))
		buildingCache = true;

	ImGui::Text(fmt::format("Cells Completed: {}", cellCount).c_str());

	if (ImGui::Button("Rebuild Skylighting"))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes below require rebuilding, a loading screen, or moving away from the current location to apply.");

	ImGui::SliderAngle("Max Zenith Angle", &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Smaller angles creates more focused top-down shadow.");

	static float debugRescale = 5.0f;
	ImGui::SliderFloat("View Resize", &debugRescale, 0.0f, 10.0f);
	if (bentNormalCacheTex) {
		ImGui::BulletText("Bent Normal View");
		BUFFER_VIEWER_NODE_BULLET(bentNormalCacheTex, debugRescale)
	}
}

void Skylighting::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		texOcclusion = new Texture2D(texDesc);
		texOcclusion->CreateSRV(srvDesc);
		texOcclusion->CreateDSV(dsvDesc);
	}

	{
		D3D11_TEXTURE3D_DESC texDesc{
			.Width = probeArrayDims[0],
			.Height = probeArrayDims[1],
			.Depth = probeArrayDims[2],
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = {
				.MipSlice = 0,
				.FirstWSlice = 0,
				.WSize = texDesc.Depth }
		};

		texProbeArray = new Texture3D(texDesc);
		texProbeArray->CreateSRV(srvDesc);
		texProbeArray->CreateUAV(uavDesc);

		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UINT;

		texAccumFramesArray = new Texture3D(texDesc);
		texAccumFramesArray->CreateSRV(srvDesc);
		texAccumFramesArray->CreateUAV(uavDesc);
	}

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;  // Use comparison filtering
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;               // Address mode (Clamp for shadow maps)
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;  // Comparison function
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
	}

	{
		DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\Skylighting\\SpatiotemporalBlueNoise\\stbn_vec3_2Dx1D_128x128x64.dds", nullptr, stbn_vec3_2Dx1D_128x128x64.put());
	}

	GetCachedWorldspaces();

	CompileComputeShaders();
}

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute
	};

	for (auto shader : shaderPtrs)
		shader = nullptr;

	CompileComputeShaders();
}

void Skylighting::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &probeUpdateCompute, "UpdateProbesCS.hlsl", {} },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

Skylighting::SkylightingCB Skylighting::GetCommonBufferData(bool a_inWorld)
{
	if (!a_inWorld)
		return Skylighting::SkylightingCB{};

	if (globals::state->isMapMenuOpen)
		return Skylighting::SkylightingCB{};

	static float3 prevCellID = { 0, 0, 0 };

	auto eyePosNI = Util::GetEyePosition(0);
	auto eyePos = float3{ eyePosNI.x, eyePosNI.y, eyePosNI.z };

	float3 cellSize = {
		occlusionDistance / probeArrayDims[0],
		occlusionDistance / probeArrayDims[1],
		occlusionDistance * .5f / probeArrayDims[2]
	};
	auto cellID = eyePos / cellSize;
	cellID = { round(cellID.x), round(cellID.y), round(cellID.z) };
	auto cellOrigin = cellID * cellSize;
	float3 cellIDDiff = prevCellID - cellID;
	prevCellID = cellID;

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionDir = OcclusionDir,
		.PosOffset = cellOrigin - eyePos,
		.ArrayOrigin = {
			((int)cellID.x - probeArrayDims[0] / 2) % probeArrayDims[0],
			((int)cellID.y - probeArrayDims[1] / 2) % probeArrayDims[1],
			((int)cellID.z - probeArrayDims[2] / 2) % probeArrayDims[2] },
		.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z },
		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility
	};
}

void Skylighting::UpdateDenseProbeGrid()
{
	auto context = globals::d3d::context;

	{
		std::array<ID3D11ShaderResourceView*, 1> srvs = { texOcclusion->srv.get() };
		std::array<ID3D11UnorderedAccessView*, 2> uavs = { texProbeArray->uav.get(), texAccumFramesArray->uav.get() };
		std::array<ID3D11SamplerState*, 1> samplers = { comparisonSampler.get() };

		// Update probe array
		{
			context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(probeUpdateCompute.get(), nullptr, 0);
			context->Dispatch((probeArrayDims[0] + 7u) >> 3, (probeArrayDims[1] + 7u) >> 3, probeArrayDims[2]);
		}

		// Reset
		{
			srvs.fill(nullptr);
			uavs.fill(nullptr);
			samplers.fill(nullptr);

			context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	}
}

void Skylighting::Prepass()
{
	if (globals::state->isMapMenuOpen)
		return;

	bool interior = true;

	if (auto sky = globals::game::sky)
		interior = sky->mode.get() != RE::Sky::Mode::kFull;

	if (interior)
		return;

	if (buildingCache)
		return;

	LoadWorldspaceBentNormalMap();

	TracyD3D11Zone(globals::state->tracyCtx, "Skylighting - Update Probes");

	UpdateDenseProbeGrid();

	// Set PS shader resources
	{
		ID3D11ShaderResourceView* srvs[2] = { texProbeArray->srv.get(), stbn_vec3_2Dx1D_128x128x64.get() };
		globals::d3d::context->PSSetShaderResources(50, 2, srvs);
	}
}

void Skylighting::PostPostLoad()
{
	logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
	stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
	stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	if (REL::Module::IsVR())
		stl::write_thunk_call<SetViewFrustumVR>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));
	else
		stl::write_thunk_call<SetViewFrustum>(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x5D9, 0x59D, 0x5DC));

	// Remove call to update local camera translation - need to add logic for when not building cache!!
	REL::safe_fill(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x511, 0x511), REL::NOP, 15);  // TEMP
	stl::detour_thunk<SetViewport>(REL::RelocationID(77240, 77240));

	MenuOpenCloseEventHandler::Register();
}

//////////////////////////////////////////////////////////////

struct BSParticleShaderRainEmitter
{
	void* vftable_BSParticleShaderRainEmitter_0;
	char _pad_8[4056];
};

enum class ShaderTechnique
{
	// Sky
	SkySunOcclude = 0x2,

	// Grass
	GrassNoAlphaDirOnlyFlatLit = 0x3,
	GrassNoAlphaDirOnlyFlatLitSlope = 0x5,
	GrassNoAlphaDirOnlyVertLitSlope = 0x6,
	GrassNoAlphaDirOnlyFlatLitBillboard = 0x13,
	GrassNoAlphaDirOnlyFlatLitSlopeBillboard = 0x14,

	// Utility
	UtilityGeneralStart = 0x2B,

	// Effect
	EffectGeneralStart = 0x4000002C,

	// Lighting
	LightingGeneralStart = 0x4800002D,

	// DistantTree
	DistantTreeDistantTreeBlock = 0x5C00002E,
	DistantTreeDepth = 0x5C00002F,

	// Grass
	GrassDirOnlyFlatLit = 0x5C000030,
	GrassDirOnlyFlatLitSlope = 0x5C000032,
	GrassDirOnlyVertLitSlope = 0x5C000033,
	GrassDirOnlyFlatLitBillboard = 0x5C000040,
	GrassDirOnlyFlatLitSlopeBillboard = 0x5C000041,
	GrassRenderDepth = 0x5C00005C,

	// Sky
	SkySky = 0x5C00005E,
	SkyMoonAndStarsMask = 0x5C00005F,
	SkyStars = 0x5C000060,
	SkyTexture = 0x5C000061,
	SkyClouds = 0x5C000062,
	SkyCloudsLerp = 0x5C000063,
	SkyCloudsFade = 0x5C000064,

	// Particle
	ParticleParticles = 0x5C000065,
	ParticleParticlesGryColorAlpha = 0x5C000066,
	ParticleParticlesGryColor = 0x5C000067,
	ParticleParticlesGryAlpha = 0x5C000068,
	ParticleEnvCubeSnow = 0x5C000069,
	ParticleEnvCubeRain = 0x5C00006A,

	// Water
	WaterSimple = 0x5C00006B,
	WaterSimpleVc = 0x5C00006C,
	WaterStencil = 0x5C00006D,
	WaterStencilVc = 0x5C00006E,
	WaterDisplacementStencil = 0x5C00006F,
	WaterDisplacementStencilVc = 0x5C000070,
	WaterGeneralStart = 0x5C000071,

	// Sky
	SkySunGlare = 0x5C006072,

	// BloodSplater
	BloodSplaterFlare = 0x5C006073,
	BloodSplaterSplatter = 0x5C006074,
};

//////////////////////////////////////////////////////////////

RE::BSShaderProperty::RenderPassArray* Skylighting::BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl::thunk(
	RE::BSLightingShaderProperty* property,
	RE::BSGeometry* geometry,
	[[maybe_unused]] uint32_t renderMode,
	[[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
{
	auto& skylighting = globals::features::skylighting;

	auto batch = accumulator->GetRuntimeData().batchRenderer;
	batch->geometryGroups[14]->flags &= ~1;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	using enum RE::BSUtilityShader::Flags;

	auto* precipitationOcclusionMapRenderPassList = &property->occlusionPasses;

	precipitationOcclusionMapRenderPassList->Clear();
	if (skylighting.inOcclusion) {
		if (property->flags.any(kSkinned) && property->flags.none(kTreeAnim))
			return precipitationOcclusionMapRenderPassList;
	} else {
		if (property->flags.any(kSkinned))
			return precipitationOcclusionMapRenderPassList;
	}

	if (skylighting.inOcclusion) {
		if (auto userData = geometry->GetUserData()) {
			RE::BSFadeNode* fadeNode = nullptr;

			RE::NiNode* parent = geometry->parent;
			while (parent && !fadeNode) {
				fadeNode = parent->AsFadeNode();
				parent = parent->parent;
			}

			if (fadeNode) {
				if (auto extraData = fadeNode->GetExtraData("BSX")) {
					auto bsxFlags = (RE::BSXFlags*)extraData;
					auto value = static_cast<int32_t>(bsxFlags->value);

					if (value & (static_cast<int32_t>(RE::BSXFlags::Flag::kRagdoll) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kEditorMarker) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kDynamic) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kAddon) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kNeedsTransformUpdate) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kMagicShaderParticles) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kLights) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kBreakable) |
									static_cast<int32_t>(RE::BSXFlags::Flag::kSearchedBreakable))) {
						return precipitationOcclusionMapRenderPassList;
					}
				}
			}
		}
	}

	bool valid = false;

	if (skylighting.inOcclusion) {
		if (!skylighting.buildingCache)
			valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
		else {
			valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kEyeReflect, kDecal, kDynamicDecal) && !(property->flags.any(kTreeAnim) && property->flags.none(kLODObjects));
		}
	} else {
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
	}

	if (valid) {
		if (geometry->worldBound.radius > 32) {
			stl::enumeration<RE::BSUtilityShader::Flags> technique;
			technique.set(RenderDepth);

			if (property->flags.any(kVertexColors)) {
				technique.set(Vc);
			}

			const auto alphaProperty = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get());
			if (alphaProperty && alphaProperty->GetAlphaTesting()) {
				technique.set(Texture);
				technique.set(AlphaTest);
			}

			if (property->flags.any(kLODObjects, kHDLODObjects)) {
				technique.set(LodObject);
			}

			if (property->flags.any(kTreeAnim)) {
				technique.set(TreeAnim);
			}

			if (property->flags.any(kLODLandscape)) {
				technique.set(LodLandscape);
			}

			precipitationOcclusionMapRenderPassList->EmplacePass(
				globals::game::utilityShader,
				property,
				geometry,
				technique.underlying() + static_cast<uint32_t>(ShaderTechnique::UtilityGeneralStart));
		}
	}

	return precipitationOcclusionMapRenderPassList;
}

void Skylighting::SetViewFrustum::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		if (!skylighting.buildingCache) {
			uint corner = skylighting.frameCount % 4;

			float frustumSize = a_frustum->fTop;
			a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
			a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
			a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
			a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
		} else {
			auto& rootCameraData = RE::Main::WorldRootCamera()->GetRuntimeData2();
			*a_frustum = rootCameraData.viewFrustum;
			a_frustum->fLeft = -a_frustum->fTop;
			a_frustum->fRight = a_frustum->fTop;
		}
	}

	func(a_camera, a_frustum);
}

void Skylighting::SetViewFrustumVR::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		if (!skylighting.buildingCache) {
			uint corner = skylighting.frameCount % 4;

			float frustumSize = a_frustum->fTop;
			a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
			a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
			a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
			a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
		} else {
			auto& rootCameraData = RE::Main::WorldRootCamera()->GetRuntimeData2();
			*a_frustum = rootCameraData.viewFrustum;
			a_frustum->fLeft = -a_frustum->fTop;
			a_frustum->fRight = a_frustum->fTop;
		}
	}

	func(a_camera, a_frustum, a_eyeIndex);
}

void Skylighting::RenderOcclusion()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;

	auto& skylighting = globals::features::skylighting;

	if (!shaderCache->IsEnabled()) {
		state->BeginPerfEvent("Precipitation Mask");
		Main_Precipitation_RenderOcclusion::func();
		state->EndPerfEvent();
		return;
	}

	if (sky) {
		if (!Util::IsInterior()) {
			auto precip = sky->precip;
			if (!buildingCache) {
				state->BeginPerfEvent("Precipitation Mask");

				auto precipObject = precip->currentPrecip;
				if (!precipObject) {
					precipObject = precip->lastPrecip;
				}

				if (precipObject) {
					precip->SetupMask();
					auto& effect = precipObject->GetGeometryRuntimeData().shaderProperty;
					auto shaderProp = effect.get();
					auto particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(shaderProp);
					auto rain = (RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);

					precip->RenderMask(rain);
				}

				state->EndPerfEvent();
			}

			{
				state->BeginPerfEvent("Skylighting Mask");

				if (queuedResetSkylighting)
					ResetSkylighting();

				frameCount++;

				auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				RE::BSGraphics::DepthStencilData precipitationCopy = precipitation;

				precipitation.depthSRV = texOcclusion->srv.get();
				precipitation.texture = texOcclusion->resource.get();
				precipitation.views[0] = texOcclusion->dsv.get();

				static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 401590).address());
				float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

				static RE::NiPoint3& PrecipitationShaderForward = (*(RE::NiPoint3*)REL::RelocationID(515509, 401648).address());
				RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderForward;

				inOcclusion = true;
				PrecipitationShaderCubeSize = occlusionDistance * !buildingCache;

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> _computeProjection{ REL::RelocationID(25643, 26185) };

				float3 PrecipitationShaderDirectionF;
				if (!buildingCache) {
					float2 vPoint;
					{
						constexpr float rcpRandMax = 1.f / RAND_MAX;
						static int randSeed = std::rand();
						static uint randFrameCount = 0;

						// r2 sequence
						vPoint = float2(randSeed * rcpRandMax) + (float)randFrameCount * float2(0.245122333753f, 0.430159709002f);
						vPoint.x -= static_cast<unsigned long long>(vPoint.x);
						vPoint.y -= static_cast<unsigned long long>(vPoint.y);

						randFrameCount++;
						if (randFrameCount == 1000) {
							randFrameCount = 0;
							randSeed = std::rand();
						}

						// disc transformation
						vPoint.x = sqrt(vPoint.x * sin(settings.MaxZenith));
						vPoint.y *= 6.28318530718f;

						vPoint = { vPoint.x * cos(vPoint.y), vPoint.x * sin(vPoint.y) };
					}

					PrecipitationShaderDirectionF = -float3{ vPoint.x, vPoint.y, sqrt(1 - vPoint.LengthSquared()) };
					PrecipitationShaderDirectionF.Normalize();

					PrecipitationShaderForward = { PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z };
				} else {
					static std::array<std::pair<RE::NiPoint3, RE::NiPoint3>, 6> cubemapDirs = { {
						{ { 1, 0, 0 }, { 0, 1, 0 } },
						{ { -1, 0, 0 }, { 0, 1, 0 } },
						{ { 0, 1, 0 }, { 0, 0, -1 } },
						{ { 0, -1, 0 }, { 0, 0, 1 } },
						{ { 0, 0, 1 }, { 0, 1, 0 } },
						{ { 0, 0, -1 }, { 0, 1, 0 } },
					} };

					static RE::NiPoint3& PrecipitationShaderUp = (*(RE::NiPoint3*)REL::RelocationID(388555, 388555).address());

					PrecipitationShaderUp = cubemapDirs[skylighting.cubemapSide].second;
					PrecipitationShaderForward = cubemapDirs[skylighting.cubemapSide].first;
					PrecipitationShaderDirectionF = *reinterpret_cast<float3*>(&cubemapDirs[skylighting.cubemapSide].first);

					precip->occlusionData.camera->local.translate = float3(sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z);  // disable the mem fill if removing

					_computeProjection(precip, precip->occlusionData.camera);

					precipitation.depthSRV = depthCubemap->srv.get();
					precipitation.texture = depthCubemap->resource.get();
					precipitation.views[0] = depthCubemapDSVs[cubemapSide];

					if (skylighting.buildingCache && skylighting.cubemapSide == 5) {  // change back to cubemapSide == 0 when we get rid of height prepass /////////////////////////////////
						static auto* precipObjectArrayList = *(RE::BSTArray<RE::NiPointer<RE::NiAVObject>>**)REL::RelocationID(415017, 415017).address();
						static uint& precipObjectArrayListSize = *(uint*)REL::RelocationID(415019, 415019).address();

						static auto* rootNodeLandLOD = *(RE::NiNode**)REL::RelocationID(402324, 402324).address();
						static auto* rootNodeObjectLOD = *(RE::NiNode**)REL::RelocationID(402325, 402325).address();
						static auto* rootNodeTreeLOD = *(RE::NiNode**)REL::RelocationID(402321, 402321).address();  // It makes sense for trees to contribute to the extent that they do not cause probe self shadowing

						std::array<RE::NiNode*, 3> nodeLODList = { rootNodeLandLOD, rootNodeObjectLOD, rootNodeTreeLOD };

						for (uint node = 0; node < nodeLODList.size(); node++) {
							auto& children = nodeLODList[node]->GetChildren();
							for (uint16_t child = 0, arrayIdx = 0; child < children.size(); child += !children[child]) {
								if (children[child]) {
									if (precipObjectArrayList[arrayIdx].size() < precipObjectArrayList[arrayIdx].capacity()) {
										precipObjectArrayList[arrayIdx].push_back(children[child++]);
									} else {
										if (++arrayIdx == precipObjectArrayListSize) {
											logger::error("No more room in culling object array");
											break;
										}
									}
								}
							}
						}
					}
				}

				precip->SetupMask();

				BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
				{
					TracyD3D11Zone(state->tracyCtx, "Skylighting - Render Height Map");
					precip->RenderMask((RE::BSParticleShaderRainEmitter*)rain);
				}
				inOcclusion = false;

				OcclusionDir = -float4{ PrecipitationShaderDirectionF.x, PrecipitationShaderDirectionF.y, PrecipitationShaderDirectionF.z, 0 };
				OcclusionTransform = ((RE::BSParticleShaderRainEmitter*)rain)->occlusionProjection;

				delete rain;

				PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
				precip->lastCubeSize = originaLastCubeSize;

				PrecipitationShaderForward = originalParticleShaderDirection;

				precipitation = precipitationCopy;

				_computeProjection(precip, precip->occlusionData.camera);

				state->EndPerfEvent();
			}
		}
	}
}

void Skylighting::Main_Precipitation_RenderOcclusion::thunk()
{
	auto& skylighting = globals::features::skylighting;

	if (!skylighting.buildingCache) {
		skylighting.RenderOcclusion();
	} else {
		skylighting.GenerateWorldspaceCache();
	}
}

RE::BSEventNotifyControl Skylighting::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// When entering a new cell through a loadscreen, update every frame until completion
	if (a_event->menuName == RE::LoadingMenu::MENU_NAME) {
		if (!a_event->opening)
			globals::features::skylighting.queuedResetSkylighting = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}

void Skylighting::GetCachedWorldspaces()
{
	for (const auto& entry : std::filesystem::directory_iterator(cachePath)) {
		auto& path = entry.path();
		if (path.extension() == ".dds") {
			auto name = path.stem().string();
			logger::debug("Found cache: {}", name);
			if (bentNormalMaps.contains(name))
				logger::warn("{} has multiple bent normal maps", name);
			bentNormalMaps.insert(name);
		}
	}
}

void Skylighting::LoadWorldspaceBentNormalMap()
{
	static auto tes = RE::TES::GetSingleton();

	auto worldspace = tes->GetRuntimeData2().worldSpace;
	while (worldspace && worldspace->parentWorld)
		worldspace = worldspace->parentWorld;

	if (!worldspace)
		return;

	std::string worldspace_name = worldspace->GetFormEditorID();
	if (currentBentNormalMap == worldspace_name)
		return;

	if (!bentNormalMaps.contains(worldspace_name)) {
		logger::info("[Skylighting] No bent normal map for worldspace");
		return;
	}

	logger::debug("[Skylighting] Loading bent normal map...");

	auto path = cachePath / (worldspace_name + ".dds");
	DirectX::ScratchImage image;
	DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));

	ID3D11Resource* pResource = nullptr;
	DX::ThrowIfFailed(DirectX::CreateTexture(globals::d3d::device, image.GetImages(), image.GetImageCount(), image.GetMetadata(), &pResource));

	bentNormalMap.reset();
	bentNormalMap = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));
	bentNormalMap->CreateSRV(nullptr);

	currentBentNormalMap = worldspace_name;
}

void Skylighting::CreateCachingResources()
{
	CD3D11_TEXTURE2D_DESC cubeDesc(DXGI_FORMAT_R32_TYPELESS, DEPTH_CUBE_SIZE, DEPTH_CUBE_SIZE, 6, 1, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2DARRAY, DXGI_FORMAT_R32_FLOAT, 0, 1, 0, 6);
	CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2DARRAY, DXGI_FORMAT_D32_FLOAT, 0, 0, 1);
	depthCubemap = eastl::make_unique<Texture2D>(cubeDesc);
	depthCubemap->CreateSRV(srvDesc);
	for (uint32_t i = 0; i < cubeDesc.ArraySize; i++) {
		dsvDesc.Texture2DArray.FirstArraySlice = i;
		globals::d3d::device->CreateDepthStencilView(depthCubemap->resource.get(), &dsvDesc, &depthCubemapDSVs[i]);
	}

	CD3D11_TEXTURE2D_DESC stagingDepthDesc(DXGI_FORMAT_R32_FLOAT, DEPTH_CUBE_SIZE, DEPTH_CUBE_SIZE, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
	stagingDepthTex = eastl::make_unique<Texture2D>(stagingDepthDesc);

	cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());
	clipRefOverrideBuffer = new ConstantBuffer(ConstantBufferDesc<AlphaRefCBStruct>());

	bentNormalComputeShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateBentNormalCS.hlsl", { { "BENT_NORMAL_COMPUTE", "" } }, "cs_5_0");
}

bool Skylighting::CreateUniqueCachingResources(int2 totalCells)
{
	stagingHeightMapTex.Release();
	bentNormalCacheTex.reset();

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded && terrainShadows.IsHeightMapReady()) {
		DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, terrainShadows.texHeightMap->resource.get(), stagingHeightMapTex);
		if (DirectX::IsCompressed(stagingHeightMapTex.GetMetadata().format)) {
			DirectX::ScratchImage decompressed;
			DirectX::Decompress(*stagingHeightMapTex.GetImages(), DXGI_FORMAT_R16_UNORM, decompressed);
			stagingHeightMapTex = std::move(decompressed);
		}
	} else {
		logger::error("Unable to retrieve a heightmap for the current worldspace from TerrainShadows");
		return false;
	}

	CD3D11_TEXTURE2D_DESC bentNormalDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, totalCells.x, totalCells.y, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, bentNormalDesc.Format);
	bentNormalCacheTex = eastl::make_unique<Texture2D>(bentNormalDesc);
	bentNormalCacheTex->CreateSRV(nullptr);
	bentNormalCacheTex->CreateUAV(uavDesc);

	return true;
}

void Skylighting::SetInitalState(RE::NiPoint3& initalPos)
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto worldspace = player ? player->GetWorldspace() : nullptr;

	const auto& rootFrustum = RE::Main::WorldRootCamera()->GetRuntimeData2().viewFrustum;

	auto& camData = RE::PlayerCamera::GetSingleton()->GetRuntimeData2();
	cachedActorFOV = camData.worldFOV;
	cachedActorPosition = player->GetPosition();

	static constexpr float viewFOV = 120.0f;  // Makes sure terrain chunks do not get culled
	camData.worldFOV = viewFOV;
	camData.firstPersonFOV = viewFOV;

	auto worldArea = worldspace->maximumCoords - worldspace->minimumCoords;
	float fovYRad = viewFOV * (DirectX::XM_PI / 180.0f);
	float fovXRad = 2.0f * atan(tan(fovYRad * 0.5f) * (globals::state->screenSize.x / globals::state->screenSize.y));
	float distZ = std::max((worldArea.x * 0.5f) / tan(fovXRad * 0.5f), (worldArea.y * 0.5f) / tan(fovYRad * 0.5f)) + 50000.0f;
	distZ = std::min(distZ, rootFrustum.fFar);

	auto worldCenterCoords = worldspace->minimumCoords + worldspace->maximumCoords;
	initalPos = RE::NiPoint3(worldCenterCoords.x, worldCenterCoords.y, distZ);

	// Look at -Z
	player->data.angle.x = 3.14159265f / 2.0f;
	player->data.angle.z = 0.0f;

	static REL::Relocation<void(uint64_t, uint64_t, uint64_t)> _toggleCollision{ REL::RelocationID(22825, 22825) };
	_toggleCollision(0, 0, 0);

	player->SetPosition(initalPos, false);
}

void Skylighting::GenerateWorldspaceCache()
{
	static constexpr float CELL = 4096.0f;

	auto tes = RE::TES::GetSingleton();
	auto player = RE::PlayerCharacter::GetSingleton();
	auto worldspace = player ? player->GetWorldspace() : nullptr;
	auto cell = (player) ? player->GetParentCell() : nullptr;

	static auto totalCells = int2();
	static auto targetCellID = int2();
	static auto worldPositionSet = RE::NiPoint3();

	if (tes && worldspace && cell && cell->IsExteriorCell()) {
		static auto prevWorldspaceID = "0";
		auto worldspaceID = worldspace->GetFormEditorID();
		static int bufferFrames = 30;

		if (worldspaceID != prevWorldspaceID) {
			totalCells = int2((int)std::ceil((std::abs(worldspace->minimumCoords.x) + worldspace->maximumCoords.x) / CELL),
				(int)std::ceil((std::abs(worldspace->minimumCoords.y) + worldspace->maximumCoords.y) / CELL));

			targetCellID = int2(0, totalCells.y);

			bool loadedResources = CreateUniqueCachingResources(totalCells);

			if (!loadedResources)
				return;

			SetInitalState(worldPositionSet);

			static bool firstLoad = true;
			if (firstLoad) {
				CreateCachingResources();
				firstLoad = false;

				logger::info("[Skylighting] Beginning worldspace cache...");
			}

			prevWorldspaceID = worldspaceID;
			bufferFrames = 30;
			return;  // Let position update
		}

		// Let world chunks load when entering new worldspace
		if (bufferFrames) {
			--bufferFrames;
			return;
		}

		const auto& rootFrustum = RE::Main::WorldRootCamera()->GetRuntimeData2().viewFrustum;
		RE::PlayerCamera::GetSingleton()->GetRuntimeData2().idleTimer = 0;

		auto positionStray = player->GetPosition() - worldPositionSet;
		bool validPosition = std::abs(std::max(positionStray.x, std::max(positionStray.y, positionStray.z))) < CELL / 2;
		if (!validPosition) {
			logger::error("[Skylighting] Invalid position; actual: {} : diff: {}... trying again", player->GetPosition(), positionStray);
			player->SetPosition(worldPositionSet, false);
			return;
		}

		float2 targetCellOffset = float2((float)targetCellID.x * CELL, (float)targetCellID.y * CELL);
		float2 cellWorldCorner = float2(worldspace->minimumCoords.x, worldspace->minimumCoords.y) + targetCellOffset;
		float2 cellWorldCenter = cellWorldCorner + float2(CELL * 0.5f, CELL * 0.5f);

		float heightMapHeight = SampleHeightMap(float2(cellWorldCenter.x, cellWorldCenter.y));
		heightMapHeight += CELL;

		sampleCoordsWS = float3(cellWorldCenter.x, cellWorldCenter.y, heightMapHeight);  // needed for terrain height render

		// Get accurate terrain height
		cubemapSide = 5;
		RenderOcclusion();

		auto context = globals::d3d::context;
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->CopySubresourceRegion(stagingDepthTex->resource.get(), D3D11CalcSubresource(0, 0, 1), 0, 0, 0, depthCubemap->resource.get(), D3D11CalcSubresource(0, 5, 1), nullptr);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		context->Map(stagingDepthTex->resource.get(), 0, D3D11_MAP_READ, 0, &mapped);

		auto center = uint((float)DEPTH_CUBE_SIZE * 0.5f);
		auto row1 = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mapped.pData) + center * mapped.RowPitch)[center];
		auto row2 = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mapped.pData) + center * mapped.RowPitch)[center - 1];
		auto row3 = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mapped.pData) + (center - 1) * mapped.RowPitch)[center];
		auto row4 = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mapped.pData) + (center - 1) * mapped.RowPitch)[center - 1];

		float nearPlane = 35.0f;
		float groundDiff = std::max({ row1, row2, row3, row4 });
		groundDiff = (nearPlane * rootFrustum.fFar) / (-groundDiff * (rootFrustum.fFar - nearPlane) + rootFrustum.fFar);

		context->Unmap(stagingDepthTex->resource.get(), 0);

		float groundHeight = sampleCoordsWS.z - groundDiff;
		float waterHeight = worldspace->GetDefaultWaterHeight();
		groundHeight += (waterHeight - groundHeight) * float(groundHeight < waterHeight);

		sampleCoordsWS.z = groundHeight + 100;

		logger::trace("Sample coords: {}, {}, {}", sampleCoordsWS.x, sampleCoordsWS.y, sampleCoordsWS.z);  //

		GenerateVisibilityCubemap();

		GenerateBentNormal(targetCellID, totalCells);

		++cellCount;

		auto rowEnd = targetCellID.y & 1 ? 0 : totalCells.x - 1;
		if (targetCellID.x == rowEnd) {
			if (--targetCellID.y < 0) {
				logger::info("[Skylighting] Finished caching worldspace");
				FinishCaching(worldspaceID);
				return;
			}
		} else {
			targetCellID.y & 1 ? --targetCellID.x : ++targetCellID.x;
		}
	}
}

float Skylighting::SampleHeightMap(float2 coords)
{
	auto& cachedHeightmap = globals::features::terrainShadows.cachedHeightmap;
	float u = (coords.x - cachedHeightmap->pos0.x) / (cachedHeightmap->pos1.x - cachedHeightmap->pos0.x);
	float v = (coords.y - cachedHeightmap->pos0.y) / (cachedHeightmap->pos1.y - cachedHeightmap->pos0.y);
	auto& img = *stagingHeightMapTex.GetImages();
	int ix = std::clamp((int)(u * img.width), 0, (int)img.width - 1);
	int iy = std::clamp((int)(v * img.height), 0, (int)img.height - 1);
	auto row = reinterpret_cast<const uint16_t*>(img.pixels + iy * img.rowPitch);
	float normalizedHeight = row[ix];

	return (normalizedHeight - 32767) * 8.0f;
}

void Skylighting::GenerateVisibilityCubemap()
{
	auto context = globals::d3d::context;

	AlphaRefCBStruct data;
	data.AlphaTestRefRS = 0.1;
	clipRefOverrideBuffer->Update(data);

	auto buffer = clipRefOverrideBuffer->CB();
	context->PSSetConstantBuffers(11, 1, &buffer);

	for (cubemapSide = 0; cubemapSide < 5; cubemapSide++) {
		globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_TEST_REF);
		RenderOcclusion();
	}

	context->OMSetRenderTargets(0, nullptr, nullptr);
}

void Skylighting::GenerateBentNormal(int2 currentCellXY, int2 totalCells)
{
	auto context = globals::d3d::context;

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Generate Bent Normal");

	context->CSSetShader(bentNormalComputeShader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, bentNormalCacheTex->uav.address(), nullptr);

	context->CSSetShaderResources(0, 1, depthCubemap->srv.address());

	CacheGenCBStruct data;
	data.BentNormalWritePx = currentCellXY;
	data.BentNormalTexSize = totalCells;
	data.CubemapParams = float4(DEPTH_CUBE_SIZE, 1.0f / (float)DEPTH_CUBE_SIZE, DEPTH_CUBE_SIZE * DEPTH_CUBE_SIZE, DEPTH_CUBE_SIZE * DEPTH_CUBE_SIZE * 5);
	data.CubeMapWriteFace = cubemapSide;
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->Dispatch(1, 1, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void Skylighting::FinishCaching(std::string worldName)
{
	if (!std::filesystem::exists(cachePath))
		std::filesystem::create_directories(cachePath);

	auto outputPath = cachePath / (worldName + ".dds");

	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, bentNormalCacheTex->resource.get(), ouputImage));
	DX::ThrowIfFailed(DirectX::SaveToDDSFile(*ouputImage.GetImages(), DirectX::DDS_FLAGS_NONE, outputPath.c_str()));

	auto player = RE::PlayerCharacter::GetSingleton();
	player->SetPosition(cachedActorPosition, false);

	auto& camData = RE::PlayerCamera::GetSingleton()->GetRuntimeData2();
	camData.worldFOV = cachedActorFOV;
	camData.firstPersonFOV = cachedActorFOV;

	static REL::Relocation<void(uint64_t, uint64_t, uint64_t)> _toggleCollision{ REL::RelocationID(22825, 22825) };
	_toggleCollision(0, 0, 0);

	buildingCache = false;
}

void Skylighting::SetViewport::thunk(RE::BSGraphics::Renderer* renderer, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
	func(renderer, arg1, arg2, arg3);

	auto& skylighting = globals::features::skylighting;
	if (skylighting.inOcclusion && skylighting.buildingCache) {
		D3D11_VIEWPORT port = {};
		port.Width = DEPTH_CUBE_SIZE;
		port.Height = DEPTH_CUBE_SIZE;
		port.MaxDepth = 1.0f;

		globals::game::shadowState->GetRuntimeData().viewPort = port;
	}
}