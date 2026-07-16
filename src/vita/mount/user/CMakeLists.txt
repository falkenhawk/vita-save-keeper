add_executable(save-data-user
  main.c
)

target_include_directories(save-data-user PRIVATE ../kernel)
target_compile_options(save-data-user PRIVATE -Wall -O2 -nostdlib)
target_link_options(save-data-user PRIVATE -nostdlib)
add_dependencies(save-data-user save_keeper_mount_kernel_stubs)
target_link_libraries(save-data-user
  ${CMAKE_CURRENT_BINARY_DIR}/../kernel/libSaveKeeperMountKernel_stub.a
  SceLibKernel_stub
)

vita_create_self(save-data-user.suprx save-data-user CONFIG exports.yml UNSAFE)
vita_create_stubs(save_keeper_mount_user_stubs save-data-user exports.yml)
