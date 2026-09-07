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

	size_t SpikeDumpCellReferences(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY, int a_maxForms)
	{
		if (!a_worldSpace)
			return 0;

		auto* sourceFiles = a_worldSpace->sourceFiles.array;
		if (!sourceFiles || sourceFiles->empty())
			return 0;

		// Signatures are stored as the four characters read back as a little endian word, so they
		// print straight out of the record header.
		auto signatureText = [](std::uint32_t a_signature) {
			char text[5] = {};
			std::memcpy(text, &a_signature, 4);
			for (auto& c : text)
				if (c != 0 && (c < 32 || c > 126))
					c = '?';
			return std::string(text);
		};

		const std::uint32_t cellSignature = Util::FCC("CELL");
		const std::uint32_t refrSignature = Util::FCC("REFR");

		// Highest priority plugin that defines the cell, same walk the height read uses.
		RE::TESFile* file = nullptr;
		for (int index = static_cast<int>(sourceFiles->size()) - 1; index >= 0; --index) {
			auto* candidate = (*sourceFiles)[index] ? (*sourceFiles)[index]->Duplicate() : nullptr;
			if (candidate && candidate->SeekCell(a_worldSpace, a_cellX, a_cellY)) {
				file = candidate;
				break;
			}
			if (candidate)
				candidate->CloseTES(true);
		}

		if (!file) {
			logger::error("[TexGen] REFR spike: no plugin defines cell {}, {}", a_cellX, a_cellY);
			return 0;
		}

		logger::info("[TexGen] REFR spike: cell {}, {} from {}", a_cellX, a_cellY, file->GetFilename());
		logger::info("[TexGen]   landed on {} formID {:08X}", signatureText(file->currentform.form), file->currentform.formID);

		std::vector<CellReference> references;
		std::map<std::string, int> signatureCounts;
		int formsWalked = 0;
		bool stoppedAtNextCell = false;

		while (formsWalked < a_maxForms && file->SeekNextForm(true)) {
			++formsWalked;

			const std::uint32_t signature = file->currentform.form;
			signatureCounts[signatureText(signature)]++;

			if (signature == cellSignature) {
				stoppedAtNextCell = true;
				logger::info("[TexGen]   stopped: reached the next CELL ({:08X}) after {} forms", file->currentform.formID, formsWalked);
				break;
			}

			if (signature != refrSignature)
				continue;

			CellReference reference;
			reference.recordFlags = file->currentform.flags;

			// Subrecord order is not fixed and any of these may be absent, so walk once and take
			// whatever turns up rather than seeking each by type.
			do {
				const std::uint32_t subrecord = file->GetCurrentSubRecordType();
				if (subrecord == Util::FCC("NAME")) {
					file->ReadData(&reference.rawBaseID, sizeof(reference.rawBaseID));
				} else if (subrecord == Util::FCC("XSCL")) {
					file->ReadData(&reference.scale, sizeof(reference.scale));
				} else if (subrecord == Util::FCC("DATA") && file->GetCurrentSubRecordSize() >= 24) {
					float placement[6] = {};
					file->ReadData(placement, sizeof(placement));
					reference.position = RE::NiPoint3(placement[0], placement[1], placement[2]);
					reference.rotation = RE::NiPoint3(placement[3], placement[4], placement[5]);
				}
			} while (file->SeekNextSubrecord());

			reference.runtimeBaseID = file->GetRuntimeFormID(reference.rawBaseID);
			references.push_back(reference);
		}

		if (!stoppedAtNextCell)
			logger::warn("[TexGen]   stopped: ran out of forms (or hit the {} form cap) after {} forms - the walk has no CELL bound here", a_maxForms, formsWalked);

		logger::info("[TexGen]   {} forms walked, {} references collected", formsWalked, references.size());

		std::string breakdown;
		for (const auto& [signature, count] : signatureCounts)
			breakdown += fmt::format("{}x{} ", signature, count);
		logger::info("[TexGen]   record types: {}", breakdown);

		// A handful of resolved references, enough to eyeball against xEdit.
		const size_t sampleCount = std::min<size_t>(references.size(), 8);
		for (size_t i = 0; i < sampleCount; ++i) {
			const auto& reference = references[i];
			auto* base = RE::TESForm::LookupByID(reference.runtimeBaseID);
			const char* editorID = base ? base->GetFormEditorID() : nullptr;
			logger::info("[TexGen]     [{}] base {:08X} -> {} {} at {:.0f},{:.0f},{:.0f} scale {:.2f} flags {:08X}",
				i,
				reference.runtimeBaseID,
				base ? std::string(magic_enum::enum_name(base->GetFormType())) : std::string("<unresolved>"),
				editorID && *editorID ? editorID : "",
				reference.position.x, reference.position.y, reference.position.z,
				reference.scale,
				reference.recordFlags);
		}

		file->CloseTES(true);
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
