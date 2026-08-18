#pragma once

// import_gltf.h -- PRIVATE (not installed). fastgltf-aware import
// orchestration: byte-source-mediated document/URI loading -> parse ->
// generic extensionsUsed/Required surfacing [log-don't-drop mechanism] ->
// per-mesh/per-primitive processing (delegating the CPU-heavy part to
// gltf_pipeline.h, fanned out via rx::task::Scheduler::parallelFor
// [D5]) -> ONE combined GeometryPool::upload() for the whole file
// [matrix row 12's "sync import may wait ONCE... never per-primitive"] ->
// D12 node-graph flattening -> Registry insertion. This is the only
// translation unit besides gltf_error.cpp that names a fastgltf type.
#include <rx_asset/byte_source.h>
#include <rx_asset/registry.h>
#include <cstddef>
#include <span>

namespace rx::task {
class Scheduler;
}  // namespace rx::task

namespace rx::asset {

class GeometryPool;
class TextureCache;

[[nodiscard]] ImportResult importGltfPipeline(Registry& registry, std::span<const std::byte> documentBytes, ByteSource& source,
                                                GeometryPool& pool, rx::task::Scheduler& scheduler, TextureCache* textures);

}  // namespace rx::asset
