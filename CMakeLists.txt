cmake_minimum_required(VERSION 3.21)
project(wxl-modern-m2 VERSION 1.0.1 LANGUAGES NONE)

set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the WarcraftXL SDK")
include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")

# x64 only: wxl.m2file is published by the runtime the recompiled client loads, and only there.
wxl_add_mod(modern-m2
    SOURCES
        src/Module.cpp
        src/fdid/Fdid.cpp
        src/load/NativeLoad.cpp
        src/load/M2Demux.cpp
        src/load/M2Fixups.cpp
        src/load/M2Normalize.cpp
        src/load/M2NormalizeParticles.cpp
        src/load/M2NormalizeCameras.cpp
        src/load/AnimUnwrap.cpp
        src/compat/ModernM2.cpp
        src/compat/BoneBudget.cpp
        src/compat/Skin.cpp
    MANIFEST mod.json
    OUTPUT_NAME wxl-modern-m2
    TARGETS win-x64)
