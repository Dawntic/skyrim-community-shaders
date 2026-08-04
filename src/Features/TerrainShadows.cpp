#include "TerrainShadows.h"

#include <DirectXTex.h>
#include <pystring/pystring.h>

#include "Features/TerrainBlending.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Util.h"

#define I18N_KEY_PREFIX "feature.terrain_shadows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainShadows::Settings,
	EnableTerrainShadow,
	EnableMinMaxMip,
	TraversalStartLevel,
	TraversalMaxIterations)

// Group size of MinMaxMip.cs.hlsl and TraversalDebug.cs.hlsl; keep in sync with [numthreads].
static constexpr uint kComputeGroupSize = 8u;

static constexpr uint DivideRoundingUp(uint a_value, uint a_divisor)
{
	return (a_value + a_divisor - 1) / a_divisor;
}

void TerrainShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainShadows::DrawTraversalSettings()
{
	if (!ImGui::CollapsingHeader(T(TKEY("ray_traversal"), "Ray Traversal")))
		return;

	if (ImGui::Checkbox(T(TKEY("enable_min_max_mip"), "Enable Min/Max Mip Chain"), &settings.EnableMinMaxMip))
		needPrecompute = true;
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_min_max_mip_tooltip"),
							  "Builds a conservative min/max hierarchy over the terrain shadow height map so "
							  "shadow visibility along a ray can be resolved by a hierarchical descent instead "
							  "of a fixed-step march.\n\n"
							  "Costs a few megabytes of VRAM and one reduction pass per shadow update sweep. "
							  "Consumers fall back to point sampling when this is off."));
	}

	if (!settings.EnableMinMaxMip)
		return;

	int startLevel = static_cast<int>(settings.TraversalStartLevel);
	if (ImGui::SliderInt(T(TKEY("traversal_start_level"), "Start Level"), &startLevel, 0, 12))
		settings.TraversalStartLevel = static_cast<uint>(std::clamp(startLevel, 0, 12));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("traversal_start_level_tooltip"),
							  "Mip level the traversal starts from. Starting at the top of the chain wastes "
							  "iterations descending through coarse footprints that are always ambiguous; "
							  "3-5 is usually the sweet spot."));
	}

	int maxIterations = static_cast<int>(settings.TraversalMaxIterations);
	if (ImGui::SliderInt(T(TKEY("traversal_max_iterations"), "Max Iterations"), &maxIterations, 8, 256))
		settings.TraversalMaxIterations = static_cast<uint>(std::clamp(maxIterations, 8, 256));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("traversal_max_iterations_tooltip"),
							  "Hard cap on traversal steps per ray. Unlike a fixed-step march the worst case "
							  "is unbounded, and grazing rays along the height field are that worst case. "
							  "Rays that hit the cap keep the state of their last conclusive test."));
	}
}

void TerrainShadows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable_terrain_shadow"), "Enable Terrain Shadow"), &settings.EnableTerrainShadow);

	DrawTraversalSettings();

	if (ImGui::CollapsingHeader(T(TKEY("debug"), "Debug"))) {
		std::string curr_worldspace = "N/A";
		std::string curr_worldspace_name = "N/A";
		auto tes = RE::TES::GetSingleton();
		if (tes) {
			auto worldspace = tes->GetRuntimeData2().worldSpace;
			if (worldspace) {
				curr_worldspace = worldspace->GetFormEditorID();
				curr_worldspace_name = worldspace->GetName();
			}
		}
		ImGui::Text(fmt::format("Current worldspace: {} ({})", curr_worldspace, curr_worldspace_name).c_str());
		ImGui::Text(fmt::format("Has height map: {}", heightmaps.contains(curr_worldspace)).c_str());

		ImGui::Separator();

		ImGui::BulletText("shadowUpdateCBData");
		ImGui::Indent();
		{
			ImGui::Text(fmt::format("LightPxDir: ({}, {})", shadowUpdateCBData.LightPxDir.x, shadowUpdateCBData.LightPxDir.y).c_str());
			ImGui::Text(fmt::format("LightDeltaZ: ({}, {})", shadowUpdateCBData.LightDeltaZ.x, shadowUpdateCBData.LightDeltaZ.y).c_str());
			ImGui::Text(fmt::format("StartPxCoord: {}", shadowUpdateCBData.StartPxCoord).c_str());
			ImGui::Text(fmt::format("PxSize: ({}, {})", shadowUpdateCBData.PxSize.x, shadowUpdateCBData.PxSize.y).c_str());
		}
		ImGui::Unindent();

		ImGui::Separator();

		DrawDebugSettings();

		if (ImGui::TreeNode(T(TKEY("buffer_viewer"), "Buffer Viewer"))) {
			static float debugRescale = .1f;
			ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

			if (texShadowHeight) {
				BUFFER_VIEWER_NODE_BULLET(texShadowHeight, debugRescale)
			}
			if (texShadowMinMaxMip) {
				BUFFER_VIEWER_NODE_BULLET(texShadowMinMaxMip, debugRescale)
			}
			if (texTraversalDebug) {
				BUFFER_VIEWER_NODE_BULLET(texTraversalDebug, debugRescale)
			}
			ImGui::TreePop();
		}
	}
}

