#include "StaticMeshSource.h"

#include "TerrainHeightSource.h"

#include <DirectXPackedVector.h>

namespace
{
	// Position always sits at offset zero; the nibble the desc keeps for it holds the dynamic vertex
	// size rather than an offset, so there is nothing to look up.
	constexpr std::uint32_t kPositionOffset = 0;

	// What GetSize assumes the position block costs. Correcting by the real figure is what turns it
	// into a usable stride.
	constexpr std::uint32_t kAssumedPositionSize = 16;
	constexpr std::uint32_t kFloatPositionSize = 16;  // three floats plus a bitangent
	constexpr std::uint32_t kHalfPositionSize = 8;    // three halves plus a bitangent

	// How many geometries still owe a layout dump. The vertex layout is guessed from a bitfield
	// CommonLibSSE-NG does not fully expose, so when the decoded positions come out wrong the raw
	// bytes are the only thing that settles what the stride and format actually are.
	int g_vertexLayoutDumpsRemaining = 0;

	std::string HexBytes(const std::uint8_t* a_data, size_t a_count)
	{
		std::string text;
		for (size_t i = 0; i < a_count; ++i)
			text += fmt::format("{:02X} ", a_data[i]);
		return text;
	}

	void DumpVertexLayout(RE::BSGeometry* a_geometry, RE::BSGraphics::TriShape* a_renderer, std::uint32_t a_vertexCount, std::uint32_t a_stride)
	{
		auto& desc = a_geometry->GetGeometryRuntimeData().vertexDesc;

		const auto layout = TexGenStatics::GetVertexLayout(desc);
		logger::info("[TexGen]   layout: flags {:04X} GetSize {} stride {} positionSize {} float {} (VF_FULLPREC says {})",
			(std::uint16_t)desc.GetFlags(), desc.GetSize(), layout.stride, layout.positionSize, layout.positionIsFloat,
			desc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_FULLPREC));
		logger::info("[TexGen]   offsets: POSITION {} TEXCOORD0 {} NORMAL {} COLOR {}",
			desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION),
			desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0),
			desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_NORMAL),
			desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR));

		const size_t bytes = std::min<size_t>((size_t)a_vertexCount * a_stride, 48);
		logger::info("[TexGen]   first {} bytes: {}", bytes, HexBytes(a_renderer->rawVertexData, bytes));

		// Decode the same leading bytes both ways. Whichever produces coordinates in the tens or
		// hundreds, rather than tens of thousands, is the real format.
		for (int i = 0; i < 3; ++i) {
			const std::uint8_t* vertex = a_renderer->rawVertexData + (size_t)i * a_stride;

			std::uint16_t asHalf[3];
			std::memcpy(asHalf, vertex, sizeof(asHalf));
			float asFloat[3];
			std::memcpy(asFloat, vertex, sizeof(asFloat));

			logger::info("[TexGen]     v{} half {:.1f},{:.1f},{:.1f} | float {:.1f},{:.1f},{:.1f}",
				i,
				DirectX::PackedVector::XMConvertHalfToFloat(asHalf[0]),
				DirectX::PackedVector::XMConvertHalfToFloat(asHalf[1]),
				DirectX::PackedVector::XMConvertHalfToFloat(asHalf[2]),
				asFloat[0], asFloat[1], asFloat[2]);
		}
	}

	void GrowBound(RE::NiPoint3& io_min, RE::NiPoint3& io_max, const RE::NiPoint3& a_point, bool a_first)
	{
		if (a_first) {
			io_min = io_max = a_point;
			return;
		}
		io_min.x = std::min(io_min.x, a_point.x);
		io_min.y = std::min(io_min.y, a_point.y);
		io_min.z = std::min(io_min.z, a_point.z);
		io_max.x = std::max(io_max.x, a_point.x);
		io_max.y = std::max(io_max.y, a_point.y);
		io_max.z = std::max(io_max.z, a_point.z);
	}

	/// @brief Collect one geometry's triangles, transformed into model space.
	bool AppendGeometry(RE::BSGeometry* a_geometry, const RE::NiTransform& a_transform, TexGenStatics::MeshGeometry& o_out, TexGenStatics::MeshLoadStats& io_stats)
	{
		auto* triShape = a_geometry->AsTriShape();
		if (!triShape)
			return false;

		auto& runtime = a_geometry->GetGeometryRuntimeData();

		// An effect plane carries only a BSEffectShaderProperty. It is not solid, so it must not
		// raise the height map however large it is.
		auto* shaderProperty = runtime.shaderProperty.get();
		if (!shaderProperty || skyrim_cast<RE::BSEffectShaderProperty*>(shaderProperty))
			return false;

		auto* renderer = runtime.rendererData;
		if (!renderer)
			return false;

		if (!renderer->rawVertexData || !renderer->rawIndexData) {
			// The CPU side copy is released once some meshes are uploaded. Report it rather than
			// guessing: whether a readback path is needed at all is a question for measurement.
			++io_stats.noRawVertexData;
			return false;
		}

		const auto& shapeData = triShape->GetTrishapeRuntimeData();
		const std::uint32_t vertexCount = shapeData.vertexCount;
		const std::uint32_t triangleCount = shapeData.triangleCount;
		if (vertexCount == 0 || triangleCount == 0)
			return false;

		const TexGenStatics::VertexLayout layout = TexGenStatics::GetVertexLayout(runtime.vertexDesc);
		if (!layout.Valid())
			return false;
		const std::uint32_t stride = layout.stride;

		if (g_vertexLayoutDumpsRemaining > 0) {
			--g_vertexLayoutDumpsRemaining;
			DumpVertexLayout(a_geometry, renderer, vertexCount, stride);
		}

		const bool fullPrecision = layout.positionIsFloat;
		const auto baseVertex = static_cast<std::uint32_t>(o_out.vertices.size());
		bool firstPoint = o_out.vertices.empty();

		o_out.vertices.reserve(o_out.vertices.size() + vertexCount);
		for (std::uint32_t i = 0; i < vertexCount; ++i) {
			const std::uint8_t* vertex = renderer->rawVertexData + (size_t)i * stride + kPositionOffset;

			RE::NiPoint3 position;
			if (fullPrecision) {
				float raw[3];
				std::memcpy(raw, vertex, sizeof(raw));
				position = RE::NiPoint3(raw[0], raw[1], raw[2]);
			} else {
				std::uint16_t raw[3];
				std::memcpy(raw, vertex, sizeof(raw));
				position = RE::NiPoint3(
					DirectX::PackedVector::XMConvertHalfToFloat(raw[0]),
					DirectX::PackedVector::XMConvertHalfToFloat(raw[1]),
					DirectX::PackedVector::XMConvertHalfToFloat(raw[2]));
			}

			position = a_transform * position;
			GrowBound(o_out.boundMin, o_out.boundMax, position, firstPoint);
			firstPoint = false;

			o_out.vertices.push_back(position);
		}

		o_out.indices.reserve(o_out.indices.size() + (size_t)triangleCount * 3);
		for (std::uint32_t i = 0; i < triangleCount * 3; ++i) {
			const std::uint32_t index = baseVertex + renderer->rawIndexData[i];
			o_out.indices.push_back(index);
		}

		io_stats.triangles += triangleCount;
		o_out.hasLitGeometry = true;
		return true;
	}

	void WalkNode(RE::NiAVObject* a_object, const RE::NiTransform& a_parentTransform, TexGenStatics::MeshGeometry& o_out, TexGenStatics::MeshLoadStats& io_stats)
	{
		if (!a_object)
			return;

		// The game would not draw a hidden subtree, so neither should the height map.
		if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return;

		const RE::NiTransform transform = a_parentTransform * a_object->local;

		if (auto* geometry = a_object->AsGeometry()) {
			AppendGeometry(geometry, transform, o_out, io_stats);
			return;
		}

		if (auto* node = a_object->AsNode()) {
			for (auto& child : node->GetChildren())
				WalkNode(child.get(), transform, o_out, io_stats);
		}
	}
}

