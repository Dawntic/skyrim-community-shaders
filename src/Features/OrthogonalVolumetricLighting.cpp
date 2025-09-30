#include "OrthogonalVolumetricLighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OrthogonalVolumetricLighting::Settings,
	useHistory, historyAlpha, weight1, weight2,
	anisotropy, extinction, color_saturation, esmExponent, useCheckerBoard)

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

	CloudShadowVS = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_VETEX", "" } }, "vs_5_0");
	CloudShadowPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_PIXEL", "" } }, "ps_5_0");
	CloudShadowCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\Volumetric Lighting.hlsl", { { "CLOUD_ESM_COMPUTE", "" } }, "cs_5_0");
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

	if (settings.useCheckerBoard)
		ScatterVolumeDesc.Depth = (UINT)std::ceil(volumeDimensions.z / 2);
	else
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
	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\WorldMap.dds", &Resource, &WorldMapSRV));
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

	DX::ThrowIfFailed(device->CreateTexture2D(&FogMapDesc, nullptr, &FogMapTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(FogMapTexture, &FogMapUAVDesc, &FogMapUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(FogMapTexture, nullptr, &FogMapSRV));

	//// SHADOW MAPPING ////////////////////////////////////////////////////

	D3D11_TEXTURE2D_DESC CascadeDesc{};
	CascadeDesc.Width = CSM_Size;
	CascadeDesc.Height = CSM_Size;
	CascadeDesc.MipLevels = 1;
	CascadeDesc.ArraySize = 4;
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
	CascadeDSVDesc.Texture2DArray.ArraySize = 4;
	D3D11_SHADER_RESOURCE_VIEW_DESC CascadeSRVDesc{};
	CascadeSRVDesc.Format = DXGI_FORMAT_R16_UNORM;
	CascadeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	CascadeSRVDesc.Texture2DArray.MostDetailedMip = 0;
	CascadeSRVDesc.Texture2DArray.MipLevels = 1;
	CascadeSRVDesc.Texture2DArray.FirstArraySlice = 0;
	CascadeSRVDesc.Texture2DArray.ArraySize = 4;

	DX::ThrowIfFailed(device->CreateTexture2D(&CascadeDesc, nullptr, &CascadeTex));
	DX::ThrowIfFailed(device->CreateShaderResourceView(CascadeTex, &CascadeSRVDesc, &CascadeSRV));

	for (UINT i = 0; i < 4; ++i) {
		CascadeDSVDesc.Texture2DArray.FirstArraySlice = i;
		CascadeDSVDesc.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView(CascadeTex, &CascadeDSVDesc, &CascadeDSV[i]);
	}

	D3D11_TEXTURE2D_DESC ExpoDesc{};
	ExpoDesc.Width = EVSM_Size;
	ExpoDesc.Height = EVSM_Size;
	ExpoDesc.MipLevels = 1;
	ExpoDesc.ArraySize = 4;
	ExpoDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
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
	ExpoUAVDesc.Texture2DArray.ArraySize = 4;

	DX::ThrowIfFailed(device->CreateTexture2D(&ExpoDesc, nullptr, &ExpoTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ExpoTexture, &ExpoUAVDesc, &ExpoUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ExpoTexture, nullptr, &ExpoSRV));

	D3D11_TEXTURE2D_DESC ExpoBlurDesc{ ExpoDesc };
	D3D11_UNORDERED_ACCESS_VIEW_DESC ExpoBlurUAVDesc{ ExpoUAVDesc };

	DX::ThrowIfFailed(device->CreateTexture2D(&ExpoBlurDesc, nullptr, &ExpoBlurTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ExpoBlurTexture, &ExpoBlurUAVDesc, &ExpoBlurUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ExpoBlurTexture, nullptr, &ExpoBlurSRV));

	D3D11_TEXTURE2D_DESC cloudMapDesc{ outputDesc };
	D3D11_TEXTURE2D_DESC cloudShadowESMDesc{ outputDesc };
	cloudShadowESMDesc.MipLevels = 1;  //change this
	cloudShadowESMDesc.Format = DXGI_FORMAT_R16_FLOAT;
	cloudShadowESMDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	D3D11_UNORDERED_ACCESS_VIEW_DESC cloudShadowESMUAVDesc{ FogMapUAVDesc };
	cloudShadowESMUAVDesc.Format = cloudShadowESMDesc.Format;

	DX::ThrowIfFailed(device->CreateTexture2D(&cloudMapDesc, nullptr, &CloudShadowTexture));
	DX::ThrowIfFailed(device->CreateRenderTargetView(CloudShadowTexture, nullptr, &CloudShadowRTV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(CloudShadowTexture, nullptr, &CloudMapSRV));

	DX::ThrowIfFailed(device->CreateTexture2D(&cloudShadowESMDesc, nullptr, &CloudShadowESMTexture));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(CloudShadowESMTexture, &cloudShadowESMUAVDesc, &CloudShadowESMUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(CloudShadowESMTexture, nullptr, &CloudShadowESMSRV));

	//// GENERAL /////////////////////////////////////////////////////////////

	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());

	*reinterpret_cast<uint32_t*>(REL::RelocationID(391108, 391108).address()) = 0;  //Disable VL maps

	skyrim_SunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());

	renderdata = new Setup::LF_RenderData;

	renderdata->SetupPass(Shaders::Perlin, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ShadowEVSM, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ShadowEVSMBlur, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::CloudESM, true, 1, { .uncond_pass = true });
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
		{ Shaders::Apply, &OrthogonalVolumetricLighting::SetupApplyVolume },
		{ Shaders::ShadowEVSM, &OrthogonalVolumetricLighting::SetupEVSM },
		{ Shaders::RenderCloudMap, &OrthogonalVolumetricLighting::SetupCloudShadowMap },
		{ Shaders::MediaVolume, &OrthogonalVolumetricLighting::SetupMediaVolume },
		{ Shaders::Perlin, &OrthogonalVolumetricLighting::SetupPerlinNoise },
		{ Shaders::ShadowEVSMBlur, &OrthogonalVolumetricLighting::SetupEVSMBlur },
		{ Shaders::CloudESM, &OrthogonalVolumetricLighting::SetupCloudESM }

	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

void OrthogonalVolumetricLighting::CheckOverride()
{
	static Util::FrameChecker frame_checker;

	if (overrideShader) {
		if (frame_checker.IsNewFrame()) {
			PerFrameUpdate();
		}
		LookupShader(shaderdesc);
	}
}

//// SHADOWS //////////////////////////////////////////////////
void OrthogonalVolumetricLighting::SetupEVSM()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &ExpoUAV, nullptr);
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

	auto shadowBuff = globals::deferred->perShadow->srv.get();
	context->CSSetShaderResources(10, 1, &shadowBuff);
	context->CSSetShaderResources(11, 1, &STBNoiseSRV);

	auto groups = EVSM_Size / 16;
	context->Dispatch(groups, groups, 4);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupEVSMBlur()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &ExpoBlurUAV, nullptr);
	context->CSSetShader(BlurEVSMCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &ExpoSRV);

	auto groups = EVSM_Size / 16;
	context->Dispatch(groups, groups, 4);

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
	context->CSSetShaderResources(1, 1, &ExpoBlurSRV);
	context->CSSetShaderResources(2, 1, &STBNoiseSRV);

	auto shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(3, 1, &shadowMap);

	context->Dispatch(60, 34, 17);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	passCount++;
	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupCloudShadowMap()
{
	auto context = globals::d3d::context;

	context->RSSetState(Rasterizer);

	context->OMSetRenderTargets(1, &CloudShadowRTV, nullptr);

	context->VSSetShader(CloudShadowVS, nullptr, 0);
	context->PSSetShader(CloudShadowPS, nullptr, 0);

	auto volumeBuff = VolumeCB->CB();
	context->VSSetConstantBuffers(0, 1, &volumeBuff);
	context->PSSetConstantBuffers(0, 1, &volumeBuff);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupCloudESM()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &CloudShadowESMUAV, nullptr);
	context->CSSetShader(CloudShadowCS, nullptr, 0);

	context->CSSetShaderResources(0, 1, &CloudShadowESMSRV);

	float2 groups = CloudESM_Size / 16;
	//logger::info("groups: {}  Size: {}  int: {}", groups.x, CloudESM_Size.x, (UINT)groups.x);

	context->Dispatch((UINT)groups.x, (UINT)groups.y, 1);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

