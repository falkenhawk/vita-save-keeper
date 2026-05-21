cmake_minimum_required(VERSION 3.16)

project(save_keeper VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vsm_core
  src/core/BackupArchive.cpp
  src/core/BackupList.cpp
  src/core/BackupName.cpp
  src/core/BackupStore.cpp
  src/core/GoogleAuth.cpp
  src/core/GoogleConfig.cpp
  src/core/GoogleDrive.cpp
  src/core/PathUtil.cpp
  src/core/SaveScanner.cpp
  src/core/Selection.cpp
  src/core/SfoParser.cpp
)
target_include_directories(vsm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(NOT VITA)
  include(CTest)

  if(BUILD_TESTING)
    add_executable(vsm_core_tests
      tests/test_core.cpp
    )
    target_link_libraries(vsm_core_tests PRIVATE vsm_core)
    add_test(NAME vsm_core_tests COMMAND vsm_core_tests)
  endif()
else()
  include(cmake/vita-toolchain-check.cmake)
  include("${VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "Save Keeper")
  set(VITA_TITLEID "SVK000001")
  set(VITA_VERSION "00.01")

  add_executable(save-keeper
    src/vita/App.cpp
    src/vita/SaveAppDbMetadata.cpp
    src/vita/main.cpp
    src/vita/net/HttpClient.cpp
    src/vita/ui/Ui.cpp
    third_party/qrcodegen/qrcodegen.cpp
  )
  target_include_directories(save-keeper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen
  )
  target_link_libraries(save-keeper PRIVATE
    vsm_core
    curl
    ssl
    crypto
    zstd
    vita2d
    png
    z
    m
    c
    SceCtrl_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceGxm_stub
    SceHttp_stub
    SceKernelThreadMgr_stub
    SceLibKernel_stub
    SceNetCtl_stub
    SceNet_stub
    ScePgf_stub
    ScePvf_stub
    SceSqlite_stub
    SceSsl_stub
    SceSysmodule_stub
  )

  vita_create_self(save-keeper.self save-keeper UNSAFE)
  vita_create_vpk(save-keeper.vpk ${VITA_TITLEID} save-keeper.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
endif()