namespace TexGenStatics
{
	VertexLayout GetVertexLayout(RE::BSGraphics::VertexDesc& a_desc)
	{
		using Attribute = RE::BSGraphics::Vertex::Attribute;
		using Flags = RE::BSGraphics::Vertex::Flags;

		VertexLayout layout;

		const std::uint32_t billed = a_desc.GetSize();
		if (billed == 0)
			return layout;

		// Whatever attribute comes first after position tells us how much room position was given.
		if (a_desc.HasFlag(Flags::VF_UV))
			layout.positionSize = a_desc.GetAttributeOffset(Attribute::VA_TEXCOORD0);
		else if (a_desc.HasFlag(Flags::VF_NORMAL))
			layout.positionSize = a_desc.GetAttributeOffset(Attribute::VA_NORMAL);

		// Nothing follows position, so fall back to the flag. It is the weaker signal, but with no
		// second attribute there is nothing to measure against.
		if (layout.positionSize == 0)
			layout.positionSize = a_desc.HasFlag(Flags::VF_FULLPREC) ? kFloatPositionSize : kHalfPositionSize;

		layout.positionIsFloat = layout.positionSize >= kFloatPositionSize;

		// GetSize is right about every other attribute, so swapping its assumed position block for
		// the measured one gives the true stride.
		if (billed + layout.positionSize < kAssumedPositionSize)
			return VertexLayout{};

		layout.stride = billed - kAssumedPositionSize + layout.positionSize;
		return layout;
	}

