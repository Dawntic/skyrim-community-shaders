#include "LensEffects.h"
#include "../Deferred.h"
#include "../Upscaling.h"
#include "State.h"
#include "TerrainShadows.h"
#include "Util.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <REX/W32/COMPTR.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LensEffects::Settings, test)

void LensEffects::CompileShaders()
{
	BypassVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\LensEffects.hlsl", { { "BYPASS_VSSHADER", "" } }, "vs_5_0");
	DownSamplePS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\LensEffects.hlsl", { { "DownSample", "" } }, "ps_5_0");
	MinifyPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\LensEffects.hlsl", { { "Minify", "" } }, "ps_5_0");
	FilterPS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\LensEffects.hlsl", { { "Filter", "" } }, "ps_5_0");
	GenerateShadowVolumeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\ShadowVolume.hlsl", { { "ShadowVolumeCompute", "" } }, "cs_5_0");
	ApplyVolumePS = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\LensEffects\\VolumetricApply.hlsl", { { "ApplyVolume", "" } }, "ps_5_0");
}

void LensEffects::SetupResources()
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
	ExponentiateDesc.Height = (UINT)DownSampleExpo_AtlasSize.y;
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

	DX::ThrowIfFailed(device->CreateTexture2D(&ExponentiateDesc, nullptr, &ExponentiateTex));
	DX::ThrowIfFailed(device->CreateTexture2D(&MinifyDesc, nullptr, &MinifyTex));

	DX::ThrowIfFailed(device->CreateRenderTargetView(ExponentiateTex, nullptr, &ExponentiateRTV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(MinifyTex, nullptr, &MinifyRTV));

	DX::ThrowIfFailed(device->CreateShaderResourceView(ExponentiateTex, nullptr, &ExponentiateSRV));
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

	DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, L"Data\\Shaders\\LensEffects\\Textures\\STBN.dds", nullptr, &STBNoiseSRV));

	D3D11_TEXTURE3D_DESC volumeDesc{};
	volumeDesc.Width = (UINT)volumeDimensions.x;
	volumeDesc.Height = (UINT)volumeDimensions.y;
	volumeDesc.Depth = (UINT)volumeDimensions.z;
	volumeDesc.MipLevels = 1;
	volumeDesc.Format = DXGI_FORMAT_R16_FLOAT;
	volumeDesc.Usage = D3D11_USAGE_DEFAULT;
	volumeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	volumeDesc.CPUAccessFlags = 0;
	volumeDesc.MiscFlags = 0;

	D3D11_UNORDERED_ACCESS_VIEW_DESC volumeUAVdesc{};
	volumeUAVdesc.Format = volumeDesc.Format;
	volumeUAVdesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	volumeUAVdesc.Texture3D.MipSlice = 0;
	volumeUAVdesc.Texture3D.FirstWSlice = 0;
	volumeUAVdesc.Texture3D.WSize = volumeDesc.Depth;

	D3D11_TEXTURE3D_DESC volumePrevDesc{ volumeDesc };
	D3D11_UNORDERED_ACCESS_VIEW_DESC volumePrevUAVdesc{ volumeUAVdesc };

	DX::ThrowIfFailed(device->CreateTexture3D(&volumeDesc, nullptr, &ShadowVolume));
	DX::ThrowIfFailed(device->CreateTexture3D(&volumePrevDesc, nullptr, &PrevShadowVolume));

	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVolume, &volumeUAVdesc, &ShadowVolumeUAV));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(PrevShadowVolume, &volumePrevUAVdesc, &PrevShadowVolumeUAV));

	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVolume, nullptr, &ShadowVolumeSRV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(PrevShadowVolume, nullptr, &PrevShadowVolumeSRV));

	ShadowVolumeBuffer = new ConstantBuffer(ConstantBufferDesc<ShadowVolBuffer>());

	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());
	LFApply_func = reinterpret_cast<decltype(LFApply_func)>(REL::RelocationID(106995, 106995).address());

	renderdata = new Setup::LF_RenderData;

	renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });

	renderdata->SetupPass(Shaders::Minify, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::Volume, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::Apply, true, 1, { .uncond_pass = true });

	//renderdata->SetupPass(Shaders::HorFilter, settings->EnableCA, 1, { .uncond_pass = true });
	//renderdata->SetupPass(Shaders::VerFilter, settings->EnableIce, 1, { .uncond_pass = true });

	renderdata->SetupRenderData();
}

void LensEffects::SetupDownSampleExpo()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	//logger::info("pass: {}", pass);

	ESMCBuffer->Update(UpdateESMBuffer(pass));

	context->RSSetViewports(1, &viewPort[pass - 1]);
	context->OMSetRenderTargets(1, &ExponentiateRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	auto buffer = ESMCBuffer->CB();
	context->PSSetConstantBuffers(1, 1, &buffer);
	context->VSSetConstantBuffers(1, 1, &buffer);

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

void LensEffects::SetupMinify()
{
	auto context = globals::d3d::context;

	ESMCBuffer->Update(UpdateESMBuffer(pass));

	context->RSSetViewports(1, &viewPort[pass - 1]);
	context->OMSetRenderTargets(1, &MinifyRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(MinifyPS, NULL, NULL);

	context->PSSetShaderResources(0, 1, &ExponentiateSRV);

	pass++;

	overrideShader = false;
}

void LensEffects::SetupShadowVolume()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& terrain = globals::features::terrainShadows;

	PrevMatrixIdx = UpdateMatrixCache();
	ShadowVolumeBuffer->Update(UpdateShadowBuffer());

	context->CSSetUnorderedAccessViews(0, 1, &ShadowVolumeUAV, nullptr);

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
	context->CSSetShaderResources(0, 1, &PrevShadowVolumeSRV);
	context->CSSetShaderResources(1, 1, &STBNoiseSRV);
	context->CSSetShaderResources(2, 1, &MinifySRV);
	context->CSSetShaderResources(3, 1, &TerrainHeightSRV);
	context->CSSetShaderResources(4, 1, &TerrainShadowSRV);
	context->CSSetShaderResources(5, 1, &shadowBuff);

	auto shadowMap = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	auto shadowMapVL = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].depthSRV;
	context->CSSetShaderResources(6, 1, &shadowMap);
	context->CSSetShaderResources(7, 1, &shadowMapVL);

	context->Dispatch(20, 11, 16);

	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

	overrideShader = false;
}

