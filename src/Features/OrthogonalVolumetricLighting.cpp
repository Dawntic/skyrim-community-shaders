#include "OrthogonalVolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OrthogonalVolumetricLighting::Settings,
	anisotropy, extinction, albedo, esmExponent, color_saturation,
	globalFogDensity, globalFogStartHeight, globalFogFalloffHeight)

void OrthogonalVolumetricLighting::CompileShaders()
{
	BypassVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "BYPASS_VSSHADER", "" } }, "vs_5_0");

	GeneratePerlinCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "PERLIN_COMPUTE", "" } }, "cs_5_0");
	DrawFogMapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "DRAW_FOGMAP", "" } }, "cs_5_0");
	GenerateEVSMCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "EVSM_COMPUTE", "" } }, "cs_5_0");
	BlurEVSMCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "EVSMBLUR_COMPUTE", "" } }, "cs_5_0");
	GenerateShadowVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SHADOW_COMPUTE", "" } }, "cs_5_0");
	GenerateMediaVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "MEDIA_COMPUTE", "" } }, "cs_5_0");
	GenerateScatteringVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SCATTER_COMPUTE", "" } }, "cs_5_0");
	SliceMarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "MARCH_COMPUTE", "" } }, "cs_5_0");

	ApplyVolumePS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "APPLY_PIXEL", "" } }, "ps_5_0");

	ShadowMapVS = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SHADOWMAPVS", "" } }, "vs_5_0");
	ShadowMapPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SHADOWMAPPS", "" } }, "ps_5_0");
	ShadowMapDebugCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "SHADOWMAPDEBUG", "" } }, "cs_5_0");

	//CloudShadowVS = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_VETEX", "" } }, "vs_5_0");
	//CloudShadowPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_PIXEL", "" } }, "ps_5_0");
	//CloudShadowCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_COMPUTE", "" } }, "cs_5_0");
}

