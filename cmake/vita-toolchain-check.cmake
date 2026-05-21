if(NOT DEFINED VITASDK OR NOT EXISTS "${VITASDK}")
  message(FATAL_ERROR "VITASDK is not set. Configure with -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake")
endif()

if(NOT EXISTS "${VITASDK}/share/vita.cmake")
  message(FATAL_ERROR "Could not find vita.cmake under VITASDK: ${VITASDK}")
endif()
