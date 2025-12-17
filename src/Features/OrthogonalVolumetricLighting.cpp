#include "OrthogonalVolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OrthogonalVolumetricLighting::Settings,
	extinction,
	anisotropy,
	localLightsAnisotropy,
	localLightsMultiplier,
	scatteringRatio,
	globalFogDensity,
	globalFogStartHeight,
	globalFogFalloffHeight,
	skyAmbientContribution,
	sceneAmbientContribution,
	esmExponent,
	color_saturation,
	preExposure,
	useHistory,
	disocclutionThreshold,
	distanceFadeIn,
	blendOpp,
	fogMapData,
	UIfogMapParams)

void OrthogonalVolumetricLighting::CompileShaders()
{
	bypassVS = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "BYPASS_VSSHADER", "" } }, "vs_5_0");
	generatePerlinCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "PERLIN_COMPUTE", "" } }, "cs_5_0");
	drawFogMapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "DRAW_FOGMAP", "" } }, "cs_5_0");

	EVSMComputeShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "EVSM_COMPUTE", "" } }, "cs_5_0");
	blurEVSMComputeShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "EVSMBLUR_COMPUTE", "" } }, "cs_5_0");
	generateShadowVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SHADOW_COMPUTE", "" } }, "cs_5_0");
	generateMediaVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "MEDIA_COMPUTE", "" } }, "cs_5_0");
	generateScatteringVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SCATTER_COMPUTE", "" } }, "cs_5_0");
	intergrationSliceMarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "MARCH_COMPUTE", "" } }, "cs_5_0");
	applyVolumetricLightingPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "APPLY_PIXEL", "" } }, "ps_5_0");
}