void OrthogonalVolumetricLighting::SetupCascadeTextures()
{
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC CascadeDesc{};
	CascadeDesc.Width = CSM_Size;
	CascadeDesc.Height = CSM_Size;
	CascadeDesc.MipLevels = 1;
	CascadeDesc.ArraySize = lightCascades;
	CascadeDesc.Format = DXGI_FORMAT_R16_TYPELESS;
	CascadeDesc.Usage = D3D11_USAGE_DEFAULT;
	CascadeDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	CascadeDesc.SampleDesc.Count = 1;
	CascadeDesc.SampleDesc.Quality = 0;
	CascadeDesc.CPUAccessFlags = 0;
	CascadeDesc.MiscFlags = 0;
	D3D11_DEPTH_STENCIL_VIEW_DESC CascadeDSVDesc{};
	CascadeDSVDesc.Format = DXGI_FORMAT_D16_UNORM;
	CascadeDSVDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
	CascadeDSVDesc.Texture2DArray.MipSlice = 0;
	CascadeDSVDesc.Texture2DArray.FirstArraySlice = 0;
	CascadeDSVDesc.Texture2DArray.ArraySize = lightCascades;
	D3D11_SHADER_RESOURCE_VIEW_DESC CascadeSRVDesc{};
	CascadeSRVDesc.Format = DXGI_FORMAT_R16_UNORM;
	CascadeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	CascadeSRVDesc.Texture2DArray.MostDetailedMip = 0;
	CascadeSRVDesc.Texture2DArray.MipLevels = 1;
	CascadeSRVDesc.Texture2DArray.FirstArraySlice = 0;
	CascadeSRVDesc.Texture2DArray.ArraySize = lightCascades;

	DX::ThrowIfFailed(device->CreateTexture2D(&CascadeDesc, nullptr, &CascadeTex));
	DX::ThrowIfFailed(device->CreateShaderResourceView(CascadeTex, &CascadeSRVDesc, &CascadeSRV));

	for (UINT i = 0; i < 4; ++i) {
		CascadeDSVDesc.Texture2DArray.FirstArraySlice = i;
		CascadeDSVDesc.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView(CascadeTex, &CascadeDSVDesc, &CascadeDSV[i]);
	}

	//// EVSM //////////////////////////////
	D3D11_TEXTURE2D_DESC ExpoDesc{};
	ExpoDesc.Width = EVSM_Size;
	ExpoDesc.Height = EVSM_Size;
	ExpoDesc.MipLevels = 1;
	ExpoDesc.ArraySize = lightCascades;
	ExpoDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	ExpoDesc.Usage = D3D11_USAGE_DEFAULT;
	ExpoDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	ExpoDesc.SampleDesc.Count = 1;
	ExpoDesc.SampleDesc.Quality = 0;
	ExpoDesc.CPUAccessFlags = 0;
	ExpoDesc.MiscFlags = 0;
	D3D11_UNORDERED_ACCESS_VIEW_DESC ExpoUAVDesc{};
	ExpoUAVDesc.Format = ExpoDesc.Format;
	ExpoUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	ExpoUAVDesc.Texture2DArray.MipSlice = 0;
	ExpoUAVDesc.Texture2DArray.FirstArraySlice = 0;
	ExpoUAVDesc.Texture2DArray.ArraySize = lightCascades;

	DX::ThrowIfFailed(device->CreateTexture2D(&ExpoDesc, nullptr, &EVSMTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(EVSMTexture, &ExpoUAVDesc, &EVSMUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(EVSMTexture, nullptr, &EVSMSRV));

	D3D11_TEXTURE2D_DESC ExpoBlurDesc{ ExpoDesc };
	D3D11_UNORDERED_ACCESS_VIEW_DESC ExpoBlurUAVDesc{ ExpoUAVDesc };

	DX::ThrowIfFailed(device->CreateTexture2D(&ExpoBlurDesc, nullptr, &EVSMBlurTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(EVSMBlurTexture, &ExpoBlurUAVDesc, &EVSMBlurUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(EVSMBlurTexture, nullptr, &EVSMBlurSRV));

	//// DEBUG ///////////////////////////////
	D3D11_TEXTURE2D_DESC VarianceDesc{ CascadeDesc };
	VarianceDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	VarianceDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

	D3D11_UNORDERED_ACCESS_VIEW_DESC VarianceUAVDesc{ ExpoUAVDesc };
	VarianceUAVDesc.Format = VarianceDesc.Format;

	DX::ThrowIfFailed(device->CreateTexture2D(&VarianceDesc, nullptr, &ShadowVarianceDebugTex));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVarianceDebugTex, &VarianceUAVDesc, &ShadowVarianceDebugUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVarianceDebugTex, nullptr, &ShadowVarianceDebugSRV));
}

void OrthogonalVolumetricLighting::SetupResources()
{
	auto device = globals::d3d::device;

	D3D11_SAMPLER_DESC linearSamplerDesc{};
	linearSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
	linearSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearSamplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	linearSamplerDesc.MinLOD = 0;
	linearSamplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

	D3D11_SAMPLER_DESC linearMinDesc{ linearSamplerDesc };
	linearMinDesc.Filter = D3D11_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT;

	D3D11_SAMPLER_DESC anisoLinearDesc{ linearSamplerDesc };
	anisoLinearDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	anisoLinearDesc.MaxAnisotropy = 4;

	D3D11_SAMPLER_DESC pointSamplerDesc{ linearSamplerDesc };
	pointSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	D3D11_SAMPLER_DESC anisoWrapSamplerDesc{ linearSamplerDesc };
	anisoWrapSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	anisoWrapSamplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	anisoWrapSamplerDesc.MaxAnisotropy = 4;

	DX::ThrowIfFailed(device->CreateSamplerState(&linearSamplerDesc, &LinearSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&linearMinDesc, &LinearMinSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&pointSamplerDesc, &PointSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&anisoLinearDesc, &AnisoLinear));
	DX::ThrowIfFailed(device->CreateSamplerState(&anisoWrapSamplerDesc, &AnisoWrapLinear));

	D3D11_BLEND_DESC addBlendDesc = {};
	addBlendDesc.AlphaToCoverageEnable = FALSE;
	addBlendDesc.IndependentBlendEnable = FALSE;
	auto& rt = addBlendDesc.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	rt.SrcBlend = D3D11_BLEND_ONE;
	rt.DestBlend = D3D11_BLEND_ONE;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ONE;
	rt.DestBlendAlpha = D3D11_BLEND_ONE;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

	DX::ThrowIfFailed(device->CreateBlendState(&addBlendDesc, &AddBlend));

	D3D11_RASTERIZER_DESC RasterizerDesc{};
	RasterizerDesc.FillMode = D3D11_FILL_SOLID;
	RasterizerDesc.CullMode = D3D11_CULL_NONE;  //D3D11_CULL_BACK;
	RasterizerDesc.FrontCounterClockwise = TRUE;
	RasterizerDesc.DepthBias = 0;
	RasterizerDesc.SlopeScaledDepthBias = 1.0f;
	RasterizerDesc.DepthBiasClamp = 0.0f;
	RasterizerDesc.DepthClipEnable = TRUE;
	RasterizerDesc.ScissorEnable = FALSE;
	RasterizerDesc.MultisampleEnable = FALSE;
	RasterizerDesc.AntialiasedLineEnable = FALSE;

	DX::ThrowIfFailed(device->CreateRasterizerState(&RasterizerDesc, &Rasterizer));

	SettingsCB = new ConstantBuffer(ConstantBufferDesc<SettingsBuffer>());
	VolumeCB = new ConstantBuffer(ConstantBufferDesc<VolumeBuffer>());
	screenSize = (float2)Util::ConvertToDynamic(globals::state->screenSize);

	//// VOLUMES /////////////////////////////////////////////////////

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
	D3D11_TEXTURE3D_DESC ScatterVolumeDesc{ R16VolumeDesc };
	ScatterVolumeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	//if (settings.useCheckerBoard)
	//	ScatterVolumeDesc.Depth = (UINT)std::ceil(volumeDimensions.z / 2);
	//else
	ScatterVolumeDesc.Depth = (UINT)volumeDimensions.z;

	D3D11_UNORDERED_ACCESS_VIEW_DESC R16VolumeUAVdesc{};
	R16VolumeUAVdesc.Format = R16VolumeDesc.Format;
	R16VolumeUAVdesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	R16VolumeUAVdesc.Texture3D.MipSlice = 0;
	R16VolumeUAVdesc.Texture3D.FirstWSlice = 0;
	R16VolumeUAVdesc.Texture3D.WSize = R16VolumeDesc.Depth;
	D3D11_UNORDERED_ACCESS_VIEW_DESC RGBA16VolumeUAVDesc{ R16VolumeUAVdesc };
	RGBA16VolumeUAVDesc.Format = RGBA16VolumeDesc.Format;
	D3D11_UNORDERED_ACCESS_VIEW_DESC ScatterVolumeUAVDesc{ R16VolumeUAVdesc };
	ScatterVolumeUAVDesc.Format = ScatterVolumeDesc.Format;
	ScatterVolumeUAVDesc.Texture3D.WSize = ScatterVolumeDesc.Depth;

	D3D11_TEXTURE3D_DESC PerlinVolumeDesc{ R16VolumeDesc };
	PerlinVolumeDesc.Width = 32;
	PerlinVolumeDesc.Height = 32;
	PerlinVolumeDesc.Depth = 32;

	D3D11_UNORDERED_ACCESS_VIEW_DESC PerlinUAVDesc{ R16VolumeUAVdesc };
	PerlinUAVDesc.Format = PerlinVolumeDesc.Format;
	PerlinUAVDesc.Texture3D.WSize = PerlinVolumeDesc.Depth;

	DX::ThrowIfFailed(device->CreateTexture3D(&PerlinVolumeDesc, nullptr, &PerlinTex));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(PerlinTex, &PerlinUAVDesc, &PerlinUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(PerlinTex, nullptr, &PerlinSRV));

	DX::ThrowIfFailed(device->CreateTexture3D(&R16VolumeDesc, nullptr, &ShadowVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&R16VolumeDesc, nullptr, &ShadowVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVolume[0], &R16VolumeUAVdesc, &ShadowVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVolume[1], &R16VolumeUAVdesc, &ShadowVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVolume[0], nullptr, &ShadowVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVolume[1], nullptr, &ShadowVolumeSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture3D(&ScatterVolumeDesc, nullptr, &ScatteringVolume));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ScatteringVolume, &ScatterVolumeUAVDesc, &ScatteringVolumeUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ScatteringVolume, nullptr, &ScatteringVolumeSRV));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &FilteringVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &FilteringVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FilteringVolume[0], &RGBA16VolumeUAVDesc, &FilterVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FilteringVolume[1], &RGBA16VolumeUAVDesc, &FilterVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FilteringVolume[0], nullptr, &FilterVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FilteringVolume[1], nullptr, &FilterVolumeSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &MediaVolume[0]));
	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &MediaVolume[1]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(MediaVolume[0], &RGBA16VolumeUAVDesc, &MediaVolumeUAV[0]));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(MediaVolume[1], &RGBA16VolumeUAVDesc, &MediaVolumeUAV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(MediaVolume[0], nullptr, &MediaVolumeSRV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(MediaVolume[1], nullptr, &MediaVolumeSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &IntergrationVolume));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(IntergrationVolume, &RGBA16VolumeUAVDesc, &IntergrationVolumeUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(IntergrationVolume, nullptr, &IntergrationVolumeSRV));

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

	DX::ThrowIfFailed(device->CreateTexture2D(&outputDesc, nullptr, &OutputTexture));
	DX::ThrowIfFailed(device->CreateRenderTargetView(OutputTexture, nullptr, &OutputRTV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(OutputTexture, nullptr, &OutputSRV));

	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\STBN.dds", nullptr, &STBNoiseSRV));

	ID3D11Resource* Resource;
	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\WorldMap.dds", &Resource, &staticWorldMapSRV));
	DX::ThrowIfFailed(Resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&WorldMapTexture)));

	D3D11_TEXTURE2D_DESC FogMapDesc{};
	WorldMapTexture->GetDesc(&FogMapDesc);
	outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	FogMapDesc.Usage = D3D11_USAGE_DEFAULT;
	FogMapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	fogMapSize = float2((float)FogMapDesc.Width, (float)FogMapDesc.Height);

	D3D11_UNORDERED_ACCESS_VIEW_DESC FogMapUAVDesc{};
	FogMapUAVDesc.Format = FogMapDesc.Format;
	FogMapUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	FogMapUAVDesc.Texture2D.MipSlice = 0;

	DX::ThrowIfFailed(device->CreateTexture2D(&FogMapDesc, nullptr, &UIFogMapTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(UIFogMapTexture, &FogMapUAVDesc, &UIFogMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(UIFogMapTexture, nullptr, &UIFogMapSRV));

	DX::ThrowIfFailed(device->CreateTexture2D(&FogMapDesc, nullptr, &FogMapTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FogMapTexture, &FogMapUAVDesc, &FogMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FogMapTexture, nullptr, &FogMapSRV));

	//// GENERAL /////////////////////////////////////////////////////////////

	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());

	*reinterpret_cast<uint32_t*>(REL::RelocationID(391108, 391108).address()) = 0;  //Disable VL maps

	skyrim_SunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	renderdata = new Setup::LF_RenderData;

	renderdata->SetupPass(Shaders::Perlin, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::RenderShadowMapDebug, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ShadowEVSM, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ShadowEVSMBlur, true, 1, { .uncond_pass = true });
	//renderdata->SetupPass(Shaders::CloudESM, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ShadowVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::MediaVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ScatterVolume, true, 1, { .uncond_pass = true });

	//renderdata->SetupPass(Shaders::FilterVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::IntergrationVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::Apply, true, 1, { .uncond_pass = true });

	renderdata->SetupRenderData();

	CompileShaders();
}

