#include "LensEffects.h"
#include "../Upscaling.h"
#include "State.h"
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

	ESMCBuffer = new ConstantBuffer(ConstantBufferDesc<ESMBuffer>());

	CompileShaders();

	viewPort.TopLeftX = 0.0f;
	viewPort.TopLeftY = 0.0f;
	viewPort.Width = 0;
	viewPort.Height = 0;
	viewPort.MinDepth = 0.0f;
	viewPort.MaxDepth = 1.0f;

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
	DX::ThrowIfFailed(device->CreateTexture3D(&volumePrevDesc, nullptr, &ShadowVolumePrev));

	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVolume, &volumeUAVdesc, &ShadowVolumeUAV));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(ShadowVolumePrev, &volumePrevUAVdesc, &ShadowVolumePrevUAV));

	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVolume, nullptr, &ShadowVolumeSRV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(ShadowVolumePrev, nullptr, &ShadowVolumePrevSRV));

	ShadowVolBuffer = new ConstantBuffer(ConstantBufferDesc<ShadowVolumeBuffer>());

	skyrim_FlareData = reinterpret_cast<uintptr_t*>(REL::RelocationID(527915, 414867).address());
	skyrim_RunFlarePtr = reinterpret_cast<uint32_t*>(REL::RelocationID(527916, 414862).address());
	LFApply_func = reinterpret_cast<decltype(LFApply_func)>(REL::RelocationID(106995, 106995).address());

	renderdata = new Setup::LF_RenderData;

	renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });
	renderdata->SetupPass(Shaders::ExpDownSample, true, 1, { .uncond_pass = true });

	renderdata->SetupPass(Shaders::Minify, true, 1, { .uncond_pass = true });

	//renderdata->SetupPass(Shaders::HorFilter, settings->EnableCA, 1, { .uncond_pass = true });
	//renderdata->SetupPass(Shaders::VerFilter, settings->EnableIce, 1, { .uncond_pass = true });

	renderdata->SetupRenderData();
}

void LensEffects::SetupDownSampleExpo()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto buffer = ESMCBuffer->CB();

	ESMBuffer data = UpdateESMBuffer();  //better way to update
	data.slice = slice;
	data.KernalWidth = 4;
	data.srcSize = CSM_Size;
	data.InvSrcSize = float2(1.0f / CSM_Size.x, 1.0f / CSM_Size.y);
	data.dstSize = float2(DownSampleExpo_TileSize, DownSampleExpo_TileSize);
	data.filterDir = float2(0.0f, 0.0f);
	data.ESM_EXP = ESM_EXP;
	data.ESM_Scale = ESM_Scale;  //missing
	ESMCBuffer->Update(data);

	viewPort.TopLeftX = 0.0f;
	viewPort.TopLeftY = float(slice * DownSampleExpo_TileSize);
	viewPort.Width = DownSampleExpo_TileSize;
	viewPort.Height = DownSampleExpo_TileSize;
	context->RSSetViewports(1, &viewPort);

	context->OMSetRenderTargets(1, &ExponentiateRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->PSSetConstantBuffers(1, 1, &buffer);
	context->VSSetConstantBuffers(1, 1, &buffer);

	ID3D11Buffer* buffer2 = PrevFrameBuffer[PrevMatrixIdx].Get();
	context->PSSetConstantBuffers(2, 1, &buffer2);
	context->VSSetConstantBuffers(2, 1, &buffer2);

	context->PSSetSamplers(10, 1, &LinearSampler);
	context->PSSetSamplers(11, 1, &PointSampler);
	context->PSSetSamplers(12, 1, &DepthSampler);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(DownSamplePS, NULL, NULL);

	auto shadowMap = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].depthSRV;
	context->PSSetShaderResources(0, 1, &shadowMap);

	slice++;
	if (slice == 2)
		slice = 0;

	overrideShader = false;
}

void LensEffects::SetupMinify()
{
	auto context = globals::d3d::context;

	ESMBuffer data = UpdateESMBuffer();
	data.slice = slice;
	data.KernalWidth = 4;
	data.srcSize = DownSampleExpo_AtlasSize;
	data.InvSrcSize = float2(1.0f / DownSampleExpo_AtlasSize.x, 1.0f / DownSampleExpo_AtlasSize.y);
	data.dstSize = float2(ESM_AtlasSize.x, ESM_AtlasSize.y);
	data.filterDir = float2(0.0f, 0.0f);
	ESMCBuffer->Update(data);

	viewPort.TopLeftX = 0.0f;
	viewPort.TopLeftY = 0.0f;
	viewPort.Width = ESM_AtlasSize.x;
	viewPort.Height = ESM_AtlasSize.y;
	context->RSSetViewports(1, &viewPort);

	context->OMSetRenderTargets(1, &MinifyRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(MinifyPS, NULL, NULL);

	context->PSSetShaderResources(0, 1, &ExponentiateSRV);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	overrideShader = false;
}

void LensEffects::GetLightMatrix()
{
	auto shaderManager = globals::game::smState;

	if (shaderManager && shaderManager->shadowSceneNode[0]) {
		logger::info("Scene node");
		if (auto runtime = &shaderManager->shadowSceneNode[0]->GetRuntimeData()) {
			logger::info("Runtime");
			if (auto bsLight = runtime->sunLight) {
				logger::info("Got Light");
				if (bsLight->IsShadowLight()) {
					logger::info("Is Shadow Light");
					if (auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight)) {
						logger::info("Got Shadow Light");
						lightMatrix[0] = shadowLight->GetRuntimeData().shadowmapDescriptors[0].lightTransform;
						lightMatrix[1] = shadowLight->GetRuntimeData().shadowmapDescriptors[1].lightTransform;
					}
				}
			}
		}
	}
}