void TerrainShadows::DrawDebugSettings()
{
	if (!ImGui::TreeNode(T(TKEY("traversal_validation"), "Traversal Validation")))
		return;

	ImGui::TextWrapped("%s", T(TKEY("traversal_validation_desc"),
								 "Runs the hierarchical traversal for every primary camera ray and renders the "
								 "result to a debug buffer (see Buffer Viewer below). Purely diagnostic and "
								 "never saved; leave it off during normal play."));

	ImGui::Checkbox(T(TKEY("enable_traversal_debug"), "Enable Traversal Debug View"), &debugSettings.EnableTraversalDebug);

	if (debugSettings.EnableTraversalDebug) {
		const char* modes[] = {
			T(TKEY("debug_mode_visibility"), "Visibility (traversal)"),
			T(TKEY("debug_mode_reference"), "Visibility (reference march)"),
			T(TKEY("debug_mode_difference"), "Difference"),
			T(TKEY("debug_mode_crossings"), "Crossing count"),
			T(TKEY("debug_mode_iterations"), "Iteration count")
		};
		ImGui::Combo(T(TKEY("debug_mode"), "View"), &debugSettings.TraversalDebugMode, modes, IM_ARRAYSIZE(modes));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("debug_mode_tooltip"),
								  "Difference is red where the traversal reports more light than the reference "
								  "march and blue where it reports less. Disagreement inside dense shadow means "
								  "the mip chain is not conservative; disagreement only at edges means leaf "
								  "refinement is too coarse.\n\n"
								  "Crossing and iteration counts ramp blue to red, and turn white where the "
								  "crossing budget or the iteration cap was exceeded."));
		}

		int referenceSteps = static_cast<int>(debugSettings.ReferenceSteps);
		if (ImGui::SliderInt(T(TKEY("debug_reference_steps"), "Reference Steps"), &referenceSteps, 4, 128))
			debugSettings.ReferenceSteps = static_cast<uint>(std::clamp(referenceSteps, 4, 128));

		ImGui::SliderFloat(T(TKEY("debug_sigma"), "Extinction (Sigma)"), &debugSettings.Sigma, 0.f, 0.001f, "%.6f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("debug_sigma_tooltip"),
								  "Mean extinction used to weight visibility along the ray. Zero gives an "
								  "unweighted mean; higher values bias the result toward the near end, which is "
								  "what a scattering medium actually sees."));
		}

		ImGui::SliderFloat(T(TKEY("debug_difference_gain"), "Difference Gain"), &debugSettings.DifferenceGain, 1.f, 100.f, "%.1f");
		ImGui::SliderFloat(T(TKEY("debug_max_ray_length"), "Max Ray Length"), &debugSettings.MaxRayLength, 1000.f, 200000.f, "%.0f");
	}

	ImGui::Separator();
	ImGui::BulletText("%s", fmt::format("Min/max mip levels: {}", GetShadowMipLevels()).c_str());
	ImGui::BulletText("%s", fmt::format("Chain built: {}", mipChainBuilt).c_str());

	ImGui::TreePop();
}