void LensEffects::SetupApplyVolume()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto buffer = ShadowVolumeBuffer->CB();

	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	//auto& mainTexCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	//context->CopyResource(mainTexCopy.textureCopy, mainTex.texture);

	auto& mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY].depthSRV;

	context->RSSetViewports(1, &viewPort[3]);
	context->OMSetRenderTargets(1, &mainTex.RTV, nullptr);

	float blendFactor[4] = { 0, 0, 0, 0 };
	context->OMSetBlendState(AddBlend, blendFactor, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(ApplyVolumePS, NULL, NULL);

	context->PSSetConstantBuffers(0, 1, &buffer);

	//auto& mainTexCopySRV = mainTexCopy.SRV;
	context->PSSetShaderResources(0, 1, &ShadowVolumeSRV);
	//context->PSSetShaderResources(1, 1, &mainTexCopySRV);
	context->PSSetShaderResources(2, 1, &mainDepthSRV);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	overrideShader = false;
}

void LensEffects::CheckOverride()
{
	static Util::FrameChecker frame_checker;

	if (overrideShader) {
		if (frame_checker.IsNewFrame()) {
			pass = 1;
			FrameIdx++;

			float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			globals::d3d::context->ClearRenderTargetView(ExponentiateRTV, clear);
		}
		LookupShader(shaderdesc);
	}
}

void LensEffects::LookupShader(int desc)
{
	static const std::unordered_map<int, void (LensEffects::*)()> effects{
		{ Shaders::ExpDownSample, &LensEffects::SetupDownSampleExpo },
		{ Shaders::Minify, &LensEffects::SetupMinify },
		{ Shaders::HorFilter, &LensEffects::SetupHorizontalFilter },
		{ Shaders::VerFilter, &LensEffects::SetupVerticalFilter },
		{ Shaders::Volume, &LensEffects::SetupShadowVolume },
		{ Shaders::Apply, &LensEffects::SetupApplyVolume }

	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

int LensEffects::UpdateMatrixCache()
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

bool LensEffects::CheckFrameBuffer()
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

LensEffects::ESMBuffer LensEffects::UpdateESMBuffer(uint passn)
{
	ESMBuffer data{};
	data.slice = passn - 1;
	data.KernalWidth = 4;
	data.ESM_EXP = ESM_EXP;
	data.ESM_Scale = ESM_Scale;
	//data.filterDir = float2(0.0f, 0.0f);

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

LensEffects::ShadowVolBuffer LensEffects::UpdateShadowBuffer()
{
	ShadowVolBuffer data{};
	data.VolumeSize = volumeDimensions;
	data.NoiseSize = noiseDimensions;
	data.PrevCameraData = PrevCameraData[PrevMatrixIdx];
	data.ShadowAtlasSize = ESM_AtlasSize;
	data.CellJitterValue = CellJitterValue;
	data.RayJitterValue = RayJitterValue;
	data.Frame = FrameIdx;
	data.ESM_Scale = ESM_Scale;
	data.ESM_EXP = ESM_EXP;
	//data.ShadowAtlasBorderPx;
	//data.AmbientTerm;
	return data;
}

void LensEffects::Override()
{
	if (renderdata && LFApply_func && BGSCamera && BGSShader) {
		logger::info("Call flare func");
		LFApply_func(BGSCamera, BGSShader, (uint64_t)0);
	}
}

void LensEffects::SetupVerticalFilter()
{
}

void LensEffects::SetupHorizontalFilter()
{
}

void LensEffects::Hooks::LensFlare_CheckResources::thunk()
{
	auto& lens = globals::features::lensEffects;

	lens.renderdata->CheckRefData();

	if (!*lens.skyrim_FlareData)
		*lens.skyrim_FlareData = reinterpret_cast<uintptr_t>(lens.renderdata);
}

void LensEffects::Hooks::LensFlareVisibility_CheckRenderCondition::thunk(RE::NiCamera* camera, void* shader)
{
	auto& lens = globals::features::lensEffects;

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

void LensEffects::Hooks::BSImagespaceShader_Render<RE::ImageSpaceManager::ISLensFlare>::thunk(void* shader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	auto& lens = globals::features::lensEffects;

	lens.overrideShader = true;
	lens.shaderdesc = lens.renderdata->UpdateCurrentEffect();

	func(shader, shape, param);
}

void LensEffects::DrawSettings()
{
	ImGui::SeparatorText("Reload");  ///////////////
	ImGui::Button("Reload Flare");
	if (ImGui::IsItemClicked()) {
		DownSamplePS = nullptr;
		MinifyPS = nullptr;
		FilterPS = nullptr;
		GenerateShadowVolumeCS = nullptr;
		ApplyVolumePS = nullptr;

		CompileShaders();
	}
}

void LensEffects::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LensEffects::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LensEffects::RestoreDefaultSettings()
{
	settings = {};
}
