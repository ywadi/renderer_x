### Task 9: GeometryPool (D8/D9)

**Files:** Create `src/rx_asset/{CMakeLists.txt,include/rx_asset/geometry_pool.h,geometry_pool.cpp,tests/...}`.
**Interfaces (produces):**
```cpp
namespace rx::asset {
struct MeshRange { uint32_t blockId; uint32_t firstIndex; uint32_t indexCount; int32_t vertexOffset; };
struct PoolVertex { float px,py,pz; float nx,ny,nz; float tx,ty,tz,tw; float u,v; }; // 48B, static_assert-pinned (D8)
class GeometryPool { // main-thread affinity (D5)
 public:
  static std::unique_ptr<GeometryPool> create(rhi::Device&, rhi::Uploader&, const PoolConfig& cfg /*chunk sizes, defaults 64MB/32MB*/);
  MeshRange upload(std::span<const PoolVertex> vertices, std::span<const uint32_t> indices); // suballoc via VmaVirtualBlock; new chunk on exhaustion
  void free(const MeshRange&);                       // virtual-free; no defrag (D9)
  void bind(VkCommandBuffer cmd, uint32_t blockId) const; // vertex+index bind for a block
  PoolStats stats() const;                            // bytes used/capacity per block — Tracy plots fed by caller
};}
```
**Steps:** device-free tests impossible (GPU) — GPU tests: upload two meshes → distinct non-overlapping ranges; free+re-upload reuses space (stats assert); exhaustion → new block, both drawable (record real indexed draws from two blocks, readback probe); zero validation errors → implement → both presets → commit.