void OrthogonalVolumetricLighting::LookupShader(int desc)
{
	static const std::unordered_map<int, void (OrthogonalVolumetricLighting::*)()> effects{
		{ Shaders::ShadowVolume, &OrthogonalVolumetricLighting::SetupShadowVolume },
		{ Shaders::ScatterVolume, &OrthogonalVolumetricLighting::SetupScatteringVolume },
		{ Shaders::FilterVolume, &OrthogonalVolumetricLighting::SetupFilterPass },
		{ Shaders::IntergrationVolume, &OrthogonalVolumetricLighting::SetupSliceMarch },
		{ Shaders::Apply, &OrthogonalVolumetricLighting::SetupApplyPass },
		{ Shaders::ShadowEVSM, &OrthogonalVolumetricLighting::SetupEVSM },
		//{ Shaders::RenderCloudMap, &OrthogonalVolumetricLighting::SetupCloudShadowMap },
		{ Shaders::MediaVolume, &OrthogonalVolumetricLighting::SetupMediaVolume },
		{ Shaders::Perlin, &OrthogonalVolumetricLighting::SetupPerlinNoise },
		{ Shaders::ShadowEVSMBlur, &OrthogonalVolumetricLighting::SetupEVSMBlur },
		//{ Shaders::CloudESM, &OrthogonalVolumetricLighting::SetupCloudESM },
		{ Shaders::Bypass, &OrthogonalVolumetricLighting::SetupBypass },
		{ Shaders::RenderShadowMap, &OrthogonalVolumetricLighting::RenderShadowMap },
		{ Shaders::RenderShadowMapDebug, &OrthogonalVolumetricLighting::RenderShadowMapDebug }

	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

void OrthogonalVolumetricLighting::CheckOverride()
{
	static Util::FrameChecker frame_checker;

	if (!CSMFinished) {
		overrideShader = true;
		shaderdesc = Shaders::RenderShadowMap;
	}

	if (overrideShader) {
		if (CSMFinished) {
			if (frame_checker.IsNewFrame()) {
				PerFrameUpdate();
			}
		}
		LookupShader(shaderdesc);
	}
}

void OrthogonalVolumetricLighting::PerFrameUpdate()
{
	float nearPlane = Util::GetCameraData().y;
	float farPlane = 11200.0f;
	frustumNearFar = float4(nearPlane, farPlane, 1.0f / nearPlane, volumeDimensions.z / std::log2(farPlane / nearPlane));

	auto eyePos = Util::GetEyePosition(0);
	if (eyePos.x > 1.0 || eyePos.x < -1.0) {
		eyePositionWS = float3(eyePos.x, eyePos.y, eyePos.z);
		//logger::info("camera pos: {}, {}, {}", eyePositionWS.x, eyePositionWS.y, eyePositionWS.z);
	}

	PrevMatrixIdx = UpdateMatrixCache();
	UpdateShadowLightMatrices();

	VolumeCB->Update(UpdateVolumeBuffer());
	SettingsCB->Update(UpdateSettingsBuffer());

	//delete
	float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	globals::d3d::context->ClearRenderTargetView(OutputRTV, clear);
	globals::d3d::context->ClearRenderTargetView(CloudShadowRTV, clear);

	if (settings.VarienceFrameIndex < varianceFrames)
		settings.VarienceFrameIndex++;

	if (resetVariance) {
		settings.VarienceFrameIndex = 0;
		resetVariance = false;
	}

	auto& matrix2 = globals::game::frameBufferCached.GetCameraProj();
	DirectX::XMMATRIX proj = DirectX::XMLoadFloat4x4(&static_cast<const DirectX::XMFLOAT4X4&>(matrix2));
	//LogMatrix("PlayerRef Proj", proj);
	auto& matrix = globals::game::frameBufferCached.GetCameraView();
	DirectX::XMMATRIX view = DirectX::XMLoadFloat4x4(&static_cast<const DirectX::XMFLOAT4X4&>(matrix));
	//LogMatrix("PlayerRef View", view);

	frameCounter++;
}

//// SHADOWS //////////////////////////////////////////////////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::SetupEVSM()
{
	auto context = globals::d3d::context;

	UpdateShadowLightMatrices();

	context->CSSetUnorderedAccessViews(0, 1, &EVSMUAV, nullptr);
	context->CSSetShader(GenerateEVSMCS, nullptr, 0);

	auto shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(0, 1, &shadowMap);

	ID3D11Buffer* terrainShadowBuffer = globals::features::terrainShadows.shadowUpdateCB.get()->CB();

	auto volumeBuff = VolumeCB->CB();
	auto settingsBuff = SettingsCB->CB();
	ID3D11Buffer* prevFrameBuff = FrameBuffer[PrevMatrixIdx].Get();
	ID3D11Buffer* FrameBuff = FrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	context->CSSetConstantBuffers(0, 1, &volumeBuff);
	context->CSSetConstantBuffers(1, 1, &settingsBuff);
	context->CSSetConstantBuffers(10, 1, &FrameBuff);
	context->CSSetConstantBuffers(3, 1, &prevFrameBuff);
	context->CSSetConstantBuffers(4, 1, &terrainShadowBuffer);

	context->CSSetSamplers(10, 1, &LinearSampler);
	context->CSSetSamplers(11, 1, &PointSampler);
	context->CSSetSamplers(13, 1, &AnisoLinear);
	context->CSSetSamplers(14, 1, &AnisoWrapLinear);
	context->CSSetSamplers(15, 1, &LinearMinSampler);

	auto shadowBuff = globals::deferred->perShadow->srv.get();
	context->CSSetShaderResources(10, 1, &shadowBuff);
	context->CSSetShaderResources(11, 1, &STBNoiseSRV);

	auto groups = EVSM_Size / 16;
	context->Dispatch(groups, groups, lightCascades);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupEVSMBlur()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &EVSMBlurUAV, nullptr);
	context->CSSetShader(BlurEVSMCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &EVSMSRV);

	auto groups = EVSM_Size / 16;
	context->Dispatch(groups, groups, lightCascades);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupShadowVolume()
{
	auto context = globals::d3d::context;
	static int passCount = 1;

	shadowVolParity = passCount & 1;
	auto volumeUAV = ShadowVolumeUAV[!shadowVolParity];
	auto prevVolumeSRV = ShadowVolumeSRV[shadowVolParity];

	context->CSSetUnorderedAccessViews(0, 1, &volumeUAV, nullptr);
	context->CSSetShader(GenerateShadowVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &prevVolumeSRV);
	context->CSSetShaderResources(1, 1, &EVSMBlurSRV);
	context->CSSetShaderResources(2, 1, &STBNoiseSRV);
	context->CSSetShaderResources(5, 1, &EVSMSRV);

	auto shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(3, 1, &shadowMap);

	auto skylightstbn = globals::features::skylighting.stbn_vec3_2Dx1D_128x128x64.get();
	context->CSSetShaderResources(4, 1, &skylightstbn);

	context->Dispatch(60, 34, 16);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	passCount++;
	overrideShader = false;
}

void OrthogonalVolumetricLighting::RenderShadowMap()
{
	auto context = globals::d3d::context;

	context->VSSetShader(ShadowMapVS, 0, 0);
	context->PSSetShader(ShadowMapPS, 0, 0);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::RenderShadowMapDebug()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->CSSetUnorderedAccessViews(0, 1, &ShadowVarianceDebugUAV, nullptr);

	context->CSSetShader(ShadowMapDebugCS, 0, 0);

	auto volumeBuff = VolumeCB->CB();
	context->CSSetConstantBuffers(0, 1, &volumeBuff);
	auto settingsBuff = SettingsCB->CB();
	context->CSSetConstantBuffers(1, 1, &settingsBuff);

	auto shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(0, 1, &shadowMap);
	auto& DepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY].depthSRV;
	context->CSSetShaderResources(1, 1, &DepthSRV);

	auto groups = CSM_Size / 16;
	context->Dispatch(groups, groups, lightCascades);

	overrideShader = false;
}

//// VOLUMES //////////////////////////////////////////////////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::SetupMediaVolume()
{
	auto context = globals::d3d::context;
	static int passCount = 1;

	mediaVolParity = passCount & 1;
	auto mediaUAV = MediaVolumeUAV[!mediaVolParity];
	auto prevMediaSRV = MediaVolumeSRV[mediaVolParity];

	//ID3D11UnorderedAccessView* UAVs[2] = { mediaUAV, FogMapUAV };
	context->CSSetUnorderedAccessViews(0, 1, &mediaUAV, nullptr);
	context->CSSetShader(GenerateMediaVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &prevMediaSRV);
	context->CSSetShaderResources(1, 1, &PerlinSRV);
	context->CSSetShaderResources(2, 1, &STBNoiseSRV);
	context->CSSetShaderResources(3, 1, &FogMapSRV);

	context->Dispatch(60, 34, 17);

	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	passCount++;
	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupScatteringVolume()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &ScatteringVolumeUAV, nullptr);
	context->CSSetShader(GenerateScatteringVolumeCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &ShadowVolumeSRV[!shadowVolParity]);
	context->CSSetShaderResources(1, 1, &MediaVolumeSRV[!mediaVolParity]);

	//context->CSSetShaderResources(0, 1, &ExpoSRV);

	//auto shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	//context->CSSetShaderResources(1, 1, &shadowMap);

	//D3D11Buffer* prevFrameBuff = FrameBuffer[PrevMatrixIdx].Get();
	//ID3D11Buffer* FrameBuff = FrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	//context->CSSetConstantBuffers(2, 1, &FrameBuff);
	//context->CSSetConstantBuffers(3, 1, &prevFrameBuff);

	//context->Dispatch(40, 23, 8);

	//if (settings.useCheckerBoard)
	//	context->Dispatch(40, 23, 11);
	//else
	context->Dispatch(60, 34, 17);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupFilterPass()
{
	auto context = globals::d3d::context;
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

	passCount++;
	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupSliceMarch()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &IntergrationVolumeUAV, nullptr);
	context->CSSetShader(SliceMarchCS, nullptr, 0);

	//context->CSSetShaderResources(0, 1, &FilterVolumeSRV[!FilterVolParity]);
	context->CSSetShaderResources(0, 1, &ScatteringVolumeSRV);

	context->Dispatch(30, 17, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupApplyPass()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	auto& mainRTV = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV;

	if (!swapOutputRT)
		context->OMSetRenderTargets(1, &mainRTV, nullptr);
	else
		context->OMSetRenderTargets(1, &OutputRTV, nullptr);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(ApplyVolumePS, NULL, NULL);

	context->PSSetSamplers(10, 1, &LinearSampler);
	context->PSSetSamplers(11, 1, &PointSampler);
	context->CSSetSamplers(13, 1, &AnisoLinear);
	context->CSSetSamplers(14, 1, &AnisoWrapLinear);
	//context->PSSetSamplers(12, 1, &DepthSampler);

	auto volumeBuff = VolumeCB->CB();
	context->PSSetConstantBuffers(0, 1, &volumeBuff);

	auto& DepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY].depthSRV;
	context->PSSetShaderResources(0, 1, &IntergrationVolumeSRV);
	context->PSSetShaderResources(1, 1, &DepthSRV);
	context->PSSetShaderResources(2, 1, &STBNoiseSRV);

	context->PSSetShaderResources(3, 1, &ShadowVolumeSRV[!shadowVolParity]);
	//context->PSSetShaderResources(4, 1, &ExpoBlurSRV);
	//context->PSSetShaderResources(3, 1, &ScatteringVolumeSRV);
	//context->PSSetShaderResources(4, 1, &FilterVolumeSRV[!FilterVolParity]);

	overrideShader = false;
}

