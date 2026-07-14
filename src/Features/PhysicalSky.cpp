#include "PhysicalSky.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <imgui_stdlib.h>

#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>

#include "CloudShadows.h"
#include "Deferred.h"
#include "I18n/I18n.h"
#include "LinearLighting.h"
#include "SkySync.h"
#include "TerrainShadows.h"
#include "VolumetricShadows.h"

#include "State.h"
#include "WeatherManager.h"
#include "WeatherVariableRegistry.h"

#define I18N_KEY_PREFIX "feature.physical_sky."
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::WorldspaceInfo,
	zBottom)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::CloudSettings,
	bottomRadius,
	topRadius,
	minDistance,
	maxDistance,
	coverage,
	heightScale,
	cloudType,
	coverage2,
	scroll,
	detailFrequency,
	detailStrength,
	detailCurlScale,
	detailCurlStrength,
	detailFadeStart,
	detailFadeEnd,
	weatherFrontWidth)

// Only the artistic tuning subset persists; the debug seams (override modes,
// debug color, overlay toggle) stay runtime-only so a save can never come
// back up with, say, the ambient chain forced red.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::CloudLightingSettings,
	sunGain,
	ambientGain,
	octaveAttenA,
	cloudScattering,
	cloudExtinction)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::Settings,
	enabled,
	enableAllExteriorCells,
	forceEnableAllInteriorCells,
	overrideDirLight,
	lightSkyStatics,
	skyStaticsBrightness,
	halfResApShadow,
	tonemapper,
	vanillaMix,
	trMix,
	apLumMix,
	apTrMix,
	cloudShadowRemapRange,
	sunlightColor,
	masserColor,
	secundaColor,
	proceduralSun,
	sunDiskRad,

	worldspaceWhitelist,
	groundAlbedo,
	planetRadius,
	atmosphereRadius,
	rayleighFalloff,
	rayleighScatter,
	aerosolFalloff,
	aerosolPhaseG,
	aerosolScatter,
	aerosolAbsorption,
	ozoneAltitude,
	ozoneThickness,
	ozoneAbsorption,
	fallbackZBottom,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread)

namespace
{
	RE::TESWorldSpace* GetCurrentWorldspace()
	{
		auto* tes = globals::game::tes ? globals::game::tes : RE::TES::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;

		auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (!worldspace && cell)
			worldspace = cell->GetRuntimeData().worldSpace;

		return worldspace;
	}

	bool IsCurrentCellInterior()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		return cell && cell->IsInteriorCell();
	}

	std::string GetCurrentWorldspaceEditorID()
	{
		auto* worldspace = GetCurrentWorldspace();
		if (!worldspace)
			return {};

		return worldspace->GetFormEditorID();
	}

	std::string TrimEditorID(std::string editorID)
	{
		const auto first = std::ranges::find_if_not(editorID, [](unsigned char c) {
			return std::isspace(c);
		});
		const auto last = std::find_if_not(editorID.rbegin(), editorID.rend(), [](unsigned char c) {
			return std::isspace(c);
		}).base();

		if (first >= last)
			return {};

		return { first, last };
	}

	void InfoBox(const char* str)
	{
		if (ImGui::BeginTable("Info", 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::TextWrapped(str);
			ImGui::EndTable();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void PhysicalSky::DataLoaded()
{
	if (!globals::features::skySync.loaded) {
		failedLoadedMessage = "Sky Sync is required for Physical Sky to function.";
		loaded = false;
	}
}

void PhysicalSky::RestoreDefaultSettings()
{
	settings = {};
	cloudSettings = {};
	cloudLighting = {};
	weatherUserShape = weatherStableShape = { cloudSettings.coverage, cloudSettings.cloudType, cloudSettings.coverage2 };
}

void PhysicalSky::LoadSettings(json& o_json)
{
	settings = o_json;
	if (o_json.contains("cloudSettings"))
		cloudSettings = o_json["cloudSettings"];
	if (o_json.contains("cloudLighting"))
		cloudLighting = o_json["cloudLighting"];
	weatherUserShape = weatherStableShape = { cloudSettings.coverage, cloudSettings.cloudType, cloudSettings.coverage2 };
}

void PhysicalSky::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["cloudSettings"] = cloudSettings;
	o_json["cloudLighting"] = cloudLighting;
}

void PhysicalSky::RegisterWeatherVariables()
{
	auto* registry = WeatherVariables::GlobalWeatherRegistry::GetSingleton()->GetOrCreateFeatureRegistry(GetShortName());

	// Registered defaults track the struct defaults so they cannot drift.
	const Settings defaults{};
	const CloudSettings cloudDefaults{};
	const CloudLightingSettings lightingDefaults{};

	// Sky scattering -- what makes a weather's sky look the way it does.
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>(
		"Rayleigh Scatter", "rayleighScatter",
		"Air molecule scattering coefficients (megameter^-1). Drives sky blue and sunset red.",
		&settings.rayleighScatter, defaults.rayleighScatter));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Rayleigh Falloff", "rayleighFalloff",
		"Air density falloff with altitude (km^-1).",
		&settings.rayleighFalloff, defaults.rayleighFalloff, 0.0f, 2.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>(
		"Aerosol Scatter", "aerosolScatter",
		"Aerosol (Mie) scattering coefficients (megameter^-1). Haze and aureole.",
		&settings.aerosolScatter, defaults.aerosolScatter));
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>(
		"Aerosol Absorption", "aerosolAbsorption",
		"Aerosol absorption coefficients (megameter^-1).",
		&settings.aerosolAbsorption, defaults.aerosolAbsorption));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Aerosol Falloff", "aerosolFalloff",
		"Aerosol density falloff with altitude (km^-1).",
		&settings.aerosolFalloff, defaults.aerosolFalloff, 0.0f, 2.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Aerosol Anisotropy", "aerosolPhaseG",
		"Mie phase anisotropy. Higher = tighter aureole around the sun.",
		&settings.aerosolPhaseG, defaults.aerosolPhaseG, -1.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>(
		"Ozone Absorption", "ozoneAbsorption",
		"Ozone absorption coefficients (megameter^-1). Keeps the zenith blue at twilight.",
		&settings.ozoneAbsorption, defaults.ozoneAbsorption));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Ozone Mean Altitude", "ozoneAltitude",
		"Center altitude of the ozone layer (km).",
		&settings.ozoneAltitude, defaults.ozoneAltitude, 0.0f, 100.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Ozone Layer Thickness", "ozoneThickness",
		"Thickness of the ozone layer tent profile (km).",
		&settings.ozoneThickness, defaults.ozoneThickness, 0.0f, 50.0f));

	// Cloud layer + shape. Coverage / Cloud Type / Coverage 2 additionally
	// feed the horizon weather front, which blends them SPATIALLY during
	// transitions instead of consuming the registry's time-lerp (the keys are
	// shared via kWeatherKey*).
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Cloud Bottom Height", "bottomRadius",
		"Cloud layer start height above sea level (km).",
		&cloudSettings.bottomRadius, cloudDefaults.bottomRadius, 0.0f, 19.9f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Cloud Top Height", "topRadius",
		"Cloud layer end height above sea level (km).",
		&cloudSettings.topRadius, cloudDefaults.topRadius, 0.01f, 20.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		kWeatherKeyCoverage, "coverage",
		"Cloud coverage. Blended in from the horizon during weather transitions.",
		&cloudSettings.coverage, cloudDefaults.coverage, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Height Scale", "heightScale",
		"Cloud noise scale.",
		&cloudSettings.heightScale, cloudDefaults.heightScale, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		kWeatherKeyCloudType, "cloudType",
		"Stratus/cumulus/cumulonimbus mix. Blended in from the horizon during weather transitions.",
		&cloudSettings.cloudType, cloudDefaults.cloudType, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		kWeatherKeyCoverage2, "coverage2",
		"Global coverage erosion. Blended in from the horizon during weather transitions.",
		&cloudSettings.coverage2, cloudDefaults.coverage2, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Frequency", "detailFrequency",
		"Billow/wisp feature frequency, relative to the base noise scale.",
		&cloudSettings.detailFrequency, cloudDefaults.detailFrequency, 1.0f, 32.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Strength", "detailStrength",
		"How much of the low-density shell the detail pass may erode.",
		&cloudSettings.detailStrength, cloudDefaults.detailStrength, 0.0f, 0.9f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Curl Scale", "detailCurlScale",
		"Curl lookup frequency, relative to the base noise scale.",
		&cloudSettings.detailCurlScale, cloudDefaults.detailCurlScale, 0.0f, 8.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Curl Strength", "detailCurlStrength",
		"Turbulent distortion of the detail lookup at the cloud base (km).",
		&cloudSettings.detailCurlStrength, cloudDefaults.detailCurlStrength, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Fade Start", "detailFadeStart",
		"Distance where detail sculpting starts fading out (km).",
		&cloudSettings.detailFadeStart, cloudDefaults.detailFadeStart, 0.0f, 100.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Detail Fade End", "detailFadeEnd",
		"Distance where detail sculpting is fully gone (km).",
		&cloudSettings.detailFadeEnd, cloudDefaults.detailFadeEnd, 0.0f, 100.0f));

	// Cloud lighting tuning -- storms want darker, denser media.
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Sun Gain", "sunGain",
		"Gain on the direct sun path.",
		&cloudLighting.sunGain, lightingDefaults.sunGain, 0.0f, 8.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Ambient Gain", "ambientGain",
		"Gain on the ambient term.",
		&cloudLighting.ambientGain, lightingDefaults.ambientGain, 0.0f, 2.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Octave Extinction Atten", "octaveAttenA",
		"Wrenninge octave extinction attenuation: how deep sunlight glows into the cloud.",
		&cloudLighting.octaveAttenA, lightingDefaults.octaveAttenA, 0.0f, 1.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Cloud Scattering", "cloudScattering",
		"Cloud medium scattering coefficient (km^-1).",
		&cloudLighting.cloudScattering, lightingDefaults.cloudScattering, 0.0f, 100.0f));
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"Cloud Extinction", "cloudExtinction",
		"Cloud medium extinction coefficient (km^-1).",
		&cloudLighting.cloudExtinction, lightingDefaults.cloudExtinction, 0.01f, 100.0f));
}