void TerrainShadows::ClearShaderCache()
{
	// Assigning nullptr is the release: the earlier explicit ->Release() alongside it dropped
	// a reference the com_ptr still owned.
	for (auto* shader : { &shadowUpdateProgram, &minMaxMipLevel0Program, &minMaxMipLevelNProgram, &traversalDebugProgram })
		*shader = nullptr;

	CompileComputeShaders();
}

void TerrainShadows::ParseHeightmapPath(std::filesystem::path p, bool xlodgen_style)
{
	auto filename = p.filename();
	if (filename.extension() != ".dds")
		return;
	logger::debug("Found dds: {}", filename.string());

	auto splitstr = pystring::split(filename.stem().string(), ".");
	if (splitstr.size() != (xlodgen_style ? 9 : 10)) {
		logger::debug("{} has incorrect number ({}) of fields", filename.string(), splitstr.size());
		return;
	}

	bool middle_check = xlodgen_style ? ((splitstr[1] == "Terrain") && (splitstr[2] == "HeightMap")) : (splitstr[1] == "HeightMap");
	if (middle_check) {
		HeightMapMetadata metadata;
		try {
			if (xlodgen_style) {
				metadata.worldspace = splitstr[0];
				metadata.pos0.x = std::stoi(splitstr[3]) * 4096.f;
				metadata.pos1.y = std::stoi(splitstr[4]) * 4096.f;
				metadata.pos1.x = (std::stoi(splitstr[5]) + 1) * 4096.f;
				metadata.pos0.y = (std::stoi(splitstr[6]) + 1) * 4096.f;
				metadata.pos0.z = -32767 * 8.f;
				metadata.pos1.z = 32767 * 8.f;
				metadata.zRange.x = std::stoi(splitstr[7]) * 8.f;
				metadata.zRange.y = std::stoi(splitstr[8]) * 8.f;
			} else {
				metadata.worldspace = splitstr[0];
				metadata.pos0.x = std::stoi(splitstr[2]) * 4096.f;
				metadata.pos1.y = std::stoi(splitstr[3]) * 4096.f;
				metadata.pos1.x = (std::stoi(splitstr[4]) + 1) * 4096.f;
				metadata.pos0.y = (std::stoi(splitstr[5]) + 1) * 4096.f;
				metadata.pos0.z = std::stoi(splitstr[6]) * 8.f;
				metadata.pos1.z = std::stoi(splitstr[7]) * 8.f;
				metadata.zRange.x = std::stoi(splitstr[8]) * 8.f;
				metadata.zRange.y = std::stoi(splitstr[9]) * 8.f;
			}
		} catch (std::exception& e) {
			logger::debug("Failed to parse {}. Error: {}", filename.string(), e.what());
			return;
		}

		metadata.dir = p.parent_path().wstring();
		metadata.filename = filename.string();

		if (heightmaps.contains(metadata.worldspace))
			logger::warn("{} has more than one height maps!", metadata.worldspace);
		heightmaps[metadata.worldspace] = metadata;

		logger::info("{} loaded.", filename.string());
	} else
		logger::debug("{} has unknown type ({})", filename.string(), splitstr[1]);
}