//// UTIL /////////////////////////////////////////////////////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawFogMap()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &UIFogMapUAV, nullptr);
	context->CSSetUnorderedAccessViews(1, 1, &FogMapUAV, nullptr);

	context->CSSetShader(DrawFogMapCS, nullptr, 0);

	auto volumeBuff = VolumeCB->CB();
	context->CSSetConstantBuffers(0, 1, &volumeBuff);

	context->CSSetShaderResources(0, 1, &staticWorldMapSRV);

	auto heightMapSRV = globals::features::terrainShadows.texHeightMap->srv.get();
	context->CSSetShaderResources(1, 1, &heightMapSRV);

	context->Dispatch((UINT)fogMapSize.x, (UINT)fogMapSize.y, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupPerlinNoise()
{
	auto context = globals::d3d::context;
	static bool run = true;

	if (run) {
		context->CSSetUnorderedAccessViews(0, 1, &PerlinUAV, nullptr);
		context->CSSetShader(GeneratePerlinCS, nullptr, 0);
		context->Dispatch(4, 4, 4);
		ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
		run = false;
	}

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupBypass()
{
	auto context = globals::d3d::context;

	context->PSSetShader(nullptr, nullptr, 0);

	overrideShader = false;
}

///// BUFFER UPDATE /////////////////////////////////////////////////////
OrthogonalVolumetricLighting::VolumeBuffer OrthogonalVolumetricLighting::UpdateVolumeBuffer()
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

	VolumeBuffer data{};
	std::memcpy(data.directionalShadowCascadeMatrices, directionalShadowCascadeMatrices, sizeof(data.directionalShadowCascadeMatrices));
	std::memcpy(data.localShadowCascadeMatrices, localShadowCascadeMatrices, sizeof(data.localShadowCascadeMatrices));
	data.fogMapMatrix = fogMapViewProj;

	data.shadowCascadeEndSplit = shadowCascadeEndSplit;
	data.frustumNearFar = frustumNearFar;
	data.CameraWSPos = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0f);
	data.cameraData = Util::GetCameraData();

	data.EVSMData = float4((float)EVSM_Size, (float)EVSM_Size, (float)std::exp(settings.esmExponent), (float)std::exp(settings.esmExponent * 2.0f));
	data.heightMapParams = float4(mapScale.x, mapScale.y, mapOffset.x, mapOffset.y);
	data.heightMapZRange = float4(mapRange.x, mapRange.y, 0.0, 0.0);

	data.VolumeSize = volumeDimensions;
	data.NoiseSize = noiseDimensions;

	data.frameCounter = frameCounter;
	data.boardCondition = frameCounter & 1;
	return data;
}

OrthogonalVolumetricLighting::SettingsBuffer OrthogonalVolumetricLighting::UpdateSettingsBuffer()
{
	SettingsBuffer data{};
	data.cbsettings = settings;
	return data;
}

int OrthogonalVolumetricLighting::UpdateMatrixCache()
{
	int outValue = 0;
	if (CheckFrameBuffer()) {
		auto buffer = *globals::game::perFrame.get();
		static int PrevFrameWrite = 0;
		static bool PrevFrameValid = false;
		if (!PrevFrameValid) {
			globals::d3d::context->CopyResource(FrameBuffer[0].Get(), buffer);
			globals::d3d::context->CopyResource(FrameBuffer[1].Get(), buffer);
			PrevFrameValid = true;
			PrevFrameWrite = 0;
		}
		outValue = PrevFrameWrite ^ 1;
		globals::d3d::context->CopyResource(FrameBuffer[PrevFrameWrite].Get(), buffer);
		PrevFrameWrite ^= 1;
	}

	return outValue;
}

bool OrthogonalVolumetricLighting::CheckFrameBuffer()
{
	if (ID3D11Buffer* buffer = *globals::game::perFrame.get()) {
		if (!FrameBuffer[0] || !FrameBuffer[1]) {
			D3D11_BUFFER_DESC srcDesc{};
			buffer->GetDesc(&srcDesc);
			D3D11_BUFFER_DESC dstDesc = srcDesc;
			dstDesc.Usage = D3D11_USAGE_DEFAULT;
			dstDesc.CPUAccessFlags = 0;
			dstDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, FrameBuffer[0].GetAddressOf()));
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, FrameBuffer[1].GetAddressOf()));
		}
		if (FrameBuffer[0] || FrameBuffer[1])
			return true;
	}

	return false;
}