void PhysicalSky::DrawSettings()
{
	if (ImGui::BeginTabBar("##PHYSSKY")) {
		if (ImGui::BeginTabItem(T(TKEY("general"), "General"))) {
			SettingsGeneral();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("celestials"), "Celestials"))) {
			SettingsCelestials();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("atmosphere"), "Atmosphere"))) {
			SettingsAtmosphere();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("clouds"), "Clouds"))) {
			SettingsClouds();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(T(TKEY("debug"), "Debug"))) {
			SettingsDebug();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void PhysicalSky::SettingsGeneral()
{
	const auto currentWorldspaceName = GetCurrentWorldspaceEditorID();
	const bool inInterior = IsCurrentCellInterior();

	if (ImGui::BeginTable("Info", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
		ImGui::TableNextColumn();
		ImGui::Text("%s", T(TKEY("shader_status"), "Shader Status: "));
		ImGui::TableNextColumn();
		if (ShadersOK())
			ImGui::TextColored({ 0, 1, 0, 1 }, "OK");
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, "ERROR");

		ImGui::TableNextColumn();
		ImGui::Text("%s", T(TKEY("worldspace"), "Worldspace: "));
		ImGui::TableNextColumn();

		if (inInterior) {
			if (settings.forceEnableAllInteriorCells)
				ImGui::Text("%s", T(TKEY("interior_enabled_forced"), "Interior (Enabled, Forced)"));
			else
				ImGui::Text("%s", T(TKEY("interior_disabled"), "Interior (Disabled)"));
		} else if (!currentWorldspaceName.empty()) {
			if (settings.worldspaceWhitelist.contains(currentWorldspaceName)) {
				ImGui::Text(T(TKEY("enabled_whitelist"), "%s (Enabled, Whitelist)"), currentWorldspaceName.c_str());
			} else if (settings.enableAllExteriorCells) {
				ImGui::Text(T(TKEY("enabled_fallback_z_bottom"), "%s (Enabled, Fallback Z Bottom)"), currentWorldspaceName.c_str());
			} else {
				ImGui::Text(T(TKEY("disabled"), "%s (Disabled)"), currentWorldspaceName.c_str());
			}
		} else {
			ImGui::Text("%s", T(TKEY("unknown_worldspace_unavailable"), "Unknown (Worldspace Unavailable)"));
		}

		ImGui::EndTable();
	}

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.enabled);
	ImGui::SameLine();
	ImGui::Checkbox(T(TKEY("enable_all_exterior_cells"), "Enable All Exterior Cells"), &settings.enableAllExteriorCells);
	ImGui::SameLine();
	ImGui::Checkbox(T(TKEY("force_enable_all_interior_cells"), "Force Enable All Interior Cells"), &settings.forceEnableAllInteriorCells);
	if (settings.enableAllExteriorCells || settings.forceEnableAllInteriorCells) {
		ImGui::InputFloat(T(TKEY("fallback_z_bottom"), "Fallback Z Bottom"), &settings.fallbackZBottom, 10.f, 100.f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("used_when_current_worldspace_is_not_in_whitelist"), "Used when current worldspace is not in whitelist (or worldspace data is unavailable), including forced interiors."));
	}

	ImGui::SeparatorText(T(TKEY("worldspace_whitelist"), "Worldspace Whitelist"));
	{
		static std::string newWorldspaceEditorID;
		static float newWorldspaceZBottom = -14500.f;

		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
		ImGui::InputTextWithHint(T(TKEY("editor_id"), "Editor ID"), "Tamriel", &newWorldspaceEditorID);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
		ImGui::InputFloat(T(TKEY("z_bottom"), "Z Bottom"), &newWorldspaceZBottom, 10.f, 100.f, "%.1f");

		const auto addOrUpdateWorldspace = [&](std::string editorID, float zBottom) {
			editorID = TrimEditorID(std::move(editorID));
			if (!editorID.empty())
				settings.worldspaceWhitelist[editorID].zBottom = zBottom;
		};

		if (ImGui::Button(T(TKEY("add_update"), "Add / Update"))) {
			addOrUpdateWorldspace(newWorldspaceEditorID, newWorldspaceZBottom);
		}

		if (!currentWorldspaceName.empty()) {
			ImGui::SameLine();
			const auto currentIt = settings.worldspaceWhitelist.find(currentWorldspaceName);
			const bool currentWhitelisted = currentIt != settings.worldspaceWhitelist.end();
			if (ImGui::Button(currentWhitelisted ? T(TKEY("remove_current_worldspace"), "Remove Current Worldspace") : T(TKEY("add_current_worldspace"), "Add Current Worldspace"))) {
				if (currentWhitelisted) {
					settings.worldspaceWhitelist.erase(currentIt);
				} else {
					addOrUpdateWorldspace(currentWorldspaceName, newWorldspaceZBottom);
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", currentWorldspaceName.c_str());
		}

		if (ImGui::BeginTable("WorldspaceWhitelist", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, { -1, 0 })) {
			ImGui::TableSetupColumn(T(TKEY("editor_id"), "Editor ID"));
			ImGui::TableSetupColumn(T(TKEY("z_bottom"), "Z Bottom"), ImGuiTableColumnFlags_WidthFixed, 160.f);
			ImGui::TableSetupColumn(T(TKEY("action"), "Action"), ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();

			std::string removeEditorID;
			for (auto& [editorID, info] : settings.worldspaceWhitelist) {
				ImGui::PushID(editorID.c_str());
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(editorID.c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputFloat("##ZBottom", &info.zBottom, 10.f, 100.f, "%.1f");

				ImGui::TableSetColumnIndex(2);
				if (ImGui::Button(T(TKEY("remove"), "Remove"), { -1, 0 }))
					removeEditorID = editorID;

				ImGui::PopID();
			}

			if (!removeEditorID.empty())
				settings.worldspaceWhitelist.erase(removeEditorID);

			ImGui::EndTable();
		}
	}

	ImGui::SeparatorText(T(TKEY("post_processing"), "Post Processing"));
	{
		const bool llEnabled = globals::features::linearLighting.settings.enableLinearLighting;
		ImGui::BeginDisabled(llEnabled);
		if (ImGui::BeginTable("tonemap", 4, ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::Text("%s", T(TKEY("tonemapper"), "Tonemapper"));
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("linear"), "Linear"), &settings.tonemapper, 0);
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("gamma"), "Gamma"), &settings.tonemapper, 1);
			ImGui::TableNextColumn();
			ImGui::RadioButton(T(TKEY("reinherd"), "Reinherd"), &settings.tonemapper, 2);
			ImGui::EndTable();
		}
		ImGui::EndDisabled();
		if (llEnabled) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("tonemapper_is_forced_to_linear_when_linear_lighting"), "Tonemapper is forced to Linear when Linear Lighting is enabled."));
		}
		ImGui::SliderFloat(T(TKEY("vanilla_mix"), "Vanilla Mix"), &settings.vanillaMix, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("blend_in_vanilla_sky_color"), "Blend in vanilla sky color."));
	}
}

