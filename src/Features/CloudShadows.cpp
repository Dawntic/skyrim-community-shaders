#include "CloudShadows.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "../I18n/I18n.h"
#include "PhysicalSky.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.cloud_shadows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	Opacity,
	EnableVolumetricShadowMap,
	VolumetricResolution,
	VolumetricRangeKm,
	VolumetricSteps,
	VolumetricMipBias)

namespace
{
	constexpr uint32_t kResolutionChoices[] = { 256u, 512u, 1024u, 2048u };

	uint32_t SanitizeResolution(uint32_t requested)
	{
		for (uint32_t choice : kResolutionChoices)
			if (choice == requested)
				return choice;
		return 512u;
	}
}

void CloudShadows::DrawSettings()
{
	ImGui::SliderFloat(T(TKEY("opacity"), "Opacity"), &settings.Opacity, 0.0f, 4.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("opacity_tooltip"),
							  "Higher values make cloud shadows darker."));
	}

	ImGui::SeparatorText(T(TKEY("volumetric_header"), "Volumetric Clouds (Physical Sky)"));

	ImGui::Checkbox(T(TKEY("volumetric_enabled"), "Volumetric Shadow Map"), &settings.EnableVolumetricShadowMap);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_enabled_tooltip"),
							  "Casts shadows from Physical Sky's volumetric clouds by integrating "
							  "their density along the light direction (Beer-Lambert), instead of "
							  "reading the vanilla cloud planes.\n\n"
							  "Requires Physical Sky to be active; falls back to the vanilla cubemap "
							  "otherwise."));
	}

	if (!globals::features::physicalSky.loaded) {
		ImGui::TextDisabled("%s", T(TKEY("volumetric_requires_physical_sky"),
									  "Physical Sky is not loaded."));
		return;
	}

	ImGui::BeginDisabled(!settings.EnableVolumetricShadowMap);

	int resolutionIndex = 1;
	for (int i = 0; i < static_cast<int>(std::size(kResolutionChoices)); ++i)
		if (kResolutionChoices[i] == settings.VolumetricResolution)
			resolutionIndex = i;

	static const char* const resolutionLabels[] = { "256", "512", "1024", "2048" };
	if (ImGui::Combo(T(TKEY("volumetric_resolution"), "Resolution"), &resolutionIndex,
			resolutionLabels, static_cast<int>(std::size(resolutionLabels)))) {
		settings.VolumetricResolution = kResolutionChoices[resolutionIndex];
		CreateVolumetricResources();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_resolution_tooltip"),
							  "Shadow map edge length in texels. Cost scales with the square of this "
							  "value; the map is blurred and mip-mapped, so higher resolutions buy "
							  "sharper cloud edges rather than less aliasing."));
	}

	ImGui::SliderFloat(T(TKEY("volumetric_range"), "Range (km)"), &settings.VolumetricRangeKm, 5.0f, 60.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_range_tooltip"),
							  "Half-size of the square the shadow map covers around the camera. "
							  "Shadows fade out past this distance, so it should roughly match the "
							  "cloud tracing distance in Physical Sky."));
	}

	int steps = static_cast<int>(settings.VolumetricSteps);
	if (ImGui::SliderInt(T(TKEY("volumetric_steps"), "Steps"), &steps, 8, 64))
		settings.VolumetricSteps = static_cast<uint32_t>(steps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_steps_tooltip"),
							  "Raymarch samples taken through the cloud layer per texel. Too few "
							  "makes thin clouds flicker as they drift between samples."));
	}

	ImGui::SliderFloat(T(TKEY("volumetric_softness"), "Softness"), &settings.VolumetricMipBias, 0.0f, 4.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("volumetric_softness_tooltip"),
							  "Extra mip bias on top of the distance-derived level. Raise it if "
							  "shadow edges still sparkle in motion; lower it for crisper shadows."));
	}

	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::Text("%s", volumetricActive ?
						  T(TKEY("volumetric_status_active"), "Status: active") :
						  T(TKEY("volumetric_status_inactive"), "Status: inactive (using vanilla cloud planes)"));

	if (texVolumetricShadow) {
		static float debugScale = 0.4f;
		ImGui::SliderFloat(T(TKEY("volumetric_view_scale"), "View Scale"), &debugScale, 0.1f, 1.0f);
		BUFFER_VIEWER_NODE_BULLET(texVolumetricShadow, debugScale);
	}
}

