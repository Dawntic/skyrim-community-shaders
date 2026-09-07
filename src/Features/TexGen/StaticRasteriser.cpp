#include "StaticRasteriser.h"

#include "TexGen.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>

namespace
{
	// Mirrors the shader's RasterTriangle. Three positions, nothing else: the height map only ever
	// asks how high a surface is.
	struct RasterTriangle
	{
		float v0[3];
		float v1[3];
		float v2[3];
	};
	static_assert(sizeof(RasterTriangle) == 36);

	/// @brief Reinterpret a float so unsigned comparison agrees with float comparison.
	/// The sign bit otherwise reads as the largest magnitude, which would make InterlockedMax keep
	/// the most negative height rather than the highest.
	std::uint32_t HeightToSortable(float a_height)
	{
		std::uint32_t bits = std::bit_cast<std::uint32_t>(a_height);
		return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
	}

	float SortableToHeight(std::uint32_t a_sortable)
	{
		const std::uint32_t bits = (a_sortable & 0x80000000u) ? (a_sortable & 0x7FFFFFFFu) : ~a_sortable;
		return std::bit_cast<float>(bits);
	}
}

namespace TexGenStatics
{
	bool StaticRasteriser::EnsureShader()
	{
		if (rasterCS)
			return true;

		rasterCS.attach(reinterpret_cast<ID3D11ComputeShader*>(
			Util::CompileShader(L"Data\\Shaders\\TexGen\\GenerateCacheMaps.hlsl", { { "STATIC_RASTER", "" } }, "cs_5_0")));

		if (!rasterCS)
			logger::error("[TexGen] Failed to compile the static geometry raster shader");

		return rasterCS != nullptr;
	}

	void StaticRasteriser::ClearShaderCache()
	{
		rasterCS = nullptr;
	}