void PhysicalSky::SettingsCelestials()
{
	InfoBox(T(TKEY("the_sun_and_moons_and_their_lights"), "The sun and moons, and their lights."));

	ImGui::Checkbox(T(TKEY("override_directional_light"), "Override Directional Light"), &settings.overrideDirLight);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("overrides_the_color_of_directional_light_linear_tonemapper"), "Overrides the color of directional light. Linear tonemapper and 1.0 transmittance mix are recommended."));
	ImGui::SliderFloat(T(TKEY("transmittance_mix"), "Transmittance Mix"), &settings.trMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("apply_additional_atmospheric_tranmisttance_on_the_directional_light"),
							  "Apply additional atmospheric tranmisttance on the directional light.\n"
							  "Introduces natural yellowening at sunset with white sunlight."));

	ImGui::SeparatorText(T(TKEY("sky_statics"), "Sky Statics"));
	{
		ImGui::Checkbox(T(TKEY("light_sky_statics"), "Light Sky Statics"), &settings.lightSkyStatics);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("treat_sky_statics_as_albedo_and_tint_them"), "Treat sky statics as albedo and tint them with directional lighting."));

		ImGui::BeginDisabled(!settings.lightSkyStatics);
		ImGui::SliderFloat(T(TKEY("brightness"), "Brightness"), &settings.skyStaticsBrightness, 0.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("multiplies_directional_lighting_applied_to_sky_statics"), "Multiplies directional lighting applied to sky statics."));
		ImGui::EndDisabled();
	}

	ImGui::SeparatorText(T(TKEY("sun"), "Sun"));
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::Checkbox(T(TKEY("procedural_sun"), "Procedural Sun"), &settings.proceduralSun);
		ImGui::SliderAngle(T(TKEY("sun_disk_angular_radius"), "Sun Disk Angular Radius"), &settings.sunDiskRad, 0.f, 10.f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("real_world_sun_disk_angular_radius_is_about"), "Real world sun disk angular radius is about 0.27 degrees."));
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("masser"), "Masser"));
	{
		ImGui::PushID("Masser");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.masserColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("secunda"), "Secunda"));
	{
		ImGui::PushID("Secunda");
		ImGui::ColorEdit3(T(TKEY("light_color"), "Light Color"), &settings.secundaColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("light_color_hint"), "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance."));
		ImGui::PopID();
	}
}

void PhysicalSky::SettingsAtmosphere()
{
	InfoBox(T(TKEY("the_composition_and_physical_properties_of_the_atmosphere"), "The composition and physical properties of the atmosphere."));

	ImGui::SliderFloat(T(TKEY("ap_luminance_mix"), "AP Luminance Mix"), &settings.apLumMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("add_light_scattered_by_air_aerial_perspective_to"), "Add light scattered by air (Aerial Perspective) to the scene."));
	ImGui::SliderFloat(T(TKEY("ap_transmittance_mix"), "AP Transmittance Mix"), &settings.apTrMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("remove_light_absorbed_by_air_aerial_perspective_from"), "Remove light absorbed by air (Aerial Perspective) from the scene."));

	ImGui::Checkbox(T(TKEY("half_resolution_cloud_shadow"), "Half Resolution Cloud Shadow"), &settings.halfResApShadow);

	ImGui::SliderFloat2(T(TKEY("cloud_shadow_remap"), "Cloud Shadow Remap"), &settings.cloudShadowRemapRange.x, 0.f, 1.f, "%.2f");

	ImGui::SeparatorText(T(TKEY("air_molecules_rayleigh"), "Air Molecules (Rayleigh)"));
	{
		ImGui::PushID("Rayleigh");
		ImGui::TextWrapped("%s", T(TKEY("particles_much_smaller_than_the_wavelength_of_light"),
									 "Particles much smaller than the wavelength of light. They have almost complete symmetry in forward and backward scattering. "
									 "On earth, they are what makes the sky blue and, at sunset, red. Usually needs no extra change."));

		ImGui::ColorEdit3(T(TKEY("scatter"), "Scatter"), &settings.rayleighScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.rayleighFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("aerosol_mie"), "Aerosol (Mie)"));
	{
		ImGui::PushID("Mie");
		ImGui::TextWrapped("%s", T(TKEY("solid_and_liquid_particles_greater_than_1_10"),
									 "Solid and liquid particles greater than 1/10 of the light wavelength but not too much, like dust. Strongly anisotropic (Mie Scattering). "
									 "They contributes to the aureole around bright celestial bodies."));

		ImGui::SliderFloat(T(TKEY("anisotropy"), "Anisotropy"), &settings.aerosolPhaseG, -1, 1);
		ImGui::ColorEdit3(T(TKEY("scatter"), "Scatter"), &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3(T(TKEY("absorption"), "Absorption"), &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("usually_1_9_of_scatter_coefficient_dust_pollution"), "Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher."));
		ImGui::SliderFloat(T(TKEY("falloff"), "Falloff"), &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("ozone"), "Ozone"));
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped("%s", T(TKEY("the_ozone_layer_high_up_in_the_sky"),
									 "The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
									 "It keeps the zenith sky blue, especially at sunrise or sunset."));

		ImGui::ColorEdit3(T(TKEY("absorption"), "Absorption"), &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::DragFloat(T(TKEY("mean_altitude"), "Mean Altitude"), &settings.ozoneAltitude, .1f, 0.f, 100.f, "%.3f km");
		ImGui::DragFloat(T(TKEY("layer_thickness"), "Layer Thickness"), &settings.ozoneThickness, .1f, 0.f, 50.f, "%.3f km");
		ImGui::PopID();
	}

	ImGui::SeparatorText(T(TKEY("planetary_parameters"), "Planetary Parameters"));
	{
		ImGui::InputFloat(T(TKEY("planet_radius"), "Planet Radius"), &settings.planetRadius, 1.f, 100000.f, "%.1f km");
		ImGui::InputFloat(T(TKEY("atmosphere_radius"), "Atmosphere Radius"), &settings.atmosphereRadius, 1.f, 100000.f, "%.1f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("planet_radius_is_the_distance_from_the_planet"),
								  "Planet radius is the distance from the planet center to sea level.\n"
								  "Atmosphere radius is the distance from the planet center to the top of atmosphere.\n"
								  "On Earth, they are about 6360 km and 6420 km respectively."));
	}
}