//// VOLUMES ///////////////////////////////////////////////////////////
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

	if (settings.useCheckerBoard)
		context->Dispatch(40, 23, 11);
	else
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

void OrthogonalVolumetricLighting::SetupApplyVolume()
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
	//context->PSSetSamplers(12, 1, &DepthSampler);

	auto volumeBuff = VolumeCB->CB();
	context->PSSetConstantBuffers(0, 1, &volumeBuff);

	auto& DepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY].depthSRV;
	context->PSSetShaderResources(0, 1, &IntergrationVolumeSRV);
	context->PSSetShaderResources(1, 1, &DepthSRV);
	context->PSSetShaderResources(2, 1, &STBNoiseSRV);

	//context->PSSetShaderResources(3, 1, &ScatteringVolumeSRV);
	//context->PSSetShaderResources(4, 1, &FilterVolumeSRV[!FilterVolParity]);

	overrideShader = false;
}

//// UTIL ////////////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::DrawFogMap()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &FogMapUAV, nullptr);
	context->CSSetShader(DrawFogMapCS, nullptr, 0);

	auto volumeBuff = VolumeCB->CB();
	context->CSSetConstantBuffers(0, 1, &volumeBuff);
	context->CSSetShaderResources(0, 1, &WorldMapSRV);

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