#undef I18N_KEY_PREFIX

void CloudShadows::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.VolumetricResolution = SanitizeResolution(settings.VolumetricResolution);
}

void CloudShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
	CreateVolumetricResources();
}

CloudShadows::BufferData CloudShadows::GetCommonBufferData()
{
	UpdateVolumetricParams();

	BufferData data = volumetricParams;
	data.Opacity = settings.Opacity * (data.VolumetricEnabled ? volumetricHorizonFade : 1.0f);
	return data;
}

void CloudShadows::CheckResourcesSide(int side)
{
	static Util::FrameChecker frame_checker[6];
	if (!frame_checker[side].IsNewFrame())
		return;

	if (previouslyRenderedSide >= 0 && previouslyRenderedSide != side)
		PropagateToCompletion(previouslyRenderedSide);
	previouslyRenderedSide = side;

	auto context = globals::d3d::context;

	float black[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(cloudShadowLayerRTVs[0][side], black);
	renderedLayersMask[side] = 0;
}

void CloudShadows::PropagateToCompletion(int side)
{
	uint32_t mask = renderedLayersMask[side];
	unsigned long highBit;
	int fromLayer = _BitScanReverse(&highBit, mask) ? static_cast<int>(highBit) : 0;

	auto context = globals::d3d::context;

	uint32_t newLayers = mask & ~globalRenderedMask;
	if (newLayers) {
		unsigned long bit;
		uint32_t remaining = newLayers;
		while (_BitScanForward(&bit, remaining)) {
			int newLayer = static_cast<int>(bit);
			for (int otherSide = 0; otherSide < 6; otherSide++) {
				if (otherSide == side)
					continue;
				if (renderedLayersMask[otherSide] & (1u << newLayer))
					continue;
				uint32_t belowMask = renderedLayersMask[otherSide] & ((1u << newLayer) - 1);
				unsigned long nearestBit;
				int srcLayer = _BitScanReverse(&nearestBit, belowMask) ? static_cast<int>(nearestBit) : 0;
				UINT otherSub = D3D11CalcSubresource(0, otherSide, cubemapMipLevels);
				context->CopySubresourceRegion(
					texCloudShadowLayers[newLayer]->resource.get(), otherSub, 0, 0, 0,
					texCloudShadowLayers[srcLayer]->resource.get(), otherSub, nullptr);
				renderedLayersMask[otherSide] |= (1u << newLayer);
			}
			remaining &= ~(1u << bit);
		}
		globalRenderedMask |= mask;
	}

	if (fromLayer < kMaxCloudLayers - 1) {
		UINT subresource = D3D11CalcSubresource(0, side, cubemapMipLevels);
		context->CopySubresourceRegion(
			texCloudShadowLayers[kMaxCloudLayers - 1]->resource.get(), subresource, 0, 0, 0,
			texCloudShadowLayers[fromLayer]->resource.get(), subresource, nullptr);
	}
}

void CloudShadows::SkyShaderHacks()
{
	if (overrideSky) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;

		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		ID3D11RenderTargetView* rtvs[4];
		ID3D11DepthStencilView* dsv;
		context->OMGetRenderTargets(3, rtvs, &dsv);

		int side = -1;
		for (int i = 0; i < 6; ++i)
			if (rtvs[0] == reflections.cubeSideRTV[i]) {
				side = i;
				break;
			}
		if (side == -1)
			return;

		CheckResourcesSide(side);

		int layer = currentLayerForDraw;

		unsigned long highBit;
		int prevLayer = _BitScanReverse(&highBit, renderedLayersMask[side]) ? static_cast<int>(highBit) : -1;

		UINT subresource = D3D11CalcSubresource(0, side, cubemapMipLevels);

		int fromLayer = std::max(prevLayer, 0);

		context->CopyResource(texSelfShadowCopy->resource.get(), texCloudShadowLayers[layer]->resource.get());

		if (layer > 0) {
			context->CopySubresourceRegion(
				texCloudShadowLayers[layer]->resource.get(), subresource, 0, 0, 0,
				texCloudShadowLayers[fromLayer]->resource.get(), subresource, nullptr);
		}

		ID3D11ShaderResourceView* selfShadowSrv = texSelfShadowCopy->srv.get();
		context->PSSetShaderResources(26, 1, &selfShadowSrv);

		rtvs[3] = cloudShadowLayerRTVs[layer][side];
		context->OMSetRenderTargets(4, rtvs, nullptr);

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		UINT sampleMask = 0xffffffff;

		context->OMSetBlendState(cloudShadowBlendState, blendFactor, sampleMask);

		auto cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		context->PSSetShaderResources(17, 1, &cubemapDepth.depthSRV);

		for (int i = 0; i < 3; ++i) {
			if (rtvs[i])
				rtvs[i]->Release();
		}
		if (dsv)
			dsv->Release();

		renderedLayersMask[side] |= (1u << layer);

		overrideSky = false;
	}
}

