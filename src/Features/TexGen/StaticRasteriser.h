#pragma once

#include "StaticMeshSource.h"
#include "TerrainHeightSource.h"

#include "Buffer.h"

/**
 * @brief Draws a cell's placed statics on top of a finished terrain tile.
 *
 * The terrain layer is a regular grid and fills on the CPU exactly. Statics are arbitrary triangles
 * in arbitrary positions, so they are rasterised on the GPU instead, blended so geometry may only
 * ever raise a texel. That reproduces what the Havok raycast produced, which took the topmost
 * terrain, ground or static hit and nothing below it.
 */
namespace TexGenStatics
{
	/// @brief One mesh placed once, as the rasteriser consumes it.
	struct RasterInstance
	{
		float rotationScale[3][4];  // rows of rotation times scale, with translation in w
		std::uint32_t firstTriangle;
		std::uint32_t triangleCount;
		std::uint32_t pad[2];
	};
	static_assert(sizeof(RasterInstance) == 64);

	struct RasterStats
	{
		size_t instances = 0;
		size_t triangles = 0;     // submitted, counting each placement separately
		size_t models = 0;        // distinct meshes uploaded for the tile
		size_t raisedTexels = 0;  // texels a static actually lifted above the terrain
		float greatestRise = 0.0f;
	};

	class StaticRasteriser
	{
	public:
		/// @brief Compile the raster shader. Safe to call repeatedly.
		bool EnsureShader();

		/// @brief Discard the compiled shader so the next call picks up an edited file.
		void ClearShaderCache();

		/**
		 * @brief Rasterise every contributing reference of a tile over its terrain heights.
		 *
		 * @param a_worldOrigin South west corner of the tile in world units.
		 * @param a_unitsPerTexel World units a texel spans.
		 * @param a_tileSize Texels per tile edge.
		 * @param io_pixels Terrain heights in, combined heights out, R16_FLOAT as written to disk.
		 * @return False when nothing was rasterised, leaving io_pixels untouched.
		 */
		bool RasteriseTile(
			const float2& a_worldOrigin,
			float a_unitsPerTexel,
			uint a_tileSize,
			const std::vector<TexGenLand::CellReference>& a_references,
			MeshCache& a_meshes,
			std::vector<uint16_t>& io_pixels,
			RasterStats& o_stats);

	private:
		/// @brief Size the working textures and buffers for a tile, reusing them where possible.
		bool EnsureResources(uint a_tileSize, size_t a_triangleCount, size_t a_instanceCount);

		winrt::com_ptr<ID3D11ComputeShader> rasterCS;

		eastl::unique_ptr<Texture2D> heightTarget;  // R32_UINT, sortable heights
		eastl::unique_ptr<Texture2D> heightStaging;

		eastl::unique_ptr<Buffer> triangleBuffer;
		eastl::unique_ptr<Buffer> instanceBuffer;
		size_t triangleCapacity = 0;
		size_t instanceCapacity = 0;
	};
}
