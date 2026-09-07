#include "TerrainHeightSource.h"

#include "Utils/Game.h"

namespace
{
	// VHGT is a fixed 1096 byte payload: a base offset, a 33x33 grid of signed deltas, three pad bytes.
	struct VHGTData
	{
		float offset;
		std::int8_t deltas[TexGenLand::landVertCount];
		std::uint8_t padding[3];
	};
	static_assert(sizeof(VHGTData) == 1096);

	constexpr float kHeightScale = 8.0f;  // LAND deltas are stored in eighths of a game unit
	constexpr int kMaxWorldspaceCells = 512;
	// A cell's children are bounded by the next CELL record, which the walk stops at. This only
	// guards against a malformed plugin leaving the walk unbounded.
	constexpr int kMaxCellChildForms = 65536;

	float ByteSwapIfNeeded(RE::TESFile* a_file, float a_value)
	{
		if (!a_file->isBigEndian)
			return a_value;
		return std::bit_cast<float>(_byteswap_ulong(std::bit_cast<std::uint32_t>(a_value)));
	}

	// Cell records use sentinels and occasionally garbage for absent water, and a bad value here
	// would clamp a whole cell flat. Same guard WaterCache applies.
	bool IsUsableWaterHeight(float a_height)
	{
		return std::isfinite(a_height) && a_height != FLT_MIN && a_height != FLT_MAX && std::fabs(a_height) < 50000.0f;
	}
}

namespace TexGenLand
{
	LandFileSet::LandFileSet(RE::TESWorldSpace* a_worldSpace) :
		worldSpace(a_worldSpace)
	{
		if (!worldSpace)
			return;

		auto* sourceFiles = worldSpace->sourceFiles.array;
		if (!sourceFiles)
			return;

		files.reserve(sourceFiles->size());
		for (auto* file : *sourceFiles) {
			// Never seek the engine's own TESFile - its cursor is shared with cell streaming.
			if (auto* duplicate = file ? file->Duplicate() : nullptr)
				files.push_back(duplicate);
		}
	}

	LandFileSet::~LandFileSet()
	{
		for (auto* file : files)
			file->CloseTES(true);
	}

	bool LandFileSet::ReadCell(int a_cellX, int a_cellY, CellHeights& o_out)
	{
		o_out = {};
		if (!Valid())
			return false;

		const auto x = static_cast<std::int32_t>(a_cellX);
		const auto y = static_cast<std::int32_t>(a_cellY);

		// Phase one: the winning CELL is the one in the highest priority plugin that defines it.
		// Its XCLW is the water height that a later plugin's edit would have overridden.
		int index = static_cast<int>(files.size()) - 1;
		float waterHeight = FLT_MAX;
		for (; index >= 0; --index) {
			if (!files[index]->SeekCell(worldSpace, x, y))
				continue;

			o_out.hasCell = true;
			if (files[index]->SeekNextSubrecordType(Util::FCC("XCLW"))) {
				float raw = 0.0f;
				if (files[index]->ReadData(&raw, sizeof(raw)))
					waterHeight = ByteSwapIfNeeded(files[index], raw);
			}
			break;
		}

		if (!o_out.hasCell)
			return false;

		o_out.waterHeight = IsUsableWaterHeight(waterHeight) ? waterHeight : worldSpace->defaultWaterHeight;
		if (!IsUsableWaterHeight(o_out.waterHeight))
			o_out.waterHeight = -FLT_MAX;  // no usable water plane, so the clamp becomes a no-op

		// Phase two: carry on from the same plugin. A file cannot hold a LAND child without holding
		// the CELL, so the first one from here down that has both wins.
		for (; index >= 0; --index) {
			if (!files[index]->SeekCell(worldSpace, x, y) || !files[index]->SeekLandscapeForCurrentCell())
				continue;
			if (!files[index]->SeekNextSubrecordType(Util::FCC("VHGT")))
				continue;

			VHGTData data{};
			if (!files[index]->ReadData(&data, sizeof(data)))
				continue;

			float rowBase = ByteSwapIfNeeded(files[index], data.offset);
			for (int row = 0; row < landGridEdge; ++row) {
				// The first delta of each row advances the running column-zero height; the rest
				// accumulate along the row from there.
				rowBase += static_cast<float>(data.deltas[row * landGridEdge]);

				float running = 0.0f;
				for (int col = 0; col < landGridEdge; ++col) {
					if (col != 0)
						running += static_cast<float>(data.deltas[row * landGridEdge + col]);
					o_out.heights[row * landGridEdge + col] = (running + rowBase) * kHeightScale;
				}
			}

			o_out.hasLand = true;
			break;
		}

		if (!o_out.hasLand)
			o_out.heights.fill(worldSpace->defaultLandHeight);

		return o_out.hasLand;
	}