int CloudShadows::FindCloudLayer(RE::BSRenderPass* Pass)
{
	auto sky = globals::game::sky;
	if (!sky || !sky->clouds)
		return -1;

	for (int i = 0; i < kMaxCloudLayers; i++) {
		if (sky->clouds->clouds[i].get() == Pass->geometry)
			return i;
	}
	return -1;
}

void CloudShadows::ModifySky(RE::BSRenderPass* Pass)
{
	auto shadowState = globals::game::shadowState;

	auto& cubeMapRenderTarget = shadowState->GetRuntimeData().cubeMapRenderTarget;

	auto skyProperty = static_cast<const RE::BSSkyShaderProperty*>(Pass->shaderProperty);

	if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS)
		return;

	int layer = FindCloudLayer(Pass);
	if (layer < 0)
		return;

	if (cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
		currentLayerForDraw = layer;
		overrideSky = true;
	} else {
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* srv = texCloudShadowLayers[layer]->srv.get();
		context->PSSetShaderResources(26, 1, &srv);
	}
}

void CloudShadows::ReflectionsPrepass()
{
	Util::FrameChecker frameChecker;
	if (frameChecker.IsNewFrame()) {
		if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
			!globals::game::sky->currentClimate)
			return;

		auto context = globals::d3d::context;

		context->CopyResource(texCubemapCloudOccCopy->resource.get(), texCloudShadowLayers[kMaxCloudLayers - 1]->resource.get());

		ID3D11ShaderResourceView* srv = texCubemapCloudOccCopy->srv.get();
		context->PSSetShaderResources(25, 1, &srv);
		context->CSSetShaderResources(25, 1, &srv);

		BindVolumetricShadowMap();
	}
}

void CloudShadows::EarlyPrepass()
{
	if (previouslyRenderedSide >= 0) {
		PropagateToCompletion(previouslyRenderedSide);
		previouslyRenderedSide = -1;
	}

	RenderVolumetricShadowMap();
	BindVolumetricShadowMap();
}

