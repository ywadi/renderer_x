# Phase 4: Scene Architecture & Asset Management — Research Findings

**Date:** 2026-08-11  
**Topic:** Precedents and best practices for scene graph, culling, geometry pooling, shadow caster management, and layer/mask conventions.

---

## 1. Filament Scene Architecture

### Entity + Component Manager Model
Filament uses an **ECS-style component manager architecture** centered on the `EntityManager` and specialized component managers:
- **RenderableManager**: Binds geometry (VertexBuffer, IndexBuffer), materials (MaterialInstance), and bounding boxes (AABB) to entities. Supports multiple primitives per entity via a builder pattern.
- **TransformManager**: Manages hierarchical transforms with O(1) local updates via transaction boundaries for deep hierarchies (>4–5 levels). World transforms compose parent's world transform with entity's local transform.
- **LightManager**: Manages directional, point, and spot lights as components.
- **Destruction**: `Engine::destroy(Entity)` automatically cleans all associated components across managers.

### Creation/Destroy API Shape
**Builder pattern for creation:**
```cpp
RenderableManager::Builder(size_t primitiveCount)
  .geometry(index, type, vertexBuffer, indexBuffer, offset, count)
  .material(index, materialInstance)
  .boundingBox(aabb)
  .build(engine, entity)
```

**Runtime modification:**
- `setMaterialInstanceAt(instance, primitiveIndex, materialInstance)` / `getMaterialInstanceAt()`
- `setGeometryAt(instance, primitiveIndex, type, vertexBuffer, indexBuffer, offset, count)`

### Data Layout
**Array-of-Structures (AoS)** organization:
- Each entity's renderable groups all related data: primitives array, material instances, bone data, morphing weights.
- Primitives stored as contiguous slice per entity; each primitive holds vertex/index buffer references, material instance, and geometry parameters.
- This prioritizes **per-entity coherency** for hierarchical cache efficiency.