///// SHADOW FUNCS //////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::UpdateShadowLightMatrices()
{
	auto smState = globals::game::smState;

	if (auto shadowSceneNode = smState->shadowSceneNode[0]) {
		auto& shadowNodeRuntime = shadowSceneNode->GetRuntimeData();

		if (shadowNodeRuntime.shadowDirLight) {
			auto& dirLight = shadowNodeRuntime.shadowDirLight;
			auto& dirLightData = dirLight->GetRuntimeData();
			auto& dirCascadeList = dirLightData.shadowmapDescriptors;
			for (uint it = 0; it < dirCascadeList.size() && it < 4; it++) {
				directionalShadowCascadeMatrices[it] = GetCascadeMatrix(dirCascadeList[it].lightTransform);
			}
			auto& dirLightData2 = dirLight->GetShadowDirectionalLightRuntimeData();
			shadowCascadeEndSplit = float4(dirLightData2.endSplitDistances[0], dirLightData2.endSplitDistances[1], dirLightData2.endSplitDistances[2], 0.0);
		}

		int index = 0;
		auto& shadowLightList = shadowNodeRuntime.activeShadowLights;
		for (uint i = 0; i < shadowLightList.size(); i++) {
			auto& lightData = shadowLightList[i]->GetRuntimeData();
			auto& cascadeList = lightData.shadowmapDescriptors;
			localShadowCascadeMatrices[index++] = GetCascadeMatrix(cascadeList[0].lightTransform);
			localShadowCascadeMatrices[index++] = GetCascadeMatrix(cascadeList[1].lightTransform);
		}
	}
}

REX::W32::XMFLOAT4X4 OrthogonalVolumetricLighting::GetCascadeMatrix(REX::W32::XMFLOAT4X4& lightMatrix)
{
	float4 pos = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0);
	float4 transform = mul(pos, lightMatrix);
	//DirectX::XMVECTOR transform = XMVector4Transform(XMVectorSetW(eyePositionWS, 1), lightMatrix);

	//auto desc = "light InvViewProj";
	//logger::info("{} row 1: {}, {}, {}, {}", desc, lightMatrix.m[0][0], lightMatrix.m[0][1], lightMatrix.m[0][2], lightMatrix.m[0][3]);
	//logger::info("{} row 2: {}, {}, {}, {}", desc, lightMatrix.m[1][0], lightMatrix.m[1][1], lightMatrix.m[1][2], lightMatrix.m[1][3]);
	//logger::info("{} row 3: {}, {}, {}, {}", desc, lightMatrix.m[2][0], lightMatrix.m[2][1], lightMatrix.m[2][2], lightMatrix.m[2][3]);
	//logger::info("{} row 4: {}, {}, {}, {}", desc, lightMatrix.m[3][0], lightMatrix.m[3][1], lightMatrix.m[3][2], lightMatrix.m[3][3]);

	REX::W32::XMFLOAT4X4 matrix = lightMatrix;
	matrix.m[3][0] = transform.x;
	matrix.m[3][1] = transform.y;
	matrix.m[3][2] = transform.z;
	matrix.m[3][3] = transform.w;

	REX::W32::XMFLOAT4X4 outMatrix = matrix;
	transpose(matrix, outMatrix);

	return outMatrix;
}

void OrthogonalVolumetricLighting::SetupShadowCascade()
{
	//	static auto firstRun = true;

	//if (shadowLight && firstRun) {
	//	auto& shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM];
	//	shadowMap.texture = CascadeTex;
	//	shadowMap.depthSRV = CascadeSRV;
	//	std::copy_n(CascadeDSV, 4, shadowMap.views);

	//auto& runtime = shadowLight->GetRuntimeData();
	//runtime.shadowmapDescriptors[2].shadowmapIndex = 2;
	//runtime.shadowmapDescriptors[3].shadowmapIndex = 3;

	//	firstRun = false;
	//	}
}

///// SETTINGS //////////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawSettings()
{
	ImGui::SeparatorText("Reload");  ///////////////
	ImGui::Button("Reload Flare");
	if (ImGui::IsItemClicked()) {
		GenerateShadowVolumeCS = nullptr;
		GenerateScatteringVolumeCS = nullptr;
		SliceMarchCS = nullptr;
		ApplyVolumePS = nullptr;
		OutputPS = nullptr;
		DrawFogMapCS = nullptr;
		BlurEVSMCS = nullptr;

		CompileShaders();
	}

	ImGui::Spacing();
	ImGui::Checkbox("Swap Output RT", (bool*)&swapOutputRT);
	ImGui::Checkbox("Swap shadow map shader", (bool*)&swapShadowMapShader);

	ImGui::Checkbox("Run Variance", (bool*)&settings.RunVarienceMapping);
	//ImGui::Checkbox("Run Split", (bool*)&settings.DebugCascadeSplit);

	ImGui::Button("Reset Variance");
	if (ImGui::IsItemClicked()) {
		resetVariance = true;
	}

	//ImGui::Button("Render Debug Pass");
	//if (ImGui::IsItemClicked()) {
	//	RenderShadowMapDebug();
	//}

	ImGui::SeparatorText("Media properties");
	ImGui::SliderFloat("Anisotropy", &settings.anisotropy, -0.2, 1.0);
	ImGui::SliderFloat("Extinction Per Meter", &settings.extinction, 0.0001, 0.5);
	ImGui::SliderFloat("Scatter to Absorption Ratio", &settings.albedo, 0.0, 1.0);
	ImGui::Spacing();

	ImGui::SeparatorText("Shadow properties");
	ImGui::SliderInt("VSM Exponent: ", (int*)&settings.esmExponent, 1, 100);
	ImGui::Spacing();

	ImGui::SeparatorText("Global height fog");
	//ImGui::SliderFloat("Fog Density", &settings.globalFogDensity, 0.0f, 1.0f);
	//ImGui::SliderFloat("Ground Level Bias", &settings.globalFogStartHeight, -2000.0f, 2000.0f);
	//ImGui::SliderFloat("End Height", &settings.globalFogStartHeight, 0.0f, 50000.0f);
	//ImGui::SliderFloat("Falloff Distance", &settings.globalFogFalloffHeight, 0.0f, 10000.0f);

	ImGui::SliderFloat("Density", &settings.globalFogDensity, 0.0f, 1.0f);
	ImGui::SliderFloat("Start Height", &settings.globalFogStartHeight, -20000.0f, 50000.0f);
	ImGui::SliderFloat("Falloff Height Above Start", &settings.globalFogFalloffHeight, 0.0f, 20000.0f);

	//ImGui::Checkbox("Use History", (bool*)&settings.useHistory);
	//ImGui::SliderFloat("History Bias", &settings.historyAlpha, 0.0, 0.5);
	//ImGui::SliderFloat("Weight 1", &settings.weight1, 0.0, 1.0);
	//ImGui::SliderFloat("Weight 2", &settings.weight2, 0.0, 1.0);
	//ImGui::SliderFloat("Media Albedo", &settings.albedo, 0.0, 1.0);
	//ImGui::SliderFloat("Color Saturation", &settings.color_saturation, 0.0, 1.0);
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

//// GENERAL HOOKS ///////////////////////////////////////////////////////////////////

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

	//auto skyProperty = reinterpret_cast<RE::BSSkyShaderProperty*>(Pass->shaderProperty);
	//if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS) {
	//	if ((Pass->passEnum == 0x5C000062 || Pass->passEnum == 0x5C000063 || Pass->passEnum == 0x5C000064) && RenderFlags == 65) {
	//OVL.overrideShader = true;
	//OVL.shaderdesc = Shaders::RenderCloudMap;

	//auto rot = Pass->geometry->world.rotate;

	//if (frame_checker.IsNewFrame()) {
	//	counter = 0;
	//}
	//counter++;
	//if ((counter % 2) == 0) {
	//	OVL.overrideShader = true;
	//	OVL.shaderdesc = Shaders::RenderCloudMap;
	//}
	//}
	//	}

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
	auto& lens = globals::features::orthogonalVolumetricLighting;

	if (lens.swapShadowMapShader) {
		lens.CSMFinished = false;
		lens.overrideShader = true;
		lens.shaderdesc = Shaders::RenderShadowMap;
	}

	func(light, unk);
}