void TerrainShadows::SetupResources()
{
	logger::debug("Listing xLODGen height maps...");
	{
		std::filesystem::path texture_dir{ L"Data\\textures\\Terrain\\" };
		std::error_code ec;
		for (auto const& dir_entry : std::filesystem::directory_iterator{ texture_dir, ec }) {
			auto dir_path = dir_entry.path();
			if (!std::filesystem::is_directory(dir_path))
				continue;

			for (auto const& sub_dir_entry : std::filesystem::directory_iterator{ dir_path })
				ParseHeightmapPath(sub_dir_entry.path(), true);
		}
	}

	logger::debug("Listing height maps...");
	{
		std::filesystem::path texture_dir{ L"Data\\textures\\heightmaps\\" };
		std::error_code ec;
		for (auto const& dir_entry : std::filesystem::directory_iterator{ texture_dir, ec })
			ParseHeightmapPath(dir_entry.path(), false);
	}

	logger::debug("Creating constant buffers...");
	{
		shadowUpdateCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShadowUpdateCB>(), "TerrainShadows::UpdateCB");
		minMaxMipCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<MinMaxMipCB>(), "TerrainShadows::MinMaxMipCB");
		traversalDebugCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TraversalDebugCB>(), "TerrainShadows::TraversalDebugCB");
	}

	logger::debug("Creating samplers...");
	{
		// Clamped: the traversal already restricts itself to the mapped region, so wrapping
		// at the edge would only fabricate occluders across the seam.
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.ComparisonFunc = D3D11_COMPARISON_NEVER,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
		Util::SetResourceName(linearClampSampler.get(), "TerrainShadows::LinearClampSampler");
	}

	CompileComputeShaders();
}

void TerrainShadows::CompileComputeShaders()
{
	logger::debug("Compiling shaders...");
	{
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\ShadowUpdate.cs.hlsl", {}, "cs_5_0"));
		if (program_ptr)
			shadowUpdateProgram.attach(program_ptr);
	}
	{
		std::vector<std::pair<const char*, const char*>> defines{ { "BUILD_LEVEL0", "" } };
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\MinMaxMip.cs.hlsl", defines, "cs_5_0"));
		if (program_ptr)
			minMaxMipLevel0Program.attach(program_ptr);
	}
	{
		std::vector<std::pair<const char*, const char*>> defines{ { "BUILD_LEVEL_N", "" } };
		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\MinMaxMip.cs.hlsl", defines, "cs_5_0"));
		if (program_ptr)
			minMaxMipLevelNProgram.attach(program_ptr);
	}
	// TraversalDebug is compiled lazily: its depth binding depends on whether Terrain
	// Blending is loaded, which is not settled at SetupResources time.
}

bool TerrainShadows::IsHeightMapReady()
{
	if (auto tes = RE::TES::GetSingleton())
		if (auto worldspace = tes->GetRuntimeData2().worldSpace)
			return cachedHeightmap && cachedHeightmap->worldspace == worldspace->GetFormEditorID();
	return false;
}

uint TerrainShadows::GetShadowMipLevels() const
{
	// Reported as 0 until the chain actually holds data, so shaders can branch away from an
	// unbuilt resource instead of reading garbage on the first frames in a worldspace.
	if (!settings.EnableMinMaxMip || !mipChainBuilt || !texShadowMinMaxMip)
		return 0;
	return texShadowMinMaxMip->desc.MipLevels;
}

TerrainShadows::PerFrame TerrainShadows::GetCommonBufferData()
{
	bool isHeightmapReady = IsHeightMapReady();

	PerFrame data = {
		.EnableTerrainShadow = settings.EnableTerrainShadow && isHeightmapReady,
		.TraversalStartLevel = settings.TraversalStartLevel,
		.TraversalMaxIterations = settings.TraversalMaxIterations,
		.ShadowMipLevels = isHeightmapReady ? GetShadowMipLevels() : 0u,
	};

	if (isHeightmapReady) {
		auto invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		data.Scale = float3(1.f, 1.f, 1.f) / invScale;
		data.Offset = -cachedHeightmap->pos0 * float2{ data.Scale.x, data.Scale.y };
		data.ZRange = cachedHeightmap->zRange;
	}

	return data;
}

void TerrainShadows::LoadHeightmap()
{
	auto tes = globals::game::tes;
	if (!tes)
		return;

	auto worldspace = tes->GetRuntimeData2().worldSpace;
	while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
		worldspace = worldspace->parentWorld;

	if (!worldspace)
		return;

	std::string worldspace_name = worldspace->GetFormEditorID();
	if (!heightmaps.contains(worldspace_name))  // no height map for that, but we don't remove cache
		return;

	if (cachedHeightmap && cachedHeightmap->worldspace == worldspace_name)  // already cached
		return;

	auto device = globals::d3d::device;

	logger::debug("Loading height map...");
	{
		auto& target_heightmap = heightmaps[worldspace_name];

		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ target_heightmap.dir };
			path /= target_heightmap.filename;

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texHeightMap.release();
		texHeightMap = std::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource), "TerrainShadows::HeightMap");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texHeightMap->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texHeightMap->CreateSRV(srvDesc);

		cachedHeightmap = &heightmaps[worldspace_name];
	}

	shadowUpdateIdx = 0;
	needPrecompute = true;
}

