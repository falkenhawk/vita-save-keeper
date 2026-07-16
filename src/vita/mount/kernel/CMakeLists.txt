add_executable(save-data-kernel
  main.c
)

target_compile_options(save-data-kernel PRIVATE -Wall -O2 -nostdlib)
target_link_options(save-data-kernel PRIVATE -nostdlib)
target_link_libraries(save-data-kernel
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceModulemgrForDriver_stub
  SceThreadmgrForDriver_stub
  SceProcessmgrForDriver_stub
  taihenForKernel_stub
  taihenModuleUtils_stub
)

vita_create_self(save-data-kernel.skprx save-data-kernel CONFIG exports.yml UNSAFE)
vita_create_stubs(save_keeper_mount_kernel_stubs save-data-kernel exports.yml KERNEL)