bool OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetFrameCamera::thunk(RE::BSShadowDirectionalLight* light, RE::NiCamera& inputCamera)
{
	auto& lens = globals::features::orthogonalVolumetricLighting;
	bool returnValue = true;

	if (!lens.swapOutputRT)
		returnValue = func(light, inputCamera);

	else {
		for (int cascade = 0; cascade < int(lens.lightCascades); ++cascade) {
			auto matrix = lens.cascadeData[cascade].cascadeRotation;
			auto row0 = RE::NiPoint3(matrix._13, matrix._12, matrix._11);
			auto row1 = RE::NiPoint3(matrix._23, matrix._22, matrix._21);
			auto row2 = RE::NiPoint3(matrix._33, matrix._32, matrix._31);

			RE::NiMatrix3 rotation = RE::NiMatrix3(row0, row1, row2);

			auto viewProj = lens.cascadeData[cascade].viewProj;
			auto translation = RE::NiPoint3(lens.cascadeData[cascade].cascadeTranslation.x, lens.cascadeData[cascade].cascadeTranslation.y, lens.cascadeData[cascade].cascadeTranslation.z);

			auto& runtime = light->GetRuntimeData();
			auto& desc = runtime.shadowmapDescriptors[cascade];
			memcpy(desc.camera->GetRuntimeData().worldToCam, &viewProj, sizeof(float) * 16);
			desc.camera->world.rotate = rotation;
			desc.camera->world.translate = translation;
		}
	}

	return returnValue;
}

//First override
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetCameraRuntimeData2::thunk(RE::NiCamera* cascadeCamera, RE::NiFrustum& frustum)
{
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	auto matrix = OVL.cascadeData[OVL.cascadeIt].cascadeRotation;
	auto row0 = RE::NiPoint3(matrix._13, matrix._12, matrix._11);
	auto row1 = RE::NiPoint3(matrix._23, matrix._22, matrix._21);
	auto row2 = RE::NiPoint3(matrix._33, matrix._32, matrix._31);

	RE::NiMatrix3 rotation = RE::NiMatrix3(row0, row1, row2);

	cascadeCamera->local.rotate = rotation;
	cascadeCamera->local.translate = RE::NiPoint3(OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.x, OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.y, OVL.cascadeData[OVL.cascadeIt].cascadeTranslation.z);

	/*
	{
		auto rotX = cascadeCamera->local.rotate.GetVectorX();
		auto rotY = cascadeCamera->local.rotate.GetVectorY();
		auto rotZ = cascadeCamera->local.rotate.GetVectorZ();
		logger::info("My GAME rotation X: {}, {}, {}", rotX.x, rotX.y, rotX.z);
		logger::info("My GAME rotation Y: {}, {}, {}", rotY.x, rotY.y, rotY.z);
		logger::info("My GAME rotation Z: {}, {}, {}", rotZ.x, rotZ.y, rotZ.z);
		logger::info("My GAME: translate {}, {}, {}", cascadeCamera->local.translate.x, cascadeCamera->local.translate.y, cascadeCamera->local.translate.z);
	}
	*/

	func(cascadeCamera, frustum);
}

struct FrustumSplit
{
	RE::NiPoint3 nearFace[4];
	RE::NiPoint3 farFace[4];
};
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_SetFrameCamera_BuildCascadeCameraCullingPlanes::thunk(RE::BSShadowDirectionalLight* dirLight, RE::NiFrustumPlanes& outPlanes, FrustumSplit& frustumSplit, uint32_t splitCornerIndices[8], uint32_t numSplitCornerIndices, RE::NiPoint3& lightDir, RE::NiPoint3& cameraPos, uint32_t cornerOffsetIndex)
{
	using namespace DirectX;
	auto& OVL = globals::features::orthogonalVolumetricLighting;

	auto& corners = OVL.cascadeData[OVL.cascadeIt].worldCorners;
	frustumSplit.nearFace[0] = { XMVectorGetX(corners[0]), XMVectorGetY(corners[0]), XMVectorGetZ(corners[0]) };
	frustumSplit.nearFace[1] = { XMVectorGetX(corners[1]), XMVectorGetY(corners[1]), XMVectorGetZ(corners[1]) };
	frustumSplit.nearFace[2] = { XMVectorGetX(corners[2]), XMVectorGetY(corners[2]), XMVectorGetZ(corners[2]) };
	frustumSplit.nearFace[3] = { XMVectorGetX(corners[3]), XMVectorGetY(corners[3]), XMVectorGetZ(corners[3]) };

	frustumSplit.farFace[0] = { XMVectorGetX(corners[4]), XMVectorGetY(corners[4]), XMVectorGetZ(corners[4]) };
	frustumSplit.farFace[1] = { XMVectorGetX(corners[5]), XMVectorGetY(corners[5]), XMVectorGetZ(corners[5]) };
	frustumSplit.farFace[2] = { XMVectorGetX(corners[6]), XMVectorGetY(corners[6]), XMVectorGetZ(corners[6]) };
	frustumSplit.farFace[3] = { XMVectorGetX(corners[7]), XMVectorGetY(corners[7]), XMVectorGetZ(corners[7]) };

	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
	func(dirLight, outPlanes, frustumSplit, splitCornerIndices, numSplitCornerIndices, lightDir, cameraPos, cornerOffsetIndex);
	outPlanes.activePlanes = static_cast<RE::NiFrustumPlanes::ActivePlane>(0);
}

#pragma warning(push)
#pragma warning(disable: 4100)
void OrthogonalVolumetricLighting::Hooks::BSShadowDirectionalLight_CreateFrustum::thunk(RE::NiFrustum& frustum, float left, float right, float top, float bottom, float nearP, float farP, bool ortho)
{
	auto& OVL = globals::features::orthogonalVolumetricLighting;
	auto frust = OVL.cascadeData[OVL.cascadeIt].frustum;
	//logger::info("GAME: left:{}  right:{}  top:{}  bottom:{}  near:{}  far:{}", left, right, top, bottom, nearP, farP);
	//logger::info("My GAME: left:{}  right:{}  top:{}  bottom:{}  near:{}  far:{}", frust.fLeft, frust.fRight, frust.fTop, frust.fBottom, frust.fNear, frust.fFar);

	func(frustum, frust.fLeft, frust.fRight, frust.fTop, frust.fBottom, frust.fNear, frust.fFar, ortho);

	OVL.cascadeIt = (++OVL.cascadeIt < 2) ? OVL.cascadeIt : 0;
}
#pragma warning(pop)

//Quantize to discrete angle steps
DirectX::XMVECTOR OrthogonalVolumetricLighting::QuantizeLightDirection(DirectX::XMVECTOR lightDir, float stepDegrees)  //0.05 - 0.1 works nicely
{
	using namespace DirectX;

	float stepRadians = XMConvertToRadians(stepDegrees);

	// Calculate azimuth (horizontal angle) and elevation (vertical angle)
	float azimuth = atan2f(XMVectorGetX(lightDir), XMVectorGetZ(lightDir));
	float elevation = asinf(XMVectorGetY(lightDir));

	// Quantize angles to nearest step
	azimuth = roundf(azimuth / stepRadians) * stepRadians;
	elevation = roundf(elevation / stepRadians) * stepRadians;

	// Convert back to Cartesian coordinates
	float cosElev = cosf(elevation);
	XMVECTOR quantized = XMVectorSet(sinf(azimuth) * cosElev, sinf(elevation), cosf(azimuth) * cosElev, 0.0f);

	return XMVector3Normalize(quantized);
}

