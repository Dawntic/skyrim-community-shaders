#pragma once

#include "OverlayFeature.h"

#include "TexGen/StaticRasteriser.h"
#include "TexGen/TerrainHeightSource.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

/**
 * @brief Offline generation of the terrain LOD texture cache.
 *
 * Owns everything that writes into Data\\textures\\SkylightingCache: the baked worldspace height
 * tiles and the atlas they stitch into, the bent normal tiles and their atlas, the cardinal and
 * diagonal occlusion set, the terrain normal map, and the albedo atlas assembled from xLODGen LOD
 * output. Features that render with those maps (Skylighting) load the finished files themselves and
 * call in here whenever one is missing or has to be rebuilt, so no rendering feature carries the
 * generation code.
 *
 * Generation is a one-off authoring step, not a per-frame cost: nothing here runs unless a bake has
 * been started from the settings UI or a consumer asked for a missing map.
 */
struct TexGen : OverlayFeature
{
public:
	virtual inline std::string GetName() override { return "TexGen"; }
	virtual inline std::string GetShortName() override { return "TexGen"; }
	virtual inline std::string GetDisplayName() override { return T("feature.texgen.name", "TexGen"); }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kUtility; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.texgen.description", "Bakes the terrain LOD texture cache that the lighting features read: height, bent normal, occlusion, normal and albedo maps for the current worldspace."),
			{ T("feature.texgen.key_feature_1", "Bakes worldspace height tiles by sampling terrain collision"),
				T("feature.texgen.key_feature_2", "Stitches baked tiles into height and bent normal atlases"),
				T("feature.texgen.key_feature_3", "Generates bent normal, occlusion and normal maps on the GPU"),
				T("feature.texgen.key_feature_4", "Assembles terrain albedo atlases from xLODGen LOD output"),
				T("feature.texgen.key_feature_5", "Tools for mapping between worldspace cells and atlas texels") } };
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override { return bentNormalTileGen; }

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/// @brief Drives whichever bake is in flight; one tile per frame, nothing at all when idle.
	virtual void Prepass() override;

	struct Settings
	{
		int cacheProgressX = -57;
		int cacheProgressY = -43;
		int cacheTileCells = 8;    // worldspace cells per height tile edge (4 or 8)
		int cacheTileSize = 1024;  // texels per height tile edge (512 or 1024)

		// Layout of the last built atlas, so texel coordinates can be mapped back to cells.
		int cacheAtlasMinCellX = 0;  // origin cell of the atlas' lower left tile
		int cacheAtlasMinCellY = 0;
		int cacheAtlasTileSize = 0;   // texels per tile edge, 0 if no atlas has been built
		int cacheAtlasTileCells = 0;  // cells per tile edge
		int cacheAtlasTilesX = 0;     // tile columns
		int cacheAtlasTilesY = 0;     // tile rows, needed to flip texel Y into cell space

		std::string dynDOLODPath;

		int smoothRadius = 16;
		float smoothFlattenHeight = 250.0f;
		float smoothRolloff = 2.0f;
		int smoothIterations = 4;

		bool skipBentNormalTiles = true;

		/// Rasterise placed statics over the terrain. The raycast bake included them, so leaving
		/// this off produces bare terrain and loses every building, rock and bridge.
		bool includeStatics = true;
	} settings;

	//////////////////////////////////////////////////////////////////////////////////
	//// Cache layout
	//////////////////////////////////////////////////////////////////////////////////

	static constexpr float worldCellSize = 4096.0f;  // world units per worldspace cell edge
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";

	static constexpr float heightRangeMin = -14500.0f;
	static constexpr float heightRangeMax = 40000.0f;

	/// @brief The worldspace cell range a stitched atlas covers, carried in its file name so the
	/// extent always describes the file on disk rather than whatever the settings last recorded.
	struct AtlasCellRange
	{
		int2 minCell = int2(0, 0);  // south west cell, inclusive
		int2 maxCell = int2(0, 0);  // north east cell, inclusive
		bool valid = false;

		float4 WorldBounds() const
		{
			return float4(
				(float)minCell.x * worldCellSize,
				(float)minCell.y * worldCellSize,
				(float)(maxCell.x + 1) * worldCellSize,
				(float)(maxCell.y + 1) * worldCellSize);
		}
	};

	/// @brief Texels per tile edge of the last stitched atlas, 0 if none has been built.
	int GetAtlasTileSize() const { return settings.cacheAtlasTileSize; }
	/// @brief Worldspace cells per tile edge of the last stitched atlas, 0 if none has been built.
	int GetAtlasTileCells() const { return settings.cacheAtlasTileCells; }

	const AtlasCellRange& GetHeightAtlasRange() const { return heightAtlasRange; }

	/// @brief World bounds (minX, minY, maxX, maxY) of the cells the height atlas covers.
	float4 GetAtlasWorldBound() const;

	//////////////////////////////////////////////////////////////////////////////////
	//// Consumer entry points
	//////////////////////////////////////////////////////////////////////////////////

	/// @brief Point every generator at the loaded height atlas. Non-owning: the consumer that loaded
	/// the texture keeps ownership and must clear this before releasing it.
	void SetHeightMapSRV(ID3D11ShaderResourceView* a_srv) { heightMapSRV = a_srv; }

	/// @brief Stitch the height atlas if it is missing, then locate it and record its layout.
	/// @param o_path Path of the atlas on disk, valid only when the call returns true.
	/// @param a_forceRebuild Restitch even when the atlas is already present.
	bool ResolveHeightAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild = false);
	/// @brief Stitch the bent normal atlas if it is missing, then locate it and record its layout.
	bool ResolveBentNormalAtlas(const std::string& a_worldspaceID, std::filesystem::path& o_path, bool a_forceRebuild = false);

	/// @brief Stitch xLODGen LOD tiles into the cell-ranged albedo atlas for this worldspace.
	bool BuildLODAtlas(const std::string& a_worldspaceID);
	/// @brief Generate the downscaled height, normal, AO, bent-normal tiles and bent-normal atlas set.
	/// Existing outputs are reused unless a_forceRebuild is set.
	/// @param a_smoothHeight Derive the occlusion maps from a flattened copy of the downscaled
	/// height map. The normal map always comes from the unflattened heights, and the flattened
	/// copy is never written to disk.
	bool BuildDerivedMaps(const std::string& a_worldspaceID, bool a_forceRebuild = false, bool a_smoothHeight = false);

	/// @brief Whether a bake is in flight. The raycast walk teleports the player; the LAND run does not.
	bool IsGenerating() const { return heightGenRunning || landGenRunning || bentNormalTileGen; }
	/// @brief Whether a height bake of either kind is in flight.
	bool IsHeightGenRunning() const { return heightGenRunning || landGenRunning; }
	bool IsBentNormalGenerationRunning() const { return bentNormalTileGen; }

	//////////////////////////////////////////////////////////////////////////////////
	//// Atlas naming and stitching
	//////////////////////////////////////////////////////////////////////////////////

	/// @brief A set of tiles stitched into one north up image, with where each tile landed.
	struct TileAtlasResult
	{
		DirectX::ScratchImage image;
		int2 minOriginCell = int2(0, 0);
		int2 maxOriginCell = int2(0, 0);
		int2 tileCounts = int2(0, 0);
		uint tileSize = 0;
		int cellsPerTile = 0;
	};

	/// @brief Stitch "<Worldspace><mapTag><tileSize>.<cells>.<x>.<y>.dds" tiles into one image.
	/// @param fill Value gaps between tiles take.
	bool StitchTileAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, const float4& fill, TileAtlasResult& o_result, const AtlasCellRange* a_range = nullptr);

	/// @brief Ensure the height atlas exists, stitching it from the generated height tiles if not.
	/// @param a_forceRebuild Rebuild even when the atlas is already on disk.
	/// @return True if the atlas exists once the call returns.
	bool EnsureHeightAtlas(const std::string& a_worldspaceID, bool a_forceRebuild = false);
	/// @brief Ensure the bent normal atlas exists, stitching it from the generated bent normal tiles.
	bool EnsureBentNormalAtlas(const std::string& a_worldspaceID, bool a_forceRebuild = false);

	//////////////////////////////////////////////////////////////////////////////////
	//// Height cache tiles
	//////////////////////////////////////////////////////////////////////////////////
	// The height cache is written out as a grid of fixed-size tiles instead of one huge
	// worldspace-sized texture. Each tile covers cacheTileCells x cacheTileCells worldspace
	// cells and is saved to disk as soon as its last cell has been sampled.

	/// @brief Cells per tile edge, sanitised to a value the tile size divides evenly (4 or 8).
	int GetHeightTileCells() const { return settings.cacheTileCells == 4 ? 4 : 8; }
	/// @brief Texels per tile edge, sanitised to a supported size (512 or 1024).
	uint GetHeightTileSize() const { return settings.cacheTileSize == 512 ? 512u : 1024u; }
	/// @brief Size and zero the staging tile so cells outside the worldspace stay at zero height.
	void ClearHeightTile(uint a_tileSize);
	/// @brief Write the staging tile to "<Worldspace>_H<tileSize>.<cellsPerTile>.<originX>.<originY>.dds".
	void SaveHeightTile(const int2& a_tileOriginCell, int a_cellsPerTile);
	/// @brief Copy the staging tile into a viewable texture holding exactly what gets written to disk.
	void UpdateHeightPreview(const int2& a_tileOriginCell);

	/// @brief Sample one cell of terrain into the staging tile, advancing the run by one cell.
	/// Teleports the player and raycasts Havok; kept only as the reference the LAND path is checked
	/// against, and removed once that check passes.
	void GenerateHeightMap();

	/// @brief Fill one whole tile from LAND records, then advance the run by one tile.
	/// Needs neither the player nor a loaded cell, so a full worldspace is a few seconds of file
	/// reads rather than a walk across the map.
	void GenerateHeightTiles();

	/// @brief Latch the tile layout and worldspace bounds, then start a LAND driven run.
	/// @param a_singleTile Generate only the tile holding a_targetCell and stop.
	bool StartLandHeightRun(bool a_singleTile, const int2& a_targetCell);

	/// @brief Rasterise one tile's worth of cells from LAND records into o_pixels. Writes nothing.
	/// @param a_clipToWorldspace Leave cells outside the worldspace extent at zero.
	void FillHeightTileFromLand(const int2& a_tileOriginCell, TexGenLand::LandFileSet& a_files, bool a_clipToWorldspace, std::vector<uint16_t>& o_pixels);

	/// @brief Collect the references that can reach a tile, its own cells plus a ring around it.
	///
	/// A reference is recorded in the cell it stands in, but its mesh does not stop at the cell
	/// boundary, so a cliff placed just outside a tile still covers part of it. Without the ring
	/// those objects would be cut off exactly at tile edges.
	///
	/// Reads are cached for the run: the ring makes each cell fall inside several tiles, and a cell
	/// read walks every plugin that defines it.
	void GatherTileReferences(const int2& a_tileOriginCell, int a_cellsPerTile, TexGenLand::LandFileSet& a_files, std::vector<TexGenLand::CellReference>& o_references);

	/// @brief Compare a freshly read LAND tile against the tile already on disk, and log the spread.
	/// Used against a raycast baked tile: the disagreement should be near zero over open terrain and
	/// one sided wherever a static stands, so its shape says whether the LAND path is right.
	void DiffHeightTileAgainstDisk(const int2& a_cell);

	/// @brief Check the plugin VHGT decode against the engine's own for the player's current cell.
	/// The engine keeps the same surface in TESObjectLAND::LoadedLandData::heights while the cell is
	/// loaded, so an exact match proves the decode before anything is built on top of it. Results go
	/// to the log; the quad and in-quad orderings are resolved by trying each and reporting the
	/// residual rather than by assuming one.
	void VerifyLandDecode();
	/// @brief Stitch a completed height run, preserve its tile names, then remove its temporary tiles.
	bool FinalizeHeightTiles();

	//////////////////////////////////////////////////////////////////////////////////
	//// LOD bent normal tiles
	//////////////////////////////////////////////////////////////////////////////////
	// One bent normal tile per height tile, matching their dimensions and naming, built
	// from the stitched height atlas so rays still see terrain beyond the tile they belong to.

	/// @brief Line sweep bent normals for one tile: one dispatch per azimuth, then a resolve pass.
	/// @param tileOriginAtlasPx North west corner of the tile in atlas texels.
	void DispatchBentNormalSweep(Texture2D* a_accumTex, const int2& tileOriginAtlasPx);
	static constexpr int bentNormalAzimuths = 64;  // must match NUM_AZIMUTH in the shader
	static constexpr int bentNormalHullCapacity = 1024;
	/// @brief Queue a bent normal tile for every entry in the retained height-tile manifest.
	bool StartBentNormalTiles();
	/// @brief Process the next queued bent normal tile, if any.
	void UpdateBentNormalTiles();

	//////////////////////////////////////////////////////////////////////////////////
	//// Height smoothing
	//////////////////////////////////////////////////////////////////////////////////

	static constexpr int smoothMaxRadius = 64;
	static constexpr int smoothMaxIterations = 8;

	//////////////////////////////////////////////////////////////////////////////////
	//// Generation state
	//////////////////////////////////////////////////////////////////////////////////

	struct alignas(16) CacheGenCBStruct
	{
		float4 TexParams;
		float4 GridBounds;    // world xy min/max of the height map
		float4 SweepDir;      // xy: world dir, z: slope, w: major step
		float4 SweepParams;   // x: first line, y: line count, z: transpose, w: units per step
		float4 SweepRect;     // xy: tile origin in atlas texels, z: tile size
		float4 SmoothParams;  // xy: filter axis, z: radius, in texels
		float4 SmoothRange;   // x: flatten height, y: rolloff multiple, zw: height clamp
	};

	/// @brief Fill the cache gen constant buffer and hand back the buffer bound with it.
	/// The generators here all drive one shader file through one constant buffer, and the static
	/// raster is another of them rather than a reason for a second.
	ID3D11Buffer* UploadCacheGenBuffer(const CacheGenCBStruct& a_data);

