#pragma once

/**
 * @brief Loads the meshes placed by a cell's references, through the game's own model loader.
 *
 * Everything here goes through RE::BSModelDB::Demand rather than touching a path directly, so BSA
 * archives, loose files and a mod manager's virtual file system all resolve exactly as they do for
 * the game. That is the whole reason mod added statics work at all, and it is why nothing in this
 * file may reach for Util::PathHelpers::GetRealPathFromDataRelative.
 *
 * Geometry comes out flattened into model space, with the node hierarchy's transforms already
 * applied, so a caller only has to place it with the reference's own transform.
 */
namespace TexGenStatics
{
	/// @brief One mesh's triangles, in model space.
	struct MeshGeometry
	{
		std::vector<RE::NiPoint3> vertices;
		std::vector<std::uint32_t> indices;  // triangle list
		RE::NiPoint3 boundMin;
		RE::NiPoint3 boundMax;

		/// @brief Whether any lighting shaded geometry was found.
		///
		/// This is the reliable version of the model path heuristic the reference filter uses.
		/// Effect planes carry only a BSEffectShaderProperty and editor markers carry nothing worth
		/// drawing, so neither is solid and neither belongs in a height map, whatever directory the
		/// mesh happens to live in.
		bool hasLitGeometry = false;

		bool Empty() const { return indices.empty(); }
	};

	/// @brief Why a mesh could not be used, for reporting rather than control flow.
	struct MeshLoadStats
	{
		size_t loaded = 0;
		size_t demandFailed = 0;     // BSModelDB could not resolve the path
		size_t noGeometry = 0;       // resolved, but nothing lit to rasterise
		size_t noRawVertexData = 0;  // geometry present, but its CPU side copy was released
		size_t triangles = 0;
		size_t demanded = 0;  // distinct models handed to BSModelDB, which retains all of them
	};

	/// @brief Distinct models a run may ask BSModelDB for before it gives up on new geometry.
	///
	/// Demand caches every model it resolves in the game's own resource database, and
	/// CommonLibSSE-NG exposes no way to release one: every IEntryDB virtual is unnamed. So the
	/// only lever is how many are ever asked for. A run that exceeds this keeps placing the models
	/// it already holds and stops loading new ones, which loses some statics but finishes.
	inline constexpr size_t maximumDemandedModels = 6000;

	/**
	 * @brief Model path to geometry, loaded once per distinct path.
	 *
	 * A worldspace places hundreds of thousands of references but only tens of thousands of distinct
	 * models, so the cache is what keeps a full bake affordable. Failures are cached too: a mesh that
	 * cannot be read will not read any better the second time.
	 */
	class MeshCache
	{
	public:
		/// @brief Geometry for a model path, loading it on first use.
		/// @return Null when the model could not be loaded or holds nothing solid.
		const MeshGeometry* Get(const char* a_modelPath);

		void Clear();

		/// @brief Drop the oldest entries until the cache fits the budget.
		///
		/// A worldspace holds far more distinct models than any bake needs at once, and keeping
		/// every one alive for the length of a run is what exhausted memory. Tiles are walked in
		/// row order so their models repeat heavily, which makes oldest first a good enough
		/// eviction order without tracking use.
		///
		/// Call this only between tiles. Pointers handed out by Get stay valid until it runs.
		void Trim(size_t a_budgetBytes);

		/// @brief Roughly what the cached geometry occupies, for reporting and for Trim.
		size_t ApproximateBytes() const { return approximateBytes; }

		size_t Size() const { return cache.size(); }
		const MeshLoadStats& Stats() const { return stats; }

		/// @brief Whether the run has asked BSModelDB for as many models as it is allowed to.
		bool ReachedDemandLimit() const { return stats.demanded >= maximumDemandedModels; }

		/// @brief Whether a path has already been resolved, successfully or not, without loading it.
		/// Lets a caller find out what a tile still has to load before committing to loading it.
		bool Contains(const char* a_modelPath) const;

	private:
		std::unordered_map<std::string, MeshGeometry> cache;
		std::deque<std::string> insertionOrder;  // eviction order for Trim
		size_t approximateBytes = 0;
		MeshLoadStats stats;
	};

	/// @brief Walk a loaded model and collect its triangles, flattening the node hierarchy.
	/// Hidden subtrees are skipped, as the game would not draw them either.
	bool ExtractGeometry(RE::NiAVObject* a_root, MeshGeometry& o_out, MeshLoadStats& io_stats);

	/// @brief Which Euler convention turns a reference's DATA angles into its rotation matrix.
	///
	/// The record stores three angles and says nothing about the order they compose in, so the
	/// convention is established by comparing against the matrix the game itself builds for a loaded
	/// reference rather than assumed.
	enum class RotationConvention
	{
		EulerXYZ,  // NiMatrix3::SetEulerAnglesXYZ
		AxesZXY,   // NiMatrix3::EulerAnglesToAxesZXY
	};

	/// @brief Place a reference's mesh: rotation, uniform scale and world translation.
	RE::NiTransform BuildReferenceTransform(const RE::NiPoint3& a_position, const RE::NiPoint3& a_rotation, float a_scale, RotationConvention a_convention);

	/// @brief Check both rotation conventions against the matrices the game built for loaded
	/// references in a cell, and report which one reproduces them.
	/// @return Number of references it was able to compare.
	size_t SpikeCheckReferenceTransforms(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY);

	/// @brief Load every mesh a cell's contributing references place, and report what came back.
	///
	/// The open question this answers is whether rawVertexData survives on a model loaded this way.
	/// If it does not, geometry has to be read back off the GPU buffers instead, which is a wholly
	/// different amount of work, so it is worth knowing before any of it is written.
	/// @return Number of references whose mesh loaded and held solid geometry.
	size_t SpikeDumpCellMeshes(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY);

	/// @brief How a geometry's vertices are packed in rawVertexData.
	struct VertexLayout
	{
		std::uint32_t stride = 0;        // bytes per vertex
		std::uint32_t positionSize = 0;  // bytes the position block occupies, including its bitangent
		bool positionIsFloat = false;    // three floats rather than three halves
		bool Valid() const { return stride > 0 && positionSize > 0; }
	};

	/// @brief Work out a geometry's vertex packing from its attribute offsets.
	///
	/// The offsets are the authority here, not VF_FULLPREC. rawVertexData holds the engine's runtime
	/// layout rather than the compressed layout the NIF was stored in, and the two disagree: meshes
	/// come through with the flag clear while carrying three float positions. Trusting the flag
	/// reads every vertex at the wrong stride and produces coordinates in the tens of thousands
	/// along with NaN, which is how this was found.
	///
	/// Since UV follows position immediately, the UV offset is the size of the position block: 16
	/// bytes for three floats plus a bitangent, 8 for three halves plus one. Normal serves the same
	/// purpose when a mesh has no UVs.
	VertexLayout GetVertexLayout(RE::BSGraphics::VertexDesc& a_desc);
}