	bool ExtractGeometry(RE::NiAVObject* a_root, MeshGeometry& o_out, MeshLoadStats& io_stats)
	{
		o_out = {};
		if (!a_root)
			return false;

		RE::NiTransform identity;
		WalkNode(a_root, identity, o_out, io_stats);

		return o_out.hasLitGeometry && !o_out.Empty();
	}

	const MeshGeometry* MeshCache::Get(const char* a_modelPath)
	{
		if (!a_modelPath || !*a_modelPath)
			return nullptr;

		std::string key(a_modelPath);
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });

		// Failures are cached alongside successes: a mesh that cannot be read will not read any
		// better on the next reference that places it.
		if (auto it = cache.find(key); it != cache.end())
			return it->second.Empty() ? nullptr : &it->second;

		MeshGeometry geometry;

		RE::NiPointer<RE::NiNode> model;

		// Only the vertices are wanted here, and the default texture load level pulls every texture
		// the model references into the game's own resource cache, where it stays. Demanding a
		// worldspace worth of models that way exhausted memory outright.
		RE::BSModelDB::DBTraits::ArgsType args{};
		args.texLoadLevel = 0;

		const auto result = RE::BSModelDB::Demand(a_modelPath, model, args);

		if (result != RE::BSResource::ErrorCode::kNone || !model) {
			++stats.demandFailed;
			cache.emplace(std::move(key), std::move(geometry));
			return nullptr;
		}

		if (!ExtractGeometry(model.get(), geometry, stats)) {
			++stats.noGeometry;
			cache.emplace(std::move(key), std::move(geometry));
			return nullptr;
		}

		++stats.loaded;
		const size_t bytes = geometry.vertices.size() * sizeof(RE::NiPoint3) + geometry.indices.size() * sizeof(std::uint32_t);
		auto [it, inserted] = cache.emplace(key, std::move(geometry));
		insertionOrder.push_back(std::move(key));
		approximateBytes += bytes;
		return &it->second;
	}

	size_t SpikeDumpCellMeshes(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY)
	{
		if (!a_worldSpace)
			return 0;

		TexGenLand::LandFileSet files(a_worldSpace);
		if (!files.Valid()) {
			logger::error("[TexGen] Mesh dump: no source files for {}", a_worldSpace->GetFormEditorID());
			return 0;
		}

		std::vector<TexGenLand::CellReference> references;
		files.ReadCellReferences(a_cellX, a_cellY, references);

		logger::info("[TexGen] Mesh dump for cell {}, {} in {}", a_cellX, a_cellY, a_worldSpace->GetFormEditorID());

		MeshCache cache;
		g_vertexLayoutDumpsRemaining = 2;  // the first couple of geometries are enough to read the layout
		size_t candidates = 0;
		size_t placed = 0;
		size_t shown = 0;

		for (const auto& reference : references) {
			if (reference.IsHidden())
				continue;

			auto* base = RE::TESForm::LookupByID(reference.runtimeBaseID);
			if (!TexGenLand::IsHeightContributingBase(base))
				continue;

			const float extent = TexGenLand::BaseBoundExtent(base) * reference.scale;
			if (extent < TexGenLand::minimumReferenceExtent)
				continue;

			++candidates;

			auto* model = skyrim_cast<const RE::TESModel*>(base);
			const char* modelPath = model ? model->GetModel() : nullptr;

			const auto* geometry = cache.Get(modelPath);
			if (!geometry)
				continue;

			++placed;

			if (shown < 8) {
				const RE::NiPoint3 size = geometry->boundMax - geometry->boundMin;
				logger::info("[TexGen]     {} tris {} verts, model bound {:.0f} x {:.0f} x {:.0f}, OBND extent {:.0f} :: {}",
					geometry->indices.size() / 3,
					geometry->vertices.size(),
					size.x, size.y, size.z,
					extent,
					modelPath ? modelPath : "<none>");
				++shown;
			}
		}

		const auto& stats = cache.Stats();
		logger::info("[TexGen]   {} candidate references, {} placed, {} distinct models cached", candidates, placed, cache.Size());
		logger::info("[TexGen]   models loaded {}, demand failed {}, nothing solid {}, triangles {}",
			stats.loaded, stats.demandFailed, stats.noGeometry, stats.triangles);

		if (stats.noRawVertexData > 0)
			logger::warn("[TexGen]   {} geometries had no CPU side vertex data - a GPU readback path would be needed", stats.noRawVertexData);
		else
			logger::info("[TexGen]   every geometry kept its CPU side vertex data, so no GPU readback is needed");

		return placed;
	}

	RE::NiTransform BuildReferenceTransform(const RE::NiPoint3& a_position, const RE::NiPoint3& a_rotation, float a_scale, RotationConvention a_convention)
	{
		RE::NiTransform transform;

		if (a_convention == RotationConvention::AxesZXY)
			transform.rotate.EulerAnglesToAxesZXY(a_rotation.x, a_rotation.y, a_rotation.z);
		else
			transform.rotate.SetEulerAnglesXYZ(a_rotation.x, a_rotation.y, a_rotation.z);

		transform.translate = a_position;
		transform.scale = a_scale > 0.0f ? a_scale : 1.0f;
		return transform;
	}

	size_t SpikeCheckReferenceTransforms(RE::TESWorldSpace* a_worldSpace, int a_cellX, int a_cellY)
	{
		if (!a_worldSpace)
			return 0;

		TexGenLand::LandFileSet files(a_worldSpace);
		if (!files.Valid()) {
			logger::error("[TexGen] Transform check: no source files for {}", a_worldSpace->GetFormEditorID());
			return 0;
		}

		std::vector<TexGenLand::CellReference> references;
		files.ReadCellReferences(a_cellX, a_cellY, references);

		logger::info("[TexGen] Transform check for cell {}, {} - the cell must be loaded to compare against", a_cellX, a_cellY);

		auto matrixError = [](const RE::NiMatrix3& a_lhs, const RE::NiMatrix3& a_rhs) {
			float worst = 0.0f;
			for (int row = 0; row < 3; ++row)
				for (int column = 0; column < 3; ++column)
					worst = std::max(worst, std::abs(a_lhs.entry[row][column] - a_rhs.entry[row][column]));
			return worst;
		};

		size_t compared = 0;
		size_t positionMismatches = 0;
		float worstXYZ = 0.0f;
		float worstZXY = 0.0f;
		float worstPosition = 0.0f;

		for (const auto& reference : references) {
			if (reference.IsHidden())
				continue;

			auto* live = RE::TESForm::LookupByID<RE::TESObjectREFR>(reference.runtimeRefID);
			auto* node = live ? live->Get3D() : nullptr;
			if (!live || !node)
				continue;  // not loaded, nothing to compare against

			// The parse itself: position and angles as the game holds them.
			const auto livePosition = live->GetPosition();
			const float positionError = std::max({ std::abs(livePosition.x - reference.position.x),
				std::abs(livePosition.y - reference.position.y),
				std::abs(livePosition.z - reference.position.z) });
			worstPosition = std::max(worstPosition, positionError);
			if (positionError > 0.1f)
				++positionMismatches;

			// The convention: score both against the matrix the game built.
			const auto xyz = BuildReferenceTransform(reference.position, reference.rotation, reference.scale, RotationConvention::EulerXYZ);
			const auto zxy = BuildReferenceTransform(reference.position, reference.rotation, reference.scale, RotationConvention::AxesZXY);

			worstXYZ = std::max(worstXYZ, matrixError(xyz.rotate, node->local.rotate));
			worstZXY = std::max(worstZXY, matrixError(zxy.rotate, node->local.rotate));

			if (compared < 4) {
				logger::info("[TexGen]     ref {:08X} parsed pos {:.1f},{:.1f},{:.1f} vs live {:.1f},{:.1f},{:.1f} | scale {:.2f} vs {:.2f}",
					reference.runtimeRefID,
					reference.position.x, reference.position.y, reference.position.z,
					livePosition.x, livePosition.y, livePosition.z,
					reference.scale, node->local.scale);
			}

			++compared;
		}

		if (compared == 0) {
			logger::warn("[TexGen]   no loaded references to compare against - stand in the cell and try again");
			return 0;
		}

		logger::info("[TexGen]   compared {} loaded references", compared);
		logger::info("[TexGen]   position: worst error {:.3f} across {} mismatches", worstPosition, positionMismatches);
		logger::info("[TexGen]   rotation: EulerXYZ worst {:.4f}, AxesZXY worst {:.4f}", worstXYZ, worstZXY);

		if (worstXYZ < 0.001f)
			logger::info("[TexGen]   rotation convention is EulerXYZ");
		else if (worstZXY < 0.001f)
			logger::info("[TexGen]   rotation convention is AxesZXY");
		else
			logger::error("[TexGen]   neither convention reproduces the game's matrix - the angles are composed some other way");

		return compared;
	}

	void MeshCache::Clear()
	{
		cache.clear();
		insertionOrder.clear();
		approximateBytes = 0;
		stats = {};
	}

	void MeshCache::Trim(size_t a_budgetBytes)
	{
		while (approximateBytes > a_budgetBytes && !insertionOrder.empty()) {
			const auto& oldest = insertionOrder.front();

			if (auto it = cache.find(oldest); it != cache.end()) {
				approximateBytes -= it->second.vertices.size() * sizeof(RE::NiPoint3) + it->second.indices.size() * sizeof(std::uint32_t);
				cache.erase(it);
			}

			insertionOrder.pop_front();
		}
	}
}