void PhysicalSky::SettingsClouds()
{
	InfoBox(T(TKEY("clouds_2"), "Clouds."));

	ImGui::SliderFloat("Bottom Height", &cloudSettings.bottomRadius, 0.0, 19.9);
	ImGui::SliderFloat("Top Height", &cloudSettings.topRadius, 0.01, 20.0);
	// Edits to the three front-blended shape params also refresh the user
	// (non-weather) copy that weather transitions fall back to.
	if (ImGui::SliderFloat("Coverage", &cloudSettings.coverage, 0.0, 1.0))
		weatherUserShape.coverage = cloudSettings.coverage;

	ImGui::SliderFloat("Height Scale", &cloudSettings.heightScale, 0.0, 1.0);
	if (ImGui::SliderFloat("Cloud Type", &cloudSettings.cloudType, 0.0, 1.0))
		weatherUserShape.cloudType = cloudSettings.cloudType;

	if (ImGui::SliderFloat("Coverage 2", &cloudSettings.coverage2, 0.0, 1.0))
		weatherUserShape.coverage2 = cloudSettings.coverage2;

	ImGui::DragFloat("Scroll", &cloudSettings.scroll, 0.005f);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", "Offsets the cloud noise pattern (the z slice of the 3D noise). Drag or animate to evolve the clouds.");

	ImGui::SeparatorText("Detail Sculpting");
	{
		ImGui::SliderFloat("Detail Strength", &cloudSettings.detailStrength, 0.f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"How much of the low-density shell the detail pass may erode. The base\n"
				"silhouette always survives (interiors cannot be carved); ~0.2 is the\n"
				"reference value, 0 disables the pass.");
		ImGui::SliderFloat("Detail Frequency", &cloudSettings.detailFrequency, 1.f, 32.f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", "Billow/wisp feature frequency, relative to the base noise scale.");
		ImGui::SliderFloat("Curl Strength", &cloudSettings.detailCurlStrength, 0.f, 1.f, "%.2f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Turbulent distortion of the detail lookup, strongest at the cloud base.\n"
				"Breaks up the regular worley pattern into sheared wisps.");
		ImGui::SliderFloat("Curl Scale", &cloudSettings.detailCurlScale, 0.f, 8.f, "%.2f");
		ImGui::SliderFloat("Detail Fade Start", &cloudSettings.detailFadeStart, 0.f, 100.f, "%.0f km");
		ImGui::SliderFloat("Detail Fade End", &cloudSettings.detailFadeEnd, 0.f, 100.f, "%.0f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", "Distance window over which detail fades out (it is subpixel far away; also saves two fetches per step).");
	}

	ImGui::SeparatorText("Weather Front");
	{
		ImGui::TextWrapped("%s",
			"During weather transitions the incoming weather's clouds sweep in from the "
			"horizon along the wind instead of cross-fading in place. Assign per-weather "
			"cloud values in the CS Editor's weather widget. Wind direction is a fixed "
			"placeholder heading for now.");

		ImGui::SliderFloat("Front Width", &cloudSettings.weatherFrontWidth, 0.5f, 50.f, "%.1f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", "Width of the soft, noise-ragged band where the two weathers mix.");

		ImGui::Checkbox("Manual Front Test", &weatherFrontTest.enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Drive the front by hand instead of game weather: blends from the current\n"
				"cloud shape to the test values below as Transition goes 0 to 1.");
		if (weatherFrontTest.enabled) {
			ImGui::SliderFloat("Transition", &weatherFrontTest.transition, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Incoming Coverage", &weatherFrontTest.coverageIn, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Incoming Cloud Type", &weatherFrontTest.cloudTypeIn, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Incoming Coverage 2", &weatherFrontTest.coverage2In, 0.f, 1.f, "%.2f");
		}

		const auto weathers = WeatherManager::GetSingleton()->GetCurrentWeathers();
		ImGui::Text("Game weather: %08X -> %08X (%.2f)",
			weathers.lastWeather ? weathers.lastWeather->GetFormID() : 0u,
			weathers.currentWeather ? weathers.currentWeather->GetFormID() : 0u,
			weathers.lerpFactor);
	}

	ImGui::SliderFloat("Min Distance", &cloudSettings.minDistance, 0.0, 400.0);
	ImGui::SliderFloat("Max Distance", &cloudSettings.maxDistance, 0.0, 600.0);
	//ImGui::Checkbox("noDelay", &cloudSettings.noDelay);

	ImGui::SeparatorText("Medium");
	{
		ImGui::SliderFloat("Scattering", &cloudLighting.cloudScattering, 0.f, 100.f, "%.1f km^-1");
		ImGui::SliderFloat("Extinction", &cloudLighting.cloudExtinction, 0.01f, 100.f, "%.1f km^-1");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Water droplets are spectrally neutral: albedo (scattering / extinction)\n"
				"should stay ~0.996. Lower albedo makes cloud interiors charcoal and\n"
				"kills twilight glow penetration.");
	}

	ImGui::SeparatorText("Lighting Verification");
	{
		static const char* sunTrModes[] = { "Live", "Force White", "Force Orange", "A/B Global Tr LUT" };
		int sunTrMode = static_cast<int>(cloudLighting.debugSunTrMode);
		if (ImGui::Combo("Sun Transmittance Mode", &sunTrMode, sunTrModes, IM_ARRAYSIZE(sunTrModes)))
			cloudLighting.debugSunTrMode = static_cast<uint>(sunTrMode);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Overrides the windowed sun-transmittance sample per raymarch step.\n"
				"Force White isolates the ambient chain; A/B samples the global Tr LUT\n"
				"through its real remap to quantify the windowed LUT's benefit.");

		static const char* ambientModes[] = { "Live", "Debug Color", "Red", "Red-Blue Height Gradient" };
		int ambientMode = static_cast<int>(cloudLighting.debugAmbientMode);
		if (ImGui::Combo("Ambient Mode", &ambientMode, ambientModes, IM_ARRAYSIZE(ambientModes)))
			cloudLighting.debugAmbientMode = static_cast<uint>(ambientMode);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Overrides the ambient endpoint lerp. With Sun Gain 0, thick cloud\n"
				"interiors must converge to albedo x color regardless of view angle;\n"
				"the height gradient must show red bases and blue tops.");

		ImGui::ColorEdit3("Debug Color", &cloudLighting.debugColor.x, ImGuiColorEditFlags_Float);

		ImGui::SliderFloat("Sun Gain", &cloudLighting.sunGain, 0.f, 8.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", "Gain on the direct sun path. Set 0 to isolate the ambient chain while verifying.");
		ImGui::SliderFloat("Ambient Gain", &cloudLighting.ambientGain, 0.f, 2.f, "%.2f");

		ImGui::SliderFloat("Octave Extinction Atten", &cloudLighting.octaveAttenA, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"How quickly the Wrenninge multi-scatter octaves relax extinction:\n"
				"higher lets sunlight glow deeper into the cloud. The octave energy\n"
				"attenuation is fixed in-shader (it duplicated Sun Gain).");

		ImGui::Checkbox("Show LUT Overlay", &cloudLighting.showDebugOverlay);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s",
				"Blits the windowed sun-Tr LUT (4x) and the two ambient endpoint\n"
				"swatches (bottom | top) into the top-left screen corner.");
	}

	//ImGui::SliderFloat("Vanilla Mix", &settings.cloudOriginalMix, 0.f, 2.f, "%.2f");
	//ImGui::SliderFloat("Relight Mix", &settings.cloudRelightMix, 0.f, 2.f, "%.2f");
	//ImGui::SliderFloat("Silver Lining Accent", &settings.silverLiningMix, 0.f, 1.f, "%.2f");
	//ImGui::SliderFloat("Silver Lining Spread", &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f");
}

