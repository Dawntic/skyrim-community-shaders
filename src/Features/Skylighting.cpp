#include "Skylighting.h"

#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.skylighting."

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
	if (ImGui::Button("Reload Shaders"))
		ClearShaderCache();

	if (ImGui::Button("Generate albedo and norm")) {
		auto outputPath = cachePath / "Tamriel_A.dds";
		auto outputPath2 = cachePath / "Tamriel_N.dds";
		BuildAtlas(outputPath, "");
		BuildAtlas(outputPath2, "_n");
	}
	if (ImGui::Button("Generate card Occl")) {
		GenerateCardinalOcclusionMap();
	}

	if (ImGui::Button("Generate bent normal")) {
		GenerateBentNormalMap();
	}

	ImGui::Text("Minimum visibility values. Diffuse darkens objects. Specular removes the sky from reflections.");
	ImGui::SliderFloat("Diffuse Min Visibility", &settings.MinDiffuseVisibility, 0.01f, 1.f, "%.2f");
	ImGui::SliderFloat("Specular Min Visibility", &settings.MinSpecularVisibility, 0.01f, 1.f, "%.2f");

	ImGui::Separator();

	ImGui::Checkbox("Enable Lighting", (bool*)&settings.toggleLighting);  //tmp
	ImGui::Checkbox("Enable Trees", (bool*)&settings.toggleTrees);
	ImGui::Checkbox("Enable Grass", (bool*)&settings.toggleGrass);
	ImGui::Checkbox("Enable Deferred", (bool*)&settings.toggleDeferred);
	ImGui::Checkbox("Enable Effect", (bool*)&settings.toggleEffect);

	std::string curr_worldspace = "N/A";
	auto tes = RE::TES::GetSingleton();
	if (tes) {
		auto worldspace = tes->GetRuntimeData2().worldSpace;
		if (worldspace) {
			curr_worldspace = worldspace->GetFormEditorID();
		}
	}
	ImGui::Text(fmt::format("Worldspace has cache: {}", worldSpaceCachedMapList.contains(curr_worldspace)).c_str());
	ImGui::Text("Cache is loaded: %s", (cacheWorldspaceID == curr_worldspace) ? "true" : "false");

	if (ImGui::Button("Rebuild Skylighting"))
		ResetSkylighting();

	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Changes below require rebuilding, a loading screen, or moving away from the current location to apply.");

	ImGui::SliderAngle("Max Zenith Angle", &settings.MaxZenith, 0, 90);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Smaller angles creates more focused top-down shadow.");

	//static float debugRescale = 5.0f;
	//ImGui::SliderFloat("View Resize", &debugRescale, 0.0f, 10.0f);
	//if (cacheOutputTexBN) {
	//	ImGui::BulletText("Bent Normal View");
	//	BUFFER_VIEWER_NODE_BULLET(cacheOutputTexBN, debugRescale)
	//}
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
		CD3D11_TEXTURE2D_DESC texDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, sparseGridSize.x, sparseGridSize.y, 3, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2DARRAY, texDesc.Format, 0, 1, 0, 3);
		CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2DARRAY, texDesc.Format, 0, 0, 3);

		texSparseProbeArray = eastl::make_unique<Texture2D>(texDesc);
		texSparseProbeArray->CreateSRV(srvDesc);
		texSparseProbeArray->CreateUAV(uavDesc);
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

	GetCachedWorldspaces();

	CompileComputeShaders();
}

void Skylighting::GetCachedWorldspaces()
{
	for (const auto& entry : std::filesystem::directory_iterator(cachePath)) {
		auto& path = entry.path();
		if (path.extension() == ".dds") {
			auto name = path.stem().string();
			logger::debug("[Skylighting] Found cache: {}", name);
			if (worldSpaceCachedMapList.contains(name))
				logger::warn("[Skylighting] Error: {} has multiple maps with same name", name);
			worldSpaceCachedMapList.insert(name);
		}
	}
}