void CloudShadows::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};

		reflections.texture->GetDesc(&texDesc);
		reflections.SRV->GetDesc(&srvDesc);

		texDesc.Format = srvDesc.Format = DXGI_FORMAT_R8_UNORM;
		cubemapMipLevels = texDesc.MipLevels;

		for (int layer = 0; layer < kMaxCloudLayers; ++layer) {
			char name[64];
			snprintf(name, sizeof(name), "CloudShadows::Layer[%d]", layer);
			texCloudShadowLayers[layer] = new Texture2D(texDesc, name);
			texCloudShadowLayers[layer]->CreateSRV(srvDesc);

			for (int face = 0; face < 6; ++face) {
				reflections.cubeSideRTV[face]->GetDesc(&rtvDesc);
				rtvDesc.Format = texDesc.Format;
				DX::ThrowIfFailed(device->CreateRenderTargetView(texCloudShadowLayers[layer]->resource.get(), &rtvDesc, &cloudShadowLayerRTVs[layer][face]));
				Util::SetResourceName(cloudShadowLayerRTVs[layer][face], "CloudShadows::Layer[%d] RTV[%d]", layer, face);
			}
		}

		texCubemapCloudOccCopy = new Texture2D(texDesc, "CloudShadows::CubemapCloudOccCopy");
		texCubemapCloudOccCopy->CreateSRV(srvDesc);

		texSelfShadowCopy = new Texture2D(texDesc, "CloudShadows::SelfShadowCopy");
		texSelfShadowCopy->CreateSRV(srvDesc);
	}
	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;

		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &cloudShadowBlendState));
		Util::SetResourceName(cloudShadowBlendState, "CloudShadows::BlendState");
	}

	volumetricShadowBuffer = std::make_unique<ConstantBuffer>(
		ConstantBufferDesc<VolumetricShadowCB>(), "CloudShadows::VolumetricShadowCB");

	CompileShaders();
	CreateVolumetricResources();
}

void CloudShadows::ClearShaderCache()
{
	CompileShaders();
}

void CloudShadows::CompileShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	const std::array shaderInfos = {
		ShaderCompileInfo{ &csVolumetricTrace, { { "CLOUD_SHADOW_TRACE", "" } } },
		ShaderCompileInfo{ &csVolumetricBlurH, { { "CLOUD_SHADOW_BLUR", "" } } },
		ShaderCompileInfo{ &csVolumetricBlurV, { { "CLOUD_SHADOW_BLUR", "" }, { "BLUR_VERTICAL", "" } } },
		ShaderCompileInfo{ &csVolumetricDownsample, { { "CLOUD_SHADOW_DOWNSAMPLE", "" } } },
	};

	for (auto& info : shaderInfos) {
		*info.csPtr = nullptr;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\CloudShadows\\VolumetricCloudShadowMapCS.hlsl", info.defines, "cs_5_0")))
			info.csPtr->attach(rawPtr);
	}
}