void PhysicalSky::SettingsDebug()
{
	InfoBox(T(TKEY("beep_boop"), "Beep Boop."));

	if (ImGui::Button(T(TKEY("recompile_shaders"), "Recompile Shaders")))
		ClearShaderCache();

	ImGui::SeparatorText(T(TKEY("values"), "Values"));
	{
		ImGui::InputFloat3(T(TKEY("sun_direction"), "Sun Direction"), &cbData.sunDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3(T(TKEY("masser_direction"), "Masser Direction"), &cbData.masserDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3(T(TKEY("secunda_direction"), "Secunda Direction"), &cbData.secundaDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
	}

	ImGui::SeparatorText("Textures");

	static float debugScale = 1.0f;
	ImGui::SliderFloat("View Scale", &debugScale, 0.5f, 4.f);

	BUFFER_VIEWER_NODE_BULLET(cloudColorTex[0], debugScale);
	BUFFER_VIEWER_NODE_BULLET(cloudDepthTex[0], debugScale);
	BUFFER_VIEWER_NODE_BULLET(texTrLut, debugScale);
	BUFFER_VIEWER_NODE_BULLET(texMsLut, debugScale);
	BUFFER_VIEWER_NODE_BULLET(texSvLut, debugScale);
	BUFFER_VIEWER_NODE_BULLET(texCloudSunTr, debugScale * 4.f);
	BUFFER_VIEWER_NODE_BULLET(texCloudAmbient, debugScale * 32.f);

	static float debugScale2 = 0.2f;
	ImGui::SliderFloat("View Scale ##2", &debugScale2, 0.1f, 1.f);
	BUFFER_VIEWER_NODE_BULLET(texApShadow, debugScale2);
}

#undef I18N_KEY_PREFIX

void PhysicalSky::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampTr.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampSv.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampNoise.put()));
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC tex2dDesc{
			.Width = kTrLutW,
			.Height = kTrLutH,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texTrLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texTrLut->CreateSRV(srvDesc);
		texTrLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kMsLutW;
		tex2dDesc.Height = kMsLutH;

		texMsLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texMsLut->CreateSRV(srvDesc);
		texMsLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kSvLutW;
		tex2dDesc.Height = kSvLutH;

		texSvLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texSvLut->CreateSRV(srvDesc);
		texSvLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kCloudTrLutW;
		tex2dDesc.Height = kCloudTrLutH;

		texCloudSunTr = eastl::make_unique<Texture2D>(tex2dDesc, "PhysicalSky::CloudSunTrLut");
		texCloudSunTr->CreateSRV(srvDesc);
		texCloudSunTr->CreateUAV(uavDesc);

		tex2dDesc.Width = 2;
		tex2dDesc.Height = 1;

		texCloudAmbient = eastl::make_unique<Texture2D>(tex2dDesc, "PhysicalSky::CloudAmbientLut");
		texCloudAmbient->CreateSRV(srvDesc);
		texCloudAmbient->CreateUAV(uavDesc);

		D3D11_TEXTURE3D_DESC tex3dDesc{
			.Width = kApLutW,
			.Height = kApLutH,
			.Depth = kApLutD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 };
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		uavDesc.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kApLutD };

		texApLut = eastl::make_unique<Texture3D>(tex3dDesc);
		texApLut->CreateSRV(srvDesc);
		texApLut->CreateUAV(uavDesc);
	}
	{
		D3D11_TEXTURE2D_DESC texDesc;
		auto mainTex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = 1;
		// texDesc.Width /= 2;
		// texDesc.Height /= 2;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texApShadow = eastl::make_unique<Texture2D>(texDesc);
		texApShadow->CreateSRV(srvDesc);
		texApShadow->CreateUAV(uavDesc);
	}

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthBias = 0;
	rasterDesc.DepthBiasClamp = 0.0f;
	rasterDesc.SlopeScaledDepthBias = 0.0f;
	rasterDesc.DepthClipEnable = FALSE;
	rasterDesc.ScissorEnable = FALSE;
	rasterDesc.MultisampleEnable = FALSE;
	rasterDesc.AntialiasedLineEnable = FALSE;
	device->CreateRasterizerState(&rasterDesc, &rasterState);

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	device->CreateBlendState(&blendDesc, &additiveBlend);

	CompileShaders();

	CreateCloudResources();
}

void PhysicalSky::ClearShaderCache()
{
	CompileShaders();
}

void PhysicalSky::CompileShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string_view entry = "main";
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &csTrLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "0" } } },
		{ &csMsLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "1" } } },
		{ &csSvLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "2" } } },
		{ &csApLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "3" } } },
		{ &csCloudTrLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "4" } } },
		{ &csCloudAmbLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "5" } } },
		{ &csShadowAccum, "ShadowAccum.cs.hlsl", {} },
		{ &csShadowAccumHalfRes, "ShadowAccum.cs.hlsl", { { "HALF_RES", "" } } }
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}

	cloudVShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\Clouds.hlsl", { { "CLOUD_VS", "" } }, "vs_5_0");
	cloudShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\Clouds.hlsl", { { "CLOUD_PS", "" } }, "ps_5_0");
	cloudBlendShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\Clouds.hlsl", { { "CLOUD_BLEND_PS", "" } }, "ps_5_0");
	cloudDebugBlitShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\Clouds.hlsl", { { "CLOUD_DEBUG_BLIT_PS", "" } }, "ps_5_0");
}

bool PhysicalSky::ShadersOK()
{
	return csTrLutGen && csMsLutGen && csSvLutGen && csApLutGen && csCloudTrLutGen && csCloudAmbLutGen && csShadowAccum && csShadowAccumHalfRes &&
	       texTrLut && texSvLut && texApLut && texApShadow && texCloudSunTr && texCloudAmbient;
}

void PhysicalSky::Reset()
{
	auto& skySync = globals::features::skySync;
	skySync.lightColors = std::nullopt;

	auto& linearLighting = globals::features::linearLighting;

	bool allGood = settings.enabled && ShadersOK() && skySync.loaded && skySync.settings.Enabled;

	// check worldspace
	bool worldspaceEnabled = false;
	bool inInterior = false;
	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	std::map<std::string, WorldspaceInfo>::const_iterator worldspaceIt = settings.worldspaceWhitelist.end();
	float zBottom = settings.fallbackZBottom;
	auto* tes = globals::game::tes ? globals::game::tes : RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* cell = player ? player->GetParentCell() : nullptr;
	inInterior = cell && cell->IsInteriorCell();
	if (tes) {
		auto* worldspace = tes->GetRuntimeData2().worldSpace;
		if (!worldspace && cell)
			worldspace = cell->GetRuntimeData().worldSpace;

		if (worldspace) {
			std::string worldspaceName = worldspace->GetFormEditorID();
			worldspaceIt = settings.worldspaceWhitelist.find(worldspaceName);
			worldspaceEnabled = worldspaceIt != settings.worldspaceWhitelist.end();
			if (worldspaceEnabled)
				zBottom = worldspaceIt->second.zBottom;
		}
	}
	bool allowForcedInterior = inInterior && settings.forceEnableAllInteriorCells;
	allGood &= (worldspaceEnabled || settings.enableAllExteriorCells || allowForcedInterior) && (!inInterior || allowForcedInterior) && !inMainLoadingMenu;

	if (!allGood) {
		cbData.enabled = allGood;
		linearLighting.isDirLightLinear = false;
		return;
	}

	// resolution
	float2 res{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = settings.sunlightColor,
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.apLumMix = settings.apLumMix,
		.masserColor = settings.masserColor,
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = cos(settings.sunDiskRad) * (settings.proceduralSun ? 1.f : 0.f),
		.secundaColor = settings.secundaColor,
		.enabled = allGood,
		.tonemapper = linearLighting.settings.enableLinearLighting ? 0 : settings.tonemapper,
		.vanillaMix = settings.vanillaMix,
		.zBottom = zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = settings.aerosolPhaseG,
		.aerosolScatter = settings.aerosolScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.halfResApShadow = settings.halfResApShadow ? 1u : 0u,
		.aerosolAbsorption = settings.aerosolAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = settings.rayleighScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = settings.ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = settings.ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = settings.ozoneAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.cloudRelightMix = settings.cloudRelightMix,
		.cloudOriginalMix = settings.cloudOriginalMix,
		.silverLiningMix = settings.silverLiningMix,
		.silverLiningSpread = settings.silverLiningSpread,
		.lightSkyStatics = settings.lightSkyStatics ? 1u : 0u,
		.skyStaticsBrightness = settings.skyStaticsBrightness,
		.pad0 = { 0u, 0u },
	};

	if (settings.overrideDirLight) {
		linearLighting.isDirLightLinear = true;
		const float pbrCompensationMult = linearLighting.settings.enableLinearLighting ? 1.0f : RE::NI_PI;  // Colors should match PBR values when not using linear lighting
		auto LightConvFn = [pbrCompensationMult](float3 color) {
			color /= pbrCompensationMult;
			return RE::NiColor(color.x, color.y, color.z);
		};
		skySync.lightColors = { LightConvFn(cbData.sunlightColor), LightConvFn(cbData.masserColor), LightConvFn(cbData.secundaColor) };
	} else {
		linearLighting.isDirLightLinear = false;
	}

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}

	// Windowed cloud sun-transmittance LUT (LUTGEN 4): center the mu axis on
	// the current sun zenith cosine and cover the whole cloud field, with a
	// margin for bilinear filtering. Radii go to the shader in game units to
	// match rPlanet; the normalized LUT axes are what the km-scale cloud
	// raymarcher samples.
	{
		// Planet-center-relative camera position in the cloud raymarcher's
		// convention (planet center on the world-origin vertical), in km.
		float3 posPlanetRelKm = { 0.f, 0.f, settings.planetRadius };
		if (auto* playerA = RE::PlayerCharacter::GetSingleton()) {
			const auto playerPos = playerA->GetPosition();
			posPlanetRelKm = float3(playerPos.x, playerPos.y, playerPos.z) * Util::Units::GAME_UNIT_TO_KM;
			posPlanetRelKm.z += settings.planetRadius;
		}
		const float posLen = std::sqrt(posPlanetRelKm.x * posPlanetRelKm.x + posPlanetRelKm.y * posPlanetRelKm.y + posPlanetRelKm.z * posPlanetRelKm.z);
		const float mu0 = (posPlanetRelKm.x * cbData.sunDir.x + posPlanetRelKm.y * cbData.sunDir.y + posPlanetRelKm.z * cbData.sunDir.z) / posLen;
		const float halfWindow = cloudSettings.maxDistance / settings.planetRadius + 0.02f;

		const float cloudBotKm = settings.planetRadius + cloudSettings.bottomRadius;
		// Guard against a degenerate/inverted layer from the UI; the LUT y axis
		// (and the cloud shader's uv remap) divide by the layer thickness.
		const float cloudTopKm = std::max(settings.planetRadius + cloudSettings.topRadius, cloudBotKm + 1e-3f);

		cbData.cloudTrMuMin = mu0 - halfWindow;
		cbData.cloudTrMuMax = mu0 + halfWindow;
		cbData.cloudTrRBot = cloudBotKm / Util::Units::GAME_UNIT_TO_KM;
		cbData.cloudTrRTop = cloudTopKm / Util::Units::GAME_UNIT_TO_KM;
	}
}