///// BUFFER UPDATE /////////////////////////////////////////////////////
void OrthogonalVolumetricLighting::PerFrameUpdate()
{
	float nearPlane = Util::GetCameraData().y;
	float farPlane = 11198.0f;
	frustumNearFar = float4(nearPlane, farPlane, 1.0f / nearPlane, volumeDimensions.z / std::log2(farPlane / nearPlane));

	auto eyePos = Util::GetEyePosition(0);
	if (eyePos.x > 1.0 || eyePos.x < -1.0) {
		eyePositionWS = float3(eyePos.x, eyePos.y, eyePos.z);
		//logger::info("camera pos: {}, {}, {}", eyePositionWS.x, eyePositionWS.y, eyePositionWS.z);
	}

	PrevMatrixIdx = UpdateMatrixCache();

	BuildCloudShadowMatrix();

	VolumeCB->Update(UpdateVolumeBuffer());
	SettingsCB->Update(UpdateSettingsBuffer());

	float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	globals::d3d::context->ClearRenderTargetView(OutputRTV, clear);
	globals::d3d::context->ClearRenderTargetView(CloudShadowRTV, clear);

	frameCounter++;
}

OrthogonalVolumetricLighting::VolumeBuffer OrthogonalVolumetricLighting::UpdateVolumeBuffer()
{
	auto& lightData = shadowLight->GetRuntimeData();

	VolumeBuffer data{};
	data.shadowCascadeMatrix[0] = GetCascadeMatrix(lightData.shadowmapDescriptors[0].lightTransform);
	data.shadowCascadeMatrix[1] = GetCascadeMatrix(lightData.shadowmapDescriptors[1].lightTransform);
	data.shadowCascadeMatrix[2] = GetCascadeMatrix(lightData.shadowmapDescriptors[2].lightTransform);
	data.shadowCascadeMatrix[3] = GetCascadeMatrix(lightData.shadowmapDescriptors[3].lightTransform);
	data.cloudShadowMatrix = cloudShadowLSViewProj;
	data.fogMapMatrix = fogMapViewProj;
	data.EVSMData = float4((float)EVSM_Size, (float)EVSM_Size, (float)std::exp(settings.esmExponent), (float)std::exp(settings.esmExponent * 2.0f));
	data.frustumNearFar = frustumNearFar;
	data.PlayerWSPos = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0f);
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
REX::W32::XMFLOAT4X4 OrthogonalVolumetricLighting::GetCascadeMatrix(REX::W32::XMFLOAT4X4& lightMatrix)
{
	float4 pos = float4(eyePositionWS.x, eyePositionWS.y, eyePositionWS.z, 1.0);
	float4 transform = mul(pos, lightMatrix);

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
	static auto firstRun = true;

	if (shadowLight && firstRun) {
		auto& shadowMap = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM];
		shadowMap.texture = CascadeTex;
		shadowMap.depthSRV = CascadeSRV;
		std::copy_n(CascadeDSV, 4, shadowMap.views);

		//auto& runtime = shadowLight->GetRuntimeData();
		//runtime.shadowmapDescriptors[2].shadowmapIndex = 2;
		//runtime.shadowmapDescriptors[3].shadowmapIndex = 3;

		firstRun = false;
	}
}

