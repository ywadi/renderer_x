## Commits (Phase 2 range 30eb296..HEAD)
fc2db08 Record Phase 2 Task 8 completion in SDD ledger
6719083 Add Task 8 report; record Phase 2 Task 8 completion in SDD ledger
ca8f4be Fix hardware-assuming Uploader tests that broke real CI (pre-existing red main)
e051006 Phase 2 Task 8: CI + packaging for all four samples
9c59024 Record Phase 2 Task 7 completion in SDD ledger
ba90eee Task 7 fix round: defer bindless descriptor rewrite to the fence-confirmed point, not just the destruction
412c4fd Add Task 7 report (sample_04_streaming)
cc9651d Add samples/04_streaming: bindless texture streaming with eviction-while-in-flight safety (Phase 2 Task 7)
732e0c8 Record Phase 2 Task 6 completion in SDD ledger
1d4f8e7 Append fix-round note to Task 6 report
bb5fe15 Task 6 fix round: matrix-layout doc, parameterized transitionImage, CACHE PATH fix, third rejection test
9e46726 Add Task 6 report (sample_03_bindless_mesh)
4f9aa12 Add samples/03_bindless_mesh: reflection + bindless + upload integration proof (Phase 2 Task 6)
b9fda92 Record Phase 2 Task 4 completion in SDD ledger
12815a5 Fix Task 4 review: real ReBAR direct upload path, mip filter check, tests
6f00fb7 Merge hotreload sample lane; record Task 4 landing in SDD ledger
3cd8a26 Merge branch 'worktree-agent-a381281ec33251156'
3fcbc3a Add Uploader, DeletionQueue, Texture2D mips, MeshBuffers (Phase 2 Task 4)
857fb45 Fix Task 5 review finding: push-constant offset was hardcoded to 0
deb8855 Add samples/02_hotreload: runtime Slang compilation + live shader reload
0e6f0dc Record Phase 2 Task 3 completion; add set-0 layout substitution requirement to Task 6
241e5cb Add device descriptor-indexing enablement + BindlessTable (Phase 2 Task 3)
33faa86 Record Phase 2 Task 2 completion in SDD ledger
ee12102 Fix Task 2 review findings: doc-only corrections, no functional change
88607a7 Add reflection-driven descriptor set/pipeline layouts (Phase 2 Task 2)
0d3387e Record Phase 2 Task 1 completion in SDD ledger
c3d70be Complete attribution-text redaction in archived review snapshot
4b9d232 Redact quoted attribution-trailer text from archived SDD records per repo policy
1f15eb8 Add rx_shader: in-process Slang compilation to SPIR-V

## Stat (code only — Phase 2 is ~all additive; review the live files at HEAD)
 .github/workflows/ci.yml                           |  135 +-
 .gitignore                                         |    2 +
 CMakeLists.txt                                     |   11 +
 samples/01_triangle/CMakeLists.txt                 |   19 +
 samples/01_triangle/main.cpp                       |   42 +-
 samples/02_hotreload/CMakeLists.txt                |   45 +
 samples/02_hotreload/hotreload.slang               |   58 +
 samples/02_hotreload/main.cpp                      | 1142 +++++++++++++
 samples/03_bindless_mesh/CMakeLists.txt            |   31 +
 samples/03_bindless_mesh/main.cpp                  | 1587 +++++++++++++++++
 samples/03_bindless_mesh/texture.png               |  Bin 0 -> 684 bytes
 samples/04_streaming/CMakeLists.txt                |   20 +
 samples/04_streaming/main.cpp                      | 1794 ++++++++++++++++++++
 samples/README.md                                  |  385 ++++-
 src/rx_rhi_vk/CMakeLists.txt                       |   74 +-
 src/rx_rhi_vk/include/rx_rhi_vk/bindless.h         |  236 +++
 src/rx_rhi_vk/include/rx_rhi_vk/buffer.h           |  154 +-
 src/rx_rhi_vk/include/rx_rhi_vk/command.h          |   28 +-
 src/rx_rhi_vk/include/rx_rhi_vk/deletion_queue.h   |  122 ++
 src/rx_rhi_vk/include/rx_rhi_vk/frame_sync.h       |   20 +-
 src/rx_rhi_vk/include/rx_rhi_vk/mesh_buffers.h     |   77 +
 src/rx_rhi_vk/include/rx_rhi_vk/pipeline_layout.h  |  163 ++
 src/rx_rhi_vk/include/rx_rhi_vk/texture.h          |  147 ++
 src/rx_rhi_vk/include/rx_rhi_vk/upload.h           |  207 +++
 src/rx_rhi_vk/src/bindless.cpp                     |  320 ++++
 src/rx_rhi_vk/src/buffer.cpp                       |   91 +
 src/rx_rhi_vk/src/command.cpp                      |    5 +-
 src/rx_rhi_vk/src/deletion_queue.cpp               |   58 +
 src/rx_rhi_vk/src/device.cpp                       |  121 ++
 src/rx_rhi_vk/src/frame_sync.cpp                   |    2 +
 src/rx_rhi_vk/src/mesh_buffers.cpp                 |   54 +
 src/rx_rhi_vk/src/pipeline_layout.cpp              |  262 +++
 src/rx_rhi_vk/src/stb_impl.cpp                     |   20 +
 src/rx_rhi_vk/src/texture.cpp                      |  274 +++
 src/rx_rhi_vk/src/upload.cpp                       |  361 ++++
 src/rx_rhi_vk/tests/bindless_test.cpp              |  395 +++++
 src/rx_rhi_vk/tests/clear_color_test.cpp           |   52 +-
 src/rx_rhi_vk/tests/deletion_queue_test.cpp        |  271 +++
 src/rx_rhi_vk/tests/frame_sync_test.cpp            |   50 +
 src/rx_rhi_vk/tests/pipeline_layout_test.cpp       |  376 ++++
 src/rx_rhi_vk/tests/texture_test.cpp               |  241 +++
 src/rx_rhi_vk/tests/upload_test.cpp                |  371 ++++
 src/rx_shader/CMakeLists.txt                       |  191 +++
 src/rx_shader/include/rx_shader/compiler.h         |  128 ++
 src/rx_shader/include/rx_shader/reflection.h       |   77 +
 .../include/rx_shader/shader_layout_info.h         |   72 +
 src/rx_shader/src/compiler.cpp                     |  343 ++++
 src/rx_shader/src/detail/global_session_mutex.h    |   36 +
 src/rx_shader/src/reflection.cpp                   |  272 +++
 src/rx_shader/tests/compiler_test.cpp              |  186 ++
 src/rx_shader/tests/doctest_main.cpp               |    7 +
 src/rx_shader/tests/link_smoketest.cpp             |   21 +
 src/rx_shader/tests/reflection_test.cpp            |  182 ++
 third_party/CMakeLists.txt                         |   30 +
 tools/fetch_slang.cmake                            |  227 ++-
 tools/package_samples.sh                           |  156 ++
 56 files changed, 11584 insertions(+), 167 deletions(-)
