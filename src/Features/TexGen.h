#pragma once

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
struct TexGen : Feature
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

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/// @brief Drives whichever bake is in flight; one tile per frame, nothing at all when idle.
	virtual void Prepass() override;

	struct Settings
	{
		int cacheProgressX = -57;
		int cacheProgressY = -43;
		int cacheTileCells = 8;            // worldspace cells per height tile edge (4 or 8)
		int cacheTileSize = 1024;          // texels per height tile edge (512 or 1024)
		bool cacheExport16Bit = true;      // export tiles as xLODGen-style 16 bit unsigned instead of raw float
		bool cacheAtlasZeroBase = false;   // bias the atlas so its lowest point sits at 0.0
		float cacheAtlasMinHeight = 0.0f;  // game unit height the last biased atlas stores as 0.0

		// Layout of the last built atlas, so texel coordinates can be mapped back to cells.
		int cacheAtlasMinCellX = 0;  // origin cell of the atlas' lower left tile
		int cacheAtlasMinCellY = 0;
		int cacheAtlasTileSize = 0;   // texels per tile edge, 0 if no atlas has been built
		int cacheAtlasTileCells = 0;  // cells per tile edge
		int cacheAtlasTilesX = 0;     // tile columns
		int cacheAtlasTilesY = 0;     // tile rows, needed to flip texel Y into cell space

		float cacheBentNormalAtlasScale = 1.0f;  // downscale factor applied when stitching the BN atlas

		// Height smoothing pass. Radius and spatial sigma are texels, everything else is game units.
		int smoothRadius = 8;                  // taps either side of centre, per separable pass
		float smoothSpatialSigma = 4.0f;       // Gaussian falloff across that radius
		float smoothRangeSigma = 96.0f;        // height difference a neighbour may have and still average in
		int smoothIterations = 3;              // horizontal plus vertical pass pairs run back to back
		float smoothPreserveThreshold = 0.0f;  // relief taller than this is handed back afterwards, 0 keeps the pass purely smoothing
	} settings;

	//////////////////////////////////////////////////////////////////////////////////
	//// Cache layout
	//////////////////////////////////////////////////////////////////////////////////

	static constexpr float worldCellSize = 4096.0f;  // world units per worldspace cell edge
	static inline const std::filesystem::path cachePath = L"Data\\textures\\SkylightingCache\\";

	// xLODGen-compatible export encoding: 16 bit unsigned, zero height stored as 32767, one step
	// per 8 game units. Decode with height = (encoded - heightExportOffset) * heightExportScale.
	static constexpr float heightExportOffset = 32767.0f;
	static constexpr float heightExportScale = 8.0f;

	// The height range a worldspace can hold, in game units. The smoothing pass clamps everything it
	// reads and writes to it, so one stray texel cannot drag its neighbourhood with it and nothing a
	// height map could not legitimately store reaches disk.
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

	/// @brief The worldspace the cache is being generated for, kept across interiors.
	const std::string& GetWorldspaceID() const { return worldspaceID; }
	/// @brief The parent worldspace the player is currently in, empty when there is none.
	static std::string GetCurrentWorldspaceID();

	/// @brief The cell containing a world XY position.
	static int2 WorldToCell(float a_worldX, float a_worldY);
	/// @brief The origin (south west) cell of the tile a cell belongs to.
	static int2 GetTileOriginCell(const int2& a_cell, int a_cellsPerTile);
	/// @brief "<Worldspace><mapTag><tileSize>.<cellsPerTile>.<originX>.<originY>.dds", e.g. Tamriel_H1024.8.-64.-40.dds
	static std::filesystem::path GetTilePath(const std::string& a_worldspaceID, const std::string& a_mapTag, uint a_tileSize, int a_cellsPerTile, const int2& a_originCell);

	/// @brief Texels per tile edge of the last stitched atlas, 0 if none has been built.
	int GetAtlasTileSize() const { return settings.cacheAtlasTileSize; }
	/// @brief Worldspace cells per tile edge of the last stitched atlas, 0 if none has been built.
	int GetAtlasTileCells() const { return settings.cacheAtlasTileCells; }

	const AtlasCellRange& GetHeightAtlasRange() const { return heightAtlasRange; }
	const AtlasCellRange& GetBentNormalAtlasRange() const { return bentNormalAtlasRange; }

	/// @brief World bounds (minX, minY, maxX, maxY) of the cells the height atlas covers.
	float4 GetHeightMapBounds() const;
	/// @brief World bounds of the bent normal atlas, which may cover a different range to the height map.
	float4 GetBentNormalAtlasBounds() const;

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

	/// @brief Stitch xLODGen LOD tiles into one albedo atlas at the given path.
	bool BuildLODAtlas(const std::filesystem::path& a_outputPath, const std::string& a_mapTag);
	/// @brief Write "<Worldspace>_N.dds" from the height atlas.
	bool GenerateNormalMap();
	/// @brief Write the full worldspace bent normal map, named with the height atlas' cell range.
	bool GenerateBentNormalMap();
	/// @brief Write the six cardinal and diagonal occlusion maps in one pass.
	bool GenerateCardinalOcclusionMap();
	/// @brief Write "<Worldspace>_HS.<minX>.<minY>.<maxX>.<maxY>.dds": the height atlas with its fine
	/// relief flattened, as 32 bit float game units. Needs the height atlas loaded.
	bool GenerateSmoothedHeightMap();

	/// @brief Whether a bake is in flight, during which the player is teleported around.
	bool IsGenerating() const { return heightGenRunning || bentNormalTileGen; }
	/// @brief Whether the height bake specifically is in flight.
	bool IsHeightGenRunning() const { return heightGenRunning; }

	//////////////////////////////////////////////////////////////////////////////////
	//// Atlas naming and stitching
	//////////////////////////////////////////////////////////////////////////////////

	/// @brief Locate "<Worldspace><mapTag>.<minX>.<minY>.<maxX>.<maxY>.dds" and read its cell range.
	bool FindAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, std::filesystem::path& o_path, AtlasCellRange& o_range) const;

	/// @brief A set of tiles stitched into one north up image, with where each tile landed.
	struct TileAtlasResult
	{
		DirectX::ScratchImage image;
		std::vector<std::pair<int2, std::string>> placements;  // atlas texel origin -> source file
		int2 minOriginCell = int2(0, 0);
		int2 maxOriginCell = int2(0, 0);
		int2 tileCounts = int2(0, 0);
		uint tileSize = 0;
		int cellsPerTile = 0;
	};

	/// @brief Stitch "<Worldspace><mapTag><tileSize>.<cells>.<x>.<y>.dds" tiles into one image.
	/// @param unormFill Value gaps between tiles take in a UNORM format, normalised to [0, 1].
	/// @param floatFill Value gaps between tiles take in a float format.
	/// @param scale Downscale applied per tile, 0 to 1. Rounded so tiles stay texel aligned.
	bool StitchTileAtlas(const std::string& a_worldspaceID, const std::string& a_mapTag, const float4& unormFill, const float4& floatFill, TileAtlasResult& o_result, float scale = 1.0f);

	/// @brief Ensure the height atlas exists, stitching it from the generated height tiles if not.
	/// @param a_forceRebuild Rebuild even when the atlas is already on disk.
	/// @return True if the atlas exists once the call returns.
	bool EnsureHeightAtlas(const std::string& a_worldspaceID, bool a_forceRebuild = false);
	/// @brief Ensure the bent normal atlas exists, stitching it from the generated bent normal tiles.
	bool EnsureBentNormalAtlas(const std::string& a_worldspaceID, bool a_forceRebuild = false);
	/// @brief Map an atlas texel to the worldspace cell it covers, using the last built atlas layout.
	/// @return False if no atlas has been built, leaving o_cell untouched.
	bool AtlasTexelToCell(const int2& a_texel, int2& o_cell) const;

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
	/// @brief (Re)create the staging tile at the given edge size. Returns false on failure.
	bool EnsureHeightTileTexture(uint a_tileSize);
	/// @brief Zero the staging tile so cells that fall outside the worldspace stay at zero height.
	void ClearHeightTile();
	/// @brief Write the staging tile to "<Worldspace>_H<tileSize>.<cellsPerTile>.<originX>.<originY>.dds".
	bool SaveHeightTile(const int2& a_tileOriginCell, int a_cellsPerTile);
	/// @brief Copy the staging tile into a viewable texture holding exactly what gets written to disk.
	void UpdateHeightPreview(const int2& a_tileOriginCell);

	/// @brief Sample one cell of terrain into the staging tile, advancing the run by one cell.
	void GenerateHeightMap();
	float GetRayIntersectionHeight(float3 a_position, float a_rayOffset);
	void SetWorldPosition(const int2& a_currentCellXY, RE::NiPoint3& o_worldPos);
	bool IsPositionValid(RE::NiPoint3 a_inputPosition);
	float SampleHeightMap(float2 a_coords);

	//////////////////////////////////////////////////////////////////////////////////
	//// LOD bent normal tiles
	//////////////////////////////////////////////////////////////////////////////////
	// One bent normal tile per height tile, matching their dimensions, format and naming, built
	// from the stitched height atlas so rays still see terrain beyond the tile they belong to.

	/// @brief Run the per texel bent normal march over a UV region of the height atlas.
	/// @param regionOffsetScale xy uv offset, zw uv scale. (0, 0, 1, 1) covers the whole atlas.
	bool DispatchBentNormals(ID3D11ComputeShader* a_computeShader, Texture2D* a_outputTex, const float4& regionOffsetScale);
	/// @brief Line sweep bent normals for one tile: one dispatch per azimuth, then a resolve pass.
	/// @param tileOriginAtlasPx North west corner of the tile in atlas texels.
	bool DispatchBentNormalSweep(Texture2D* a_accumTex, const int2& tileOriginAtlasPx);
	static constexpr int bentNormalAzimuths = 64;  // must match NUM_AZIMUTH in the shader
	static constexpr int bentNormalHullCapacity = 1024;
	/// @brief Queue a bent normal tile for every height tile on disk. Tiles are processed one per frame.
	bool StartBentNormalTiles();
	/// @brief Process the next queued bent normal tile, if any.
	void UpdateBentNormalTiles();
	/// @brief Generate and save the bent normal tile whose grid starts at the given cell.
	bool GenerateBentNormalTile(const int2& a_tileOriginCell);
	void StopBentNormalTiles();

	//////////////////////////////////////////////////////////////////////////////////
	//// Height smoothing
	//////////////////////////////////////////////////////////////////////////////////
	// An edge aware blur over the height atlas: relief smaller than the range sigma is averaged
	// away, relief larger than it keeps its edge. Separable, so a full smooth is a run of cheap
	// single axis dispatches rather than one long quadratic one.

	// Caps on what the UI and a hand edited config may ask for. A pass costs 2R taps per texel and
	// a run is 2N of them, so these bound one bake to something that stays well inside the driver
	// timeout even on a worldspace sized atlas.
	static constexpr int smoothMaxRadius = 64;
	static constexpr int smoothMaxIterations = 8;

	/// @brief Run one axis of the bilateral filter from a_source into a_target.
	/// @param a_axis Texel step the taps walk, (1, 0) or (0, 1).
	/// @param a_sourceEncoded Whether a_source still carries the atlas' on disk encoding. False for
	/// an intermediate target, which already holds game units and needs no decode.
	void DispatchHeightSmoothPass(ID3D11ComputeShader* a_computeShader, ID3D11ShaderResourceView* a_source, Texture2D* a_target, const int2& a_axis, bool a_sourceEncoded);

	//////////////////////////////////////////////////////////////////////////////////
	//// Generation state
	//////////////////////////////////////////////////////////////////////////////////

	struct alignas(16) CacheGenCBStruct
	{
		float4 TexParams;
		float4 RegionOffsetScale = float4(0.0f, 0.0f, 1.0f, 1.0f);  // height map sub rect to process
		float4 GridBounds;                                          // world xy min/max of the height map
		float4 SweepDir;                                            // xy: world dir, z: slope, w: major step
		float4 SweepParams;                                         // x: first line, y: line count, z: transpose, w: units per step
		float4 SweepRect;                                           // xy: tile origin in atlas texels, z: tile size
		float4 SmoothParams;                                        // xy: filter axis, z: radius, w: spatial sigma, in texels
		float4 SmoothRange;                                         // x: range sigma, y: preserve threshold, zw: height clamp, game units
	};

	/// @brief Fill the cache gen constants shared by every generator.
	CacheGenCBStruct MakeCacheGenCB(const float2& outputSize, const float4& regionOffsetScale = float4(0.0f, 0.0f, 1.0f, 1.0f)) const;
	/// @brief The cache gen constants plus the smoothing pass' own, for a dispatch of the given size.
	/// @param a_axis Texel step the taps walk; (0, 0) for the resolve pass, which has no axis.
	/// @param a_sourceEncoded False to dispatch with an identity decode, for an already decoded source.
	CacheGenCBStruct MakeSmoothCB(const float2& outputSize, const int2& a_axis, bool a_sourceEncoded) const;

