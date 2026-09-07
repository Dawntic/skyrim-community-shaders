#pragma once

/**
 * @brief Reads terrain heights straight out of the plugin files, with no cell loaded.
 *
 * The height bake used to teleport the player across the worldspace because Havok collision only
 * exists for cells inside the streamed grid. LAND records carry the same surface at its native
 * 33x33 vertices per cell, and RE::TESFile can seek any cell in any plugin at any time, so the
 * terrain layer needs neither the player nor the cell grid.
 *
 * Reference implementation for the seek and decode is WaterCache::TryGetCellData /
 * ReadMinLandHeightData; this widens that from a per-cell minimum to the whole vertex grid.
 */
namespace TexGenLand
{
	/// Vertices per cell edge. 33 rather than 32 because the last row and column are shared with the
	/// neighbouring cell, so every cell carries its own copy of the seam.
	inline constexpr int landGridEdge = 33;
	inline constexpr int landVertCount = landGridEdge * landGridEdge;

	struct CellHeights
	{
		std::array<float, landVertCount> heights{};  // world Z, row major, row index runs +Y
		float waterHeight = 0.0f;
		bool hasCell = false;  // a plugin defines this cell at all
		bool hasLand = false;  // ...and it carries a LAND record
	};

	/// @brief One placed reference, after the load order has been merged.
	struct CellReference
	{
		RE::FormID runtimeRefID = 0;   // the reference itself, mapped through the load order
		RE::FormID runtimeBaseID = 0;  // what it places
		RE::NiPoint3 position;
		RE::NiPoint3 rotation;  // radians
		float scale = 1.0f;
		std::uint32_t recordFlags = 0;

		/// @brief Whether the reference is deleted or starts disabled, and so is not really there.
		/// Mods remove vanilla clutter by disabling a reference and dropping it far below the world
		/// rather than by omitting it, so this has to be honoured or those objects come back.
		bool IsHidden() const
		{
			return (recordFlags & (RE::TESObjectREFR::RecordFlags::kDeleted | RE::TESObjectREFR::RecordFlags::kInitiallyDisabled)) != 0;
		}
	};

	/**
	 * @brief Private view of a worldspace's source plugins for terrain reads.
	 *
	 * Every seek mutates cursor state on the TESFile, and the engine's own copies are shared with
	 * cell streaming, so this holds a Duplicate() of each and never touches the originals. One
	 * instance belongs to exactly one thread; give each worker its own.
	 */
	class LandFileSet
	{
	public:
		explicit LandFileSet(RE::TESWorldSpace* a_worldSpace);
		~LandFileSet();

		LandFileSet(const LandFileSet&) = delete;
		LandFileSet& operator=(const LandFileSet&) = delete;

		/// @brief Decode one cell's height grid from the highest priority plugin that defines it.
		/// @return True when the cell yielded a LAND record; o_out still reports hasCell either way.
		bool ReadCell(int a_cellX, int a_cellY, CellHeights& o_out);

		/// @brief Collect a cell's references, merged across every plugin that touches the cell.
		///
		/// Unlike LAND, references cannot be taken from the winning plugin alone. An overriding CELL
		/// carries only the references that plugin adds or edits, so the real set is the union over
		/// the load order keyed by reference form ID, with later plugins replacing earlier ones.
		/// Taking only the top plugin loses everything it does not mention and reinstates everything
		/// it deliberately disabled.
		///
		/// Hidden references are kept rather than dropped, because a later plugin may re-enable one;
		/// filter with CellReference::IsHidden once the merge is done.
		///
		/// @param a_log Report each plugin's contribution and the merged totals.
		/// @return Number of references in o_out.
		size_t ReadCellReferences(int a_cellX, int a_cellY, std::vector<CellReference>& o_out, bool a_log = false);

		bool Valid() const { return worldSpace && !files.empty(); }

	private:
		RE::TESWorldSpace* worldSpace = nullptr;
		std::vector<RE::TESFile*> files;  // duplicates, ascending load order
	};

	/// @brief Worldspace cell extent, inclusive on both corners.
	/// Prefers the worldspace's own NAM0/NAM9 corners, then MNAM, then the cell map's keys.
	/// @param o_source Names which of those answered, for logging and the settings UI.
	/// @param a_log Report every candidate. Off by default: the settings UI calls this each frame,
	/// and the cell map scan behind the last candidate is not free.
	bool ResolveCellBounds(RE::TESWorldSpace* a_worldSpace, int2& o_minCell, int2& o_maxCell, const char*& o_source, bool a_log = false);

	/// @brief Log a cell's merged reference set, with the per-plugin contributions that produced it.
	size_t SpikeDumpCellReferences(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY);

	/// @brief Touch every source plugin's cell offset table once from the calling thread.
	/// SeekCell consults a per-file table on the worldspace; if the engine builds it lazily then
	/// first touch from several workers at once races on it. Priming costs one seek per plugin.
	void PrimeFileTables(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY);
}
