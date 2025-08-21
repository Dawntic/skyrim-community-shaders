#include "OrthogonalVolumetricLighting.h"
#include "../Deferred.h"
#include "../Upscaling.h"
#include "State.h"
#include "TerrainShadows.h"
#include "Util.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <REX/W32/COMPTR.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OrthogonalVolumetricLighting::Settings, test)

void OrthogonalVolumetricLighting::CompileShaders()
{
	BypassVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\DownSample.hlsl", { { "BYPASS_VSSHADER", "" } }, "vs_5_0");
	DownSamplePS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\DownSample.hlsl", { { "DownSample", "" } }, "ps_5_0");
	MinifyPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\DownSample.hlsl", { { "Minify", "" } }, "ps_5_0");
	GenerateShadowVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\ShadowVolume.hlsl", { { "ShadowVolumeCompute", "" } }, "cs_5_0");
	GenerateScatteringVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\ScatteringVolume.hlsl", { { "ScatterVolumeCompute", "" } }, "cs_5_0");
	FilterVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\ScatteringVolume.hlsl", { { "FilterVolumeCompute", "" } }, "cs_5_0");
	SliceMarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\MarchAndApply.hlsl", { { "MarchVolumeCompute", "" } }, "cs_5_0");
	ApplyVolumePS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\MarchAndApply.hlsl", { { "ApplyVolume", "" } }, "ps_5_0");
	OutputPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\MarchAndApply.hlsl", { { "OutputPixel", "" } }, "ps_5_0");

	FilterPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\OrthogonalVolumetricLighting\\DownSample.hlsl", { { "Filter", "" } }, "ps_5_0");
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

	D3D11_SAMPLER_DESC pointSamplerDesc{ linearSamplerDesc };
	pointSamplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	D3D11_SAMPLER_DESC depthSamplerDesc{ linearSamplerDesc };
	depthSamplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	depthSamplerDesc.ComparisonFunc = D3D11_COMPARISON_EQUAL;

	DX::ThrowIfFailed(device->CreateSamplerState(&linearSamplerDesc, &LinearSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&pointSamplerDesc, &PointSampler));
	DX::ThrowIfFailed(device->CreateSamplerState(&depthSamplerDesc, &DepthSampler));

	D3D11_BLEND_DESC desc = {};
	desc.AlphaToCoverageEnable = FALSE;
	desc.IndependentBlendEnable = FALSE;
	auto& rt = desc.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	rt.SrcBlend = D3D11_BLEND_ONE;
	rt.DestBlend = D3D11_BLEND_ONE;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ONE;
	rt.DestBlendAlpha = D3D11_BLEND_ONE;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;

	DX::ThrowIfFailed(device->CreateBlendState(&desc, &AddBlend));

	ESMCBuffer = new ConstantBuffer(ConstantBufferDesc<ESMBuffer>());

	screenSize = (float2)Util::ConvertToDynamic(globals::state->screenSize);

	CompileShaders();

	viewPort[0].TopLeftX = 0.0f;
	viewPort[0].TopLeftY = 0.0f;
	viewPort[0].Width = DownSampleExpo_TileSize;
	viewPort[0].Height = DownSampleExpo_TileSize;
	viewPort[0].MinDepth = 0.0f;
	viewPort[0].MaxDepth = 1.0f;

	viewPort[1] = viewPort[0];
	viewPort[1].TopLeftY = DownSampleExpo_TileSize;

	viewPort[2] = viewPort[0];
	viewPort[2].Width = ESM_AtlasSize.x;
	viewPort[2].Height = ESM_AtlasSize.y;

	viewPort[3] = viewPort[0];
	viewPort[3].Width = screenSize.x;
	viewPort[3].Height = screenSize.y;

	D3D11_TEXTURE2D_DESC ExponentiateDesc{};
	ExponentiateDesc.Width = (UINT)DownSampleExpo_AtlasSize.x;
	ExponentiateDesc.Height = (UINT)DownSampleExpo_AtlasSize.x;
	ExponentiateDesc.MipLevels = 1;
	ExponentiateDesc.ArraySize = 1;
	ExponentiateDesc.Format = DXGI_FORMAT_R32_FLOAT;
	ExponentiateDesc.Usage = D3D11_USAGE_DEFAULT;
	ExponentiateDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ExponentiateDesc.SampleDesc.Count = 1;
	ExponentiateDesc.SampleDesc.Quality = 0;
	ExponentiateDesc.CPUAccessFlags = 0;
	ExponentiateDesc.MiscFlags = 0;

	D3D11_TEXTURE2D_DESC MinifyDesc{ ExponentiateDesc };
	MinifyDesc.Width = (UINT)ESM_AtlasSize.x;
	MinifyDesc.Height = (UINT)ESM_AtlasSize.y;

	DX::ThrowIfFailed(device->CreateTexture2D(&ExponentiateDesc, nullptr, &ExponentiateTex[0]));
	DX::ThrowIfFailed(device->CreateRenderTargetView(ExponentiateTex[0], nullptr, &ExponentiateRTV[0]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ExponentiateTex[0], nullptr, &ExponentiateSRV[0]));

	DX::ThrowIfFailed(device->CreateTexture2D(&ExponentiateDesc, nullptr, &ExponentiateTex[1]));
	DX::ThrowIfFailed(device->CreateRenderTargetView(ExponentiateTex[1], nullptr, &ExponentiateRTV[1]));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ExponentiateTex[1], nullptr, &ExponentiateSRV[1]));

	DX::ThrowIfFailed(device->CreateTexture2D(&MinifyDesc, nullptr, &MinifyTex));
	DX::ThrowIfFailed(device->CreateRenderTargetView(MinifyTex, nullptr, &MinifyRTV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(MinifyTex, nullptr, &MinifySRV));

	D3D11_TEXTURE2D_DESC FilterTexDesc{ ExponentiateDesc };
	FilterTexDesc.Width = (UINT)ESM_AtlasSize.x;
	FilterTexDesc.Height = (UINT)ESM_AtlasSize.y;

	D3D11_TEXTURE2D_DESC ESMTextureDesc{ FilterTexDesc };

	DX::ThrowIfFailed(device->CreateTexture2D(&FilterTexDesc, nullptr, &HorizontalTex));
	DX::ThrowIfFailed(device->CreateTexture2D(&ESMTextureDesc, nullptr, &ESMTexture));

	DX::ThrowIfFailed(device->CreateRenderTargetView(HorizontalTex, nullptr, &HorizontalRTV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(ESMTexture, nullptr, &ESM_RTV));

	DX::ThrowIfFailed(device->CreateShaderResourceView(HorizontalTex, nullptr, &HorizontalSRV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ESMTexture, nullptr, &ESM_SRV));

	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\STBN.dds", nullptr, &STBNoiseSRV));
	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\OrthogonalVolumetricLighting\\Textures\\VectorSTBN.dds", nullptr, &STBNoiseFloat3SRV));

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

	D3D11_UNORDERED_ACCESS_VIEW_DESC R16VolumeUAVdesc{};
	R16VolumeUAVdesc.Format = R16VolumeDesc.Format;
	R16VolumeUAVdesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	R16VolumeUAVdesc.Texture3D.MipSlice = 0;
	R16VolumeUAVdesc.Texture3D.FirstWSlice = 0;
	R16VolumeUAVdesc.Texture3D.WSize = R16VolumeDesc.Depth;

	D3D11_TEXTURE3D_DESC RGBA16VolumeDesc{ R16VolumeDesc };
	RGBA16VolumeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

	D3D11_UNORDERED_ACCESS_VIEW_DESC RGBA16VolumeUAVDesc{ R16VolumeUAVdesc };
	RGBA16VolumeUAVDesc.Format = RGBA16VolumeDesc.Format;

	D3D11_TEXTURE3D_DESC ScatterVolumeDesc{ R16VolumeDesc };
	ScatterVolumeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	ScatterVolumeDesc.Depth = (UINT)std::ceil(volumeDimensions.z / 2);

	D3D11_UNORDERED_ACCESS_VIEW_DESC ScatterVolumeUAVDesc{ R16VolumeUAVdesc };
	ScatterVolumeUAVDesc.Format = ScatterVolumeDesc.Format;
	ScatterVolumeUAVDesc.Texture3D.WSize = ScatterVolumeDesc.Depth;

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

	DX::ThrowIfFailed(device->CreateTexture3D(&RGBA16VolumeDesc, nullptr, &IntergrationVolume));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(IntergrationVolume, &RGBA16VolumeUAVDesc, &IntergrationVolumeUAV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(IntergrationVolume, nullptr, &IntergrationVolumeSRV));

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

	ShadowVolumeBuffer = new ConstantBuffer(ConstantBufferDesc<ShadowVolBuffer>());

	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());
	LFApply_func = reinterpret_cast<decltype(LFApply_func)>(REL::RelocationID(106995, 106995).address());

	renderdata = new Setup::LF_RenderData;

	//renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });
	//renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });

	//renderdata->SetupPass(Shaders::Minify,		 true, 1, { .uncond_pass = true });

	renderdata->SetupPass(Shaders::ShadowVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ScatterVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::FilterVolume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::IntergrationVolume, true, 1, { .uncond_pass = true });

	renderdata->SetupPass(Shaders::Apply, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::Output, true, 1, { .uncond_pass = true });

	renderdata->SetupRenderData();
}

