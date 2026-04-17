#include "OrthogonalVolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OrthogonalVolumetricLighting::Settings, bentNormalCacheProgress)

void OrthogonalVolumetricLighting::CompileShaders()
{
	generateBentNormalCS = nullptr;
	copyDepthCS = nullptr;
	updateSparseGridCS = nullptr;
	generateBentNormalCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Cubemaps.hlsl", { { "BENT_NORMAL_CS", "" } }, "cs_5_0");
	copyDepthCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Cubemaps.hlsl", { { "COPY_DEPTH_MAP", "" } }, "cs_5_0");
	updateSparseGridCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\ProbeGrid.hlsl", { { "UPDATE_GRID", "" } }, "cs_5_0");
}

RE::BSShaderProperty::RenderPassArray* OrthogonalVolumetricLighting::Hooks::GetRenderPassArray::thunk(RE::BSShaderProperty* prop, RE::BSGeometry* geometry, std::uint32_t flags, RE::BSShaderAccumulator* accumulator)
{
	RE::BSShaderProperty::RenderPassArray* renderPasses = func(prop, geometry, flags, accumulator);

	auto& ovl = globals::features::orthogonalVolumetricLighting;
	if (ovl.disablePipelineUI && ovl.disablePipeline)
		renderPasses = nullptr;

	return renderPasses;
}

void OrthogonalVolumetricLighting::disablePasses()
{
	if (disablePipelineUI && disablePipeline) {
		globals::d3d::context->PSSetShader(nullptr, 0, 0);
	}
}

void OrthogonalVolumetricLighting::SetupResources()
{
	auto device = globals::d3d::device;

	logger::info("[Debug] RUNNING SetupSkylightingResources()");

	// Probe Resources
	gridUpdateBuffer = new ConstantBuffer(ConstantBufferDesc<GridUpdateCBStruct>());
	clipRefOverrideBuffer = new ConstantBuffer(ConstantBufferDesc<GridUpdateCBStruct>());

	// Probe grid
	D3D11_TEXTURE2D_DESC probeDesc{};
	probeDesc.Width = PROBE_ARRAY_SIZE;
	probeDesc.Height = PROBE_ARRAY_SIZE;
	probeDesc.MipLevels = 1;
	probeDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	probeDesc.SampleDesc.Count = 1;
	probeDesc.Usage = D3D11_USAGE_DEFAULT;
	probeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	probeDesc.CPUAccessFlags = 0;
	probeDesc.MiscFlags = 0;
	probeDesc.ArraySize = 3;

	D3D11_UNORDERED_ACCESS_VIEW_DESC puavDesc{};
	puavDesc.Format = probeDesc.Format;
	puavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	puavDesc.Texture2DArray.MipSlice = 0;
	puavDesc.Texture2DArray.FirstArraySlice = 0;
	puavDesc.Texture2DArray.ArraySize = probeDesc.ArraySize;

	probeGridArray = eastl::make_unique<Texture2D>(probeDesc);
	probeGridArray->CreateSRV(nullptr);
	probeGridArray->CreateUAV(puavDesc);
	//

	// Bent Resources
	cacheGenBuffer = new ConstantBuffer(ConstantBufferDesc<CacheGenCBStruct>());

	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = CUBE_SIZE;
	viewport.Height = CUBE_SIZE;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	// depth cubemap
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = (uint)CUBE_SIZE;
	desc.Height = (uint)CUBE_SIZE;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	desc.ArraySize = 6;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = 6;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	uavDesc.Texture2DArray.MipSlice = 0;
	uavDesc.Texture2DArray.FirstArraySlice = 0;
	uavDesc.Texture2DArray.ArraySize = 6;

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	dsvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
	dsvDesc.Texture2DArray.MipSlice = 0;
	dsvDesc.Texture2DArray.FirstArraySlice = 0;
	dsvDesc.Texture2DArray.ArraySize = 6;

	depthCubemap = eastl::make_unique<Texture2D>(desc);
	depthCubemap->CreateSRV(srvDesc);
	depthCubemap->CreateUAV(uavDesc);
	//

	// Bent normal tex
	D3D11_TEXTURE2D_DESC bentNormalDesc{};
	bentNormalDesc.Width = (uint)BENT_NORMAL_SIZE.x;
	bentNormalDesc.Height = (uint)BENT_NORMAL_SIZE.y;
	bentNormalDesc.MipLevels = 1;
	bentNormalDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	bentNormalDesc.SampleDesc.Count = 1;
	bentNormalDesc.Usage = D3D11_USAGE_DEFAULT;
	bentNormalDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	bentNormalDesc.CPUAccessFlags = 0;
	bentNormalDesc.MiscFlags = 0;
	bentNormalDesc.ArraySize = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc{};
	UAVDesc.Format = bentNormalDesc.Format;
	UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	UAVDesc.Texture2D.MipSlice = 0;

	bentNormalTex = eastl::make_unique<Texture2D>(bentNormalDesc);
	bentNormalTex->CreateSRV(nullptr);
	bentNormalTex->CreateUAV(UAVDesc);
	//

	// Main depth override tex  - remove when merging into skylighting
	D3D11_TEXTURE2D_DESC depthDesc = {};
	depthDesc.Width = CUBE_SIZE;
	depthDesc.Height = CUBE_SIZE;
	depthDesc.MipLevels = 1;
	depthDesc.ArraySize = 1;
	depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Usage = D3D11_USAGE_DEFAULT;
	depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDescD{};
	dsvDescD.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsvDescD.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDescD = {};
	srvDescD.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	srvDescD.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDescD.Texture2D.MipLevels = 1;

	device->CreateTexture2D(&depthDesc, nullptr, &mainDepthTex);
	device->CreateDepthStencilView(mainDepthTex, &dsvDescD, &mainDepthDSV);
	device->CreateShaderResourceView(mainDepthTex, &srvDescD, &mainDepthSRV);
	//

	//tmp debug tex
	placementMap = eastl::make_unique<Texture2D>(bentNormalDesc);
	placementMap->CreateSRV(nullptr);
	placementMap->CreateUAV(UAVDesc);

	TryLoadCacheProgress();

	CompileShaders();
}