void PhysicalSky::EarlyPrepass()
{
	if (cbData.enabled) {
		GenerateLuts();
	}
}

void PhysicalSky::ReflectionsPrepass()
{
	if (cbData.enabled) {
		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::Prepass()
{
	if (cbData.enabled) {
		AccumShadow();

		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get(), texApShadow->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::GenerateLuts()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	constexpr auto debugStr = "Physical Sky: LUT Generation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto samplers = std::array{ sampTr.get(), sampSv.get(), sampNoise.get() };
		std::array<ID3D11ShaderResourceView*, 2> srvs = {};
		ID3D11UnorderedAccessView* uav = nullptr;

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());

		// -> transmittance
		uav = texTrLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csTrLutGen.get(), nullptr, 0);
		context->Dispatch((kTrLutW + 7) >> 3, (kTrLutH + 7) >> 3, 1);

		// -> windowed cloud sun transmittance (needs only the cbuffer)
		uav = texCloudSunTr->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csCloudTrLutGen.get(), nullptr, 0);
		context->Dispatch((kCloudTrLutW + 7) >> 3, (kCloudTrLutH + 7) >> 3, 1);

		// -> multiscatter
		uav = texMsLut->uav.get();
		srvs.at(0) = texTrLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csMsLutGen.get(), nullptr, 0);
		context->Dispatch((kMsLutW + 7) >> 3, (kMsLutH + 7) >> 3, 1);

		// -> cloud ambient endpoints (samples the Tr and Ms LUTs)
		uav = texCloudAmbient->uav.get();
		srvs.at(1) = texMsLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csCloudAmbLutGen.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);

		// -> sky-view
		uav = texSvLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csSvLutGen.get(), nullptr, 0);
		context->Dispatch((kSvLutW + 7) >> 3, (kSvLutH + 7) >> 3, 1);

		// -> aerial perspective
		uav = texApLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csApLutGen.get(), nullptr, 0);
		context->Dispatch((kApLutW + 7) >> 3, (kApLutH + 7) >> 3, 1);

		/* ---- RESTORE ---- */
		samplers.fill(nullptr);
		srvs.fill(nullptr);
		uav = nullptr;

		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::AccumShadow()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	auto& volumetricShadows = globals::features::volumetricShadows;
	if (!volumetricShadows.loaded)
		return;
	auto& terrainShadows = globals::features::terrainShadows;
	auto& cloudShadows = globals::features::cloudShadows;

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 size = Util::ConvertToDynamic(screenSize);
	uint resolution[2] = { (uint)size.x, (uint)size.y };

	constexpr auto debugStr = "Physical Sky: Shadow Accumulation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto sampler = sampTr.get();
		ID3D11ShaderResourceView* directionalShadowLights = nullptr;
		if (auto* directionalShadowBuffer = Deferred::GetSingleton()->directionalShadowLights)
			directionalShadowLights = directionalShadowBuffer->srv.get();
		auto srvs = std::array{
			globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,
			volumetricShadows.shadowView,
			static_cast<ID3D11ShaderResourceView*>(nullptr),
			terrainShadows.IsHeightMapReady() ? terrainShadows.texShadowHeight->srv.get() : nullptr,
			cloudShadows.loaded ? cloudShadows.texCloudShadowLayers[CloudShadows::kMaxCloudLayers - 1]->srv.get() : nullptr,
		};
		auto uav = texApShadow->uav.get();

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShaderResources(98, 1, &directionalShadowLights);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(settings.halfResApShadow ? csShadowAccumHalfRes.get() : csShadowAccum.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		/* ---- RESTORE ---- */
		sampler = nullptr;
		srvs.fill(nullptr);
		directionalShadowLights = nullptr;
		uav = nullptr;

		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShaderResources(98, 1, &directionalShadowLights);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::ModifySky()
{
	auto context = globals::d3d::context;

	context->PSGetSamplers(3, 2, originalPSSamplers);

	auto samplers = std::array{ sampTr.get(), sampSv.get() };
	context->PSSetSamplers(3, static_cast<UINT>(samplers.size()), samplers.data());
}

void PhysicalSky::RestoreSamplers()
{
	auto context = globals::d3d::context;
	context->PSSetSamplers(3, 2, originalPSSamplers);
}

void PhysicalSky::Hooks::RenderSky::thunk()
{
	func();

	globals::state->BeginPerfEvent("Render Clouds");
	globals::features::physicalSky.RenderClouds();
	globals::state->EndPerfEvent();

	globals::state->BeginPerfEvent("Clouds Compose");
	globals::features::physicalSky.CloudCompose();
	globals::state->EndPerfEvent();
}

void PhysicalSky::Hooks::BSSkyShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.ModifySky();
	func(This, Pass, RenderFlags);
}

void PhysicalSky::Hooks::BSSkyShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.RestoreSamplers();
	func(This, Pass, RenderFlags);
}

#pragma warning(push)
#pragma warning(disable: 4244)
void PhysicalSky::CreateCloudResources()
{
	auto device = globals::d3d::device;

	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\dataFields.png", nullptr, dataFieldsSRV.put());
	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\vertProfile.png", nullptr, vertProfileSRV.put());
	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\noiseShape.png", nullptr, noiseShapeSRV.put());
	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\cirrusShape.png", nullptr, cirrusShapeSRV.put());

	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\CurlNoise.png", nullptr, curlNoiseSRV.put());
	DirectX::CreateWICTextureFromFile(device, L"Data\\Shaders\\PhysicalSky\\textures\\WeatherMap.png", nullptr, weatherMapSRV.put());
	DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\PhysicalSky\\textures\\CloudBase.dds", nullptr, cloudBaseSRV.put());
	DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\PhysicalSky\\textures\\CloudDetail.dds", nullptr, cloudDetailSRV.put());

	{
		CD3D11_TEXTURE2D_DESC texDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, CLOUD_TEX_SIZE.x, CLOUD_TEX_SIZE.y, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);  // change format later. rgba8 unorm?
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 1, 0, 1);
		CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 1);

		cloudColorTex[0] = eastl::make_unique<Texture2D>(texDesc);
		cloudColorTex[0]->CreateSRV(srvDesc);
		cloudColorTex[0]->CreateRTV(rtvDesc);

		cloudColorTex[1] = eastl::make_unique<Texture2D>(texDesc);
		cloudColorTex[1]->CreateSRV(srvDesc);
		cloudColorTex[1]->CreateRTV(rtvDesc);
	}

	{
		CD3D11_TEXTURE2D_DESC texDesc(DXGI_FORMAT_R32_FLOAT, CLOUD_TEX_SIZE.x, CLOUD_TEX_SIZE.y, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);  // change format later. rgba8 unorm?
		CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 1, 0, 1);
		CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, texDesc.Format, 0, 1);

		cloudDepthTex[0] = eastl::make_unique<Texture2D>(texDesc);
		cloudDepthTex[0]->CreateSRV(srvDesc);
		cloudDepthTex[0]->CreateRTV(rtvDesc);

		cloudDepthTex[1] = eastl::make_unique<Texture2D>(texDesc);
		cloudDepthTex[1]->CreateSRV(srvDesc);
		cloudDepthTex[1]->CreateRTV(rtvDesc);

		disoccTex = eastl::make_unique<Texture2D>(texDesc);
		disoccTex->CreateSRV(srvDesc);
		disoccTex->CreateRTV(rtvDesc);
	}

	cloudBuffer = new ConstantBuffer(ConstantBufferDesc<CloudCB>());
	cloudDebugBuffer = new ConstantBuffer(ConstantBufferDesc<CloudDebugCB>(), "PhysicalSky::CloudDebugCB");
}
#pragma warning(pop)

