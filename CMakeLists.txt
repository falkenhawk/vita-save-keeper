cmake_minimum_required(VERSION 3.16)

project(save_keeper VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
  # An unset build type compiles without optimization: slower TLS/zip/QR work on the Vita CPU and
  # a visibly larger eboot.bin.
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(SAVE_KEEPER_GOOGLE_CLIENT_ID "" CACHE STRING "Google OAuth client ID to embed in the Vita app")
set(SAVE_KEEPER_GOOGLE_CLIENT_SECRET "" CACHE STRING "Google OAuth client secret to embed in the Vita app")

function(save_keeper_escape_definition OUTPUT_NAME VALUE)
  string(REPLACE "\\" "\\\\" ESCAPED_VALUE "${VALUE}")
  string(REPLACE "\"" "\\\"" ESCAPED_VALUE "${ESCAPED_VALUE}")
  set(${OUTPUT_NAME} "${ESCAPED_VALUE}" PARENT_SCOPE)
endfunction()

add_library(vsm_core
  src/core/AppSettings.cpp
  src/core/BackupArchive.cpp
  src/core/BackupList.cpp
  src/core/BackupName.cpp
  src/core/BackupStore.cpp
  src/core/GoogleAuth.cpp
  src/core/GoogleConfig.cpp
  src/core/GoogleDrive.cpp
  src/core/GridWindow.cpp
  src/core/PathUtil.cpp
  src/core/SaveCategory.cpp
  src/core/SaveScanner.cpp
  src/core/Selection.cpp
  src/core/SfoParser.cpp
  src/core/SyncPlan.cpp
  src/core/TextUtil.cpp
)
target_include_directories(vsm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(SAVE_KEEPER_GOOGLE_CLIENT_ID AND SAVE_KEEPER_GOOGLE_CLIENT_SECRET)
  save_keeper_escape_definition(SAVE_KEEPER_ESCAPED_GOOGLE_CLIENT_ID
                                "${SAVE_KEEPER_GOOGLE_CLIENT_ID}")
  save_keeper_escape_definition(SAVE_KEEPER_ESCAPED_GOOGLE_CLIENT_SECRET
                                "${SAVE_KEEPER_GOOGLE_CLIENT_SECRET}")
  target_compile_definitions(vsm_core PUBLIC
    SAVE_KEEPER_GOOGLE_CLIENT_ID="${SAVE_KEEPER_ESCAPED_GOOGLE_CLIENT_ID}"
    SAVE_KEEPER_GOOGLE_CLIENT_SECRET="${SAVE_KEEPER_ESCAPED_GOOGLE_CLIENT_SECRET}"
  )
endif()

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
  set(VITA_VERSION "01.00")

  add_executable(save-keeper
    src/vita/App.cpp
    src/vita/SaveAppDbMetadata.cpp
    src/vita/main.cpp
    src/vita/net/HttpClient.cpp
    src/vita/ui/Ui.cpp
    third_party/qrcodegen/qrcodegen.cpp
    third_party/vitasqlite/vita_sqlite_rw.c
  )
  target_include_directories(save-keeper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitasqlite
  )
  target_link_libraries(save-keeper PRIVATE
    vsm_core
    curl
    ssl
    crypto
    zstd
    vita2d
    freetype
    bz2
    png
    z
    m
    c
    SceCtrl_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceGxm_stub
    SceKernelThreadMgr_stub
    SceLibKernel_stub
    SceNetCtl_stub
    SceNet_stub
    ScePgf_stub
    ScePvf_stub
    SceSqlite_stub
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
    FILE sce_sys/resources/Roboto-Regular.ttf sce_sys/resources/Roboto-Regular.ttf
    FILE sce_sys/resources/cacert.pem sce_sys/resources/cacert.pem
  )
endif()