int LensEffects::UpdateMatrixCache()
{
	int outValue;
	if (!PrevFrameBuffer[0] || !PrevFrameBuffer[1]) {
		if (ID3D11Buffer* buffer = *globals::game::perFrame.get()) {
			logger::info("Buffer");
			D3D11_BUFFER_DESC srcDesc{};
			buffer->GetDesc(&srcDesc);

			D3D11_BUFFER_DESC dstDesc = srcDesc;
			dstDesc.Usage = D3D11_USAGE_DEFAULT;
			dstDesc.CPUAccessFlags = 0;
			dstDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, PrevFrameBuffer[0].GetAddressOf()));
			DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&dstDesc, nullptr, PrevFrameBuffer[1].GetAddressOf()));
		} else {
			logger::info("No Buffer");
			return;
		}
	}

	if (ID3D11Buffer* buffer = *globals::game::perFrame.get()) {
		if (!PrevFrameBuffer[0] || !PrevFrameBuffer[1]) {
			logger::info("Buffer Error");
			return;
		}

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
		PrevFrameWrite ^= 1;
		logger::info("Copy Buffer");
	}
	return outValue;
}

void LensEffects::CheckOverride()
{
	static Util::FrameChecker frame_checker;

	if (overrideShader) {
		if (frame_checker.IsNewFrame()) {
			PrevMatrixIdx = UpdateMatrixCache();
			GetLightMatrix();

			//ESMCBuffer->Update(UpdateESMBuffer());
			//ShadowVolBuffer->Update(UpdateShadowBuffer());

			FrameIdx++;
			slice = 0;
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
		{ Shaders::VerFilter, &LensEffects::SetupVerticalFilter }

	};
	auto it = effects.find(desc);
	if (it != effects.cend())
		(this->*(it->second))();
}

LensEffects::ESMBuffer LensEffects::UpdateESMBuffer()
{
	ESMBuffer data{};
	return data;
}

LensEffects::ShadowVolumeBuffer LensEffects::UpdateShadowBuffer()
{
	ShadowVolumeBuffer data{};
	data.lightMat[0] = lightMatrix[0];
	data.lightMat[1] = lightMatrix[1];
	data.ShadowAtlasSize = ESM_AtlasSize;
	data.ShadowAtlasBorderPx;
	//data.AmbientTerm;
	data.CellJitterValue;
	data.RayJitterValue;
	data.Frame = FrameIdx;
	data.ESM_Scale = ESM_Scale;
	data.ESM_EXP = ESM_EXP;
	return data;
}

void LensEffects::Override()
{
	if (renderdata && LFApply_func && BGSCamera && BGSShader) {
		logger::info("Call flare func");
		LFApply_func(BGSCamera, BGSShader, (uint64_t)0);
	}
}

void LensEffects::SetupHorizontalFilter()
{
	auto context = globals::d3d::context;

	ESMBuffer data = UpdateESMBuffer();
	data.slice = slice;
	data.KernalWidth = 11;
	data.srcSize = ESM_AtlasSize;
	data.InvSrcSize = float2(1.0f / ESM_AtlasSize.x, 1.0f / ESM_AtlasSize.y);
	data.dstSize = float2(ESM_AtlasSize.x, ESM_AtlasSize.y);
	data.filterDir = float2(1.0f, 0.0f);
	ESMCBuffer->Update(data);

	viewPort.TopLeftX = 0.0f;
	viewPort.TopLeftY = 0.0f;
	viewPort.Width = ESM_AtlasSize.x;
	viewPort.Height = ESM_AtlasSize.y;
	context->RSSetViewports(1, &viewPort);

	context->OMSetRenderTargets(1, &HorizontalRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(FilterPS, NULL, NULL);

	context->PSSetShaderResources(0, 1, &MinifySRV);

	overrideShader = false;
}

void LensEffects::SetupVerticalFilter()
{
	auto context = globals::d3d::context;

	slice++;
	if (slice == 2)
		slice = 0;

	ESMBuffer data = UpdateESMBuffer();
	data.slice = slice;
	data.KernalWidth = 11;
	data.srcSize = ESM_AtlasSize;
	data.InvSrcSize = float2(1.0f / ESM_AtlasSize.x, 1.0f / ESM_AtlasSize.y);
	data.dstSize = float2(ESM_AtlasSize.x, ESM_AtlasSize.y);
	data.filterDir = float2(0.0f, 1.0f);
	ESMCBuffer->Update(data);

	viewPort.TopLeftX = 0.0f;
	viewPort.TopLeftY = 0.0f;
	viewPort.Width = ESM_AtlasSize.x;
	viewPort.Height = ESM_AtlasSize.y;
	context->RSSetViewports(1, &viewPort);

	context->OMSetRenderTargets(1, &ESM_RTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	context->VSSetShader(BypassVertexShader, NULL, NULL);
	context->PSSetShader(FilterPS, NULL, NULL);

	context->PSSetShaderResources(0, 1, &HorizontalSRV);

	overrideShader = false;
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