void CloudShadows::CreateVolumetricResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	// Reachable from the settings UI and from RestoreDefaultSettings, either of
	// which can fire before the renderer has handed us a device.
	if (!device || !context)
		return;

	volumetricActive = false;
	volumetricParams = {};

	volumetricShadowMipSrvs.clear();
	volumetricShadowMipUavs.clear();
	volumetricShadowSrv = nullptr;
	texVolumetricShadow.reset();
	texVolumetricShadowBlur.reset();

	volumetricResolution = SanitizeResolution(settings.VolumetricResolution);
	settings.VolumetricResolution = volumetricResolution;

	uint32_t mipLevels = 1;
	while ((volumetricResolution >> mipLevels) >= 1u && mipLevels < kMaxVolumetricMips)
		++mipLevels;
	volumetricMipLevels = mipLevels;

	// R16_FLOAT: transmittance is in [0, 1] but is filtered and mipped, and an 8-bit
	// quantisation of it bands visibly across the soft gradients that make up most of
	// a cloud shadow.
	D3D11_TEXTURE2D_DESC texDesc{
		.Width = volumetricResolution,
		.Height = volumetricResolution,
		.MipLevels = volumetricMipLevels,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16_FLOAT,
		.SampleDesc = { .Count = 1, .Quality = 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.CPUAccessFlags = 0,
		.MiscFlags = 0
	};

	texVolumetricShadow = eastl::make_unique<Texture2D>(texDesc, "CloudShadows::VolumetricShadowMap");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = volumetricMipLevels }
	};
	texVolumetricShadow->CreateSRV(srvDesc);
	volumetricShadowSrv = texVolumetricShadow->srv;

	for (uint32_t mip = 0; mip < volumetricMipLevels; ++mip) {
		D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc{
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = mip, .MipLevels = 1 }
		};
		winrt::com_ptr<ID3D11ShaderResourceView> mipSrv;
		DX::ThrowIfFailed(device->CreateShaderResourceView(texVolumetricShadow->resource.get(), &mipSrvDesc, mipSrv.put()));
		Util::SetResourceName(mipSrv.get(), "CloudShadows::VolumetricShadowMap SRV[mip %u]", mip);
		volumetricShadowMipSrvs.push_back(mipSrv);

		D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc{
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = mip }
		};
		winrt::com_ptr<ID3D11UnorderedAccessView> mipUav;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(texVolumetricShadow->resource.get(), &mipUavDesc, mipUav.put()));
		Util::SetResourceName(mipUav.get(), "CloudShadows::VolumetricShadowMap UAV[mip %u]", mip);
		volumetricShadowMipUavs.push_back(mipUav);

		// Start fully lit: the map is sampled from the first frame the feature turns
		// on, before the first trace has run.
		const float lit[4] = { 1.f, 1.f, 1.f, 1.f };
		context->ClearUnorderedAccessViewFloat(mipUav.get(), lit);
	}

	texDesc.MipLevels = 1;
	texVolumetricShadowBlur = eastl::make_unique<Texture2D>(texDesc, "CloudShadows::VolumetricShadowBlur");
	srvDesc.Texture2D.MipLevels = 1;
	texVolumetricShadowBlur->CreateSRV(srvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC blurUavDesc{
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	texVolumetricShadowBlur->CreateUAV(blurUavDesc);
}

void CloudShadows::UpdateVolumetricParams()
{
	static Util::FrameChecker frameChecker;
	if (!frameChecker.IsNewFrame())
		return;

	volumetricParams = {};
	volumetricActive = false;
	volumetricHorizonFade = 0.0f;

	auto& physicalSky = globals::features::physicalSky;

	if (!settings.EnableVolumetricShadowMap || !physicalSky.loaded || !physicalSky.cbData.enabled)
		return;
	if (!texVolumetricShadow || !texVolumetricShadowBlur || !volumetricShadowBuffer)
		return;
	// Settings can be deserialized after SetupResources has already sized the map.
	if (SanitizeResolution(settings.VolumetricResolution) != volumetricResolution) {
		CreateVolumetricResources();
		if (!texVolumetricShadow)
			return;
	}
	if (!csVolumetricTrace || !csVolumetricBlurH || !csVolumetricBlurV || !csVolumetricDownsample)
		return;
	// Set once PhysicalSky has uploaded a cloud cbuffer; before that there is no
	// layer to march.
	if (physicalSky.cloudLayerTopRadiusKm <= physicalSky.cloudLayerBottomRadiusKm)
		return;

	// Direction toward the active directional light, matching how State.cpp derives
	// SharedData::DirLightDirection. Using the game's light rather than the sun keeps
	// the map correct for moonlit nights.
	auto* smState = globals::game::smState;
	if (!smState)
		return;
	auto shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;
	auto sunLight = shadowSceneNode->GetRuntimeData().sunLight;
	if (!sunLight || !sunLight->light)
		return;
	auto* dirLight = skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get());
	if (!dirLight)
		return;

	const auto& worldDirection = dirLight->GetWorldDirection();
	float3 lightDir(-worldDirection.x, -worldDirection.y, -worldDirection.z);
	const float lightDirLength = lightDir.Length();
	if (lightDirLength < 1e-4f)
		return;
	lightDir /= lightDirLength;

	// A light at or below the horizon needs an unbounded path to escape the layer,
	// and casts nothing worth projecting. The vanilla cubemap path takes over.
	if (lightDir.z <= kMinLightCos)
		return;

	// Ramp the shadow out over the last few degrees above the horizon. The plane
	// projection stretches without bound as the light flattens, so the map stops
	// being meaningful well before it is switched off -- without this the shadows
	// would pop at the cut.
	volumetricHorizonFade = std::clamp(
		(lightDir.z - kMinLightCos) / (kHorizonFadeCos - kMinLightCos), 0.0f, 1.0f);

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return;
	const auto playerPos = player->GetPosition();

	const float extentKm = std::clamp(settings.VolumetricRangeKm, 1.0f, 200.0f);
	const float extentGameUnits = extentKm / Util::Units::GAME_UNIT_TO_KM;
	const float texelGameUnits = 2.0f * extentGameUnits / static_cast<float>(volumetricResolution);

	// Snap the centre to the texel grid. Without this every texel resamples a
	// slightly different part of the density field each frame and the shadows crawl
	// across the ground as the player walks.
	float2 centerGameUnits(
		std::round(playerPos.x / texelGameUnits) * texelGameUnits,
		std::round(playerPos.y / texelGameUnits) * texelGameUnits);

	// Altitudes of the layer, converted out of the raymarcher's planet-centre-relative
	// km space into absolute world Z.
	const float groundRadiusKm = physicalSky.settings.planetRadius;
	const float layerBottomZ = (physicalSky.cloudLayerBottomRadiusKm - groundRadiusKm) / Util::Units::GAME_UNIT_TO_KM;
	const float layerTopZ = (physicalSky.cloudLayerTopRadiusKm - groundRadiusKm) / Util::Units::GAME_UNIT_TO_KM;

	// One screen pixel subtends this much world space per unit of view distance;
	// dividing by the texel size gives the shadow texels it covers, which is exactly
	// the mip that avoids undersampling the map.
	const float screenHeight = std::max(static_cast<float>(globals::game::graphicsState->screenHeight), 1.0f);
	const float pixelAngle = 2.0f * std::tan(Util::GetVerticalFOVRad() * 0.5f) / screenHeight;

	volumetricParams.VolumetricEnabled = 1u;
	volumetricParams.VolCenter = centerGameUnits;
	volumetricParams.VolLightDirXY = float2(lightDir.x, lightDir.y);
	volumetricParams.VolRcpLightZ = 1.0f / lightDir.z;
	volumetricParams.VolRcpExtent = 1.0f / extentGameUnits;
	volumetricParams.VolLayerBottomZ = layerBottomZ;
	volumetricParams.VolLayerTopZ = layerTopZ;
	volumetricParams.VolMipScale = pixelAngle / texelGameUnits;
	volumetricParams.VolMaxMip = static_cast<float>(volumetricMipLevels - 1);
	volumetricParams.VolMipBias = std::max(settings.VolumetricMipBias, 0.0f);
	volumetricParams.VolRcpBorderFade = 1.0f / kVolumetricBorderFade;

	volumetricLightDir = lightDir;
	volumetricCenterKm = float2(
		centerGameUnits.x * Util::Units::GAME_UNIT_TO_KM,
		centerGameUnits.y * Util::Units::GAME_UNIT_TO_KM);
	volumetricExtentKm = extentKm;
	volumetricActive = true;
}