private:
	/// @brief Refresh the worldspace the cache is generated for, keeping the last known one indoors.
	void UpdateWorldspaceID();
	/// @brief Ask the features that render with the cache to reload it from disk.
	void NotifyCacheMapsChanged();
	/// @brief Create the shared cache gen constant buffer on first use.
	void EnsureCacheGenBuffer();

	std::string worldspaceID = "";

	ID3D11ShaderResourceView* heightMapSRV = nullptr;  // non-owning, set by the consumer that loaded it
	float heightMapOffset = 0.0f;                      // decode bias for the loaded atlas format
	float heightMapScale = 1.0f;                       // decode scale for the loaded atlas format

	AtlasCellRange heightAtlasRange;
	AtlasCellRange bentNormalAtlasRange;

	ConstantBuffer* cacheGenBuffer = nullptr;

	//// Cache gen resources ////
	static constexpr uint COMapSize = 1024;
	static constexpr int2 BNMapSize = int2(3808, 3008);

	std::filesystem::path lodPath = L"C:\\Skyrim Modding Utilities\\DynDOLOD\\xLODGen\\Output\\textures\\terrain\\tamriel";

	//// Height bake state ////
	bool heightGenRunning = false;
	bool heightGenInit = true;              // set to restart the height run from the stored tile boundary
	bool heightGenSingleTile = false;       // generate only the tile covering heightGenTargetCell, then stop
	int2 heightGenTargetCell = int2(0, 0);  // cell whose tile a single tile run generates
	int heightSettleFrames = 20;            // frames to let terrain stream in after a teleport
	int cellsDone = 0;
	int tilesDone = 0;
	bool skipRaycast = false;    // sample land height only, for timing the walk without collision costs
	bool skipTileWrite = false;  // walk the worldspace without touching the staging tile
	bool skipTileSave = false;   // keep the run entirely in memory

	eastl::unique_ptr<Texture2D> cacheOutputTexH = nullptr;

	eastl::unique_ptr<Texture2D> heightPreviewTex = nullptr;
	int2 heightPreviewOrigin = int2(0, 0);
	bool heightPreviewValid = false;

	//// Bent normal tile bake state ////
	bool bentNormalTileGen = false;
	size_t bentNormalTileIndex = 0;
	std::vector<int2> bentNormalTileQueue;
	int2 bentNormalAtlasSize = int2(0, 0);
	eastl::unique_ptr<Texture2D> bentNormalTileTex = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalSweepCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> bentNormalFinalizeCS = nullptr;
	winrt::com_ptr<ID3D11Buffer> bentNormalHullBuffer = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> bentNormalHullUAV = nullptr;
};