void TerrainShadows::Precompute()
{
	if (!cachedHeightmap)
		return;

	auto device = globals::d3d::device;

	logger::info("Creating shadow texture...");
	{
		UnbindShadowResources();

		texShadowHeight.release();

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = texHeightMap->desc.Width,
			.Height = texHeightMap->desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texShadowHeight = std::make_unique<Texture2D>(texDesc, "TerrainShadows::ShadowHeight");
		texShadowHeight->CreateSRV(srvDesc);
		texShadowHeight->CreateUAV(uavDesc);
	}

	minMaxMipLevelSRVs.clear();
	minMaxMipLevelUAVs.clear();
	texShadowMinMaxMip.release();
	mipChainBuilt = false;

	if (settings.EnableMinMaxMip) {
		logger::info("Creating terrain shadow min/max mip chain...");

		const uint width = texShadowHeight->desc.Width;
		const uint height = texShadowHeight->desc.Height;
		// Full chain down to 1x1: 1 + floor(log2(max(W, H))).
		const uint mipLevels = static_cast<uint>(std::bit_width(std::max(width, height)));

		// R16G16_UNORM matches the source encoding exactly, so reducing the chain introduces
		// no quantisation of its own and the bounds stay conservative by construction.
		// ~1.33x the base at 4 bytes per texel, which is a few MB even for a 4096 map.
		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = width,
			.Height = height,
			.MipLevels = mipLevels,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};

		texShadowMinMaxMip = std::make_unique<Texture2D>(texDesc, "TerrainShadows::ShadowMinMaxMip");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = mipLevels }
		};
		texShadowMinMaxMip->CreateSRV(srvDesc);

		// Per-level views: the chain is reduced one level at a time, reading L-1 and writing L.
		minMaxMipLevelSRVs.reserve(mipLevels);
		minMaxMipLevelUAVs.reserve(mipLevels);
		for (uint level = 0; level < mipLevels; ++level) {
			D3D11_SHADER_RESOURCE_VIEW_DESC levelSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = level,
					.MipLevels = 1 }
			};
			winrt::com_ptr<ID3D11ShaderResourceView> levelSrv;
			DX::ThrowIfFailed(device->CreateShaderResourceView(texShadowMinMaxMip->resource.get(), &levelSrvDesc, levelSrv.put()));
			Util::SetResourceName(levelSrv.get(), "TerrainShadows::ShadowMinMaxMip SRV mip%u", level);
			minMaxMipLevelSRVs.push_back(levelSrv);

			D3D11_UNORDERED_ACCESS_VIEW_DESC levelUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = level }
			};
			winrt::com_ptr<ID3D11UnorderedAccessView> levelUav;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texShadowMinMaxMip->resource.get(), &levelUavDesc, levelUav.put()));
			Util::SetResourceName(levelUav.get(), "TerrainShadows::ShadowMinMaxMip UAV mip%u", level);
			minMaxMipLevelUAVs.push_back(levelUav);
		}
	}

	needPrecompute = false;
}