**References:**
- [RenderableManager.h](https://github.com/google/filament/blob/main/filament/include/filament/RenderableManager.h)
- [RenderableManager.cpp](https://github.com/google/filament/blob/main/filament/src/components/RenderableManager.cpp)
- [TransformManager API (React Native Filament docs)](https://margelo.github.io/react-native-filament/docs/api/interfaces/TransformManager)
- [Engine Architecture (DeepWiki)](https://deepwiki.com/google/filament/2-engine-architecture)

---

## 2. Frustum Culling Implementations

### Standard Approach
1. **Plane extraction** (once per frame): Extract 6 frustum planes from `proj * view` matrix.
2. **Per-instance test**: Transform local AABB to world space, test against all 6 planes.
3. **AABB vs sphere testing**:
   - **AABB**: 6 plane tests (2D/3D box-plane signed distance).
   - **Sphere**: Distance from center to plane; if > radius on any side, reject.

### SIMD Optimization
- **4-wide SIMD**: Cull AABB against all 4 left/right and top/bottom planes in parallel using ~6 multiplications + ~6 additions.
- Theoretical 4× speedup via parallelism when batching AABBs.
- **Practical results**: SIMD culling reduced time from ~1.1 ms → 0.3 ms (desktop) and 3.0 ms → 0.6 ms (laptop) with SSE + threading achieving ~9× improvement over naive C++.

### Batched Culling Architecture (Vulkan)
**niagara's compute-based approach:**
- Compute shader processes 64 draws per workgroup (one thread per draw).
- **Culling stages**:
  - Frustum culling using symmetry (left/right, top/bottom planes in parallel; near/far via camera-space Z).
  - Occlusion culling via screen-space sphere projection + hierarchical depth pyramid sampling (2×2 texel quad).
- **Data layout**: `MeshDraw` array (mesh index, position, scale, radius), `depthPyramid` texture, atomic append to visible command buffer.
- Visible draws append draw commands via atomic operations to prevent race conditions.

### Performance Metrics
- **Throughput**: ~6,907 operations/sec with 256-object leaf batches (Benny benchmark).
- **Vkguide example**: "Frustum culling will easily cut half the objects" (50% typical rejection rate).
- No explicit per-millisecond throughput published in production renderers, but GPU-driven approaches (compute-based) process entire scenes without CPU roundtrips.

**References:**
- [Vulkan Tutorial: Frustum Culling](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Advanced_Topics/Culling.html)
- [Vkguide: Compute-based Culling](https://vkguide.dev/docs/gpudriven/compute_culling/)
- [GameDev.net: Frustum Culling](https://gamedev.net/tutorials/programming/general-and-gameplay-programming/frustum-culling-r4613)
- [ryg blog: Frustum Culling Notes](https://fgiesen.wordpress.com/2010/10/20/some-more-frustum-culling-notes/)
- [niagara drawcull.comp.glsl](https://github.com/zeux/niagara/blob/master/src/shaders/drawcull.comp.glsl)
- [niagara GitHub](https://github.com/zeux/niagara) — MIT licensed, open-source educational Vulkan renderer.

---

## 3. Global Geometry Pooling

### Single-Big-Buffer Pattern
**Principle:** Allocate one large vertex + index buffer; suballocate ranges via offset allocator, draw with `firstIndex` (index buffer offset in indices) and `baseVertex` (vertex buffer offset in vertices).

**Suballocation with OffsetAllocator:**
- **Algorithm**: Two-level segregated fit (TLSF) via 256 bins with 8-bit floating-point distribution (3-bit mantissa + 5-bit exponent).
- **Performance**: Hard real-time **O(1)** allocation and free via 2 LZCNT instructions (regardless of size); **minimal fragmentation** (≤12.5% overhead, avg 6.25%).
- **Metadata storage**: Separate from resource memory; returns offset (not pointer) to allocated range.
- **Multi-draw indirect (MDI)**: Each draw command specifies `firstIndex` and `baseVertex` to reference its suballocated region; single bind of global buffers.

### Draw Call Integration
```
DrawIndexedIndirectCommand {
  indexCount, instanceCount, firstIndex, baseVertex, firstInstance
}
```
- Multiple mesh instances per scene packed into single index/vertex buffer.
- Each draw pulls its geometry via offset; zero-indexed within that region.

### OffsetAllocator Status (2026)
- **Source**: [github.com/sebbbi/OffsetAllocator](https://github.com/sebbbi/OffsetAllocator)
- **License**: MIT
- **Maintenance**: "Early one-weekend prototype" (2023); unit tests green but author recommends caution ("Use at your own risk!"). **UNVERIFIED for production use in 2026.**
- **Community adoption**: 1.1k GitHub stars; ports to Rust ([offset-allocator crate](https://crates.io/crates/offset-allocator)), used in Bevy and other graphics projects.
- **Recommendation**: OffsetAllocator is the de-facto standard for GPU buffer suballocation; alternative ready-made options are minimal. Consider peer review of 2025+ forks/alternatives before production deployment.

**References:**
- [OffsetAllocator GitHub](https://github.com/sebbbi/OffsetAllocator)
- [Rust port: offset-allocator crate](https://crates.io/crates/offset-allocator)
- [C++ Gist-based implementations](https://gist.github.com/cshenton/d8db9bded49706ed4b28adb9bd937fcb)
- [Vulkan MDI Documentation](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html)
- [Base Vertex Optimization](https://paroj.github.io/gltut/Positioning/Tut05%20Optimization%20Base%20Vertex.html)

---

## 4. Directional-Light Shadow Caster Culling

### Orthographic Light Frustum Setup
For directional lights (sun-like), shadow casting requires an **orthographic light frustum** that:
- Covers the main camera's view frustum.
- Extends behind the camera to catch "back-casters" (objects behind camera but projecting shadows into view).
- Represented as a **6-DOP** (axis-aligned box: 3 pairs of min/max scalars) vs. perspective frustum's 10-DOP.

### Conservative Bounds Strategy
**Two-stage culling**:
1. **Light frustum culling**: AABB vs orthographic box; removes objects clearly outside light's reach.
2. **Conservative shadow extent**: Estimate shadow volume extent (e.g., via HZB depth read or shadow map coverage); test if shadow does not intersect main camera frustum.

### "Casters Behind Camera Still Cast" Subtlety
Problem: Objects behind camera can project visible shadows into the view.  
**Solution**: 
- Extend the light frustum **backward** to include object bounds beyond the camera near plane.
- Progressive modes test shadow volume extent via projected depth; aggressive modes may miss shadows if occlusion/depth is unreliable.
- Trade-off: Conservative extents guarantee no shadows; tight bounds risk flicker.

### Practical Implementation Notes
- Initial pass: Camera frustum culling on caster bounds.
- Follow-up: Test caster's bounding volume against light frustum.
- Late-stage: Occlusion-based rejection (if receiver isn't visible, no shadow needed).

**References:**
- [DigitalRune: Shadow Caster Culling](https://digitalrune.github.io/DigitalRune-Documentation/html/4058fb6c-8794-46cb-9d22-fb8558857179.htm)
- [GameDev.net: Tightly Culling Shadow Casters for Directional Lights](https://www.gamedev.net/forums/topic/612925-tutorial-tightly-culling-shadow-casters-for-directional-lights/)
- [L. Spiro Engine: Shadow Caster Culling](http://lspiroengine.com/?p=153)

---

## 5. Per-Object Data Layout: Layer/Cull Mask Conventions

### Industry Bitmask Widths
- **Unity**: 32 layers (LayerMask as 32-bit int); first 8 built-in, next 24 user-defined. Rendering layers also 32-bit bitmask.
- **Unreal Engine**: **3 lighting channels** (hard limit as of UE 5.8); lights and renderers each have a 3-bit channel mask. Not power-of-2; fixed design for cinematic control.
- **Godot**: 32 layers (32-bit bitmask for physics/rendering/visibility). Layer indices are 1-based in inspector (Layer 1–32) but 0-based in code (bit 0–31).

### Data Layout Recommendations
- **32-bit bitmask** is the de-facto standard for layer/visibility/culling masks (scalable to 32 logical groups).
- **3-channel cap** (Unreal) is intentionally conservative; most scenes don't exceed 8–16 distinct shadow/lighting groups.
- **Defaults**: Commonly all layers enabled (0xFFFFFFFF) or Layer 0 only (0x1), depending on use case (opt-in vs. opt-out).

### Practical Conventions
- Use named layer constants (avoid raw bitmasks in code).
- Document per-project layer semantics: shadow casters, shadow receivers, UI exclusions, etc.
- Consider sparse bit usage (e.g., bits 0–7 for shadow/light, 8–15 for visibility, 16–23 for physics) if layering multiple concerns.

**References:**
- [Unity LayerMask Scripting API](https://docs.unity3d.com/ScriptReference/LayerMask.html)
- [Unity Layers Manual](https://docs.unity.cn/530/Documentation/Manual/Layers.html)
- [Unreal Lighting Channels (UE 4.26)](https://docs.unrealengine.com/4.26/en-US/BuildingWorlds/LightingAndShadows/LightingChannels)
- [Godot Layers & Masks (Medium)](https://medium.com/codex/using-mask-culling-visibility-layers-godot-4-c-7d3b8b0415d5)
- [Godot Blog: Layers & Masks Notes](https://blog.luevano.xyz/g/godot_layers_and_masks_notes)

---

## Summary for RendererX Phase 4

1. **Scene graph**: Adopt Filament's Entity + component manager model (ECS-like); AoS per-entity layout for coherency.
2. **Culling**: GPU-driven compute-based frustum + occlusion (niagara precedent); CPU fallback for small scenes.
3. **Geometry pooling**: OffsetAllocator is the ready-made choice; use MDI with `firstIndex`/`baseVertex` per-draw.
4. **Shadow casters**: Extend orthographic light frustum conservatively; multi-stage CPU culling (camera frustum → light frustum → occlusion).
5. **Masks**: Use 32-bit bitmask; document semantic ranges per project (e.g., bits 0–7 shadow, 8–15 visibility).

---

## Not Yet Covered (Future Research)

- Hierarchy update order and parent transform batching strategies.
- Mesh LOD selection in relation to culling (combined LOD + frustum).
- Memory defragmentation strategies if OffsetAllocator usage grows over time.
- GPU memory heap organization (Vulkan memory types, alignment requirements).