void CloudShadows::RenderVolumetricShadowMap()
{
	auto& physicalSky = globals::features::physicalSky;

	// The trace marches PhysicalSky's density field, so its cloud cbuffers have to
	// describe this frame before we integrate them.
	if (physicalSky.loaded && physicalSky.cbData.enabled)
		physicalSky.UpdateCloudBuffers();

	UpdateVolumetricParams();
	if (!volumetricActive)
		return;

	if (!physicalSky.cloudBuffer || !physicalSky.cloudDebugBuffer || !physicalSky.cloudBaseSRV)
		return;

	auto state = globals::state;
	auto context = globals::d3d::context;

	constexpr auto debugStr = "Cloud Shadows: Volumetric Shadow Map";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		const uint32_t stepCount = std::clamp(settings.VolumetricSteps, 4u, 128u);
		// From the cached params rather than the live settings, so the clamp always
		// describes the same layer the sampling side was set up for.
		const float layerThicknessKm = (volumetricParams.VolLayerTopZ - volumetricParams.VolLayerBottomZ) * Util::Units::GAME_UNIT_TO_KM;

		VolumetricShadowCB cb{};
		cb.lightDir = volumetricLightDir;
		cb.rcpStepCount = 1.0f / static_cast<float>(stepCount);
		cb.mapCenterKm = volumetricCenterKm;
		cb.mapExtentKm = volumetricExtentKm;
		cb.stepCount = stepCount;
		cb.dstDim[0] = volumetricResolution;
		cb.dstDim[1] = volumetricResolution;
		// Bounds the sample spacing when the light is low: past this the layer is
		// effectively opaque along the path anyway.
		cb.maxPathKm = layerThicknessKm * 6.0f;

		ID3D11Buffer* constantBuffers[3] = {
			physicalSky.cloudBuffer->CB(),
			physicalSky.cloudDebugBuffer->CB(),
			volumetricShadowBuffer->CB()
		};
		ID3D11SamplerState* samplers[2] = { physicalSky.sampTr.get(), physicalSky.sampNoise.get() };

		// Drop last frame's binding of the map before writing its UAVs, so D3D does
		// not have to break the read/write conflict itself. EarlyPrepass rebinds it
		// as soon as this returns.
		ID3D11ShaderResourceView* srv = nullptr;
		context->PSSetShaderResources(27, 1, &srv);
		context->CSSetShaderResources(27, 1, &srv);

		const auto setViews = [&](ID3D11ShaderResourceView* a_srv, ID3D11UnorderedAccessView* a_uav) {
			// Always drop both first: consecutive passes read the level the previous
			// one wrote, and D3D will silently unbind a conflicting view otherwise.
			ID3D11ShaderResourceView* nullSrv = nullptr;
			ID3D11UnorderedAccessView* nullUav = nullptr;
			context->CSSetShaderResources(20, 1, &nullSrv);
			context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
			context->CSSetShaderResources(20, 1, &a_srv);
			context->CSSetUnorderedAccessViews(0, 1, &a_uav, nullptr);
		};

		const auto dispatch = [&](uint32_t dim) {
			context->Dispatch((dim + 7u) >> 3, (dim + 7u) >> 3, 1);
		};

		context->CSSetConstantBuffers(0, 3, constantBuffers);
		context->CSSetSamplers(0, 2, samplers);

		// -> trace the cloud volume into mip 0
		volumetricShadowBuffer->Update(cb);
		srv = physicalSky.cloudBaseSRV.get();
		context->CSSetShaderResources(8, 1, &srv);
		setViews(nullptr, volumetricShadowMipUavs[0].get());
		context->CSSetShader(csVolumetricTrace.get(), nullptr, 0);
		dispatch(volumetricResolution);

		srv = nullptr;
		context->CSSetShaderResources(8, 1, &srv);

		// -> separable Gaussian over mip 0, via the ping-pong target
		setViews(volumetricShadowMipSrvs[0].get(), texVolumetricShadowBlur->uav.get());
		context->CSSetShader(csVolumetricBlurH.get(), nullptr, 0);
		dispatch(volumetricResolution);

		setViews(texVolumetricShadowBlur->srv.get(), volumetricShadowMipUavs[0].get());
		context->CSSetShader(csVolumetricBlurV.get(), nullptr, 0);
		dispatch(volumetricResolution);

		// -> mip chain
		context->CSSetShader(csVolumetricDownsample.get(), nullptr, 0);
		for (uint32_t mip = 1; mip < volumetricMipLevels; ++mip) {
			const uint32_t dstDim = volumetricResolution >> mip;
			cb.dstDim[0] = dstDim;
			cb.dstDim[1] = dstDim;
			volumetricShadowBuffer->Update(cb);

			setViews(volumetricShadowMipSrvs[mip - 1].get(), volumetricShadowMipUavs[mip].get());
			dispatch(dstDim);
		}

		/* ---- RESTORE ---- */
		setViews(nullptr, nullptr);
		std::fill(std::begin(samplers), std::end(samplers), nullptr);
		context->CSSetSamplers(0, 2, samplers);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void CloudShadows::BindVolumetricShadowMap()
{
	if (!volumetricShadowSrv)
		return;

	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* srv = volumetricShadowSrv.get();
	context->PSSetShaderResources(27, 1, &srv);
	context->CSSetShaderResources(27, 1, &srv);
}

void CloudShadows::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::state->UpdateSkyShaderPermutation(Pass);
	globals::features::cloudShadows.ModifySky(Pass);
	func(This, Pass, RenderFlags);
}