void TerrainShadows::UpdateShadow()
{
	ZoneScoped;

	if (!IsHeightMapReady())
		return;

	// don't forget to change NTHREADS in shader!
	constexpr uint updateLength = 128u;
	constexpr uint logUpdateLength = std::bit_width(128u) - 1;  // integer log2, https://stackoverflow.com/questions/994593/how-to-do-an-integer-log2-in-c

	auto context = globals::d3d::context;

	UnbindShadowResources();

	auto accumulator = *globals::game::currentAccumulator.get();
	auto shadowSceneNode = accumulator->GetRuntimeData().activeShadowSceneNode;
	if (!shadowSceneNode)
		return;
	auto sunLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get());
	if (!sunLight)
		return;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Occlusion - Update Shadows");

	/* ---- UPDATE CB ---- */
	uint width = texHeightMap->desc.Width;
	uint height = texHeightMap->desc.Height;

	// only update direction at the start of each cycle
	static uint edgePxCoord;
	static int signDir;
	static uint maxUpdates;
	if (shadowUpdateIdx == 0) {
		auto direction = sunLight->GetWorldDirection();
		float3 dirLightDir = { direction.x, direction.y, direction.z };
		if (dirLightDir.z > 0)
			dirLightDir = -dirLightDir;

		// in UV
		float3 invScale = cachedHeightmap->pos1 - cachedHeightmap->pos0;
		invScale.z = cachedHeightmap->zRange.y - cachedHeightmap->zRange.x;
		float3 dirLightPxDir = dirLightDir / invScale;
		dirLightPxDir.x *= width;
		dirLightPxDir.y *= height;

		float stepMult;
		if (abs(dirLightPxDir.x) >= abs(dirLightPxDir.y)) {
			stepMult = 1.f / abs(dirLightPxDir.x);
			edgePxCoord = dirLightPxDir.x > 0 ? 0 : (width - 1);
			signDir = dirLightPxDir.x > 0 ? 1 : -1;
			maxUpdates = (width + updateLength - 1) >> logUpdateLength;
		} else {
			stepMult = 1.f / abs(dirLightPxDir.y);
			edgePxCoord = dirLightPxDir.y > 0 ? 0 : height - 1;
			signDir = dirLightPxDir.y > 0 ? 1 : -1;
			maxUpdates = (height + updateLength - 1) >> logUpdateLength;
		}
		dirLightPxDir *= stepMult;

		shadowUpdateCBData.LightPxDir = { dirLightPxDir.x, dirLightPxDir.y };

		// soft shadow angles
		float lenUV = float2{ dirLightDir.x, dirLightDir.y }.Length();
		float dirLightAngle = atan2(-dirLightDir.z, lenUV);
		float shadowSofteningRadiusAngle = RE::NI_PI / 180.f;
		float upperAngle = std::max(0.f, dirLightAngle - shadowSofteningRadiusAngle);
		float lowerAngle = std::min(RE::NI_HALF_PI - 1e-2f, dirLightAngle + shadowSofteningRadiusAngle);

		shadowUpdateCBData.LightDeltaZ = -(lenUV / invScale.z * stepMult) * float2{ std::tan(upperAngle), std::tan(lowerAngle) };
	}

	shadowUpdateCBData.StartPxCoord = edgePxCoord + signDir * shadowUpdateIdx * updateLength;
	shadowUpdateCBData.PxSize = { 1.f / texHeightMap->desc.Width, 1.f / texHeightMap->desc.Height };

	shadowUpdateCBData.PosRange = { cachedHeightmap->pos0.z, cachedHeightmap->pos1.z };
	shadowUpdateCBData.ZRange = cachedHeightmap->zRange;

	shadowUpdateCB->Update(shadowUpdateCBData);

	shadowUpdateIdx = (shadowUpdateIdx + 1) % maxUpdates;

	/* ---- BACKUP ---- */
	struct ShaderState
	{
		ID3D11ShaderResourceView* srvs[1] = { nullptr };
		ID3D11ComputeShader* shader = nullptr;
		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		ID3D11Buffer* buffer = nullptr;
	} old, newer;

	/* ---- DISPATCH ---- */

	newer.srvs[0] = texHeightMap->srv.get();
	newer.uavs[0] = texShadowHeight->uav.get();
	newer.buffer = shadowUpdateCB->CB();

	context->CSSetShaderResources(0, ARRAYSIZE(newer.srvs), newer.srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(newer.uavs), newer.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &newer.buffer);
	context->CSSetShader(shadowUpdateProgram.get(), nullptr, 0);
	globals::profiler->BeginPass("TerrainShadows::ShadowUpdate");
	context->Dispatch(abs(shadowUpdateCBData.LightPxDir.x) >= abs(shadowUpdateCBData.LightPxDir.y) ? height : width, 1, 1);
	globals::profiler->EndPass();

	/* ---- RESTORE ---- */
	context->CSSetShaderResources(0, ARRAYSIZE(old.srvs), old.srvs);
	context->CSSetShader(old.shader, nullptr, 0);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(old.uavs), old.uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &old.buffer);
}