// Resolve what the horizon weather front should show this frame: which cloud
// shape is departing, which is arriving, and how far the front has swept.
// The registry time-lerps every other weather variable in place; these three
// blend SPATIALLY in the shader instead, so the endpoints are reconstructed
// here rather than consumed post-lerp.
PhysicalSky::WeatherFrontState PhysicalSky::ResolveWeatherFront()
{
	WeatherFrontState state{};
	state.from = state.to = { cloudSettings.coverage, cloudSettings.cloudType, cloudSettings.coverage2 };

	if (weatherFrontTest.enabled) {
		// Manual driver: blend from the live shape to the test values.
		state.to = { weatherFrontTest.coverageIn, weatherFrontTest.cloudTypeIn, weatherFrontTest.coverage2In };
		state.transition = weatherFrontTest.transition;
		state.active = true;
		return state;
	}

	auto* weatherManager = WeatherManager::GetSingleton();
	const auto weathers = weatherManager->GetCurrentWeathers();

	const bool transitioning = weathers.currentWeather && weathers.lastWeather &&
	                           weathers.currentWeather != weathers.lastWeather &&
	                           weathers.lerpFactor < 1.0f;

	if (!transitioning) {
		// Stable: remember the effective shape (any current-weather override
		// has already been applied to cloudSettings by the registry) so the
		// next transition departs from it. Only previous-frame values are
		// consumed when a transition starts, so this is independent of the
		// WeatherManager update order within the frame.
		weatherStableShape = state.from;
		weatherTrackedFrom = weathers.lastWeather;
		weatherTrackedTo = weathers.currentWeather;
		weatherFrontLive = false;
		return state;
	}

	if (weathers.currentWeather != weatherTrackedTo || weathers.lastWeather != weatherTrackedFrom) {
		// New transition: freeze the departure shape. If one was already in
		// flight, fold its progress in (a global approximation of the
		// half-swept field); otherwise depart from the last stable shape.
		if (weatherFrontLive) {
			weatherFromShape.coverage = std::lerp(weatherFromShape.coverage, weatherToShape.coverage, weatherLastLerp);
			weatherFromShape.cloudType = std::lerp(weatherFromShape.cloudType, weatherToShape.cloudType, weatherLastLerp);
			weatherFromShape.coverage2 = std::lerp(weatherFromShape.coverage2, weatherToShape.coverage2, weatherLastLerp);
		} else {
			weatherFromShape = weatherStableShape;
		}
		weatherTrackedFrom = weathers.lastWeather;
		weatherTrackedTo = weathers.currentWeather;
	}

	// Arrival shape: the incoming weather's override where present, the user
	// (non-weather) values where not -- mirroring the registry's fallback.
	json toJson;
	state.to = weatherUserShape;
	if (weatherManager->LoadSettingsFromWeather(weathers.currentWeather, GetShortName(), toJson)) {
		state.to.coverage = toJson.value(kWeatherKeyCoverage, state.to.coverage);
		state.to.cloudType = toJson.value(kWeatherKeyCloudType, state.to.cloudType);
		state.to.coverage2 = toJson.value(kWeatherKeyCoverage2, state.to.coverage2);
	}
	state.from = weatherFromShape;
	state.transition = weathers.lerpFactor;

	// A front is only worth drawing if the shape actually changes.
	state.active = std::abs(state.from.coverage - state.to.coverage) > 1e-3f ||
	               std::abs(state.from.cloudType - state.to.cloudType) > 1e-3f ||
	               std::abs(state.from.coverage2 - state.to.coverage2) > 1e-3f;

	weatherToShape = state.to;
	weatherLastLerp = state.transition;
	weatherFrontLive = true;
	return state;
}

