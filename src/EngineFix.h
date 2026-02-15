#pragma once

struct ShadowmapCascadeCullingFix;
struct ShadowmapRasterizerFix;
struct ShadowmapMatrixFix;

struct EngineFix
{
	virtual ~EngineFix() = default;

	virtual std::string GetName() = 0;

	virtual void Install() {}

	static void InstallOnPostPostLoadFixes();
	static void InstallOnDataLoadedFixes();

	static inline bool installed = false;

private:
	static const std::vector<EngineFix*>& GetOnPostPostLoadFixesList();
	static const std::vector<EngineFix*>& GetOnDataLoadedFixesList();
	static void InstallFixes(const std::vector<EngineFix*>& fixes);

public:
	template <typename T>
	static inline T& GetEngineFix(int index)
	{
		auto& fixes = GetOnPostPostLoadFixesList();
		return *static_cast<T*>(fixes[index]);
	}
};