#pragma warning(push)
#pragma warning(disable: 4100)
void OrthogonalVolumetricLighting::BuildShadowCascade(RE::BSShadowLight* light)
{
	using namespace DirectX;

	const float farPlane = Util::GetCameraData().x;
	const float nearPlane = Util::GetCameraData().y;

	auto tmp = Util::GetEyePosition(0);
	XMVECTOR rootCameraPos = XMVectorSet(tmp.x, tmp.y, tmp.z, 1.0);

	XMMATRIX rootViewProj = {};
	XMMATRIX rootInvViewProj = {};
	auto rootCamera = RE::Main::WorldRootCamera();
	if (rootCamera) {
		logger::info("Root Camera: {:p}", static_cast<void*>(rootCamera));
		XMFLOAT4X4 tmp2{};
		auto& worldToCam = rootCamera->GetRuntimeData().worldToCam;
		std::memcpy(&tmp2, &worldToCam, sizeof(tmp2));
		rootViewProj = XMMatrixTranspose(XMLoadFloat4x4(&tmp2));
		rootViewProj.r[3] = XMVector4Transform(rootCameraPos, rootViewProj);
		rootInvViewProj = XMMatrixInverse(nullptr, rootViewProj);
	} else {
		logger::info("Invalid camera");
	}

	float cascadeSplits[2] = { 1000, 3500 };
	int ind[8] = { 7, 3, 5, 1, 8, 4, 6, 2 };

	//auto dirLight = skyrim_cast<RE::BSShadowDirectionalLight*>(light);
	//auto lightDir = dirLight->GetShadowDirectionalLightRuntimeData().lightDirection;

	//XMVECTOR lightDirection = { lightDir.x, lightDir.y, lightDir.z, 0 };

	auto lightDir = *skyrim_SunPosition;

	//static XMVECTOR lightDirection = XMVectorNegate(XMVector3Normalize(XMVectorSet(lightDir.x, lightDir.y, lightDir.z, 0)));
	XMVECTOR lightDirection = XMVectorNegate(XMVector3Normalize(XMVectorSet(lightDir.x, lightDir.y, lightDir.z, 0)));
	lightDirection = QuantizeLightDirection(lightDirection, settings.albedo);
	LogVector("lightDirection", lightDirection);

	//static int counter = 0;
	//auto UpdateTimer = brushRadius;
	//if (counter >= UpdateTimer) {
	//	lightDirection = XMVectorNegate(XMVector3Normalize(XMVectorSet(lightDir.x, lightDir.y, lightDir.z, 0)));
	//	counter = 0;
	//}
	//counter++;

	const XMVECTOR frustum_corners[] = {
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
		logger::info("split_far: {}", split_far);
		logger::info("split_far v2: {}", cascadeSplits[cascade] / farPlane);

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

		XMVECTOR up = XMVectorSet(0, 1, 0, 0);
		XMVECTOR forward = lightDirection;
		XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
		up = XMVector3Cross(right, forward);

		XMMATRIX worldRotation = XMMatrixTranspose(XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1)));  //inverse

		XMVECTOR cornerMin = XMVectorReplicate(1e6f);
		XMVECTOR cornerMax = XMVectorNegate(cornerMin);
		for (int j = 0; j < 8; ++j) {
			XMVECTOR cascadeCorner = XMVector3Transform(corners[j], worldRotation);
			cornerMin = XMVectorMin(cornerMin, cascadeCorner);
			cornerMax = XMVectorMax(cornerMax, cascadeCorner);
		}
		////////////////////////////////////////////////////

		////
		XMVECTOR cornersV2[] = {
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_near), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[0], frustum_corners[1], split_far), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_near), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[2], frustum_corners[3], split_far), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_near), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[4], frustum_corners[5], split_far), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_near), worldRotation),
			XMVector3Transform(XMVectorLerp(frustum_corners[6], frustum_corners[7], split_far), worldRotation),
		};
		////////////////////////////////////////////////////

		//// Bound Sphere //////////////////////////////////
		float radius = 0;
		XMVECTOR center = {};
		// Compute cascade bounding sphere center:
		for (int j = 0; j < 8; ++j)
			center = XMVectorAdd(center, cornersV2[j]);
		center = center / 8;

		// Compute cascade bounding sphere radius:
		for (int j = 0; j < 8; ++j)
			radius = std::max(radius, XMVectorGetX(XMVector3Length(XMVectorSubtract(cornersV2[j], center))));

		LogVector("center", center);
		logger::info("radius: {}", radius);

		LogVector("old Min", cornerMin);
		LogVector("old Max", cornerMax);
		// Fit AABB onto bounding sphere
		XMVECTOR vRadius = XMVectorReplicate(radius);
		cornerMin = XMVectorSubtract(center, vRadius);
		cornerMax = XMVectorAdd(center, vRadius);
		LogVector("new Min", cornerMin);
		LogVector("new Max", cornerMax);
		/////////////////////////////////////////////////////

		//// Snap cascade to texel grid ///////////////////
		XMVECTOR extent = XMVectorSubtract(cornerMax, cornerMin);
		XMVECTOR texelSize = extent / float(CSM_Size);
		cornerMin = XMVectorFloor(cornerMin / texelSize) * texelSize;
		cornerMax = XMVectorFloor(cornerMax / texelSize) * texelSize;
		//center = (cornerMin + cornerMax) * 0.5f;

		LogVector("Effective Res", XMVectorMultiply(texelSize, XMVectorReplicate(0.01427)));
		LogVector("After Min", cornerMin);
		LogVector("After Max", cornerMax);
		/////////////////////////////////////////////////////

		cornerMin = XMVectorSetZ(cornerMin, XMVectorGetZ(cornerMin) + 15000);  //game expects it like this, (15,000 is the maximum shadow distance supported i think)
		cornerMax = XMVectorSetZ(cornerMax, XMVectorGetZ(cornerMax) + 15000);
		float3 minValues = cornerMin;
		float3 maxValues = cornerMax;

		XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(minValues.x, maxValues.x, minValues.y, maxValues.y, minValues.z, maxValues.z);
		XMMATRIX lightViewProj = XMMatrixMultiply(worldRotation, lightProjection);

		XMStoreFloat4x4(&cascadeData[cascade].viewProj, lightViewProj);

		XMStoreFloat4x4(&cascadeData[cascade].cascadeRotation, worldRotation);
		cascadeData[cascade].cascadeTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));  //only thing that seems to work

		cascadeData[cascade].frustum.fLeft = minValues.x;
		cascadeData[cascade].frustum.fRight = maxValues.x;
		cascadeData[cascade].frustum.fTop = maxValues.y;
		cascadeData[cascade].frustum.fBottom = minValues.y;
		cascadeData[cascade].frustum.fNear = minValues.z;
		cascadeData[cascade].frustum.fFar = maxValues.z;
	}
}