//const DirectX::XMMATRIX lightRotation = DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&scene.weather.stars_rotation_quaternion));  // We only care about prioritized directional light anyway
//const DirectX::XMVECTOR up = DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), lightRotation);

void OrthogonalVolumetricLighting::BuildCloudShadowMatrix()
{
	using namespace DirectX;

	const float planetRadiusF = 6371e3f / 1.428e-2;  //446,148,459
	XMVECTOR planetRadius = { planetRadiusF, planetRadiusF, planetRadiusF };
	XMVECTOR planetCentre = { 0.0, 0.0, -planetRadiusF };

	const float cloudShadowSnapLength = 5000.0f;  //World-space snapping causes shimmering as sun angle / field of view changes
	const float cloudShadowExtent = 10000.0f;     // The cloud shadow bounding box size
	const float cloudShadowNearPlane = 0.0f;
	const float cloudShadowFarPlane = cloudShadowExtent * 2.0;

	const XMVECTOR up = { 0.0f, 0.0f, 1.0f, 0.0f };  //XMMatrixLookAtLH will produce garbage when up is nearly colinear with the view direction

	auto sunWSPos = *skyrim_SunPosition;
	XMVECTOR sunDirection = XMVector3Normalize({ sunWSPos.x, sunWSPos.y, sunWSPos.z });

	XMVECTOR lookAtPosition = XMVector3Normalize(XMVectorSubtract({ eyePositionWS.x, eyePositionWS.y, eyePositionWS.z }, planetCentre));
	lookAtPosition = XMVectorMultiplyAdd(lookAtPosition, planetRadius, planetCentre);
	lookAtPosition = XMVectorAdd(lookAtPosition, XMVectorReplicate(0.5f * cloudShadowSnapLength));
	lookAtPosition = XMVectorScale(XMVectorFloor(XMVectorScale(lookAtPosition, 1.0f / cloudShadowSnapLength)), cloudShadowSnapLength);

	XMVECTOR lightPosition = XMVectorMultiplyAdd(sunDirection, { cloudShadowExtent, cloudShadowExtent, cloudShadowExtent }, lookAtPosition);

	XMMATRIX cloudShadowView = XMMatrixLookAtLH(lightPosition, lookAtPosition, up);
	XMMATRIX cloudShadowProjection = XMMatrixOrthographicOffCenterLH(-cloudShadowExtent, cloudShadowExtent, -cloudShadowExtent, cloudShadowExtent, cloudShadowNearPlane, cloudShadowFarPlane);

	//XMMATRIX cloudShadowViewProj = XMMatrixMultiply(cloudShadowView, cloudShadowProjection);
	XMMATRIX cloudShadowViewProj = XMMatrixMultiply(cloudShadowProjection, cloudShadowView);
	XMMATRIX cloudShadowViewProjInverse = XMMatrixInverse(nullptr, cloudShadowViewProj);

	XMStoreFloat4x4(&cloudShadowLSViewProj, XMMatrixTranspose(cloudShadowViewProj));
	XMStoreFloat4x4(&cloudShadowLSViewProjInverse, XMMatrixTranspose(cloudShadowViewProjInverse));
}