void TerrainShadows::BuildMinMaxMip()
{
	ZoneScoped;

	if (!texShadowHeight || !texShadowMinMaxMip || !minMaxMipLevel0Program || !minMaxMipLevelNProgram)
		return;
	if (minMaxMipLevelUAVs.empty() || minMaxMipLevelSRVs.size() != minMaxMipLevelUAVs.size())
		return;

	auto context = globals::d3d::context;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Shadows - Build Min/Max Mip");

	// The chain is written through UAVs, so the read-only bindings have to go first.
	UnbindShadowResources();

	const uint width = texShadowMinMaxMip->desc.Width;
	const uint height = texShadowMinMaxMip->desc.Height;
	const uint mipLevels = static_cast<uint>(minMaxMipLevelUAVs.size());

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;

	globals::profiler->BeginPass("TerrainShadows::MinMaxMip");

	// Level 0: convert the penumbra band into (lower bound, upper bound) over a 3x3
	// neighbourhood, which is what makes the chain valid for bilinear reconstruction.
	{
		ID3D11ShaderResourceView* srv = texShadowHeight->srv.get();
		ID3D11UnorderedAccessView* uav = minMaxMipLevelUAVs[0].get();

		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(minMaxMipLevel0Program.get(), nullptr, 0);
		context->Dispatch(DivideRoundingUp(width, kComputeGroupSize), DivideRoundingUp(height, kComputeGroupSize), 1);
	}

	// Levels 1..N: plain reduction, one dispatch per level.
	{
		ID3D11Buffer* cb = minMaxMipCB->CB();
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetShader(minMaxMipLevelNProgram.get(), nullptr, 0);

		for (uint level = 1; level < mipLevels; ++level) {
			// The previous level moves from UAV to SRV; detach it before rebinding, or D3D
			// silently drops one of the two views.
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

			const uint srcWidth = std::max(width >> (level - 1), 1u);
			const uint srcHeight = std::max(height >> (level - 1), 1u);
			const uint dstWidth = std::max(width >> level, 1u);
			const uint dstHeight = std::max(height >> level, 1u);

			minMaxMipCBData = { { srcWidth, srcHeight }, { dstWidth, dstHeight } };
			minMaxMipCB->Update(minMaxMipCBData);

			ID3D11ShaderResourceView* srv = minMaxMipLevelSRVs[level - 1].get();
			ID3D11UnorderedAccessView* uav = minMaxMipLevelUAVs[level].get();

			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->Dispatch(DivideRoundingUp(dstWidth, kComputeGroupSize), DivideRoundingUp(dstHeight, kComputeGroupSize), 1);
		}
	}

	globals::profiler->EndPass();

	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);

	mipChainBuilt = true;
}

void TerrainShadows::BindShadowResources()
{
	if (!texShadowHeight)
		return;

	auto context = globals::d3d::context;

	std::array<ID3D11ShaderResourceView*, 2> srvs = {
		texShadowHeight->srv.get(),
		(settings.EnableMinMaxMip && texShadowMinMaxMip) ? texShadowMinMaxMip->srv.get() : nullptr
	};
	context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
}

void TerrainShadows::UnbindShadowResources()
{
	if (!texShadowHeight && !texShadowMinMaxMip)
		return;

	auto context = globals::d3d::context;

	std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr, nullptr };
	context->PSSetShaderResources(60, (uint)srvs.size(), srvs.data());
	context->CSSetShaderResources(60, (uint)srvs.size(), srvs.data());
}

void TerrainShadows::ReflectionsPrepass()
{
	BindShadowResources();
}