private:
	/// @brief Refresh the worldspace the cache is generated for, keeping the last known one indoors.
	void UpdateWorldspaceID();
	/// @brief Ask the features that render with the cache to reload it from disk.
	void NotifyCacheMapsChanged();
	/// @brief Write the normal map at exactly the downscaled height map dimensions.
	bool GenerateNormalMap(const int2& a_mapSize);
	/// @brief Write the cardinal and diagonal AO set at exactly the downscaled height map dimensions.
	bool GenerateCardinalOcclusionMaps(const int2& a_mapSize);

	/// @brief Run one axis of the bilateral filter from a_source into a_target.
	void DispatchHeightSmoothPass(ID3D11ComputeShader* a_computeShader, ID3D11ShaderResourceView* a_source, Texture2D* a_target, const int2& a_axis);
	/// @brief Flatten a height map into a fresh R32_FLOAT texture holding game units.
	eastl::unique_ptr<Texture2D> SmoothHeightMap(ID3D11ShaderResourceView* a_source, const int2& a_mapSize);

	std::string worldspaceID = "";

	ID3D11ShaderResourceView* heightMapSRV = nullptr;  // non-owning, set by the consumer that loaded it

	AtlasCellRange heightAtlasRange;

	ConstantBuffer* cacheGenBuffer = nullptr;

	//// Cache gen resources ////
	float derivedHeightScale = 1.0f / 8.0f;  //0.25f;

	//// Height bake state ////
	bool heightGenRunning = false;
	bool heightGenInit = true;              // set to restart the height run from the stored tile boundary
	bool heightGenSingleTile = false;       // generate only the tile covering heightGenTargetCell, then stop
	int2 heightGenTargetCell = int2(0, 0);  // cell whose tile a single tile run generates
	int heightSettleFrames = 20;            // frames to let terrain stream in after a teleport
	int cellsDone = 0;
	int tilesDone = 0;

	// Staging tile, exactly what gets written to disk. Both bakes fill this.
	std::vector<uint16_t> heightTilePixels;
	uint heightTileSize = 0;

	//// LAND height run state ////
	// Latched when a run starts so changing the settings mid-run cannot desync the tile layout.
	bool landGenRunning = false;
	RE::TESWorldSpace* landRunWorldspace = nullptr;
	std::unique_ptr<TexGenLand::LandFileSet> landRunFiles;
	int landRunCellsPerTile = 8;
	uint landRunTileSize = 1024;
	int2 landRunMinCell = int2(0, 0);  // worldspace extent, inclusive
	int2 landRunMaxCell = int2(0, 0);
	int2 landRunMinTile = int2(0, 0);  // tile grid covering that extent, inclusive
	int2 landRunMaxTile = int2(0, 0);
	int2 landRunCurrentTile = int2(0, 0);
	int landRunTileTotal = 0;
	int landRunSeamMismatches = 0;  // cells whose west edge disagrees with their neighbour's east

	//// Static layer ////
	TexGenStatics::MeshCache staticMeshes;
	TexGenStatics::StaticRasteriser staticRasteriser;
	std::unordered_map<int64_t, std::vector<TexGenLand::CellReference>> cellReferenceCache;
	TexGenStatics::RasterStats lastRasterStats;
	size_t landRunRaisedTexels = 0;
	size_t landRunInstances = 0;

	eastl::unique_ptr<Texture2D> heightPreviewTex = nullptr;
	int2 heightPreviewOrigin = int2(0, 0);
	bool heightPreviewValid = false;

	//// Bent normal tile bake state ////
	bool bentNormalTileGen = false;
	size_t bentNormalTileIndex = 0;
	std::vector<int2> bentNormalTileQueue;
	int2 bentNormalAtlasSize = int2(0, 0);
	eastl::unique_ptr<Texture2D> bentNormalTileTex = nullptr;
	winrt::com_ptr<ID3D11Resource> bentNormalHeightResource = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> bentNormalHeightSRV = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalSweepCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalFinalizeCS = nullptr;
	winrt::com_ptr<ID3D11Buffer> bentNormalHullBuffer = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> bentNormalHullUAV = nullptr;
};