RE::BSShaderProperty::RenderPassArray* OrthogonalVolumetricLighting::Hooks::BSSkyShader_GetRenderPasses::thunk(RE::BSGeometry* geometry, std::uint32_t arg2, RE::BSShaderAccumulator* accumulator)
{
	RE::BSShaderProperty::RenderPassArray* passArray = func(geometry, arg2, accumulator);

	if (!passArray || !passArray->head)
		return passArray;

	std::vector<RE::BSRenderPass*> newArray;

	auto pass = passArray->head;
	while (pass) {
		if (reinterpret_cast<RE::BSSkyShaderProperty*>(pass->shaderProperty)->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS)
			newArray.push_back(pass);
		pass = pass->next;
	}

	if (newArray.size() < 2) {
		auto tail = passArray->head;
		while (tail->next)
			tail = tail->next;

		for (auto p : newArray) {
			auto newPass = new RE::BSRenderPass(*p);
			newPass->next = nullptr;
			tail->next = newPass;
			tail = newPass;
		}
	}

	return passArray;
}

void OrthogonalVolumetricLighting::Hooks::SetShadowMapCount::thunk(RE::BSShadowLight* light, uint64_t numLights)
{
	if (light->IsDirectionalLight())
		numLights = 4;

	func(light, numLights);
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

	ImGui::Checkbox("Swap Output RT", (bool*)&swapOutputRT);

	ImGui::Checkbox("Use History", (bool*)&settings.useHistory);
	//ImGui::SliderFloat("History Bias", &settings.historyAlpha, 0.0, 0.5);
	//ImGui::SliderFloat("Weight 1", &settings.weight1, 0.0, 1.0);
	//ImGui::SliderFloat("Weight 2", &settings.weight2, 0.0, 1.0);
	ImGui::SliderFloat("Anisotropy", &settings.anisotropy, -0.2, 1.0);
	ImGui::SliderFloat("Extinction", &settings.extinction, 0.0001, 0.1);
	ImGui::SliderFloat("Color Saturation", &settings.color_saturation, 0.0, 1.0);
	//ImGui::SliderFloat("Shadow Threshold", &settings.shadow_threshold, 0.0, 1.0);
	ImGui::SliderInt("VSM Exponent: ", (int*)&settings.esmExponent, 1, 100);

	ImGui::SeparatorText("Fog Maps");
	ImGui::SliderFloat("Density", &density, 0.0f, 1.0f);
	ImGui::SliderFloat("Start Height", &fogStartHeight, 0.0f, 1.0f);
	ImGui::SliderFloat("Falloff Rate", &fogFalloffRate, 0.0f, 1.0f);

	ImGui::SliderFloat("Radius", &brushRadius, 1.0f, 200.0f, "%.0f");
	ImGui::SliderFloat("Feather", &brushFeather, 0.0f, 1.0f);
	ImGui::SliderFloat("Erase", &settings.blendOpp, -1.0f, 0.0f, "%.0f");

	//ImGui::Button("Export Map");
	//if (ImGui::IsItemClicked()) {

	//}

	ImVec2 displaySize = ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f);

	if (ImGui::BeginChild("PaintCanvas", displaySize, true, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)) {
		ImVec2 mouse = ImGui::GetIO().MousePos;

		ImVec2 PosTL = ImGui::GetCursorScreenPos();
		ImVec2 PosBR = ImVec2(PosTL.x + displaySize.x, PosTL.y + displaySize.y);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddImage((ImTextureID)WorldMapSRV, PosTL, PosBR);
		drawList->AddImage((ImTextureID)FogMapSRV, PosTL, PosBR);

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
				settings.fogMapColor = float4(1.0f + fogFalloffRate, 1.0f, 1.0f + fogStartHeight, density);

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
	static Util::FrameChecker frame_checker;
	static int counter = 0;

	auto skyProperty = reinterpret_cast<RE::BSSkyShaderProperty*>(Pass->shaderProperty);
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS) {
		if ((Pass->passEnum == 0x5C000062 || Pass->passEnum == 0x5C000063) && RenderFlags == 65) {
			if (frame_checker.IsNewFrame()) {
				counter = 0;
			}
			counter++;
			if ((counter % 2) == 0) {
				OVL.overrideShader = true;
				OVL.shaderdesc = Shaders::RenderCloudMap;
			}
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