void TerrainShadows::EarlyPrepass()
{
	LoadHeightmap();

	if (!settings.EnableTerrainShadow)
		return;

	if (needPrecompute)
		Precompute();

	UpdateShadow();

	// The height map is rewritten one slab per frame, so rebuilding the chain every frame
	// would pay for nine-tap reductions over data that has barely moved. Rebuilding when the
	// update index wraps matches the rate at which the sun direction is actually re-read, and
	// it also means the first build lands after a complete sweep rather than over the
	// half-written texture a freshly created shadow map starts out as.
	if (settings.EnableMinMaxMip && IsHeightMapReady() && shadowUpdateIdx == 0)
		BuildMinMaxMip();

	BindShadowResources();
}

void TerrainShadows::Prepass()
{
	if (debugSettings.EnableTraversalDebug)
		DrawTraversalDebug();
}

void TerrainShadows::DrawTraversalDebug()
{
	ZoneScoped;

	if (!settings.EnableTerrainShadow || !IsHeightMapReady() || GetShadowMipLevels() == 0)
		return;

	auto context = globals::d3d::context;

	if (!traversalDebugProgram) {
		// TERRAIN_BLENDING flips the depth binding from R24_UNORM_X8_TYPELESS game depth to
		// the R32_FLOAT blended depth, matching what GetCurrentSceneDepthSRV hands back.
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });

		auto program_ptr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\TerrainShadows\\TraversalDebug.cs.hlsl", defines, "cs_5_0"));
		if (!program_ptr) {
			logger::error("Failed to compile terrain shadow traversal debug shader; disabling the debug view.");
			debugSettings.EnableTraversalDebug = false;
			return;
		}
		traversalDebugProgram.attach(program_ptr);
	}

	const auto screenSize = globals::state->screenSize;
	const uint bufferWidth = static_cast<uint>(screenSize.x);
	const uint bufferHeight = static_cast<uint>(screenSize.y);
	if (bufferWidth == 0 || bufferHeight == 0)
		return;

	if (!texTraversalDebug || texTraversalDebug->desc.Width != bufferWidth || texTraversalDebug->desc.Height != bufferHeight) {
		texTraversalDebug.release();

		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = bufferWidth,
			.Height = bufferHeight,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texTraversalDebug = std::make_unique<Texture2D>(texDesc, "TerrainShadows::TraversalDebug");
		texTraversalDebug->CreateSRV(srvDesc);
		texTraversalDebug->CreateUAV(uavDesc);
	}

	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Shadows - Traversal Debug");

	traversalDebugCBData = {
		.BufferDim = { static_cast<float>(bufferWidth), static_cast<float>(bufferHeight) },
		.RcpBufferDim = { 1.f / bufferWidth, 1.f / bufferHeight },
		.DebugMode = static_cast<uint>(std::max(debugSettings.TraversalDebugMode, 0)),
		.ReferenceSteps = debugSettings.ReferenceSteps,
		.Sigma = debugSettings.Sigma,
		.DifferenceGain = debugSettings.DifferenceGain,
		.MaxRayLength = debugSettings.MaxRayLength,
		.MaxIterationsForDisplay = settings.TraversalMaxIterations,
	};
	traversalDebugCB->Update(traversalDebugCBData);

	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	if (!depthSRV)
		return;

	ID3D11ShaderResourceView* srv = depthSRV;
	ID3D11UnorderedAccessView* uav = texTraversalDebug->uav.get();
	ID3D11Buffer* cb = traversalDebugCB->CB();
	ID3D11Buffer* sharedCB = globals::state->sharedDataCB->CB();
	ID3D11Buffer* featureCB = globals::state->featureDataCB->CB();
	ID3D11SamplerState* sampler = linearClampSampler.get();

	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetConstantBuffers(5, 1, &sharedCB);
	context->CSSetConstantBuffers(6, 1, &featureCB);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetShader(traversalDebugProgram.get(), nullptr, 0);

	globals::profiler->BeginPass("TerrainShadows::TraversalDebug");
	context->Dispatch(DivideRoundingUp(bufferWidth, kComputeGroupSize), DivideRoundingUp(bufferHeight, kComputeGroupSize), 1);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetShader(nullptr, nullptr, 0);
}

#undef I18N_KEY_PREFIX
