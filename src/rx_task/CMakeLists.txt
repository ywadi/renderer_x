# rx_task: the engine's sole task scheduler wrapper [spec D2] -- a thin
# rx::task::Scheduler around enkiTS (third_party/CMakeLists.txt's
# enkiTS::enkiTS, pinned v1.12, zlib license), entirely hidden behind a
# pimpl (scheduler.cpp) so this library's own PUBLIC header
# (include/rx_task/scheduler.h) names no enkiTS type at all -- see that
# header's own comment for why. Device-free: no Vulkan, no rx_rhi_vk.
# Links rx_core for RX_LOG_INFO only (Scheduler::Scheduler() logs its
# resolved worker/IO-thread accounting once at construction) -- nothing
# else in this task's scope consumes rx_task yet (brief: "Zero integration
# into the frame loop yet -- no existing library/sample links rx_task in
# this task").
add_library(rx_task STATIC
    scheduler.cpp
)
target_include_directories(rx_task PUBLIC include)
target_link_libraries(rx_task PUBLIC rx_core PRIVATE enkiTS::enkiTS)

add_subdirectory(tests)