	bool StaticRasteriser::EnsureResources(uint a_tileSize, size_t a_triangleCount, size_t a_instanceCount)
	{
		if (!heightTarget || heightTarget->desc.Width != a_tileSize) {
			CD3D11_TEXTURE2D_DESC targetDesc(DXGI_FORMAT_R32_UINT, a_tileSize, a_tileSize, 1, 1,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			heightTarget = eastl::make_unique<Texture2D>(targetDesc, "TexGen::StaticRasterTarget");

			CD3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc(D3D11_UAV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_UINT);
			heightTarget->CreateUAV(uavDesc);

			CD3D11_TEXTURE2D_DESC stagingDesc(DXGI_FORMAT_R32_UINT, a_tileSize, a_tileSize, 1, 1, 0,
				D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE);
			heightStaging = eastl::make_unique<Texture2D>(stagingDesc, "TexGen::StaticRasterStaging");
		}

		// Grow only: a run walks many tiles and the largest sets the working size.
		if (!triangleBuffer || a_triangleCount > triangleCapacity) {
			triangleCapacity = std::max<size_t>(a_triangleCount, triangleCapacity ? triangleCapacity * 2 : 1 << 16);

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = (UINT)(triangleCapacity * sizeof(RasterTriangle));
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(RasterTriangle);
			triangleBuffer = eastl::make_unique<Buffer>(desc, nullptr, "TexGen::RasterTriangles");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.NumElements = (UINT)triangleCapacity;
			triangleBuffer->CreateSRV(srvDesc);
		}

		if (!instanceBuffer || a_instanceCount > instanceCapacity) {
			instanceCapacity = std::max<size_t>(a_instanceCount, instanceCapacity ? instanceCapacity * 2 : 4096);

			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = (UINT)(instanceCapacity * sizeof(RasterInstance));
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(RasterInstance);
			instanceBuffer = eastl::make_unique<Buffer>(desc, nullptr, "TexGen::RasterInstances");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.NumElements = (UINT)instanceCapacity;
			instanceBuffer->CreateSRV(srvDesc);
		}

		return heightTarget && triangleBuffer && instanceBuffer;
	}

	bool StaticRasteriser::RasteriseTile(
		const float2& a_worldOrigin,
		float a_unitsPerTexel,
		uint a_tileSize,
		const std::vector<TexGenLand::CellReference>& a_references,
		MeshCache& a_meshes,
		std::vector<uint16_t>& io_pixels,
		RasterStats& o_stats)
	{
		o_stats = {};

		if (!EnsureShader())
			return false;
		if (io_pixels.size() != (size_t)a_tileSize * a_tileSize)
			return false;

		const float tileWorldSize = a_unitsPerTexel * (float)a_tileSize;
		const float2 worldMax = a_worldOrigin + float2(tileWorldSize, tileWorldSize);

		// Build the tile's geometry. Meshes are held in model space and shared, so a model placed
		// many times is uploaded once and the transforms do the rest.
		std::vector<RasterTriangle> triangles;
		std::vector<RasterInstance> instances;
		std::unordered_map<const MeshGeometry*, std::pair<std::uint32_t, std::uint32_t>> modelRanges;

		for (const auto& reference : a_references) {
			if (reference.IsHidden())
				continue;

			auto* base = RE::TESForm::LookupByID(reference.runtimeBaseID);
			if (!TexGenLand::IsHeightContributingBase(base))
				continue;

			const float extent = TexGenLand::BaseBoundExtent(base) * reference.scale;
			if (extent < TexGenLand::minimumReferenceExtent)
				continue;

			// Cheap reject before the mesh is touched: a reference whose bound cannot reach the tile
			// contributes nothing. The bound is a radius about the placement, so half an extent of
			// slack is enough.
			const float reach = extent;
			if (reference.position.x + reach < a_worldOrigin.x || reference.position.x - reach > worldMax.x ||
				reference.position.y + reach < a_worldOrigin.y || reference.position.y - reach > worldMax.y)
				continue;

			auto* model = skyrim_cast<const RE::TESModel*>(base);
			const auto* geometry = a_meshes.Get(model ? model->GetModel() : nullptr);
			if (!geometry || geometry->Empty())
				continue;

			auto found = modelRanges.find(geometry);
			if (found == modelRanges.end()) {
				const auto first = (std::uint32_t)triangles.size();
				const size_t triangleCount = geometry->indices.size() / 3;

				triangles.reserve(triangles.size() + triangleCount);
				for (size_t i = 0; i + 2 < geometry->indices.size(); i += 3) {
					const auto& p0 = geometry->vertices[geometry->indices[i]];
					const auto& p1 = geometry->vertices[geometry->indices[i + 1]];
					const auto& p2 = geometry->vertices[geometry->indices[i + 2]];

					RasterTriangle triangle{
						{ p0.x, p0.y, p0.z },
						{ p1.x, p1.y, p1.z },
						{ p2.x, p2.y, p2.z }
					};
					triangles.push_back(triangle);
				}

				found = modelRanges.emplace(geometry, std::make_pair(first, (std::uint32_t)triangleCount)).first;
				++o_stats.models;
			}

			const auto transform = BuildReferenceTransform(reference.position, reference.rotation, reference.scale, RotationConvention::EulerXYZ);

			RasterInstance instance{};
			for (int row = 0; row < 3; ++row) {
				instance.rotationScale[row][0] = transform.rotate.entry[row][0] * transform.scale;
				instance.rotationScale[row][1] = transform.rotate.entry[row][1] * transform.scale;
				instance.rotationScale[row][2] = transform.rotate.entry[row][2] * transform.scale;
			}
			instance.rotationScale[0][3] = transform.translate.x;
			instance.rotationScale[1][3] = transform.translate.y;
			instance.rotationScale[2][3] = transform.translate.z;
			instance.firstTriangle = found->second.first;
			instance.triangleCount = found->second.second;

			instances.push_back(instance);
			o_stats.triangles += instance.triangleCount;
		}

		o_stats.instances = instances.size();
		if (instances.empty())
			return false;

		if (!EnsureResources(a_tileSize, triangles.size(), instances.size()))
			return false;

		auto context = globals::d3d::context;

		// Seed the target with the terrain the CPU already produced, so a static can only raise it.
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(heightStaging->resource.get(), 0, D3D11_MAP_WRITE, 0, &mapped));
			for (uint y = 0; y < a_tileSize; ++y) {
				auto* row = (std::uint32_t*)((std::uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch);
				for (uint x = 0; x < a_tileSize; ++x) {
					const float height = DirectX::PackedVector::XMConvertHalfToFloat(io_pixels[(size_t)y * a_tileSize + x]);
					row[x] = HeightToSortable(height);
				}
			}
			context->Unmap(heightStaging->resource.get(), 0);
			context->CopyResource(heightTarget->resource.get(), heightStaging->resource.get());
		}

		auto uploadBuffer = [&](Buffer* a_buffer, const void* a_data, size_t a_bytes) {
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(a_buffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			memcpy(mapped.pData, a_data, a_bytes);
			context->Unmap(a_buffer->resource.get(), 0);
		};
		uploadBuffer(triangleBuffer.get(), triangles.data(), triangles.size() * sizeof(RasterTriangle));
		uploadBuffer(instanceBuffer.get(), instances.data(), instances.size() * sizeof(RasterInstance));

		// The raster reads the tile placement out of SweepRect, which the cache gen buffer already
		// carries for the bent normal sweep.
		auto& texGen = globals::features::texGen;
		TexGen::CacheGenCBStruct data{};
		data.TexParams = float4((float)a_tileSize, (float)a_tileSize, 0.0f, 0.0f);
		data.SweepRect = float4(a_worldOrigin.x, a_worldOrigin.y, a_unitsPerTexel, (float)a_tileSize);

		ID3D11ShaderResourceView* srvs[2] = { triangleBuffer->srv.get(), instanceBuffer->srv.get() };
		ID3D11UnorderedAccessView* uavs[1] = { heightTarget->uav.get() };
		ID3D11Buffer* constants[1] = { texGen.UploadCacheGenBuffer(data) };

		context->CSSetShader(rasterCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, constants);

		context->Dispatch((UINT)instances.size(), 1, 1);

		ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
		ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
		context->CSSetShaderResources(0, 2, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		// Back to the tile buffer, counting what actually changed so a run that silently does
		// nothing is visible rather than merely plausible.
		context->CopyResource(heightStaging->resource.get(), heightTarget->resource.get());

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(heightStaging->resource.get(), 0, D3D11_MAP_READ, 0, &mapped));
		for (uint y = 0; y < a_tileSize; ++y) {
			const auto* row = (const std::uint32_t*)((const std::uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch);
			for (uint x = 0; x < a_tileSize; ++x) {
				const size_t index = (size_t)y * a_tileSize + x;
				const float before = DirectX::PackedVector::XMConvertHalfToFloat(io_pixels[index]);
				const float after = SortableToHeight(row[x]);

				if (after > before) {
					++o_stats.raisedTexels;
					o_stats.greatestRise = std::max(o_stats.greatestRise, after - before);
					io_pixels[index] = DirectX::PackedVector::XMConvertFloatToHalf(after);
				}
			}
		}
		context->Unmap(heightStaging->resource.get(), 0);

		return true;
	}
}