//skyrim_SunPosition = reinterpret_cast<RE::NiPoint3*>(REL::RelocationID(527924, 414871).address());
//auto sunWSPos = *skyrim_SunPosition;
//float4 sunPosition = { sunWSPos.x, sunWSPos.y, sunWSPos.z, 1.0f };

void OrthogonalVolumetricLighting::SetupDownSampleExpo()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	ESMCBuffer->Update(UpdateESMBuffer(pass));

	static bool Parity = pass & 1;
	auto ExpoRTV = ExponentiateRTV[!Parity];

	//context->RSSetViewports(1, &viewPort[pass - 1]);
	context->RSSetViewports(1, &viewPort[0]);
	context->OMSetRenderTargets(1, &ExpoRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	auto buffer = ESMCBuffer->CB();
	context->PSSetConstantBuffers(1, 1, &buffer);
	//context->VSSetConstantBuffers(1, 1, &buffer);

	context->PSSetSamplers(10, 1, &LinearSampler);
	context->PSSetSamplers(11, 1, &PointSampler);
	context->PSSetSamplers(12, 1, &DepthSampler);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(DownSamplePS, NULL, NULL);

	auto shadowMap = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->PSSetShaderResources(0, 1, &shadowMap);

	pass++;

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupMinify()
{
	auto context = globals::d3d::context;

	ESMCBuffer->Update(UpdateESMBuffer(pass));

	context->RSSetViewports(1, &viewPort[pass - 1]);
	context->OMSetRenderTargets(1, &MinifyRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(MinifyPS, NULL, NULL);

	//context->PSSetShaderResources(0, 1, &ExponentiateSRV);

	pass++;

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupShadowVolume()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& terrain = globals::features::terrainShadows;
	static int passCount = 1;

	PrevMatrixIdx = UpdateMatrixCache();
	ShadowVolumeBuffer->Update(UpdateShadowBuffer());

	shadowVolParity = passCount & 1;
	auto volumeUAV = ShadowVolumeUAV[!shadowVolParity];
	auto prevVolumeSRV = ShadowVolumeSRV[shadowVolParity];

	context->CSSetUnorderedAccessViews(0, 1, &volumeUAV, nullptr);

	context->CSSetShader(GenerateShadowVolumeCS, nullptr, 0);
	auto buffer = ShadowVolumeBuffer->CB();
	ID3D11Buffer* prevFrameBuff = PrevFrameBuffer[PrevMatrixIdx].Get();
	ID3D11Buffer* FrameBuff = PrevFrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetConstantBuffers(1, 1, &FrameBuff);
	context->CSSetConstantBuffers(2, 1, &prevFrameBuff);

	auto TerrainHeightSRV = terrain.texHeightMap->srv.get();
	auto TerrainShadowSRV = terrain.texShadowHeight->srv.get();
	auto shadowBuff = globals::deferred->perShadow->srv.get();
	context->CSSetShaderResources(0, 1, &prevVolumeSRV);
	context->CSSetShaderResources(1, 1, &STBNoiseSRV);
	//context->CSSetShaderResources(2, 1, &MinifySRV);
	context->CSSetShaderResources(2, 2, ExponentiateSRV);  //////////
	context->CSSetShaderResources(3, 1, &TerrainHeightSRV);
	context->CSSetShaderResources(4, 1, &TerrainShadowSRV);
	context->CSSetShaderResources(5, 1, &shadowBuff);
	context->CSSetShaderResources(8, 1, &InvRepartitionSRV);
	context->CSSetShaderResources(9, 1, &RepartitionSRV);

	auto shadowMap = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	auto shadowMapVL = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(6, 1, &shadowMap);
	context->CSSetShaderResources(7, 1, &shadowMapVL);

	//context->Dispatch(40, 24, 23);
	context->Dispatch(dispatchNormal[0], dispatchNormal[1], dispatchNormal[2]);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	passCount++;
	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupScatteringVolume()
{
	auto context = globals::d3d::context;

	context->CSSetUnorderedAccessViews(0, 1, &ScatteringVolumeUAV, nullptr);

	context->CSSetShader(GenerateScatteringVolumeCS, nullptr, 0);

	auto buffer = ShadowVolumeBuffer->CB();
	ID3D11Buffer* FrameBuff = PrevFrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetConstantBuffers(1, 1, &FrameBuff);

	context->CSSetShaderResources(0, 1, &ShadowVolumeSRV[!shadowVolParity]);
	context->CSSetShaderResources(1, 1, &InvRepartitionSRV);
	context->CSSetShaderResources(2, 1, &STBNoiseSRV);

	//context->Dispatch(40, 24, 23);
	context->Dispatch(dispatchScatter[0], dispatchScatter[1], dispatchScatter[2]);

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

	auto buffer = ShadowVolumeBuffer->CB();
	ID3D11Buffer* FrameBuff = PrevFrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	ID3D11Buffer* prevFrameBuff = PrevFrameBuffer[PrevMatrixIdx].Get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetConstantBuffers(1, 1, &FrameBuff);
	context->CSSetConstantBuffers(2, 1, &prevFrameBuff);

	context->CSSetShaderResources(0, 1, &prevVolumeSRV);
	context->CSSetShaderResources(1, 1, &InvRepartitionSRV);
	context->CSSetShaderResources(2, 1, &RepartitionSRV);
	context->CSSetShaderResources(3, 1, &STBNoiseSRV);
	context->CSSetShaderResources(4, 1, &ScatteringVolumeSRV);

	//context->Dispatch(40, 24, 23);
	context->Dispatch(dispatchNormal[0], dispatchNormal[1], dispatchNormal[2]);

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

	auto buffer = ShadowVolumeBuffer->CB();
	ID3D11Buffer* FrameBuff = PrevFrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetConstantBuffers(1, 1, &FrameBuff);

	//context->CSSetShaderResources(0, 1, &ScatteringVolumeSRV);
	context->CSSetShaderResources(0, 1, &FilterVolumeSRV[!FilterVolParity]);
	context->CSSetShaderResources(1, 1, &InvRepartitionSRV);

	//context->Dispatch(40, 24, 1);
	context->Dispatch(dispatchMarch[0], dispatchMarch[1], dispatchMarch[2]);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupApplyVolume()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(1, &OutputRTV, nullptr);

	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(ApplyVolumePS, NULL, NULL);

	auto buffer = ShadowVolumeBuffer->CB();
	ID3D11Buffer* FrameBuff = PrevFrameBuffer[(PrevMatrixIdx == 0) ? 1 : 0].Get();
	context->PSSetConstantBuffers(0, 1, &buffer);
	context->PSSetConstantBuffers(1, 1, &FrameBuff);

	auto& mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN].depthSRV;
	context->PSSetShaderResources(0, 1, &IntergrationVolumeSRV);
	context->PSSetShaderResources(1, 1, &mainDepthSRV);
	context->PSSetShaderResources(2, 1, &RepartitionSRV);
	context->PSSetShaderResources(3, 1, &STBNoiseSRV);
	context->CSSetShaderResources(4, 1, &STBNoiseFloat3SRV);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::SetupOutput()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	auto& mainRTV = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].RTV;

	context->OMSetRenderTargets(1, &mainRTV, nullptr);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(OutputPS, NULL, NULL);

	context->PSSetShaderResources(0, 1, &OutputSRV);
	context->PSSetShaderResources(1, 1, &ScatteringVolumeSRV);
	context->PSSetShaderResources(2, 1, &FilterVolumeSRV[!FilterVolParity]);
	context->PSSetShaderResources(3, 1, &IntergrationVolumeSRV);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	overrideShader = false;
}

void OrthogonalVolumetricLighting::CheckOverride()
{
	static Util::FrameChecker frame_checker;

	if (overrideCalled) {
		if (overrideNum == 1) {
			globals::d3d::context->PSGetShaderResources(3, 1, &RepartitionSRV);
			overrideNum = 0;
		} else {
			globals::d3d::context->CSGetShaderResources(2, 1, &InvRepartitionSRV);
		}
		overrideCalled = false;
	}

	if (overrideShader) {
		if (frame_checker.IsNewFrame()) {
			UpdateFrustum();

			pass = 1;
			float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			//globals::d3d::context->ClearRenderTargetView(ExponentiateRTV, clear);
			//globals::d3d::context->ClearRenderTargetView(OutputRTV, clear);
		}
		LookupShader(shaderdesc);
	}
}

void OrthogonalVolumetricLighting::LookupShader(int desc)
{
	static const std::unordered_map<int, void (OrthogonalVolumetricLighting::*)()> effects{
		{ Shaders::ExpDownSample, &OrthogonalVolumetricLighting::SetupDownSampleExpo },
		{ Shaders::Minify, &OrthogonalVolumetricLighting::SetupMinify },
		{ Shaders::ShadowVolume, &OrthogonalVolumetricLighting::SetupShadowVolume },
		{ Shaders::ScatterVolume, &OrthogonalVolumetricLighting::SetupScatteringVolume },
		{ Shaders::FilterVolume, &OrthogonalVolumetricLighting::SetupFilterPass },
		{ Shaders::IntergrationVolume, &OrthogonalVolumetricLighting::SetupSliceMarch },
		{ Shaders::Apply, &OrthogonalVolumetricLighting::SetupApplyVolume },
		{ Shaders::Output, &OrthogonalVolumetricLighting::SetupOutput },

		{ Shaders::HorFilter, &OrthogonalVolumetricLighting::SetupHorizontalFilter },
		{ Shaders::VerFilter, &OrthogonalVolumetricLighting::SetupVerticalFilter },
	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

void OrthogonalVolumetricLighting::UpdateFrustum()
{
	auto tmp = Util::GetAverageEyePosition();
	cameraPosition = float4(tmp.x, tmp.y, tmp.z, 1.0f);

	float FOVy = Util::GetVerticalFOVRad();
	float aspect = screenSize.x / screenSize.y;

	Matrix projMat = Matrix(DirectX::XMMatrixPerspectiveFovLH(FOVy, aspect, FrustumNearFar.x, FrustumNearFar.y));
	float invPx = 1.0f / projMat.m[0][0];
	float invPy = 1.0f / projMat.m[1][1];

	Matrix invViewMat = Util::GetCameraData(0).viewMat.Invert();

	Matrix ViewFromUVZ(
		2 * invPx, 0, 0, 0,
		0, -2 * invPy, 0, 0,
		-invPx, +invPy, 1, 0,
		0, 0, 0, 1);

	WorldFromUVZ = ViewFromUVZ * invViewMat;

	//Matrix WorldFromUVZ_GPU = WorldFromUVZ.Transpose();
}

int OrthogonalVolumetricLighting::UpdateMatrixCache()
{
	int outValue = 0;
	if (CheckFrameBuffer()) {
		auto buffer = *globals::game::perFrame.get();
		static int PrevFrameWrite = 0;
		static bool PrevFrameValid = false;

		if (!PrevFrameValid) {
			globals::d3d::context->CopyResource(PrevFrameBuffer[0].Get(), buffer);
			globals::d3d::context->CopyResource(PrevFrameBuffer[1].Get(), buffer);
			PrevFrameValid = true;
			PrevFrameWrite = 0;
		}

		outValue = PrevFrameWrite ^ 1;
		globals::d3d::context->CopyResource(PrevFrameBuffer[PrevFrameWrite].Get(), buffer);
		PrevCameraData[PrevFrameWrite] = Util::GetCameraData();
		PrevFrameWrite ^= 1;
	}

	return outValue;
}

bool OrthogonalVolumetricLighting::CheckFrameBuffer()
{
	if (ID3D11Buffer* buffer = *globals::game::perFrame.get()) {
		if (!PrevFrameBuffer[0] || !PrevFrameBuffer[1]) {
			D3D11_BUFFER_DESC srcDesc{};
			buffer->GetDesc(&srcDesc);
			D3D11_BUFFER_DESC dstDesc = srcDesc;
			dstDesc.Usage = D3D11_USAGE_DEFAULT;
			dstDesc.CPUAccessFlags = 0;
			dstDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, PrevFrameBuffer[0].GetAddressOf()));
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, PrevFrameBuffer[1].GetAddressOf()));
		}

		if (PrevFrameBuffer[0] || PrevFrameBuffer[1]) {
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

OrthogonalVolumetricLighting::ESMBuffer OrthogonalVolumetricLighting::UpdateESMBuffer(uint passn)
{
	ESMBuffer data{};
	data.slice = passn - 1;
	data.KernalWidth = 4;
	data.ESM_EXP = ESM_EXP;
	data.ESM_Scale = ESM_Scale;

	if (passn < 3) {
		data.srcSize = CSM_Size;
		data.InvSrcSize = float2(1.0f / CSM_Size.x, 1.0f / CSM_Size.y);
		data.dstSize = float2(DownSampleExpo_TileSize, DownSampleExpo_TileSize);
	} else {
		data.srcSize = DownSampleExpo_AtlasSize;
		data.InvSrcSize = float2(1.0f / DownSampleExpo_AtlasSize.x, 1.0f / DownSampleExpo_AtlasSize.y);
		data.dstSize = float2(ESM_AtlasSize.x, ESM_AtlasSize.y);
	}

	return data;
}

OrthogonalVolumetricLighting::ShadowVolBuffer OrthogonalVolumetricLighting::UpdateShadowBuffer()
{
	ShadowVolBuffer data{};
	data.frustum = WorldFromUVZ.Transpose();
	data.FrustumNearFar = FrustumNearFar;
	data.CameraPosition = cameraPosition;
	data.VolumeSize = volumeDimensions;
	data.NoiseSize = noiseDimensions;
	data.PrevCameraData = PrevCameraData[PrevMatrixIdx];
	data.ShadowAtlasSize = ESM_AtlasSize;
	data.CellJitterValue = CellJitterValue;
	data.RayJitterValue = RayJitterValue;
	data.ESM_Scale = ESM_Scale;
	data.ESM_EXP = ESM_EXP;
	return data;
}

void OrthogonalVolumetricLighting::Override()
{
	if (renderdata && LFApply_func && BGSCamera && BGSShader) {
		logger::info("Call flare func");
		LFApply_func(BGSCamera, BGSShader, (uint64_t)0);
	}
}

void OrthogonalVolumetricLighting::SetupVerticalFilter()
{
}

void OrthogonalVolumetricLighting::SetupHorizontalFilter()
{
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

	if (!lens.BGSCamera || !lens.BGSShader) {
		lens.BGSCamera = camera;
		lens.BGSShader = shader;
	}

	func(camera, shader);  //set skyrim_RunFlarePtr

	bool sunVisble = static_cast<bool>(*lens.skyrim_RunFlarePtr);

	if (*lens.skyrim_FlareData) {
		lens.renderdata->ResetEffects(sunVisble);
		*lens.skyrim_RunFlarePtr = 1;
	} else
		*lens.skyrim_RunFlarePtr = 0;

	//*lens.skyrim_RunFlarePtr = 0; ////////
}

void OrthogonalVolumetricLighting::Hooks::BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>::thunk(void* shader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& lens = globals::features::orthogonalVolumetricLighting;

	lens.overrideShader = true;
	lens.shaderdesc = lens.renderdata->UpdateCurrentEffect();

	func(shader, shape, param);
}

void OrthogonalVolumetricLighting::DrawSettings()
{
	ImGui::SeparatorText("Reload");  ///////////////
	ImGui::Button("Reload Flare");
	if (ImGui::IsItemClicked()) {
		DownSamplePS = nullptr;
		MinifyPS = nullptr;
		FilterPS = nullptr;
		GenerateShadowVolumeCS = nullptr;
		GenerateScatteringVolumeCS = nullptr;
		SliceMarchCS = nullptr;
		ApplyVolumePS = nullptr;
		OutputPS = nullptr;

		CompileShaders();
	}

	ImGui::SliderInt("ESM Exponent: ", (int*)&ESM_EXP, 20, 250);
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