/*
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







		//XMMATRIX lightInvViewProj = XMMatrixInverse(nullptr, lightViewProj);

		//float3 center = (minValues + maxValues) * 0.5;
		//XMVECTOR worldSpaceCenter = XMVector3TransformNormal(center, worldRotation);
		//cascadeData[cascade].cascadeTranslation = XMVectorSubtract(rootCameraPos, worldSpaceCenter);
		//cascadeData[cascade].cascadeTranslation = XMVectorZero();
		//logger::info("center: {}, {}, {}", center.x, center.y, center.z);
		//LogVector("world Space Center", worldSpaceCenter);

		//LogMatrix("New ViewProj", lightViewProj);
		//LogMatrix("InvViewProj", lightInvViewProj);
	//}



			//for (int i = 0; i < 8; ++i)
	//	cascadeData[cascade].worldCorners[i] = corners[ind[i] - 1];
	//if (cascade == 1) {
	//	cascadeData[cascade].worldCorners[0] = cornersFirst[ind[4] - 1];
	//	cascadeData[cascade].worldCorners[1] = cornersFirst[ind[5] - 1];
	//	cascadeData[cascade].worldCorners[2] = cornersFirst[ind[6] - 1];
	//	cascadeData[cascade].worldCorners[3] = cornersFirst[ind[7] - 1];
	//}

		//XMVECTOR localTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));   //this causes shadows everywhere
		//localTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(1)));          //this causes 0 dist towards light

		//cornerMin = XMVectorSubtract(cornerMin, center);
		//cornerMax = XMVectorSubtract(cornerMax, center);

		//XMVECTOR vMin = XMVectorSubtract(center, vRadius);
		//XMVECTOR vMax = XMVectorAdd(center, vRadius);


		/*

		const float split_farV2 = LinearStep(nearPlane, farPlane, cascadeSplits[0]);
		const XMVECTOR cornersFirst[] = {
			XMVectorLerp(frustum_corners[0], frustum_corners[1], 0),
			XMVectorLerp(frustum_corners[0], frustum_corners[1], split_farV2),
			XMVectorLerp(frustum_corners[2], frustum_corners[3], 0),
			XMVectorLerp(frustum_corners[2], frustum_corners[3], split_farV2),
			XMVectorLerp(frustum_corners[4], frustum_corners[5], 0),
			XMVectorLerp(frustum_corners[4], frustum_corners[5], split_farV2),
			XMVectorLerp(frustum_corners[6], frustum_corners[7], 0),
			XMVectorLerp(frustum_corners[6], frustum_corners[7], split_farV2)
		};

		//for (int i = 0; i < 8; ++i)
		//	cascadeData[cascade].worldCorners[i] = corners[ind[i] - 1];
		//if (cascade == 1) {
		//	cascadeData[cascade].worldCorners[0] = cornersFirst[ind[4] - 1];
		//	cascadeData[cascade].worldCorners[1] = cornersFirst[ind[5] - 1];
		//	cascadeData[cascade].worldCorners[2] = cornersFirst[ind[6] - 1];
		//	cascadeData[cascade].worldCorners[3] = cornersFirst[ind[7] - 1];
		//}
		//for (int i = 0; i < 8; ++i)
		//	LogVector("Corners", cascadeData[cascade].worldCorners[i]);

		{
			XMVECTOR up = XMVectorSet(0, 1, 0, 0); //bgs basis is a gta cheat code
			XMVECTOR forward = lightDirection;
			XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
			up = XMVector3Cross(right, forward);

			XMVECTOR localTranslation = XMVectorSubtract(rootCameraPos, XMVectorMultiply(lightDirection, XMVectorReplicate(15000)));
			//cascadeData[cascade].cascadeTranslation = localTranslation;

			XMMATRIX localRotation = XMMATRIX(right, up, forward, XMVectorSet(0, 0, 0, 1));
			//XMStoreFloat4x4(&cascadeData[cascade].cascadeRotation, XMMatrixTranspose(localRotation));


			XMMATRIX viewMatrix = XMMatrixInverse(nullptr, XMMATRIX(localRotation.r[0], localRotation.r[1], localRotation.r[2], localTranslation));
			XMMATRIX baseProj = XMMatrixOrthographicOffCenterLH(0.0, 1.0, 0.0, 1.0, 10, 15000); //get rid of this in game. set at frustum clear
			XMMATRIX cascadeViewProj = XMMatrixMultiply(viewMatrix, baseProj);


			XMVECTOR cornerMin = XMVectorReplicate(1e6f);
			XMVECTOR cornerMax = XMVectorNegate(cornerMin);
			for (int i = 0; i < 8; i++) {
				XMVECTOR cameraCornerPos = XMVectorAdd(rootCameraPos, cascadeData[cascade].worldCorners[i]);
				XMVECTOR CascadeCameraPosition = XMVector3TransformCoord(cameraCornerPos, cascadeViewProj);
				CascadeCameraPosition = XMVectorMultiply(CascadeCameraPosition, XMVectorSet(0.5, 0.5, 1.0, 1.0));
				//LogVector("Transformed Pos", CascadeCameraPosition);

				cornerMin = XMVectorMin(cornerMin, CascadeCameraPosition);
				cornerMax = XMVectorMax(cornerMax, CascadeCameraPosition);
			}
			float3 clipMin = cornerMin;  //left, bottom, near
			float3 clipMax = cornerMax;  //right, top, far
			clipMax.z = clipMax.z * 14980 - 20;
			clipMin.z = clipMin.z * 14980 + 20;


			//cascadeData[cascade].frustum.fLeft = clipMin.x;  //correct
			//cascadeData[cascade].frustum.fRight = clipMax.x;
			//cascadeData[cascade].frustum.fTop = clipMax.y;
			//cascadeData[cascade].frustum.fBottom = clipMin.y;
			///cascadeData[cascade].frustum.fNear = clipMin.z;
			//cascadeData[cascade].frustum.fFar = clipMax.z;



			XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(clipMin.x, clipMax.x, clipMin.y, clipMax.y, clipMin.z, clipMax.z);
			XMMATRIX lightViewProj = XMMatrixMultiply(viewMatrix, lightProjection);


			LogVector("Min", XMVectorSetZ(cornerMin, XMVectorGetZ(cornerMin) * 14980));
			LogVector("Max", XMVectorSetZ(cornerMax, XMVectorGetZ(cornerMax) * 14980));
			LogMatrix("View", viewMatrix);
			LogMatrix("Projection", lightProjection);
			LogMatrix("ViewProj", lightViewProj);
		}
			*/

//XMVECTOR center = (cornerMin + cornerMax) * 0.5f;
//float Zadjust = abs(XMVectorGetZ(center) - XMVectorGetZ(cornerMin));
//Zadjust *= 4;
//cornerMin = XMVectorSetZ(XMVectorGetZ(center) - Zadjust);
//cornerMax = XMVectorSetZ(XMVectorGetZ(center) + Zadjust);

//XMFLOAT3 _center;
//XMStoreFloat3(&_center, center);

// clipping extrusion for projection:
//	Tight Z distribution for precision (16-bit unorm especially) but allowing some extra room for cascade blending in Z
/*
			{
				XMFLOAT3 _min;
				XMFLOAT3 _max;
				XMStoreFloat3(&_min, cornerMin);
				XMStoreFloat3(&_max, cornerMax);
				XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _min.z, _max.z);
				XMMATRIX lightViewProj = XMMatrixMultiply(cascadeView, lightProjection);
				LogMatrix("New cascadeViewProj", lightViewProj);
				float ext = abs(_center.z - _min.z);
				ext *= 4;
				_min.z = _center.z - ext;
				_max.z = _center.z + ext;

				//logger::info("New Min 3: {}, {}, {}", _min.x, _min.y, _min.z);
				//logger::info("New Max 3: {}, {}, {}", _max.x, _max.y, _max.z);

				lightProjection = XMMatrixOrthographicOffCenterLH(_min.x, _max.x, _min.y, _max.y, _min.z, _max.z);
				lightViewProj = XMMatrixMultiply(cascadeView, lightProjection);
				LogMatrix("New cascadeViewProj 2", lightViewProj);
			}


//}


struct WindParams
{
	float3 dirWS = float3(1.0f, 0.0f, 0.0f);
	float speedMps = 10.0f;
	float dirSmoothSec = 0.5f;
	float spdSmoothSec = 0.5f;
	float fadeDistanceM = 5000.0f;
	float noiseScaleXY = 0.02f;
	float noiseScaleZ = 0.02f;
};

struct WindState
{
	float3 offsetWS;      // accumulated scroll in meters (what becomes _m18.xyz)
	float3 periodWS;      // wrap period per axis in meters  = (1/scaleXY, 1/scaleXY, 1/scaleZ)
	float3 dirWS_smooth;  // smoothed direction
	float speed_smooth;   // smoothed speed
};

float Saturate(float x) { return std::max(0.0f, std::min(1.0f, x)); }
inline float3 floor(const float3& v) { return { std::floor(v.x), std::floor(v.y), std::floor(v.z) }; }
inline float3 lerp(float3& start, float3& end, float factor) { return start + (end - start) * factor; }
float SmoothK(float dt, float tau) { return 1.0f - exp(-dt / std::max(1e-3f, tau)); }
float3 WrapToPeriod(float3 x, float3 period) { return x - period * floor(x / period); }
float3 NormalizeSafe(float3 v, float3 fallback = { 1, 0, 0 })
{
	float len = v.Length();
	return (len > 1e-6f) ? (v / len) : fallback;
}

void InitWind(const WindParams& windParams, WindState& windState)
{
	windState.offsetWS = { 0, 0, 0 };
	windState.periodWS = { 1.0f / windParams.noiseScaleXY, 1.0f / windParams.noiseScaleXY, 1.0f / windParams.noiseScaleZ };
	windState.dirWS_smooth = NormalizeSafe(windParams.dirWS);
	windState.speed_smooth = windParams.speedMps;
}

void UpdateWind(const WindParams& windParams, float dt, WindState& windState)
{
	float kDir = SmoothK(dt, windParams.dirSmoothSec);
	float kSpd = SmoothK(dt, windParams.spdSmoothSec);

	float3 dirTarget = NormalizeSafe(windParams.dirWS);
	windState.dirWS_smooth = NormalizeSafe(lerp(windState.dirWS_smooth, dirTarget, kDir), dirTarget);
	windState.speed_smooth = std::lerp(windState.speed_smooth, windParams.speedMps, kSpd);

	// 2) Integrate displacement in world meters
	float3 velocityWS = windState.dirWS_smooth * windState.speed_smooth;  // m/windState
	windState.offsetWS += velocityWS * dt;                                // meters

	// 3) Wrap to noise period so values stay small and tile seamlessly
	windState.offsetWS = WrapToPeriod(windState.offsetWS, windState.periodWS);
}
*/

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