	bool ResolveCellBounds(RE::TESWorldSpace* a_worldSpace, int2& o_minCell, int2& o_maxCell, const char*& o_source, bool a_log)
	{
		o_source = "none";
		if (!a_worldSpace || a_worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoLandscape))
			return false;

		auto plausible = [](const int2& min, const int2& max) {
			const int width = max.x - min.x + 1;
			const int height = max.y - min.y + 1;
			return width > 0 && height > 0 && width <= kMaxWorldspaceCells && height <= kMaxWorldspaceCells;
		};

		// NAM0/NAM9 are the worldspace's own corners in world units, and the maximum is exclusive.
		// For Tamriel this gives -57,-43 to 61,50, the extent DynDOLOD and the shipped terrain
		// heightmaps also use.
		std::int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
		Util::WorldToCell(a_worldSpace->minimumCoords, minX, minY);
		Util::WorldToCell(a_worldSpace->maximumCoords, maxX, maxY);
		const int2 boundsMin{ minX, minY };
		const int2 boundsMax{ maxX - 1, maxY - 1 };

		// MNAM is the *world map* extent, the region the map screen draws rather than the region
		// that has terrain. For Tamriel it reports -30,-40 to 40,15, barely half the worldspace, so
		// it is a fallback rather than the preferred answer.
		const auto& map = a_worldSpace->worldMapData;
		const int2 mapMin{ map.nwCellX, map.seCellY };
		const int2 mapMax{ map.seCellX, map.nwCellY };

		// The cells the plugins actually defined. Only the ones the engine has instantiated appear,
		// so this under-reports and is scanned last, and only when something needs it.
		int2 cellMapMin{ 0, 0 };
		int2 cellMapMax{ 0, 0 };
		bool anyCells = false;
		auto scanCellMap = [&]() {
			for (const auto& [key, cell] : a_worldSpace->cellMap) {
				const int2 coords{ key.x, key.y };
				if (!anyCells) {
					cellMapMin = cellMapMax = coords;
					anyCells = true;
					continue;
				}
				cellMapMin = int2{ std::min(cellMapMin.x, coords.x), std::min(cellMapMin.y, coords.y) };
				cellMapMax = int2{ std::max(cellMapMax.x, coords.x), std::max(cellMapMax.y, coords.y) };
			}
		};

		if (a_log) {
			scanCellMap();
			logger::info("[TexGen] Bounds for {}: NAM0/NAM9 {},{} to {},{} | MNAM {},{} to {},{} | cellMap {},{} to {},{} ({} cells known)",
				a_worldSpace->GetFormEditorID(),
				boundsMin.x, boundsMin.y, boundsMax.x, boundsMax.y,
				mapMin.x, mapMin.y, mapMax.x, mapMax.y,
				cellMapMin.x, cellMapMin.y, cellMapMax.x, cellMapMax.y,
				a_worldSpace->cellMap.size());
		}

		if (plausible(boundsMin, boundsMax)) {
			o_minCell = boundsMin;
			o_maxCell = boundsMax;
			o_source = "NAM0/NAM9";
			return true;
		}

		if (plausible(mapMin, mapMax)) {
			o_minCell = mapMin;
			o_maxCell = mapMax;
			o_source = "MNAM";
			return true;
		}

		if (!anyCells)
			scanCellMap();

		if (anyCells && plausible(cellMapMin, cellMapMax)) {
			o_minCell = cellMapMin;
			o_maxCell = cellMapMax;
			o_source = "cellMap";
			return true;
		}

