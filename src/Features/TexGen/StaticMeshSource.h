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
	};

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

		size_t Size() const { return cache.size(); }
		const MeshLoadStats& Stats() const { return stats; }

	private:
		std::unordered_map<std::string, MeshGeometry> cache;
		MeshLoadStats stats;
	};

	/// @brief Walk a loaded model and collect its triangles, flattening the node hierarchy.
	/// Hidden subtrees are skipped, as the game would not draw them either.
	bool ExtractGeometry(RE::NiAVObject* a_root, MeshGeometry& o_out, MeshLoadStats& io_stats);

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