bool Skylighting::LoadWorldspaceCache()
{
	static auto tes = RE::TES::GetSingleton();

	auto worldspace = tes->GetRuntimeData2().worldSpace;
	while (worldspace && worldspace->parentWorld)
		worldspace = worldspace->parentWorld;

	if (!worldspace)
		return false;

	std::string newWorldspaceID = worldspace->GetFormEditorID();

	if (cacheWorldspaceID == newWorldspaceID)
		return true;

	// need to test again texture suffix ig
	//if (!worldSpaceCachedMapList.contains(newWorldspaceID)) {
	//	logger::info("[Skylighting] No cache found for current worldspace");  //tmp otherwise flooding log
	//	return false;
	//}

	logger::info("[Skylighting] Loading cached texture maps...");

	// TODO: Check if xlodgen Lod exists first before trying to generate
	// Should package all these prebuilt
	{
		auto path = cachePath / (newWorldspaceID + "_A.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &AMapSRV);
		if (FAILED(result)) {
			BuildAtlas(path.stem(), "");
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_N.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &NMapSRV);
		if (FAILED(result)) {
			BuildAtlas(path.stem(), "_n");
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_BN.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &BNMapSRV);
		if (FAILED(result)) {
			GenerateBentNormalMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_CO.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &COMapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	{
		auto path = cachePath / (newWorldspaceID + "_CO2.dds");
		auto result = DirectX::CreateDDSTextureFromFile(globals::d3d::device, globals::d3d::context, path.c_str(), nullptr, &CO2MapSRV);
		if (FAILED(result)) {
			GenerateCardinalOcclusionMap();
		}
	}

	cacheWorldspaceID = newWorldspaceID;

	return true;
}

void Skylighting::GenerateBentNormalMap()
{
	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexBN = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> BNComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, BNMapSize, BNMapSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexBN = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexBN->CreateSRV(nullptr);
	cacheOutputTexBN->CreateUAV(uavDesc);

	BNComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "BENT_NORMALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav = cacheOutputTexBN->uav.get();
	context->CSSetShader(BNComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto heightSRV = globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	CacheGenCBStruct data;
	data.TexParams = float4(BNMapSize, BNMapSize, HeightMapOffset, HeightMapScale);
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto groups = (BNMapSize + 7) / 8;
	context->Dispatch(groups, groups, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	// Save output
	auto outputPath = cachePath / (cacheWorldspaceID + "_BN.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexBN->resource.get(), ouputImage));
	DX::ThrowIfFailed(DirectX::SaveToDDSFile(*ouputImage.GetImages(), DirectX::DDS_FLAGS_NONE, outputPath.c_str()));

	BNComputeShader.release();
}

void Skylighting::GenerateCardinalOcclusionMap()
{
	// Setup resources
	eastl::unique_ptr<Texture2D> cacheOutputTexCO = nullptr;
	eastl::unique_ptr<Texture2D> cacheOutputTexCO2 = nullptr;
	eastl::unique_ptr<ID3D11ComputeShader> COComputeShader = nullptr;

	CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R32G32B32A32_FLOAT, COMapSize, COMapSize, 1, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
	CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, desc.Format);

	cacheOutputTexCO = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexCO->CreateSRV(nullptr);
	cacheOutputTexCO->CreateUAV(uavDesc);

	cacheOutputTexCO2 = eastl::make_unique<Texture2D>(desc);
	cacheOutputTexCO2->CreateSRV(nullptr);
	cacheOutputTexCO2->CreateUAV(uavDesc);

	COComputeShader.reset(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Skylighting\\GenerateCacheMaps.hlsl", { { "CSHADER", "" }, { "CARDINALS", "" } }, "cs_5_0")));

	if (!cacheGenBuffer)
		cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	// Generate map
	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uav[2] = { cacheOutputTexCO->uav.get(), cacheOutputTexCO2->uav.get() };
	context->CSSetShader(COComputeShader.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 2, uav, nullptr);

	auto heightSRV = globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(0, 1, &heightSRV);

	ID3D11SamplerState* linSampler = globals::deferred->linearSampler;
	context->CSSetSamplers(0, 1, &linSampler);

	CacheGenCBStruct data;
	data.TexParams = float4(COMapSize, COMapSize, HeightMapOffset, HeightMapScale);
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto groups = (COMapSize + 7) / 8;
	context->Dispatch(groups, groups, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);

	// Save output
	auto outputPath = cachePath / (cacheWorldspaceID + "_CO.dds");
	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexCO->resource.get(), ouputImage));
	DX::ThrowIfFailed(DirectX::SaveToDDSFile(*ouputImage.GetImages(), DirectX::DDS_FLAGS_NONE, outputPath.c_str()));

	outputPath = cachePath / (cacheWorldspaceID + "_CO2.dds");
	DX::ThrowIfFailed(DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, cacheOutputTexCO2->resource.get(), ouputImage));
	DX::ThrowIfFailed(DirectX::SaveToDDSFile(*ouputImage.GetImages(), DirectX::DDS_FLAGS_NONE, outputPath.c_str()));

	COComputeShader.release();
}

void Skylighting::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&probeUpdateCompute,
		&updateSparseGridCS,
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
			{ &probeUpdateCompute, "UpdateProbesCS.hlsl", { { "DENSE_PROBE_GRID", "" } } },
			{ &updateSparseGridCS, "UpdateProbesCS.hlsl", { { "SPARSE_PROBE_GRID", "" } } },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\Skylighting") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}
}

void Skylighting::UpdateDenseProbeGrid()
{
	auto context = globals::d3d::context;

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Skylighting - Update Dense Probes");

	TracyD3D11Zone(globals::state->tracyCtx, "Skylighting - Update Dense Probes");

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

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void Skylighting::UpdateSparseProbeGrid()
{
	auto context = globals::d3d::context;

	//GenerateCardinalOcclusionMap();

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Skylighting - Update Sparse Probes");

	TracyD3D11Zone(state->tracyCtx, "Skylighting - Update Sparse Probes");

	auto uav = texSparseProbeArray->uav.get();
	context->CSSetShader(updateSparseGridCS.get(), nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11SamplerState* sampArray[3] = { globals::deferred->linearSampler, globals::features::physicalSky.sampNoise.get(), globals::features::physicalSky.sampSv.get() };
	context->CSSetSamplers(0, 1, sampArray);

	auto buffer = globals::features::physicalSky.cloudBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto& physSky = globals::features::physicalSky;
	auto& depthTexture = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11ShaderResourceView* srvs[] = {
		depthTexture.depthSRV,
		physSky.disoccTex->srv.get(),
		nullptr,
		nullptr,
		physSky.dataFieldsSRV.get(),
		physSky.cirrusShapeSRV.get(),
		physSky.vertProfileSRV.get(),
		physSky.noiseShapeSRV.get(),
		physSky.cloudBaseSRV.get(),
		physSky.cloudDetailSRV.get(),
		physSky.curlNoiseSRV.get(),
		physSky.weatherMapSRV.get(),

		physSky.texSvLut->srv.get(),
		globals::features::terrainShadows.texHeightMap->srv.get(),
		BNMapSRV,
		COMapSRV,
		CO2MapSRV,
		AMapSRV,
		NMapSRV,
	};

	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

	context->Dispatch((sparseGridSize.x + 7) / 8, (sparseGridSize.y + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

Skylighting::SkylightingCB Skylighting::GetCommonBufferData(bool a_inWorld)
{
	if (!a_inWorld)
		return Skylighting::SkylightingCB{};

	if (globals::state->isMapMenuOpen)
		return Skylighting::SkylightingCB{};

	auto tes = RE::TES::GetSingleton();
	auto worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;

	if (!worldspace)
		return Skylighting::SkylightingCB{};

	static float3 prevCellID = { 0, 0, 0 };

	auto eyePosNI = Util::GetEyePosition();
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

	float2 gridSpan = worldspace->maximumCoords - worldspace->minimumCoords;
	gridSpan = float2(std::abs(gridSpan.x), std::abs(gridSpan.y));

	auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());
	const auto& direction = dirLight->GetWorldDirection();
	float3 lightDir = { -direction.x, -direction.y, -direction.z };
	lightDir.Normalize();

	float4 Basis0;
	float4 Basis1;
	BuildOcclusionBasis(lightDir, Basis0, Basis1, 1.0);

	return {
		.OcclusionViewProj = OcclusionTransform,
		.OcclusionDir = OcclusionDir,
		.PosOffset = cellOrigin - eyePos,
		.ArrayOrigin = {
			((int)cellID.x - probeArrayDims[0] / 2) % probeArrayDims[0],
			((int)cellID.y - probeArrayDims[1] / 2) % probeArrayDims[1],
			((int)cellID.z - probeArrayDims[2] / 2) % probeArrayDims[2] },
		.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y, (int)cellIDDiff.z },

		.GridTexSize = sparseGridSize,
		.GridBounds = float4(worldspace->minimumCoords.x, worldspace->minimumCoords.y, worldspace->maximumCoords.x, worldspace->maximumCoords.y),
		.InvGridTexSize = 1.0f / float2((float)sparseGridSize.x, (float)sparseGridSize.y),
		.GridMinWorldCorner = worldspace->minimumCoords - float2(eyePos.x, eyePos.y),

		.InvGridSpan = 1.0 / gridSpan,
		.HasCache = worldHasCache,
		.toggleLighting = settings.toggleLighting,
		.toggleTrees = settings.toggleTrees,
		.toggleGrass = settings.toggleGrass,
		.toggleDeferred = settings.toggleDeferred,
		.toggleEffect = settings.toggleEffect,

		.MinDiffuseVisibility = settings.MinDiffuseVisibility,
		.MinSpecularVisibility = settings.MinSpecularVisibility,
		.Basis0 = Basis0,
		.Basis1 = Basis1,
	};
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

	worldHasCache = LoadWorldspaceCache();

	UpdateDenseProbeGrid();

	UpdateSparseProbeGrid();

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srvs[2] = { texProbeArray->srv.get(), texSparseProbeArray->srv.get() };
	context->PSSetShaderResources(50, 2, srvs);
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

	// Remove call to update local camera translation - we'll do it manually
	REL::safe_fill(REL::RelocationID(25643, 26185).address() + REL::Relocate(0x558, 0x511), REL::NOP, 15);

	MenuOpenCloseEventHandler::Register();
}

//////////////////////////////////////////////////////////////

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
		valid = property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal);
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
		uint corner = skylighting.frameCount % 4;

		float frustumSize = a_frustum->fTop;
		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum);
}

void Skylighting::SetViewFrustumVR::thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex)
{
	auto& skylighting = globals::features::skylighting;

	if (skylighting.inOcclusion) {
		uint corner = skylighting.frameCount % 4;

		float frustumSize = a_frustum->fTop;
		a_frustum->fBottom = (corner == 0 || corner == 1) ? -frustumSize : 0.0f;
		a_frustum->fLeft = (corner == 0 || corner == 2) ? -frustumSize : 0.0f;
		a_frustum->fRight = (corner == 1 || corner == 3) ? frustumSize : 0.0f;
		a_frustum->fTop = (corner == 2 || corner == 3) ? frustumSize : 0.0f;
	}

	func(a_camera, a_frustum, a_eyeIndex);
}

void Skylighting::RenderOcclusion()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto sky = globals::game::sky;

	if (sky) {
		auto precip = sky->precip;
		if (!shaderCache->IsEnabled()) {
			state->BeginPerfEvent("Precipitation Mask");
			precip->occlusionData.camera->local.translate = RE::PlayerCamera::GetSingleton()->cameraRoot->world.translate;
			Main_Precipitation_RenderOcclusion::func();
			state->EndPerfEvent();
			return;
		}

		if (!Util::IsInterior()) {
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
				PrecipitationShaderCubeSize = occlusionDistance;

				float originaLastCubeSize = precip->lastCubeSize;
				precip->lastCubeSize = PrecipitationShaderCubeSize;

				static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> _computeProjection{ REL::RelocationID(25643, 26185) };

				float3 PrecipitationShaderDirectionF;
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

				precip->occlusionData.camera->local.translate = RE::PlayerCamera::GetSingleton()->cameraRoot->world.translate;

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

	skylighting.RenderOcclusion();
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

struct TileInfo
{
	int cellX, cellY;
	DirectX::ScratchImage image;
};

static bool ParseTile(const std::filesystem::path& path, int& cellX, int& cellY)
{
	std::string stem = path.stem().string();
	// split by '.'
	std::vector<std::string> parts;
	std::string cur;
	for (char c : stem) {
		if (c == '.') {
			parts.push_back(cur);
			cur.clear();
		} else
			cur += c;
	}
	parts.push_back(cur);
	if (parts.size() < 4)
		return false;
	try {
		cellX = std::stoi(parts[2]);
		cellY = std::stoi(parts[3]);
	} catch (...) {
		return false;
	}
	return true;
}

void Skylighting::BuildAtlas(const std::filesystem::path& outputPath, std::string mapTag)
{
	std::vector<TileInfo> tiles;

	using namespace DirectX;

	for (auto& entry : std::filesystem::directory_iterator(lodPath)) {
		auto& path = entry.path();
		if (!path.has_extension() || _stricmp(path.extension().string().c_str(), ".dds") != 0)
			continue;

		std::string stem = path.stem().string();
		if (stem.rfind("tamriel.32.", 0) != 0)
			continue;

		if (stem[stem.size() - 2] == '_' && (mapTag.empty() || !stem.ends_with(mapTag)))
			continue;

		//logger::info("Stem: {}  :  INtag: {}", stem, mapTag);
		if (mapTag.empty())
			if (!stem.ends_with(mapTag)) {
				logger::info("Found Tag");
				continue;
			}

		TileInfo ti;
		if (!ParseTile(path, ti.cellX, ti.cellY))
			continue;

		HRESULT hr = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, nullptr, ti.image);
		if (FAILED(hr)) {
			logger::info("Failed load: {}", stem);
			return;
		}

		// Convert to RGBA32 for uniform blitting
		auto img = ti.image.GetImage(0, 0, 0);
		if (img && img->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			ScratchImage converted;
			hr = Convert(*img, DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
			if (FAILED(hr)) {
				logger::info("Failed Convert: {}", stem);
				return;
			}
			ti.image = std::move(converted);
		}

		logger::info("Added tile");
		tiles.push_back(std::move(ti));
	}

	logger::info("Tile Count: {}", tiles.size());
	if (tiles.empty())
		return;

	size_t lodTexSize = tiles[0].image.GetImage(0, 0, 0)->width;

	// Sort tiles and assign grid positions
	std::sort(tiles.begin(), tiles.end(), [](const TileInfo& a, const TileInfo& b) {
		return a.cellY != b.cellY ? a.cellY > b.cellY : a.cellX < b.cellX;
	});

	std::vector<int> uniqueX, uniqueY;
	for (auto& t : tiles) {
		if (std::find(uniqueX.begin(), uniqueX.end(), t.cellX) == uniqueX.end())
			uniqueX.push_back(t.cellX);
		if (std::find(uniqueY.begin(), uniqueY.end(), t.cellY) == uniqueY.end())
			uniqueY.push_back(t.cellY);
	}
	std::sort(uniqueX.begin(), uniqueX.end());
	std::sort(uniqueY.begin(), uniqueY.end(), std::greater<int>());  // north-up

	size_t atlasW = lodTexSize * uniqueX.size();
	size_t atlasH = lodTexSize * uniqueY.size();

	logger::info("Atlas Size: {}, {}", atlasW, atlasH);

	ScratchImage atlas;
	HRESULT hr = atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, atlasW, atlasH, 1, 1);
	if (FAILED(hr)) {
		logger::info("Failed Init");
		return;
	}

	// Zero-fill
	const Image* atlasImg = atlas.GetImage(0, 0, 0);
	memset(atlasImg->pixels, 0, atlasImg->slicePitch);

	for (auto& t : tiles) {
		logger::info("Doing tile");
		const Image* src = t.image.GetImage(0, 0, 0);
		auto col = std::find(uniqueX.begin(), uniqueX.end(), t.cellX) - uniqueX.begin();
		auto row = std::find(uniqueY.begin(), uniqueY.end(), t.cellY) - uniqueY.begin();

		size_t dstX = col * lodTexSize;
		size_t dstY = row * lodTexSize;

		for (size_t y = 0; y < lodTexSize; ++y) {
			uint8_t* dst = atlasImg->pixels + (dstY + y) * atlasImg->rowPitch + dstX * 4;
			const uint8_t* s = src->pixels + y * src->rowPitch;
			memcpy(dst, s, lodTexSize * 4);
		}
	}

	logger::info("Finished Atlas");

	//auto savePath =
	DirectX::SaveToDDSFile(*atlasImg, DDS_FLAGS_NONE, outputPath.c_str());
}

#undef I18N_KEY_PREFIX