		return false;
	}

	size_t LandFileSet::ReadCellReferences(int a_cellX, int a_cellY, std::vector<CellReference>& o_out, bool a_log)
	{
		o_out.clear();
		if (!Valid())
			return 0;

		const std::uint32_t cellSignature = Util::FCC("CELL");
		const std::uint32_t refrSignature = Util::FCC("REFR");

		// Keyed by the reference's own form ID so a later plugin's edit replaces the earlier
		// placement rather than adding a second copy of the same object.
		std::unordered_map<RE::FormID, CellReference> merged;

		const auto x = static_cast<std::int32_t>(a_cellX);
		const auto y = static_cast<std::int32_t>(a_cellY);

		// Ascending load order: every plugin that defines the cell contributes, and the last one to
		// touch a given reference wins.
		for (size_t index = 0; index < files.size(); ++index) {
			auto* file = files[index];
			if (!file->SeekCell(worldSpace, x, y))
				continue;

			size_t added = 0;
			size_t replaced = 0;
			int formsWalked = 0;

			while (formsWalked < kMaxCellChildForms && file->SeekNextForm(true)) {
				++formsWalked;

				const std::uint32_t signature = file->currentform.form;
				if (signature == cellSignature)
					break;  // the children of this cell have ended
				if (signature != refrSignature)
					continue;

				CellReference reference;
				reference.runtimeRefID = file->GetRuntimeFormID(file->currentform.formID);
				reference.recordFlags = file->currentform.flags;

				// Subrecord order is not fixed and any of these may be absent, so walk once and take
				// whatever turns up rather than seeking each by type: a missing one would leave the
				// cursor at the end of the record and put the rest out of reach.
				RE::FormID rawBaseID = 0;
				do {
					const std::uint32_t subrecord = file->GetCurrentSubRecordType();
					if (subrecord == Util::FCC("NAME")) {
						file->ReadData(&rawBaseID, sizeof(rawBaseID));
					} else if (subrecord == Util::FCC("XSCL")) {
						file->ReadData(&reference.scale, sizeof(reference.scale));
					} else if (subrecord == Util::FCC("DATA") && file->GetCurrentSubRecordSize() >= 24) {
						float placement[6] = {};
						file->ReadData(placement, sizeof(placement));
						reference.position = RE::NiPoint3(placement[0], placement[1], placement[2]);
						reference.rotation = RE::NiPoint3(placement[3], placement[4], placement[5]);
					}
				} while (file->SeekNextSubrecord());

				reference.runtimeBaseID = file->GetRuntimeFormID(rawBaseID);

				if (merged.insert_or_assign(reference.runtimeRefID, reference).second)
					++added;
				else
					++replaced;
			}

			if (a_log && (added || replaced))
				logger::info("[TexGen]   {}: +{} new, {} overridden ({} forms)", file->GetFilename(), added, replaced, formsWalked);
		}

		o_out.reserve(merged.size());
		size_t hidden = 0;
		for (auto& [refID, reference] : merged) {
			if (reference.IsHidden())
				++hidden;
			o_out.push_back(reference);
		}

		if (a_log)
			logger::info("[TexGen]   merged {} references for cell {}, {} ({} disabled or deleted)", o_out.size(), a_cellX, a_cellY, hidden);

		return o_out.size();
	}

	bool IsExcludedModelPath(const char* a_modelPath)
	{
		if (!a_modelPath || !*a_modelPath)
			return true;  // nothing to rasterise

		// Leading directory only, matched case insensitively against how the game stores paths.
		static constexpr std::array<std::string_view, 3> excludedRoots = {
			"markers\\",  // critter and editor markers, invisible and collisionless
			"effects\\",  // mist, fog and other ambient planes
			"plants\\",   // nirnroot and friends, placed as Activators
		};

		std::string path(a_modelPath);
		std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		std::replace(path.begin(), path.end(), '/', '\\');

		for (const auto& root : excludedRoots)
			if (path.starts_with(root))
				return true;

		return false;
	}

	bool IsHeightContributingBase(const RE::TESForm* a_base)
	{
		if (!a_base)
			return false;

		switch (a_base->GetFormType()) {
		// Placed world geometry that sits on kStatic, which is what the raycast accepted. Containers,
		// furniture and doors are left out: they are solid, but none of them are large enough to
		// register once the derived maps flatten relief below 250 units.
		case RE::FormType::Static:
		case RE::FormType::MovableStatic:
		case RE::FormType::Activator:
			break;

		// Everything else is either kTrees, has no collision worth rasterising, or is not scenery:
		// trees, flora, lights, markers, actors, items.
		default:
			return false;
		}

		auto* model = skyrim_cast<const RE::TESModel*>(const_cast<RE::TESForm*>(a_base));
		return model && !IsExcludedModelPath(model->GetModel());
	}

	float BaseBoundExtent(const RE::TESForm* a_base)
	{
		auto* bound = a_base ? skyrim_cast<const RE::TESBoundObject*>(a_base) : nullptr;
		if (!bound)
			return 0.0f;

		const auto& bounds = bound->boundData;
		const float extentX = (float)(bounds.boundMax.x - bounds.boundMin.x);
		const float extentY = (float)(bounds.boundMax.y - bounds.boundMin.y);
		const float extentZ = (float)(bounds.boundMax.z - bounds.boundMin.z);
		return std::max({ extentX, extentY, extentZ });
	}

	size_t SpikeDumpCellReferences(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY)
	{
		if (!a_worldSpace)
			return 0;

		LandFileSet files(a_worldSpace);
		if (!files.Valid()) {
			logger::error("[TexGen] Reference dump: no source files for {}", a_worldSpace->GetFormEditorID());
			return 0;
		}

		logger::info("[TexGen] Reference dump for cell {}, {} in {}", a_cellX, a_cellY, a_worldSpace->GetFormEditorID());

		std::vector<CellReference> references;
		files.ReadCellReferences(a_cellX, a_cellY, references, true);

		// Account for the merge in stages, so it is clear what each filter removes rather than only
		// what comes out the end.
		std::map<std::string, int> visibleTypes;
		std::map<std::string, int> rejectedTypes;
		size_t visible = 0;
		size_t contributing = 0;
		size_t tooSmall = 0;
		size_t rejectedByPath = 0;
		float largestExtent = 0.0f;

		for (const auto& reference : references) {
			if (reference.IsHidden())
				continue;
			++visible;

			auto* base = RE::TESForm::LookupByID(reference.runtimeBaseID);
			const std::string typeName = base ? std::string(magic_enum::enum_name(base->GetFormType())) : std::string("<unresolved>");
			visibleTypes[typeName]++;

			if (!IsHeightContributingBase(base)) {
				auto* model = base ? skyrim_cast<const RE::TESModel*>(base) : nullptr;
				const bool byPath = model && IsExcludedModelPath(model->GetModel());
				if (byPath) {
					++rejectedByPath;
					logger::info("[TexGen]     excluded by path: {} {}", typeName, model->GetModel());
				} else {
					rejectedTypes[typeName]++;
				}
				continue;
			}

			const float extent = BaseBoundExtent(base) * reference.scale;
			if (extent < minimumReferenceExtent) {
				++tooSmall;
				continue;
			}

			++contributing;
			largestExtent = std::max(largestExtent, extent);
		}

		auto describe = [](const std::map<std::string, int>& a_counts) {
			std::string text;
			for (const auto& [name, count] : a_counts)
				text += fmt::format("{}x{} ", name, count);
			return text;
		};

		logger::info("[TexGen]   {} visible by base type: {}", visible, describe(visibleTypes));
		logger::info("[TexGen]   rejected by type: {}", describe(rejectedTypes));
		logger::info("[TexGen]   rejected by model path: {}", rejectedByPath);
		logger::info("[TexGen]   {} contributing references, {} skipped as smaller than {:.0f} units, largest extent {:.0f}",
			contributing, tooSmall, minimumReferenceExtent, largestExtent);

		size_t shown = 0;
		for (const auto& reference : references) {
			if (reference.IsHidden() || shown >= 8)
				continue;

			auto* base = RE::TESForm::LookupByID(reference.runtimeBaseID);
			if (!IsHeightContributingBase(base))
				continue;

			const float extent = BaseBoundExtent(base) * reference.scale;
			if (extent < minimumReferenceExtent)
				continue;

			auto* model = base ? skyrim_cast<const RE::TESModel*>(base) : nullptr;
			const char* modelPath = model ? model->GetModel() : nullptr;

			logger::info("[TexGen]     ref {:08X} {} extent {:.0f} at {:.0f},{:.0f},{:.0f} scale {:.2f} model {}",
				reference.runtimeRefID,
				base ? std::string(magic_enum::enum_name(base->GetFormType())) : std::string("<unresolved>"),
				extent,
				reference.position.x, reference.position.y, reference.position.z,
				reference.scale,
				modelPath && *modelPath ? modelPath : "<none>");
			++shown;
		}

		return references.size();
	}

	void PrimeFileTables(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY)
	{
		if (!a_worldSpace)
			return;

		LandFileSet primer(a_worldSpace);
		CellHeights scratch;
		primer.ReadCell(a_cellX, a_cellY, scratch);
	}
}
