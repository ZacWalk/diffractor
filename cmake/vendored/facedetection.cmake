# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build libfacedetection, fetched at its pinned upstream commit, as diffractor::facedetection.
#
# Only the Highway detector is built. Upstream ships two implementations of the same network: the
# original src/facedetectcnn.cpp, whose SIMD is hand-written AVX2/AVX512/NEON selected by a macro at
# compile time, and highway/, which expresses the same kernels through google/highway. libfiles
# already builds Highway, and the macro-selected original would have to be compiled for one fixed
# instruction set - the thing DiffractorCompilerPolicy.cmake exists to prevent. So the model weights
# (src/facedetectcnn-data.cpp) are shared and everything above them comes from highway/.
#
# Upstream's highway/CMakeLists.txt is not configured: it finds Highway as an installed package, builds
# tests and benchmarks, and on x86 compiles the whole library for AVX2. The sources below are its
# fdt_hw_kernels library. Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake
# owns those for every target.

include_guard(GLOBAL)
include(DiffractorFetch)
diffractor_fetch(facedetection)

# highway/src/hw_kernels_intrinsics.cpp is built, and FDT_HW_ENABLE_X86_AVX2 is deliberately NOT
# defined. Every function in that file is an AVX2 body with an `#else` that forwards to the scalar
# kernel, so leaving the switch off costs nothing above SSE2 while still defining the symbols
# hw_layers.cpp and hw_blob.cpp reference unconditionally. Omitting the file instead does not lower
# the baseline; it makes the library fail to link. FDT_HW_ENABLE_HYBRID_CEILING, which selects those
# AVX2 bodies, stays off for the same reason.
add_library(diffractor_facedetection STATIC
        "${facedetection_SOURCE_DIR}/src/facedetectcnn-data.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/facedetect_hw.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_blob.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_filter.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_image.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_kernels_hw.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_kernels_intrinsics.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_kernels_scalar.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_layers.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_model.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_network.cpp"
        "${facedetection_SOURCE_DIR}/highway/src/hw_postprocess.cpp"
)

# facedetect_hw.h is the only header the C entry point needs, but Diffractor does not use that entry
# point: it owns the input stage so the network can be fed luminance straight out of a decoded
# surface. The internals below are what that costs - they declare the blob, the model table, the
# forward pass and the post-process, and they include nothing beyond the standard library.
target_include_directories(diffractor_facedetection PUBLIC
        "${facedetection_SOURCE_DIR}/highway/include"
        "${facedetection_SOURCE_DIR}/highway/src"
)

target_include_directories(diffractor_facedetection PRIVATE
        "${facedetection_SOURCE_DIR}/src"
)

target_link_libraries(diffractor_facedetection PRIVATE diffractor::highway)

diffractor_apply_vendored_policy(diffractor_facedetection)

add_library(diffractor::facedetection ALIAS diffractor_facedetection)