void OrthogonalVolumetricLighting::EarlyPrepass()
{
	frameCounter++;
	//if (globals::game::tes) {
	//	if (auto worldSpace = globals::game::tes->GetRuntimeData2().worldSpace) {
	//		static auto height = worldSpace->worldMapData.cameraData.maxHeight;
	//		worldSpace->worldMapData.cameraData.initialPitch = 90.0f;
	//		worldSpace->worldMapData.cameraData.maxHeight = height * 5.0f;
	//	}
	//}

	//if (auto tes = globals::game::tes) {
	//	if (auto worldSpace = tes->GetRuntimeData2().worldSpace) {
	//logger::info("Min: {}, {}", worldSpace->minimumCoords.x, worldSpace->minimumCoords.y);
	//logger::info("Max: {}, {}", worldSpace->maximumCoords.x, worldSpace->maximumCoords.y);
	//	}
	//}

	//cacheComplete = CheckWorldspaceCache();
	auto cell = RE::PlayerCharacter::GetSingleton()->parentCell;
	if (!cacheComplete && cell && !cell->IsInteriorCell()) {
		IterateWorldFullDepth();
	}
}

void OrthogonalVolumetricLighting::Prepass()
{
	if (test)
		UpdateSparseProbeGrid();
}

//// Bent Normals ///////////////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::TryLoadCacheProgress()
{
	if (!std::filesystem::exists(bentNormalPath)) {
		logger::info("[Skylighting] Cache not found; one will be generated");
		return;
	}

	cData.wave = (int)settings.bentNormalCacheProgress;
	cData.tile.y = cData.wave;
	if (cData.tile.y >= cData.TILE_TOTAL.y) {
		cData.tile.x = cData.wave - (cData.TILE_TOTAL.y - 1);
		cData.tile.y = cData.TILE_TOTAL.y - 1;  // Don't touch
	}

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::ScratchImage bentTex;
	DX::ThrowIfFailed(DirectX::LoadFromDDSFile(bentNormalPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, bentTex));

	ID3D11Resource* resource = nullptr;
	DX::ThrowIfFailed(DirectX::CreateTexture(device, bentTex.GetImages(), bentTex.GetImageCount(), bentTex.GetMetadata(), &resource));

	context->CopyResource(bentNormalTex->resource.get(), resource);

	context->CopyResource(placementMap->resource.get(), resource);  //tmp

	resource->Release();
}

