#pragma once

struct ShadowmapCascadeCullingFix;
struct ShadowmapRasterizerFix;
struct ShadowmapMatrixFix;

struct EngineFix
{
	virtual ~EngineFix() = default;

	virtual std::string GetName() = 0;
	virtual bool SupportsVR() = 0;

	virtual bool Install() = 0;

	static void InstallOnPostPostLoadFixes();
	static void InstallOnDataLoadedFixes();

	bool installed = false;

private:
	static const std::vector<EngineFix*>& GetOnPostPostLoadFixesList();
	static const std::vector<EngineFix*>& GetOnDataLoadedFixesList();
	static void InstallFixes(const std::vector<EngineFix*>& fixes);

public:
	template <typename T>
	static inline T& GetPPLEngineFix(int index)
	{
		auto& fixes = GetOnPostPostLoadFixesList();
		return *static_cast<T*>(fixes[index]);
	}
};

// Need to add bool for when all post post load fixes are installed