void PhysicalSky::RenderClouds()
{
	// The cloud lighting chain depends on physSkyData and the per-frame LUTs
	// (LUTGEN 4/5), which are only valid while the feature is active.
	if (!cbData.enabled)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	static int frameCount = 0;

	par = !par;

	D3D11_VIEWPORT port;
	port.MinDepth = port.TopLeftX = port.TopLeftY = 0.0;
	port.Width = CLOUD_TEX_SIZE.x;
	port.Height = CLOUD_TEX_SIZE.y;
	port.MaxDepth = 1.0;

	context->RSSetViewports(1, &port);

	context->RSSetState(rasterState);

	context->VSSetShader(cloudVShader, nullptr, NULL);
	context->PSSetShader(cloudShader, nullptr, NULL);

	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	ID3D11RenderTargetView* rtvs[] = { cloudColorTex[par]->rtv.get(), cloudDepthTex[par]->rtv.get() };

	float clear[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(rtvs[0], clear);
	context->ClearRenderTargetView(rtvs[1], clear);
	context->OMSetRenderTargets(2, rtvs, nullptr);

	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ID3D11ShaderResourceView* srvs[] = {
		depthTexture.depthSRV,
		disoccTex->srv.get(),
		cloudColorTex[!par]->srv.get(),
		cloudDepthTex[!par]->srv.get(),
		dataFieldsSRV.get(),
		cirrusShapeSRV.get(),
		vertProfileSRV.get(),
		noiseShapeSRV.get(),
		cloudBaseSRV.get(),
		cloudDetailSRV.get(),
		curlNoiseSRV.get(),
		weatherMapSRV.get()
	};
	context->PSSetShaderResources(0, 12, srvs);

	// Lighting LUTs: windowed sun transmittance + ambient endpoints (t18/t19),
	// and the global Tr LUT (t61) for the DebugSunTrMode == 3 A/B path.
	ID3D11ShaderResourceView* lutSrvs[] = { texCloudSunTr->srv.get(), texCloudAmbient->srv.get() };
	context->PSSetShaderResources(18, 2, lutSrvs);
	ID3D11ShaderResourceView* trLutSrv = texTrLut->srv.get();
	context->PSSetShaderResources(61, 1, &trLutSrv);

	auto bayerIndex = bayerIndices4x4[frameCount % 16];
	auto playerPos = RE::PlayerCharacter::GetSingleton()->GetPosition();

	CloudCB cb{};
	cb.cameraPos = float3(playerPos.x, playerPos.y, playerPos.z);
	cb.bayerPos = { float(bayerIndex % 4), (float)bayerIndex / 4 };
	cb.groundRadius = settings.planetRadius;
	cb.atmTopRadius = settings.atmosphereRadius;
	cb.bottomRadius = settings.planetRadius + cloudSettings.bottomRadius;
	cb.topRadius = settings.planetRadius + cloudSettings.topRadius;

	// The sky LUTs (physSkyData) work in game units; this cbuffer works in km.
	// Both must describe the same planet and cloud layer -- a scale mismatch
	// does not crash, it just silently produces slightly wrong cloud colors.
	if (cbData.enabled) {
		const auto sameScale = [](float gameUnits, float km) {
			return std::abs(gameUnits * Util::Units::GAME_UNIT_TO_KM - km) <= 0.5f;
		};
		assert(sameScale(cbData.rPlanet, cb.groundRadius));
		assert(sameScale(cbData.cloudTrRBot, cb.bottomRadius));
		assert(sameScale(cbData.cloudTrRTop, std::max(cb.topRadius, cb.bottomRadius + 1e-3f)));
		static bool loggedScaleMismatch = false;
		if (!loggedScaleMismatch &&
			(!sameScale(cbData.rPlanet, cb.groundRadius) ||
				!sameScale(cbData.cloudTrRBot, cb.bottomRadius) ||
				!sameScale(cbData.cloudTrRTop, std::max(cb.topRadius, cb.bottomRadius + 1e-3f)))) {
			loggedScaleMismatch = true;
			logger::error(
				"PhysicalSky: sky-LUT radii (game units) and cloud radii (km) disagree "
				"(rPlanet {} gu vs groundRadius {} km, layer [{}, {}] gu vs [{}, {}] km); "
				"cloud sunset colors will be wrong",
				cbData.rPlanet, cb.groundRadius,
				cbData.cloudTrRBot, cbData.cloudTrRTop, cb.bottomRadius, cb.topRadius);
		}
	}
	cb.minDistance = cloudSettings.minDistance;
	cb.maxDistance = cloudSettings.maxDistance;

	// Horizon weather front. While a front is active the shader blends the
	// shape params spatially from the frozen departure values (base slots) to
	// the arrival values (*In slots); the registry's in-place time-lerp of
	// cloudSettings is deliberately not consumed for these three during that
	// window.
	const auto front = ResolveWeatherFront();
	cb.coverage = front.active ? front.from.coverage : cloudSettings.coverage;
	cb.cloudType = front.active ? front.from.cloudType : cloudSettings.cloudType;
	cb.coverage2 = front.active ? front.from.coverage2 : cloudSettings.coverage2;
	cb.coverageIn = front.to.coverage;
	cb.cloudTypeIn = front.to.cloudType;
	cb.coverage2In = front.to.coverage2;
	cb.windDir = kWeatherWindDir;
	cb.weatherFrontWidth = std::max(cloudSettings.weatherFrontWidth, 0.1f);
	// Sweep the front from just beyond the upwind horizon to just past the
	// downwind one, so t = 0 shows only outgoing and t = 1 only incoming.
	const float frontReach = cb.maxDistance + cb.weatherFrontWidth;
	cb.weatherFrontPos = std::lerp(-frontReach, frontReach, std::clamp(front.transition, 0.f, 1.f));
	cb.weatherBlendActive = front.active ? 1.f : 0.f;

	cb.heightScale = 1.0f - std::clamp(cloudSettings.heightScale, 0.0f, 1.0f);
	cb.scroll = cloudSettings.scroll;
	cb.detailFrequency = std::max(cloudSettings.detailFrequency, 0.f);
	// Full-range erosion would let detail delete the base shape entirely.
	cb.detailStrength = std::clamp(cloudSettings.detailStrength, 0.f, 0.9f);
	cb.detailCurlScale = std::max(cloudSettings.detailCurlScale, 0.f);
	cb.detailCurlStrength = std::max(cloudSettings.detailCurlStrength, 0.f);
	// The shader's fade remap divides by (end - start).
	cb.detailFadeStart = std::max(cloudSettings.detailFadeStart, 0.f);
	cb.detailFadeEnd = std::max(cloudSettings.detailFadeEnd, cb.detailFadeStart + 0.1f);
	cloudBuffer->Update(cb);

	CloudDebugCB debugCb{};
	debugCb.debugSunTrMode = cloudLighting.debugSunTrMode;
	debugCb.debugAmbientMode = cloudLighting.debugAmbientMode;
	debugCb.debugColor = cloudLighting.debugColor;
	debugCb.sunGain = cloudLighting.sunGain;
	debugCb.ambientGain = cloudLighting.ambientGain;
	debugCb.cloudTrMuMin = cbData.cloudTrMuMin;
	debugCb.cloudTrMuMax = cbData.cloudTrMuMax;
	// Converting from the game-unit values LUTGEN 4 used guarantees the shader
	// remaps onto exactly the generated axes.
	debugCb.cloudTrRBot = cbData.cloudTrRBot * Util::Units::GAME_UNIT_TO_KM;
	debugCb.cloudTrRTop = cbData.cloudTrRTop * Util::Units::GAME_UNIT_TO_KM;
	debugCb.octaveAttenA = cloudLighting.octaveAttenA;
	// The shader divides by extinction (albedo) and single-scatter albedo
	// cannot exceed 1; enforce both no matter what the UI fed us.
	debugCb.cloudExtinction = std::max(cloudLighting.cloudExtinction, 1e-3f);
	debugCb.cloudScattering = std::clamp(cloudLighting.cloudScattering, 0.f, debugCb.cloudExtinction);
	cloudDebugBuffer->Update(debugCb);

	ID3D11Buffer* buffers[2] = { cloudBuffer->CB(), cloudDebugBuffer->CB() };
	context->PSSetConstantBuffers(0, 2, buffers);

	ID3D11SamplerState* samplers[2] = { sampTr.get(), sampNoise.get() };
	context->PSSetSamplers(0, 2, samplers);

	context->Draw(3, 0);

	ID3D11ShaderResourceView* nullSrvs[] = {
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
	};
	context->PSSetShaderResources(0, 12, nullSrvs);
	context->PSSetShaderResources(18, 2, nullSrvs);
	context->PSSetShaderResources(61, 1, nullSrvs);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RENDERTARGET, RE::BSGraphics::DIRTY_VIEWPORT);

	++frameCount;
}

void PhysicalSky::CloudCompose()
{
	if (!cbData.enabled)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	float2 size = Util::ConvertToDynamic(screenSize);

	D3D11_VIEWPORT port;
	port.MinDepth = port.TopLeftX = port.TopLeftY = 0.0;
	port.Width = size.x;
	port.Height = size.y;
	port.MaxDepth = 1.0;

	context->RSSetViewports(1, &port);

	context->VSSetShader(cloudVShader, nullptr, NULL);
	context->PSSetShader(cloudBlendShader, nullptr, NULL);

	float f[4] = { 1, 1, 1, 1 };
	context->OMSetBlendState(additiveBlend, f, 0xFFFFFFFF);

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	context->OMSetRenderTargets(1, &main.RTV, nullptr);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	ID3D11ShaderResourceView* srv[] = { cloudColorTex[par]->srv.get(), cloudDepthTex[par]->srv.get(), depth.depthSRV };
	context->PSSetShaderResources(0, 3, srv);

	context->Draw(3, 0);

	// Verification overlay: the windowed sun-Tr LUT scaled up 4x, plus the two
	// ambient endpoint texels as swatches, in the top-left screen corner.
	if (cloudLighting.showDebugOverlay && cloudDebugBlitShader) {
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context->PSSetShader(cloudDebugBlitShader, nullptr, NULL);

		constexpr float overlayMargin = 16.f;
		constexpr float overlayScale = 4.f;

		D3D11_VIEWPORT lutPort;
		lutPort.MinDepth = 0.f;
		lutPort.MaxDepth = 1.f;
		lutPort.TopLeftX = overlayMargin;
		lutPort.TopLeftY = overlayMargin;
		lutPort.Width = kCloudTrLutW * overlayScale;
		lutPort.Height = kCloudTrLutH * overlayScale;
		context->RSSetViewports(1, &lutPort);

		ID3D11ShaderResourceView* blitSrv = texCloudSunTr->srv.get();
		context->PSSetShaderResources(0, 1, &blitSrv);
		context->Draw(3, 0);

		D3D11_VIEWPORT swatchPort = lutPort;
		swatchPort.TopLeftY = overlayMargin + lutPort.Height + 8.f;
		swatchPort.Width = 96.f;  // two 48px swatches: bottom | top
		swatchPort.Height = 48.f;
		context->RSSetViewports(1, &swatchPort);

		blitSrv = texCloudAmbient->srv.get();
		context->PSSetShaderResources(0, 1, &blitSrv);
		context->Draw(3, 0);

		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}

	ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 3, nullSrvs);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RENDERTARGET, RE::BSGraphics::DIRTY_VIEWPORT);
}