void OrthogonalVolumetricLighting::BackupCacheProgress()
{
	settings.bentNormalCacheProgress = cData.wave;
	globals::state->Save();

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	DirectX::ScratchImage ouputImage;
	DX::ThrowIfFailed(DirectX::CaptureTexture(device, context, bentNormalTex->resource.get(), ouputImage));
	DX::ThrowIfFailed(DirectX::SaveToDDSFile(*ouputImage.GetImages(), DirectX::DDS_FLAGS_NONE, bentNormalPath.c_str()));
}

void OrthogonalVolumetricLighting::DisableCellPortals()
{
	static constexpr int RADIUS = 400;

	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		if (auto tes = RE::TES::GetSingleton()) {
			tes->ForEachReferenceInRange(player, RADIUS, [](RE::TESObjectREFR* ref) {
				if (ref->GetBaseObject() && ref->GetBaseObject()->Is(RE::FormType::Door)) {
					if (auto door = ref->GetBaseObject()->As<RE::TESObjectDOOR>()) {
						ref->SetActivationBlocked(true);
					}
					//if (auto npc = ref->GetBaseObject()->As<RE::TESNPC>()) {
					//	ref->SetActivationBlocked(true);
					//}
				}

				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
	}
}

float OrthogonalVolumetricLighting::GetRayIntersectionHeight(float3 position)
{
	static constexpr float RAY_OFFSET = 20000.0f;  // We check +- offset
	static constexpr float EYE_OFFSET = 0.0f;

	static float prevZ = 0.0f;
	auto player = RE::PlayerCharacter::GetSingleton();
	auto cell = (player) ? player->GetParentCell() : nullptr;
	auto worldspace = (player) ? player->GetWorldspace() : nullptr;

	if (worldspace && cell && cell->GetbhkWorld() && cell->cellState.any(RE::TESObjectCELL::CellState::kAttached)) {
		if (auto hkpWorld = cell->GetbhkWorld()->GetWorld1()) {
			float scale = RE::bhkWorld::GetWorldScale();
			float2 posScaledXY = float2(position.x * scale, position.y * scale);

			float currentZ = position.z + RAY_OFFSET;
			float endZ = position.z - RAY_OFFSET;
			int maxAttempts = 10;

			for (int i = 0; i < maxAttempts; i++) {
				RE::hkpWorldRayCastInput input;
				input.from.quad.m128_f32[0] = posScaledXY.x;
				input.from.quad.m128_f32[1] = posScaledXY.y;
				input.from.quad.m128_f32[2] = currentZ * scale;
				input.from.quad.m128_f32[3] = 0;
				input.to.quad.m128_f32[0] = posScaledXY.x;
				input.to.quad.m128_f32[1] = posScaledXY.y;
				input.to.quad.m128_f32[2] = endZ * scale;
				input.to.quad.m128_f32[3] = 0;

				RE::hkpWorldRayCastOutput output;
				hkpWorld->CastRay(input, output);

				if (!output.HasHit()) {
					logger::info("No hit");
					return prevZ + EYE_OFFSET;
				}
				auto collisionObj = output.rootCollidable->GetCollisionLayer();

				// Degenerate case - skipped obj is close to ground, we move currentZ past it and into something solid and dont find any hits
				if (!(collisionObj == RE::COL_LAYER::kTerrain || collisionObj == RE::COL_LAYER::kGround || collisionObj == RE::COL_LAYER::kStatic)) {
					float rayLength = currentZ - endZ;
					currentZ = currentZ - output.hitFraction * rayLength - (50.0f * scale);
					logger::trace("skipping obj: {}", collisionObj);
					continue;
				}

				if (i + 1 == maxAttempts) {
					logger::info("No valid hits");
					return prevZ + EYE_OFFSET;
				}

				logger::trace("good hit on obj: {}", collisionObj);

				// Valid hit
				float rayLength = currentZ - endZ;
				float hitZ = currentZ - output.hitFraction * rayLength;
				prevZ = hitZ;
				return hitZ + EYE_OFFSET;
			}
		}
	}

	logger::info("something is cooked");
	return prevZ + EYE_OFFSET;
}

//check if cache complete
void OrthogonalVolumetricLighting::IterateWorldFullDepth()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto cell = (player) ? player->GetParentCell() : nullptr;
	auto tes = RE::TES::GetSingleton();

	if (!runIterateWorld || !tes || !cell || !cell->IsAttached() || !cell->IsExteriorCell() || !cell->IsInitialized() || !player->Is3DLoaded())
		return;

	auto worldSpace = tes->GetRuntimeData2().worldSpace;  //tmp
	GRID_BOUND_TL = int2((int)worldSpace->minimumCoords.x, (int)worldSpace->minimumCoords.y);
	GRID_BOUND_BR = int2((int)worldSpace->maximumCoords.x, (int)worldSpace->maximumCoords.y);

	static bool updateLocation = true;
	auto& [START, END, STEP, TILE_SIZE, TILE_TOTAL, local, tile, wave] = cData;

	// Manual override: jump to specific world coords
	if (manualOverride) {
		manualOverride = false;
		int2 targetPX = (int2((int)manualStartWS.x, (int)manualStartWS.y) - START) / STEP;
		targetPX = int2(
			std::clamp(targetPX.x, 0, BENT_NORMAL_SIZE.x - 1),
			std::clamp(targetPX.y, 0, BENT_NORMAL_SIZE.y - 1));
		tile = targetPX / TILE_SIZE;
		local = targetPX - tile * TILE_SIZE;
		wave = tile.x + tile.y;
		updateLocation = true;
		logger::trace("Manual override: WS({}, {}) -> PX({}, {}) -> Tile({}, {}) Local({}, {}) Wave({})",
			manualStartWS.x, manualStartWS.y, targetPX.x, targetPX.y,
			tile.x, tile.y, local.x, local.y, wave);
	}

	DisableCellPortals();
	RE::GetINISetting("bBorderRegionsEnabled:General")->data.b = false;

	auto advanceTile = [&]() {
		tile.x++;
		tile.y--;
		if (tile.x >= TILE_TOTAL.x || tile.y < 0) {
			wave++;
			tile.x = 0;
			tile.y = wave;
			if (tile.y >= TILE_TOTAL.y) {
				tile.x = wave - (TILE_TOTAL.y - 1);
				tile.y = TILE_TOTAL.y - 1;
			}
			BackupCacheProgress();
		}
	};

	auto PixelAtBoundry = [&](int pos, int txPos, int txMax) {
		return pos >= TILE_SIZE || txPos >= txMax;
	};

	auto advancePixel = [&]() {
		local.y++;
		if (PixelAtBoundry(local.y, tile.y * TILE_SIZE + local.y, BENT_NORMAL_SIZE.y)) {
			local.y = 0;
			local.x++;
			if (PixelAtBoundry(local.x, tile.x * TILE_SIZE + local.x, BENT_NORMAL_SIZE.x)) {
				local.x = 0;
				advanceTile();
			}
		}
	};

	coordsPX = tile * TILE_SIZE + local;

	if (updateLocation) {
		int2 worldXY = START + coordsPX * STEP;
		coordsWS = float3((float)worldXY.x, (float)worldXY.y, 0);

		tes->GetLandHeight(RE::NiPoint3(coordsWS.x, coordsWS.y, 0), coordsWS.z);

		logger::trace("land height: {}", coordsWS.z);

		coordsWS.z = GetRayIntersectionHeight(coordsWS);

		float waterHeight = tes->GetWaterHeight(RE::NiPoint3(), cell);
		coordsWS.z += (waterHeight - coordsWS.z) * float(coordsWS.z < waterHeight);

		logger::trace("eye: {}", player->GetInfoRuntimeData().eyeHeight);

		logger::trace("Stage: Teleport:  Wave: {}  :  CoordsWS: {}, {}, {}", wave, coordsWS.x, coordsWS.y, coordsWS.z);

		RE::PlayerCharacter::GetSingleton()->SetPosition(RE::NiPoint3(coordsWS.x, coordsWS.y, coordsWS.z), false);
		updateLocation = false;
		return;
	} else {
		if (!IsPositionValid()) {
			RE::PlayerCharacter::GetSingleton()->SetPosition(RE::NiPoint3(coordsWS.x, coordsWS.y, coordsWS.z), false);
			return;
		}
		if (UpdateCubemapCapture()) {
			advancePixel();
			updateLocation = true;
		}
	}
}

bool OrthogonalVolumetricLighting::UpdateCubemapCapture()
{
	logger::trace("Updating cubemap");

	static int currentFace = -1;
	static RE::NiPoint3 valueSet;

	static const float pitchYaw[6][2] = {
		{ 0.0f, 3.14159265f / 2.0f },   // +X (east)
		{ 0.0f, -3.14159265f / 2.0f },  // -X (west)
		{ 0.0f, 0.0f },                 // +Y (north)
		{ 0.0f, 3.14159265f },          // -Y (south)
		{ -3.14159265f / 2.0f, 0.0f },  // +Z (up)
		{ 3.14159265f / 2.0f, 0.0f },   // -Z (down)
	};

	auto player = RE::PlayerCharacter::GetSingleton();
	auto camera = RE::PlayerCamera::GetSingleton();
	camera->GetRuntimeData2().idleTimer = 0;

	if (currentFace >= 0 && currentFace < 6) {  // Needs to happen before clearing last frames depth buffer
		auto& rot = camera->cameraRoot->world.rotate;
		RE::NiPoint3 currentForward = { std::round(rot.entry[0][1]), std::round(rot.entry[1][1]), std::round(rot.entry[2][1]) };  // Direction we just rendered for
		if (currentForward != valueSet) {
			logger::info("[Debug] INCORRECT CUBEMAP DIRECTION  :  look dir: {}, {}, {}    set dir: {}, {}, {}", currentForward.x, currentForward.y, currentForward.z, valueSet.x, valueSet.y, valueSet.z);
			player->data.angle.x = pitchYaw[currentFace][0];
			player->data.angle.z = pitchYaw[currentFace][1];
			RE::PlayerCamera::GetSingleton()->Update();

			return false;  // Let function run again
		}

		if (!IsPositionValid()) {
			RE::PlayerCharacter::GetSingleton()->SetPosition(RE::NiPoint3(coordsWS.x, coordsWS.y, coordsWS.z), false);
			return false;  // Early out so position updates
		}
		CopyDepthBufferToCubemap(currentFace);
	}
	currentFace++;

	if (currentFace >= 6) {
		logger::trace("Finished cubemap and rendered bent normal");
		GenerateBentNormalMap();
		currentFace = -1;
		return true;
	}

	// Set look at direction for the upcoming render
	player->data.angle.x = pitchYaw[currentFace][0];
	player->data.angle.z = pitchYaw[currentFace][1];

	valueSet.x = std::round(std::sin(pitchYaw[currentFace][1]) * std::cos(pitchYaw[currentFace][0]));  // Forward vector we just set
	valueSet.y = std::round(std::cos(pitchYaw[currentFace][1]) * std::cos(pitchYaw[currentFace][0]));
	valueSet.z = std::round(-std::sin(pitchYaw[currentFace][0]));

	return false;
}

// Render depth into seperate 512 tex for cubemap
void OrthogonalVolumetricLighting::RenderMainDepth()  // just render direct to cubemap??
{
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Bent Normal Depth Pass");

	globals::d3d::context->ClearDepthStencilView(mainDepthDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);

	globals::d3d::context->OMSetRenderTargets(0, nullptr, mainDepthDSV);
	globals::d3d::context->RSSetViewports(1, &viewport);

	CacheClipAlphaRefOverrideCBStruct data;
	data.AlphaTestRefRS = 0.1;
	clipRefOverrideBuffer->Update(data);

	auto buffer = clipRefOverrideBuffer->CB();

	globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);

	for (const auto& pass : depthPasses) {
		globals::d3d::context->PSSetConstantBuffers(11, 1, &buffer);
		globals::game::stateUpdateFlags->reset(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_TEST_REF);
		Hooks::BSBatchRenderer_RenderPassImmediately::func(pass.a_pass, pass.a_technique, pass.a_alphaTest, pass.a_renderFlags);
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);

	depthPasses.clear();

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::CopyDepthBufferToCubemap(int face)
{
	ZoneScoped;
	TracyD3D11Zone(state->tracyCtx, "Copy Depth Buffer - Bent Normals");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Copy Depth Buffer - Bent Normals");

	auto context = globals::d3d::context;

	CacheGenCBStruct data;
	data.BentNormalWritePx = coordsPX;
	data.BentNormalTexSize = BENT_NORMAL_SIZE;
	data.CubemapParams = float4(CUBE_SIZE, 1.0 / (float)CUBE_SIZE, CUBE_SIZE * CUBE_SIZE, CUBE_SIZE * CUBE_SIZE * 5);
	data.CubeMapWriteFace = face;
	cacheGenBuffer->Update(data);

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(copyDepthCS, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, depthCubemap->uav.address(), nullptr);

	context->CSSetShaderResources(0, 1, &mainDepthSRV);

	auto waveGroups = CUBE_SIZE / 8;
	context->Dispatch(waveGroups, waveGroups, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

// Seems game only renders once side per frame?
void OrthogonalVolumetricLighting::GenerateBentNormalMap()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "Generate Bent Normals");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Generate Bent Normals");

	context->CSSetShader(generateBentNormalCS, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, bentNormalTex->uav.address(), nullptr);

	context->CSSetShaderResources(0, 1, depthCubemap->srv.address());

	auto buffer = cacheGenBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->Dispatch(1, 1, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

bool OrthogonalVolumetricLighting::IsPositionValid()
{
	static int sequCounter = 0;

	auto tmpPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	float3 playerPos = float3(std::floor(tmpPos.x), std::floor(tmpPos.y), std::floor(tmpPos.z));

	bool valid = true;
	float3 diff = playerPos - coordsWS;
	if (diff.x > 5.0f || diff.y > 5.0f || diff.z > 5.0f) {
		logger::info("[Debug] INCORRECT WORLD POSITION  :  player {}, {}, {}   set coords {}, {}, {}", playerPos.x, playerPos.y, playerPos.z, coordsWS.x, coordsWS.y, coordsWS.z);
		valid = false;
	}

	auto tmpCPos = RE::Main::WorldRootCamera()->world.translate;
	float3 cameraPos = float3(tmpCPos.x, tmpCPos.y, tmpCPos.z);
	float2 diffB = float2(abs(cameraPos.x - coordsWS.x), abs(cameraPos.y - coordsWS.y));
	if (diffB.x > 100.0f || diffB.y > 100.0f) {
		logger::info("[Debug] INCORRECT CAMERA POSITION  :  camera {}, {}, {}   set coords {}, {}, {}", cameraPos.x, cameraPos.y, 0, coordsWS.x, coordsWS.y, 0);
		valid = false;
	}

	if (!valid) {
		++sequCounter;
	} else {
		sequCounter = 0;
	}

	if (sequCounter == 10) {  // should never happen
		coordsWS.z = playerPos.z;
		sequCounter = 0;
	}

	return valid;
}
/////////////////////////////////////////////////////////////////////////////////////////

//// Probe Grid /////////////////////////////////////////////////////////////////////////

#pragma warning(push)
#pragma warning(disable: 4244)  // Stop cast warnings for buffer update
void OrthogonalVolumetricLighting::UpdateSparseProbeGrid()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "Update Sparse Probe Grid");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Update Sparse Probe Grid");

	context->CSSetShader(updateSparseGridCS, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, probeGridArray->uav.address(), nullptr);
	context->CSSetUnorderedAccessViews(1, 1, placementMap->uav.address(), nullptr);

	context->CSSetSamplers(0, 1, &globals::deferred->linearSampler);
	context->CSSetShaderResources(0, 1, bentNormalTex->srv.address());

	GridUpdateCBStruct data;
	data.InvViewProj = globals::game::frameBufferCached.GetCameraViewProjInverse();
	data.GridTexSize = int2(PROBE_ARRAY_SIZE, PROBE_ARRAY_SIZE);
	data.InvGridTexSize = 1.0f / float2(PROBE_ARRAY_SIZE, PROBE_ARRAY_SIZE);
	data.GridMinCorner = float2(GRID_BOUND_TL.x, GRID_BOUND_BR.y);
	data.GridMaxCorner = float2(GRID_BOUND_TL.y, GRID_BOUND_BR.x);
	int2 GridSpan = int2(std::abs(GRID_BOUND_BR.x - GRID_BOUND_TL.x), std::abs(GRID_BOUND_TL.y - GRID_BOUND_BR.y));
	data.InvGridSpan = 1.0 / float2(GridSpan.x, GridSpan.y);
	gridUpdateBuffer->Update(data);

	auto buffer = gridUpdateBuffer->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto dispatch = PROBE_ARRAY_SIZE / 8;
	context->Dispatch(dispatch, dispatch, 1);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	context->PSSetShaderResources(69, 1, probeGridArray->srv.address());
	context->CSSetShaderResources(69, 1, probeGridArray->srv.address());

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}
#pragma warning(pop)

///////////////////////////////////////////////////////////
#pragma warning(push)
#pragma warning(disable: 4244)  // Stop cast warnings for buffer update
OrthogonalVolumetricLighting::GridUpdateCBStruct OrthogonalVolumetricLighting::GetCommonBufferData()
{
	GridUpdateCBStruct data;
	data.InvViewProj = globals::game::frameBufferCached.GetCameraViewProjInverse();
	data.GridTexSize = int2(PROBE_ARRAY_SIZE, PROBE_ARRAY_SIZE);
	data.InvGridTexSize = 1.0f / float2(PROBE_ARRAY_SIZE, PROBE_ARRAY_SIZE);
	data.GridMinCorner = float2(GRID_BOUND_TL.x, GRID_BOUND_BR.y);
	data.GridMaxCorner = float2(GRID_BOUND_TL.y, GRID_BOUND_BR.x);
	int2 GridSpan = int2(std::abs(GRID_BOUND_BR.x - GRID_BOUND_TL.x), std::abs(GRID_BOUND_TL.y - GRID_BOUND_BR.y));
	data.InvGridSpan = 1.0 / float2(GridSpan.x, GridSpan.y);
	data.toggleLighting = settings.toggleLighting | settings.toggleAll;
	data.toggleTrees = settings.toggleTrees | settings.toggleAll;
	data.toggleGrass = settings.toggleGrass | settings.toggleAll;
	data.toggleDeferred = settings.toggleDeferred | settings.toggleAll;
	data.toggleEffect = settings.toggleEffect | settings.toggleAll;

	return data;
}
#pragma warning(pop)

///// Settings ////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawSettings()
{
	if (CUBE_SIZE != globals::game::renderer->GetScreenSize().height) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
		ImGui::Text("Warning: depth buffer size does not match cube map size");
		ImGui::PopStyleColor();
	}

	ImGui::SeparatorText("Reload");
	ImGui::Button("Reload Shaders");
	if (ImGui::IsItemClicked()) {
		CompileShaders();
	}
	ImGui::Spacing();

	ImGui::Checkbox("Render Cubemap Depth", &enableUI);

	ImGui::Checkbox("Toggle All", (bool*)&settings.toggleAll);
	ImGui::Checkbox("Enable Lighting", (bool*)&settings.toggleLighting);
	ImGui::Checkbox("Enable Trees", (bool*)&settings.toggleTrees);
	ImGui::Checkbox("Enable Grass", (bool*)&settings.toggleGrass);
	ImGui::Checkbox("Enable Deferred", (bool*)&settings.toggleDeferred);
	ImGui::Checkbox("Enable Effect", (bool*)&settings.toggleEffect);
	ImGui::Checkbox("Render Probe Grid", (bool*)&test);
	//ImGui::SliderInt("Frames per pos", &BUFFER_FRAMES, 0, 1000);

	ImGui::SliderFloat("X", &manualStartWS.x, -230000, 230000);
	ImGui::SliderFloat("Y", &manualStartWS.y, -230000, 230000);
	ImGui::Button("Get Player Pos");
	if (ImGui::IsItemClicked()) {
		auto pos = RE::PlayerCharacter::GetSingleton()->GetPosition();
		manualStartWS.x = pos.x;
		manualStartWS.y = pos.y;
	}
	ImGui::Checkbox("Override", (bool*)&manualOverride);
	ImGui::Checkbox("Disable Rendering Pipeline", (bool*)&disablePipelineUI);
	ImGui::Checkbox("Iterate World", (bool*)&runIterateWorld);

	//static auto validPos = float3(0, 0, 0);
	//ImGui::Button("Check Pos");
	//if (ImGui::IsItemClicked()) {
	//	if (auto player = RE::PlayerCharacter::GetSingleton()) {
	//		auto pos = player->GetPosition();
	//		validPos.z = GetRayIntersectionHeight(float3(pos.x, pos.y, pos.z));
	//	}
	//}
	//ImGui::Text(fmt::format("Valid Z: {}", validPos.z).c_str());

	float2 posA = float2(0, 0);
	ImGui::SliderFloat("X", &posA.x, -230000, 230000);
	ImGui::SliderFloat("Y", &posA.y, -230000, 230000);
	ImGui::Button("Set camera pos");
	if (ImGui::IsItemClicked()) {
		RE::Main::WorldRootCamera()->world.translate = RE::NiPoint3(posA.x, posA.x, 10000.0f);
	}

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = 1.0f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.0f, 2.0f);
		if (bentNormalTex) {
			ImGui::BulletText("Bent Normal View");
			auto drawList = ImGui::GetWindowDrawList();
			drawList->AddCallback([](const ImDrawList*, const ImDrawCmd*) {
				auto context = globals::d3d::context;
				context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // disable blending
			},
				nullptr);

			ImGui::Image(bentNormalTex->srv.get(), { bentNormalTex->desc.Width * debugRescale, bentNormalTex->desc.Height * debugRescale });

			//BUFFER_VIEWER_NODE_BULLET(bentNormalTex, debugRescale)

			drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		}
		ImGui::TreePop();
	}

	ImGui::Button("Reload Placement Map");
	if (ImGui::IsItemClicked()) {
		globals::d3d::context->CopyResource(placementMap->resource.get(), bentNormalTex->resource.get());
	}

	if (ImGui::TreeNode("Buffer Viewer 2")) {
		static float debugRescaleT = 1.0f;
		ImGui::SliderFloat("View Resize 2", &debugRescaleT, 0.0f, 2.0f);
		if (bentNormalTex) {
			ImGui::BulletText("Placement View");
			auto drawList = ImGui::GetWindowDrawList();
			drawList->AddCallback([](const ImDrawList*, const ImDrawCmd*) {
				auto context = globals::d3d::context;
				context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);  // disable blending
			},
				nullptr);

			ImGui::Image(placementMap->srv.get(), { placementMap->desc.Width * debugRescaleT, placementMap->desc.Height * debugRescaleT });

			drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		}
		ImGui::TreePop();
	}

	//static float coordsX = 0;
	//static float coordsY = 0;
	//if (ImGui::Button("Teleport")) {
	//	RE::PlayerCharacter::GetSingleton()->SetPosition(RE::NiPoint3(coordsX, coordsY, 0), false);
	//}
	//ImGui::SliderFloat("X", &coordsX, -225000, 225000);
	//ImGui::SliderFloat("Y", &coordsY, -225000, 225000);
}

void OrthogonalVolumetricLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void OrthogonalVolumetricLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void OrthogonalVolumetricLighting::RestoreDefaultSettings()
{
	settings = {};
}