void OrthogonalVolumetricLighting::SetupPostLoadResources()
{
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC EVSMDesc{};
	EVSMDesc.Width = EVSMDesc.Height = EVSM_Size;
	EVSMDesc.MipLevels = 1;
	EVSMDesc.ArraySize = nCascades;
	EVSMDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	EVSMDesc.Usage = D3D11_USAGE_DEFAULT;
	EVSMDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	EVSMDesc.SampleDesc.Count = 1;
	EVSMDesc.SampleDesc.Quality = 0;
	EVSMDesc.CPUAccessFlags = 0;
	EVSMDesc.MiscFlags = 0;

	D3D11_UNORDERED_ACCESS_VIEW_DESC EVSMDescUAV{};
	EVSMDescUAV.Format = EVSMDesc.Format;
	EVSMDescUAV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	EVSMDescUAV.Texture2DArray.MipSlice = 0;
	EVSMDescUAV.Texture2DArray.FirstArraySlice = 0;
	EVSMDescUAV.Texture2DArray.ArraySize = EVSMDesc.ArraySize;

	DX::ThrowIfFailed(device->CreateTexture2D(&EVSMDesc, nullptr, &expVarianceMapTex));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(expVarianceMapTex, &EVSMDescUAV, &expVarianceMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(expVarianceMapTex, nullptr, &expVarianceMapSRV));

	DX::ThrowIfFailed(device->CreateTexture2D(&EVSMDesc, nullptr, &EVSMBlurTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(EVSMBlurTexture, &EVSMDescUAV, &EVSMBlurUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(EVSMBlurTexture, nullptr, &EVSMBlurSRV));
}

void OrthogonalVolumetricLighting::SetupResources()
{
	auto device = globals::d3d::device;

	D3D11_SAMPLER_DESC linearSamplerDesc{};
	linearSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	linearSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	linearSamplerDesc.MinLOD = 0;
	linearSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	D3D11_SAMPLER_DESC pointSamplerDesc{ linearSamplerDesc };
	pointSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	D3D11_SAMPLER_DESC anisoLinearDesc{ linearSamplerDesc };
	anisoLinearDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	anisoLinearDesc.MaxAnisotropy = 4;

	D3D11_SAMPLER_DESC anisoWrapSamplerDesc{ linearSamplerDesc };
	anisoWrapSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	anisoWrapSamplerDesc.MaxAnisotropy = 4;

	DX::ThrowIfFailed(device->CreateSamplerState(&linearSamplerDesc, &linearSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&pointSamplerDesc, &pointSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&anisoLinearDesc, &anisoLinear));
	DX::ThrowIfFailed(device->CreateSamplerState(&anisoWrapSamplerDesc, &anisoWrapLinear));

	D3D11_BLEND_DESC outputBlendDesc = {};
	outputBlendDesc.AlphaToCoverageEnable = FALSE;
	outputBlendDesc.IndependentBlendEnable = FALSE;
	auto& rtDesc = outputBlendDesc.RenderTarget[0];
	rtDesc.BlendEnable = FALSE;
	rtDesc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	rtDesc.SrcBlend = D3D11_BLEND_ONE;
	rtDesc.DestBlend = D3D11_BLEND_SRC_ALPHA;
	rtDesc.BlendOp = D3D11_BLEND_OP_ADD;
	rtDesc.SrcBlendAlpha = D3D11_BLEND_ZERO;
	rtDesc.DestBlendAlpha = D3D11_BLEND_ONE;
	rtDesc.BlendOpAlpha = D3D11_BLEND_OP_ADD;

	DX::ThrowIfFailed(device->CreateBlendState(&outputBlendDesc, &outVolumetricsBlendState));

	shadowDataCB = new ConstantBuffer(ConstantBufferDesc<ShadowDataCB>());
	froxelGridCB = new ConstantBuffer(ConstantBufferDesc<FroxelGridCB>());
	volumeCB = new ConstantBuffer(ConstantBufferDesc<VolumeBuffer>());
	settingsCB = new ConstantBuffer(ConstantBufferDesc<SettingsBuffer>());

	screenSize = (float2)Util::ConvertToDynamic(globals::state->screenSize);

	//// VOLUMES //////////////////////////////////////////
	D3D11_TEXTURE3D_DESC R16VolumeDesc{};
	R16VolumeDesc.Width = (UINT)volumeDimensions.x;
	R16VolumeDesc.Height = (UINT)volumeDimensions.y;
	R16VolumeDesc.Depth = (UINT)volumeDimensions.z;
	R16VolumeDesc.MipLevels = 1;
	R16VolumeDesc.Format = DXGI_FORMAT_R16_FLOAT;
	R16VolumeDesc.Usage = D3D11_USAGE_DEFAULT;
	R16VolumeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	R16VolumeDesc.CPUAccessFlags = 0;
	R16VolumeDesc.MiscFlags = 0;
	D3D11_TEXTURE3D_DESC RGBA16VolumeDesc{ R16VolumeDesc };
	RGBA16VolumeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	D3D11_UNORDERED_ACCESS_VIEW_DESC R16VolumeUAVdesc{};
	R16VolumeUAVdesc.Format = R16VolumeDesc.Format;
	R16VolumeUAVdesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	R16VolumeUAVdesc.Texture3D.MipSlice = 0;
	R16VolumeUAVdesc.Texture3D.FirstWSlice = 0;
	R16VolumeUAVdesc.Texture3D.WSize = R16VolumeDesc.Depth;
	D3D11_UNORDERED_ACCESS_VIEW_DESC RGBA16VolumeUAVDesc{ R16VolumeUAVdesc };
	RGBA16VolumeUAVDesc.Format = RGBA16VolumeDesc.Format;

	D3D11_TEXTURE3D_DESC PerlinDesc{ R16VolumeDesc };
	PerlinDesc.Width = PerlinDesc.Height = PerlinDesc.Depth = 32;
	D3D11_UNORDERED_ACCESS_VIEW_DESC PerlinUAVDesc{ R16VolumeUAVdesc };
	PerlinUAVDesc.Texture3D.WSize = PerlinDesc.Depth;

	//D3D11_TEXTURE3D_DESC ScatterVolumeDesc{ R16VolumeDesc };
	//ScatterVolumeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	//if (settings.useCheckerBoard)
	//	ScatterVolumeDesc.Depth = (UINT)std::ceil(volumeDimensions.z / 2);
	//else
	//ScatterVolumeDesc.Depth = (UINT)volumeDimensions.z;
	//D3D11_UNORDERED_ACCESS_VIEW_DESC ScatterVolumeUAVDesc{ R16VolumeUAVdesc };
	//ScatterVolumeUAVDesc.Format = ScatterVolumeDesc.Format;
	//ScatterVolumeUAVDesc.Texture3D.WSize = ScatterVolumeDesc.Depth;
	/*
	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &FilteringVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &FilteringVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FilteringVolume[0], &RGBA16VolumeUAVDesc, &FilterVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FilteringVolume[1], &RGBA16VolumeUAVDesc, &FilterVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FilteringVolume[0], nullptr, &FilterVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FilteringVolume[1], nullptr, &FilterVolumeSRV[1]));
	*/

	DX::ThrowIfFailed(device->CreateTexture3D(&PerlinDesc, nullptr, &perlinTex));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(perlinTex, &PerlinUAVDesc, &perlinUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(perlinTex, nullptr, &perlinSRV));

	DX::ThrowIfFailed(device->CreateTexture3D(&R16VolumeDesc, nullptr, &shadowVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&R16VolumeDesc, nullptr, &shadowVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(shadowVolume[0], &R16VolumeUAVdesc, &shadowVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(shadowVolume[1], &R16VolumeUAVdesc, &shadowVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(shadowVolume[0], nullptr, &shadowVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(shadowVolume[1], nullptr, &shadowVolumeSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &mediaVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &mediaVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(mediaVolume[0], &RGBA16VolumeUAVDesc, &mediaVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(mediaVolume[1], &RGBA16VolumeUAVDesc, &mediaVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(mediaVolume[0], nullptr, &mediaVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(mediaVolume[1], nullptr, &mediaVolumeSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &scatteringVolume));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(scatteringVolume, &RGBA16VolumeUAVDesc, &scatteringVolumeUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(scatteringVolume, nullptr, &scatteringVolumeSRV));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &intergrationVolume));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(intergrationVolume, &RGBA16VolumeUAVDesc, &intergrationVolumeUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(intergrationVolume, nullptr, &intergrationVolumeSRV));

	//// MISC ///////////////////////////////////////////////////////////////////////////
	D3D11_TEXTURE2D_DESC outputDesc{};
	outputDesc.Width = (UINT)screenSize.x;
	outputDesc.Height = (UINT)screenSize.y;
	outputDesc.MipLevels = 1;
	outputDesc.ArraySize = 1;
	outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	outputDesc.Usage = D3D11_USAGE_DEFAULT;
	outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	outputDesc.SampleDesc.Count = 1;
	outputDesc.SampleDesc.Quality = 0;
	outputDesc.CPUAccessFlags = 0;
	outputDesc.MiscFlags = 0;

	DX::ThrowIfFailed(device->CreateTexture2D(&outputDesc, nullptr, &outputTexture));
	DX::ThrowIfFailed(device->CreateRenderTargetView(outputTexture, nullptr, &outputRTV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(outputTexture, nullptr, &outputSRV));

	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\STBN.dds", nullptr, &blueNoiseSRV));
	ID3D11Resource* Resource;
	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\WorldMap.dds", &Resource, &staticWorldMapSRV));
	DX::ThrowIfFailed(Resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&worldMapTexture)));

	D3D11_TEXTURE2D_DESC FogMapDesc{};
	worldMapTexture->GetDesc(&FogMapDesc);
	outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	FogMapDesc.Usage = D3D11_USAGE_DEFAULT;
	FogMapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_UNORDERED_ACCESS_VIEW_DESC FogMapUAVDesc{};
	FogMapUAVDesc.Format = FogMapDesc.Format;
	FogMapUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	FogMapUAVDesc.Texture2D.MipSlice = 0;

	DX::ThrowIfFailed(device->CreateTexture2D(&FogMapDesc, nullptr, &UIFogMapTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(UIFogMapTexture, &FogMapUAVDesc, &UIFogMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(UIFogMapTexture, nullptr, &UIFogMapSRV));

	DX::ThrowIfFailed(device->CreateTexture2D(&FogMapDesc, nullptr, &fogMapTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(fogMapTexture, &FogMapUAVDesc, &fogMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(fogMapTexture, nullptr, &fogMapSRV));

	fogMapSize = float2((float)FogMapDesc.Width, (float)FogMapDesc.Height);

	//// GENERAL /////////////////////////////////////////////////////////////
	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());

	*reinterpret_cast<uint32_t*>(REL::RelocationID(391108, 391108).address()) = 0;  //Disable VL maps

	skyrim_SunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	renderdata = new Setup::LF_RenderData;
	renderdata->SetupPass(Shaders::RenderVL, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::Apply, true, 1, { .uncond_pass = true });
	renderdata->SetupRenderData();

	CompileShaders();
}

void OrthogonalVolumetricLighting::LookupShader(int desc)
{
	static const std::unordered_map<int, void (OrthogonalVolumetricLighting::*)()> effects{
		{ Shaders::Bypass, &OrthogonalVolumetricLighting::SetupBypass },
		{ Shaders::RenderVL, &OrthogonalVolumetricLighting::VLightingRenderChain },
		{ Shaders::Apply, &OrthogonalVolumetricLighting::RenderToScreen },
	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

void OrthogonalVolumetricLighting::CheckOverride()
{
	if (overrideShader) {
		LookupShader(shaderdesc);
	}
}

void OrthogonalVolumetricLighting::VLightingRenderChain()
{
	UpdateAndSetupResources();
	std::swap(currentVolumeIdx, historyVolumeIdx);
	frameCounter++;

	static bool renderPerlin = true;
	if (std::exchange(renderPerlin, false)) {
		SetupPerlinNoise();
	}

	RenderEVSM();
	RenderEVSMBlur();
	GenerateShadowVolume();
	GenerateMediaVolume();
	GenerateScatteringVolume();
	RunIntergrationPass();

	SetupApplyPassResources();
	SetupApplyPass();

	overrideShader = false;
}

//// Per Frame ////////////////////////////////////////////
void OrthogonalVolumetricLighting::UpdateAndSetupResources()
{
	UpdateShadowBuffer();
	UpdateFroxelBuffer();
	UpdateGeneralBuffers();

	auto context = globals::d3d::context;
	ID3D11Buffer* buffers[4] = { shadowDataCB->CB(), froxelGridCB->CB(), volumeCB->CB(), settingsCB->CB() };
	context->CSSetConstantBuffers(0, 4, buffers);

	//std::array<ID3D11SamplerState*, 2> samplers = { Deferred::GetSingleton()->linearSampler, Deferred::GetSingleton()->pointSampler };
	//context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetSamplers(10, 1, &linearSampler);
	context->CSSetSamplers(11, 1, &pointSampler);
	context->CSSetSamplers(13, 1, &anisoLinear);
	context->CSSetSamplers(14, 1, &anisoWrapLinear);
}

void OrthogonalVolumetricLighting::UpdateFroxelBuffer()
{
	//float nearPlane = Util::GetCameraData().y;
	//float farPlane = 10000.0f;
	//frustumNearFar = float4(nearPlane, farPlane, 1.0f / nearPlane, volumeDimensions.z / std::log2(farPlane / nearPlane));

	frustumNearFar = float4(nearPlane, farPlane, 1.0f / nearPlane, distributionLambda);  //float exponent = 1.5f;  // Lower = more linear (1.0 = fully linear, 2.0 = quadratic)

	auto eyePos = Util::GetEyePosition(0);
	if (eyePos.x > 1.0 || eyePos.x < -1.0)
		eyePositionWS = float3(eyePos.x, eyePos.y, eyePos.z);

	FroxelGridCB froxelData{};
	froxelData.cameraView = globals::game::frameBufferCached.GetCameraView(0);
	froxelData.cameraProj = globals::game::frameBufferCached.GetCameraProj(0);
	froxelData.cameraViewInverse = globals::game::frameBufferCached.GetCameraViewInverse(0);
	froxelData.cameraProjInverse = globals::game::frameBufferCached.GetCameraProjInverse(0);
	froxelData.prevCameraViewProj = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered(0);
	froxelData.cameraViewProjInverse = globals::game::frameBufferCached.GetCameraViewProjInverse(0);
	froxelData.cameraPosition = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0f);
	froxelData.cameraData = Util::GetCameraData();
	froxelData.volumeSize = volumeDimensions;
	froxelData.frustumNearFar = frustumNearFar;
	froxelData.lightDirection = lightDir;
	froxelData.frameparams = globals::game::frameBufferCached.GetFrameParams();
	std::copy(globals::features::lightLimitFix.clusterSize, globals::features::lightLimitFix.clusterSize + 3, froxelData.lightClusterGridSize);
	froxelGridCB->Update(froxelData);
}

void OrthogonalVolumetricLighting::UpdateShadowBuffer()
{
	auto smState = globals::game::smState;

	if (auto shadowSceneNode = smState->shadowSceneNode[0]) {
		auto& shadowNodeRuntime = shadowSceneNode->GetRuntimeData();

		if (shadowNodeRuntime.shadowDirLight) {
			auto& dirLight = shadowNodeRuntime.shadowDirLight;
			auto& dirCascadeList = dirLight->GetRuntimeData().shadowmapDescriptors;
			for (uint it = 0; it < dirCascadeList.size() && it < 4; it++) {
				directionalShadowCascadeMatrices[it] = GetCascadeMatrix(dirCascadeList[it].lightTransform);
			}
			auto& dirLightData2 = dirLight->GetShadowDirectionalLightRuntimeData();
			shadowCascadeEndSplit = float4(dirLightData2.endSplitDistances[0], dirLightData2.endSplitDistances[1], dirLightData2.endSplitDistances[2], 0.0);

			if (updateLightDir)
				lightDir = { dirLightData2.lightDirection.x, dirLightData2.lightDirection.y, dirLightData2.lightDirection.z, 0.0f };
		}

		uint matrixArraySize = 4;
		auto& shadowLightList = shadowNodeRuntime.activeShadowLights;
		for (uint i = 0; i < shadowLightList.size() && i < matrixArraySize; i++) {
			auto& SLRuntime = shadowLightList[i]->GetRuntimeData();
			auto& SLDesc = SLRuntime.shadowmapDescriptors;
			auto SLIndex = SLRuntime.shadowLightIndex;

			if (SLIndex < matrixArraySize && SLIndex != 255) {
				localShadowLightMatrices[SLIndex].matrix = GetCascadeMatrix(SLDesc[0].lightTransform);
				localShadowLightMatrices[SLIndex].shadowmapIndex = SLDesc[0].shadowmapIndex;
			}
		}
	}

	ShadowDataCB shadowData{};
	std::memcpy(shadowData.directionalShadowCascadeMatrices, directionalShadowCascadeMatrices, sizeof(shadowData.directionalShadowCascadeMatrices));
	std::memcpy(shadowData.localShadowLightMatrices, localShadowLightMatrices, sizeof(shadowData.localShadowLightMatrices));
	shadowData.shadowCascadeEndSplit = shadowCascadeEndSplit;
	shadowData.EVSMData = float4((float)EVSM_Size, (float)EVSM_Size, (float)std::exp(settings.esmExponent), (float)std::exp(settings.esmExponent * 2.0f));
	shadowDataCB->Update(shadowData);
}

void OrthogonalVolumetricLighting::UpdateGeneralBuffers()
{
	float3 mapScale = float3(0, 0, 0);
	float2 mapOffset = float2(0, 0);
	float2 mapRange = float2(0, 0);
	if (globals::features::terrainShadows.IsHeightMapReady()) {
		auto& heightMap = globals::features::terrainShadows.cachedHeightmap;
		mapScale = float3(1.0f, 1.0f, 1.0f) / float3(heightMap->pos1 - heightMap->pos0);
		mapOffset = -heightMap->pos0 * float2{ mapScale.x, mapScale.y };
		mapRange = float2(heightMap->pos0.z, heightMap->pos1.z);
	}

	VolumeBuffer generalData{};
	generalData.fogMapMatrix = fogMapViewProj;
	generalData.heightMapParams = float4(mapScale.x, mapScale.y, mapOffset.x, mapOffset.y);
	generalData.heightMapZRange = float4(mapRange.x, mapRange.y, 0.0, 0.0);
	generalData.NoiseSize = noiseDimensions;
	//generalData.frameCounter = frameCounter;
	//generalData.boardCondition = frameCounter & 1;
	volumeCB->Update(generalData);

	SettingsBuffer settingsData{};
	settingsData.cbsettings = settings;
	settingsData.cbsettings.extinction *= Util::Units::GAME_UNIT_TO_M;  // Multiply since extinction is a rate per unit distance
	settingsCB->Update(settingsData);
}
///////////////////////////////////////////////////////////

//// Shadows //////////////////////////////////////////////
void OrthogonalVolumetricLighting::RenderEVSM()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Generate EVSM");

	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Generate EVSM");

	context->CSSetShader(EVSMComputeShader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &expVarianceMapUAV, nullptr);

	auto shadowCascade = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(0, 1, &shadowCascade);

	auto waveGroups = EVSM_Size / 16;
	context->Dispatch(waveGroups, waveGroups, nCascades);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::RenderEVSMBlur()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Blur EVSM");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Blur EVSM");

	context->CSSetShader(blurEVSMComputeShader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &EVSMBlurUAV, nullptr);

	context->CSSetShaderResources(0, 1, &expVarianceMapSRV);

	auto waveGroups = EVSM_Size / 16;
	context->Dispatch(waveGroups, waveGroups, nCascades);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::GenerateShadowVolume()
{
	ZoneScoped;
	auto context = globals::d3d::context;
	auto& LLF = globals::features::lightLimitFix;

	TracyD3D11Zone(state->tracyCtx, "VL - Generate Shadow Volume");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Generate Shadow Volume");

	context->CSSetShader(generateShadowVolumeCS, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, 1, &shadowVolumeUAV[currentVolumeIdx], nullptr);

	context->CSSetShaderResources(0, 1, &shadowVolumeSRV[historyVolumeIdx]);
	context->CSSetShaderResources(1, 1, &blueNoiseSRV);
	context->CSSetShaderResources(2, 1, &EVSMBlurSRV);

	auto localShadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS].depthSRV;
	auto lightsSB = LLF.lights->srv.get();
	auto lightListSB = LLF.lightIndexList->srv.get();
	auto lightGridSB = LLF.lightGrid->srv.get();
	context->CSSetShaderResources(3, 1, &localShadowMap);
	context->CSSetShaderResources(4, 1, &lightsSB);
	context->CSSetShaderResources(5, 1, &lightListSB);
	context->CSSetShaderResources(6, 1, &lightGridSB);

	context->Dispatch(60, 34, 16);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}
///////////////////////////////////////////////////////////

//// Render Passes ////////////////////////////////////////
void OrthogonalVolumetricLighting::GenerateMediaVolume()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Generate Media Volume");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Generate Media Volume");

	//ID3D11UnorderedAccessView* UAVs[2] = { mediaUAV, FogMapUAV };
	context->CSSetUnorderedAccessViews(0, 1, &mediaVolumeUAV[currentVolumeIdx], nullptr);
	context->CSSetShader(generateMediaVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &mediaVolumeSRV[historyVolumeIdx]);
	context->CSSetShaderResources(1, 1, &perlinSRV);
	context->CSSetShaderResources(2, 1, &blueNoiseSRV);
	context->CSSetShaderResources(3, 1, &fogMapSRV);

	context->Dispatch(60, 34, 17);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::GenerateScatteringVolume()
{
	ZoneScoped;
	auto context = globals::d3d::context;
	auto& LLF = globals::features::lightLimitFix;
	auto& skyLighting = globals::features::skylighting;
	auto& imageBasedLighting = globals::features::ibl;

	TracyD3D11Zone(state->tracyCtx, "VL - Generate Scattering Volume");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Generate Scattering Volume");

	context->CSSetUnorderedAccessViews(0, 1, &scatteringVolumeUAV, nullptr);
	context->CSSetShader(generateScatteringVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &shadowVolumeSRV[currentVolumeIdx]);
	context->CSSetShaderResources(1, 1, &mediaVolumeSRV[currentVolumeIdx]);
	context->CSSetShaderResources(2, 1, &blueNoiseSRV);

	auto skylightProbeGrid = skyLighting.texProbeArray->srv.get();
	context->CSSetShaderResources(3, 1, &skylightProbeGrid);

	auto diffuseIBL = imageBasedLighting.diffuseIBLTexture->srv.get();
	auto diffuseSkyIBL = imageBasedLighting.diffuseSkyIBLTexture->srv.get();
	context->CSSetShaderResources(76, 1, &diffuseIBL);
	context->CSSetShaderResources(77, 1, &diffuseSkyIBL);

	auto lightsSB = LLF.lights->srv.get();
	auto lightListSB = LLF.lightIndexList->srv.get();
	auto lightGridSB = LLF.lightGrid->srv.get();
	context->CSSetShaderResources(4, 1, &lightsSB);
	context->CSSetShaderResources(5, 1, &lightListSB);
	context->CSSetShaderResources(6, 1, &lightGridSB);

	auto strictLightDataCB = LLF.strictLightDataCB->CB();
	context->CSSetConstantBuffers(9, 1, &strictLightDataCB);  //Do i need this for anything?

	context->Dispatch(60, 34, 17);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::RunIntergrationPass()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Volumetric Intergration Pass");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Volumetric Intergration Pass");

	context->CSSetUnorderedAccessViews(0, 1, &intergrationVolumeUAV, nullptr);
	context->CSSetShader(intergrationSliceMarchCS, nullptr, 0);

	//context->CSSetShaderResources(0, 1, &FilterVolumeSRV[!FilterVolParity]);
	context->CSSetShaderResources(0, 1, &scatteringVolumeSRV);

	context->Dispatch(30, 17, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::SetupApplyPassResources()
{
	auto context = globals::d3d::context;
	ID3D11Buffer* buffers[4] = { shadowDataCB->CB(), froxelGridCB->CB(), volumeCB->CB(), settingsCB->CB() };
	context->PSSetConstantBuffers(0, 4, buffers);

	context->PSSetSamplers(10, 1, &linearSampler);
	context->PSSetSamplers(11, 1, &pointSampler);
	context->PSSetSamplers(12, 1, &anisoLinear);
	context->PSSetSamplers(13, 1, &anisoWrapLinear);
}

void OrthogonalVolumetricLighting::SetupApplyPass()
{
	ZoneScoped;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	TracyD3D11Zone(state->tracyCtx, "VL - Apply Volumetric Lighting");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Apply Volumetric Lighting");

	//auto& mainRTV = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV;
	//if (!swapOutputRT)
	//	context->OMSetRenderTargets(1, &mainRTV, nullptr);
	//else
	context->OMSetRenderTargets(1, &outputRTV, nullptr);

	context->OMSetBlendState(outVolumetricsBlendState, nullptr, 0xffffffff);  // BLEND DISABLED AT CREATION
	auto& mainSRV = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV;
	context->PSSetShaderResources(4, 1, &mainSRV);

	context->VSSetShader(bypassVS, NULL, NULL);
	context->PSSetShader(applyVolumetricLightingPS, NULL, NULL);

	context->PSSetShaderResources(1, 1, &intergrationVolumeSRV);
	context->PSSetShaderResources(2, 1, &blueNoiseSRV);
	context->PSSetShaderResources(3, 1, &shadowVolumeSRV[historyVolumeIdx]);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::RenderToScreen()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& mainTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;

	context->CopyResource(mainTexture, outputTexture);

	overrideShader = false;
}
/*
void OrthogonalVolumetricLighting::SetupFilterPass()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "Volumetric Lighting");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Volumetric Lighting");

	static int passCount = 1;

	FilterVolParity = passCount & 1;
	auto volumeUAV = FilterVolumeUAV[!FilterVolParity];
	auto prevVolumeSRV = FilterVolumeSRV[FilterVolParity];

	context->CSSetUnorderedAccessViews(0, 1, &volumeUAV, nullptr);
	context->CSSetShader(FilterVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &prevVolumeSRV);
	context->CSSetShaderResources(1, 1, &ScatteringVolumeSRV);

	context->Dispatch(40, 23, 22);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();

	overrideShader = false;
}
*/
///////////////////////////////////////////////////////////

//// Utility //////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawFogMap()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Draw Fog Map");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Draw Fog Map");

	context->CSSetUnorderedAccessViews(0, 1, &UIFogMapUAV, nullptr);
	context->CSSetUnorderedAccessViews(1, 1, &fogMapUAV, nullptr);

	context->CSSetShader(drawFogMapCS, nullptr, 0);

	auto volumeBuff = volumeCB->CB();
	context->CSSetConstantBuffers(0, 1, &volumeBuff);

	context->CSSetShaderResources(0, 1, &staticWorldMapSRV);

	auto heightMapSRV = globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(1, 1, &heightMapSRV);

	context->Dispatch((UINT)fogMapSize.x, (UINT)fogMapSize.y, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupPerlinNoise()
{
	ZoneScoped;
	auto context = globals::d3d::context;

	TracyD3D11Zone(state->tracyCtx, "VL - Render Perlin Noise");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("VL - Render Perlin Noise");

	context->CSSetUnorderedAccessViews(0, 1, &perlinUAV, nullptr);
	context->CSSetShader(generatePerlinCS, nullptr, 0);

	context->Dispatch(4, 4, 4);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void OrthogonalVolumetricLighting::SetupBypass()
{
	auto context = globals::d3d::context;
	context->PSSetShader(nullptr, nullptr, 0);
	overrideShader = false;
}
///////////////////////////////////////////////////////////

///// Settings ////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawSettings()
{
	ImGui::SeparatorText("Reload");
	ImGui::Button("Reload Flare");
	if (ImGui::IsItemClicked()) {
		generateShadowVolumeCS = nullptr;
		generateScatteringVolumeCS = nullptr;
		intergrationSliceMarchCS = nullptr;
		applyVolumetricLightingPS = nullptr;
		outputPS = nullptr;
		drawFogMapCS = nullptr;
		blurEVSMComputeShader = nullptr;

		CompileShaders();
	}

	ImGui::Spacing();

	ImGui::Checkbox("Swap Output RT", (bool*)&swapOutputRT);
	ImGui::Checkbox("Update Light Dir", &updateLightDir);

	ImGui::Checkbox("Use History", (bool*)&settings.useHistory);
	ImGui::SliderFloat("Disocclution Threshold", &settings.disocclutionThreshold, 0.0001, 0.2);
	ImGui::SliderFloat("Distance Fade In", &settings.distanceFadeIn, 0.0, 250.0);

	//// Frustum Grid ////
	ImGui::SliderFloat("Near Plane", &nearPlane, 1, 250);
	ImGui::SliderFloat("Far Plane", &farPlane, 2000, 353840);
	ImGui::SliderFloat("Distribution Lambda", &distributionLambda, 0.5, 3.0);

	//// Color Params ////
	ImGui::SliderFloat("Color Saturation", &settings.color_saturation, 0.0, 1.0);
	ImGui::SliderFloat("Exposure", &settings.preExposure, 0.1, 3.0);
	ImGui::SliderFloat("Ambient Light Multiplier", &settings.amibentLightingMultiplier, 0.1, 2.0);
	ImGui::SliderFloat("Sky Ambient Contribution", &settings.skyAmbientContribution, 0.1, 2.0);
	ImGui::SliderFloat("Scene Ambient Contribution", &settings.sceneAmbientContribution, 0.1, 2.0);
	ImGui::SliderFloat("Dir Light Radiance Multiplier", &settings.dirLightRadianceMultiplier, 1.0, 250.0);

	//// Scattering Params ////
	ImGui::SeparatorText("Scattering Properties");

	ImGui::SliderFloat("Anisotropy", &settings.anisotropy, -0.2, 1.0);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much the amount of light scattering towards the viewer depends on direction");

	ImGui::SliderFloat("Local Light Anisotropy", &settings.localLightsAnisotropy, -0.2, 1.0);
	ImGui::SliderFloat("Local Light Multiplier", &settings.localLightsMultiplier, 1.0, 5.0);

	ImGui::SliderFloat("Extinction Per Meter", &settings.extinction, 0.001, 0.6, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The rate of light loss per unit distance (light absorpted + light scattered)");

	ImGui::SliderFloat("Red Scatter to Absorption Ratio", &settings.scatteringRatio.x, 0.0, 1.0);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The ratio of light that is scattered compared to absorped");
	ImGui::SliderFloat("Green Scatter to Absorption Ratio", &settings.scatteringRatio.y, 0.0, 1.0);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The ratio of light that is scattered compared to absorped");
	ImGui::SliderFloat("Blue Scatter to Absorption Ratio", &settings.scatteringRatio.z, 0.0, 1.0);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The ratio of light that is scattered compared to absorped");

	//// Fog Params ////
	ImGui::SeparatorText("Fog Properties");

	ImGui::SliderFloat("Fog Falloff Height", &settings.globalFogFalloffHeight, 250.0f, 10000.0f);  // Lower min values cause aliasing
	ImGui::SliderFloat("Fog Base Height", &settings.globalFogStartHeight, -10000.0f, 10000.0f);

	ImGui::Spacing();
	ImGui::SeparatorText("Shadow properties");
	ImGui::SliderInt("VSM Exponent: ", (int*)&settings.esmExponent, 1, 100);
	ImGui::Spacing();

	//ImGui::SeparatorText("Global height fog");
	//ImGui::SliderFloat("Fog Density", &settings.globalFogDensity, 0.0f, 1.0f);
	//ImGui::SliderFloat("Ground Level Bias", &settings.globalFogStartHeight, -2000.0f, 2000.0f);
	//ImGui::SliderFloat("End Height", &settings.globalFogStartHeight, 0.0f, 50000.0f);
	//ImGui::SliderFloat("Falloff Distance", &settings.globalFogFalloffHeight, 0.0f, 10000.0f);

	//ImGui::SliderFloat("Start Height", &settings.globalFogStartHeight, -20000.0f, 50000.0f);

	//ImGui::Checkbox("Use History", (bool*)&settings.useHistory);
	//ImGui::SliderFloat("History Bias", &settings.historyAlpha, 0.0, 0.5);
	//ImGui::SliderFloat("Weight 1", &settings.weight1, 0.0, 1.0);
	//ImGui::SliderFloat("Weight 2", &settings.weight2, 0.0, 1.0);
	//ImGui::SliderFloat("Media Albedo", &settings.albedo, 0.0, 1.0);

	//ImGui::SliderFloat("Shadow Threshold", &settings.shadow_threshold, 0.0, 1.0);

	ImGui::SeparatorText("Fog Maps");
	ImGui::SliderFloat("Radius", &brushRadius, 1.0f, 200.0f, "%.0f");
	ImGui::SliderFloat("Feather", &brushFeather, 0.0f, 1.0f);
	ImGui::SliderFloat("Erase", &settings.blendOpp, -1.0f, 0.0f, "%.0f");

	static float localFogDensity = 0.0;
	static float localFogGroundLevelBias = 0.0;
	static float localFogMaxHeight = 0.0;
	static float localFogFalloffDistance = 0.0;
	ImGui::SliderFloat("Fog Density", &localFogDensity, 0.0f, 1.0f);
	ImGui::SliderFloat("DISABLED Ground Level Bias", &localFogGroundLevelBias, -2000.0f, 2000.0f);
	ImGui::SliderFloat("Fog Height", &localFogMaxHeight, 0.0f, 50000.0f);              //(GroundLevel + Bias) + This   = FogTop
	ImGui::SliderFloat("Falloff Distance", &localFogFalloffDistance, 0.0f, 10000.0f);  //EndHeight - This  = FalloffStart

	ImVec2 displaySize = ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f);

	if (ImGui::BeginChild("PaintCanvas", displaySize, true, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
		ImVec2 mouse = ImGui::GetIO().MousePos;

		ImVec2 PosTL = ImGui::GetCursorScreenPos();
		ImVec2 PosBR = ImVec2(PosTL.x + displaySize.x, PosTL.y + displaySize.y);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddImage((ImTextureID)staticWorldMapSRV, PosTL, PosBR);
		drawList->AddImage((ImTextureID)UIFogMapSRV, PosTL, PosBR);

		ImGui::SetCursorScreenPos(PosTL);
		ImGui::InvisibleButton("PaintHit", displaySize, ImGuiButtonFlags_MouseButtonLeft);

		if (ImGui::IsItemHovered()) {
			float ScaleX = displaySize.x / fogMapSize.x, ScaleY = displaySize.y / fogMapSize.y;
			float RadiusX = brushRadius * ScaleX, RadiusY = brushRadius * ScaleY;
			const int Seg = 64;

			ImGui::GetIO().MouseDrawCursor = false;

			drawList->PushClipRect(PosTL, PosBR, true);
			drawList->PathClear();
			for (int i = 0; i < Seg; ++i) {
				float a = i * (2.0f * (float)std::_Pi_val / Seg);
				drawList->PathLineTo(ImVec2(mouse.x + cosf(a) * RadiusX, mouse.y + sinf(a) * RadiusY));
			}
			drawList->PathStroke(IM_COL32(0, 0, 0, 255), true, 2.0f);
			drawList->PopClipRect();

			if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				float2 Coords = float2(mouse.x - PosTL.x, mouse.y - PosTL.y) / float2(displaySize.x, displaySize.y) * fogMapSize;
				settings.fogMapData = float4(Coords.x, Coords.y, brushRadius, brushFeather);
				settings.UIfogMapParams = float4(localFogGroundLevelBias, localFogMaxHeight, localFogFalloffDistance, localFogDensity);

				DrawFogMap();
			}
		}
	}
	ImGui::EndChild();
}

//// General Hooks ////////////////////////////////////////
void OrthogonalVolumetricLighting::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	auto skyProperty = reinterpret_cast<RE::BSSkyShaderProperty*>(Pass->shaderProperty);

	if (skyProperty) {
		if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE) {
			OVL.overrideShader = true;
			OVL.shaderdesc = Shaders::Bypass;
		}
	}

	func(This, Pass, RenderFlags);
}

void OrthogonalVolumetricLighting::Hooks::LensFlare_CheckResources::thunk()
{
	auto& lens = globals::features::orthogonalVolumetricLighting;

	lens.renderdata->CheckRefData();

	if (!*lens.skyrim_FlareData)
		*lens.skyrim_FlareData = reinterpret_cast<uintptr_t>(lens.renderdata);
}

void OrthogonalVolumetricLighting::Hooks::LensFlareVisibility_CheckRenderCondition::thunk(RE::NiCamera* camera, void* shader)
{
	auto& lens = globals::features::orthogonalVolumetricLighting;

	func(camera, shader);

	bool sunVisble = static_cast<bool>(*lens.skyrim_RunFlarePtr);

	if (*lens.skyrim_FlareData) {
		lens.renderdata->ResetEffects(sunVisble);
		*lens.skyrim_RunFlarePtr = 1;
	} else
		*lens.skyrim_RunFlarePtr = 0;
}

void OrthogonalVolumetricLighting::Hooks::BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>::thunk(void* shader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& lens = globals::features::orthogonalVolumetricLighting;

	lens.overrideShader = true;
	lens.shaderdesc = lens.renderdata->UpdateCurrentEffect();

	func(shader, shape, param);
}
///////////////////////////////////////////////////////////

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
