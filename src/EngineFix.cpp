#include "EngineFix.h"

#include "EngineFixes/ShadowmapCascadeCullingFix.h"
#include "EngineFixes/ShadowmapCascadeMatrixFix.h"
#include "EngineFixes/ShadowmapCascadeRasterizerFix.h"

const std::vector<EngineFix*>& EngineFix::GetOnPostPostLoadFixesList()
{
	static ShadowmapMatrixFix shadowmapMatrixFix;
	static ShadowmapRasterizerFix shadowmapRasterizerFix;
	static ShadowmapCascadeCullingFix shadowmapCascadeCullingFix;

	static std::vector<EngineFix*> fixes = {
		&shadowmapMatrixFix,
		&shadowmapRasterizerFix,
		&shadowmapCascadeCullingFix,
	};

	return fixes;
}

const std::vector<EngineFix*>& EngineFix::GetOnDataLoadedFixesList()
{
	static std::vector<EngineFix*> fixes = {};

	return fixes;
}

void EngineFix::InstallFixes(const std::vector<EngineFix*>& fixes)
{
	for (const auto fix : fixes) {
		if (REL::Module::IsVR() && !fix->SupportsVR())
			continue;

		if ((fix->installed = fix->Install()))
			logger::info("[Engine Fixes] Installed {}", fix->GetName());
	}
}

void EngineFix::InstallOnPostPostLoadFixes()
{
	InstallFixes(GetOnPostPostLoadFixesList());
}

void EngineFix::InstallOnDataLoadedFixes()
{
	InstallFixes(GetOnDataLoadedFixesList());
}
