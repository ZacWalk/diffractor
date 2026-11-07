# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build the vendored libfacedetection as diffractor::facedetection.
#
# Only the Highway detector is built. Upstream ships two implementations of the same network: the
# original src/facedetectcnn.cpp, whose SIMD is hand-written AVX2/AVX512/NEON selected by a macro at
# compile time, and highway/, which expresses the same kernels through google/highway. Diffractor
# already vendors highway, and the macro-selected original would have to be compiled for one fixed
# instruction set - the thing DiffractorCompilerPolicy.cmake exists to prevent. So the model weights
# (facedetectcnn-data.cpp) are shared and everything above them comes from highway/.
#
# Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake owns those for
# every target.

include_guard(GLOBAL)

set(_facedetection_dir "${CMAKE_SOURCE_DIR}/third-party/libfacedetection")

# highway/src/hw_kernels_intrinsics.cpp is built, and FDT_HW_ENABLE_X86_AVX2 is deliberately NOT
# defined. Every function in that file is an AVX2 body with an `#else` that forwards to the scalar
# kernel, so leaving the switch off costs nothing above SSE2 while still defining the symbols
# hw_layers.cpp and hw_blob.cpp reference unconditionally. Omitting the file instead does not lower
# the baseline; it makes the library fail to link.
add_library(diffractor_facedetection STATIC
        "${_facedetection_dir}/src/facedetectcnn-data.cpp"
        "${_facedetection_dir}/highway/src/facedetect_hw.cpp"
        "${_facedetection_dir}/highway/src/hw_blob.cpp"
        "${_facedetection_dir}/highway/src/hw_filter.cpp"
        "${_facedetection_dir}/highway/src/hw_image.cpp"
        "${_facedetection_dir}/highway/src/hw_kernels_hw.cpp"
        "${_facedetection_dir}/highway/src/hw_kernels_intrinsics.cpp"
        "${_facedetection_dir}/highway/src/hw_kernels_scalar.cpp"
        "${_facedetection_dir}/highway/src/hw_layers.cpp"
        "${_facedetection_dir}/highway/src/hw_model.cpp"
        "${_facedetection_dir}/highway/src/hw_network.cpp"
        "${_facedetection_dir}/highway/src/hw_postprocess.cpp"
)

# facedetect_hw.h is the only header the C entry point needs, but Diffractor does not use that entry
# point: it owns the input stage so the network can be fed luminance straight out of a decoded
# surface. The internals below are what that costs - they declare the blob, the model table, the
# forward pass and the post-process, and they include nothing beyond <vector>.
target_include_directories(diffractor_facedetection PUBLIC
        "${_facedetection_dir}/highway/include"
        "${_facedetection_dir}/highway/src"
)

target_include_directories(diffractor_facedetection PRIVATE
        "${_facedetection_dir}/src"
)

target_link_libraries(diffractor_facedetection PRIVATE diffractor::highway)

diffractor_apply_vendored_policy(diffractor_facedetection)

# Anything upstream's own CMakeLists could not be carried across - a flag a different compiler
# needs, an architecture-specific decision. Hand written, and kept out of this file so that
# re-vendoring does not discard it.
include("${CMAKE_CURRENT_LIST_DIR}/facedetection.local.cmake" OPTIONAL)

add_library(diffractor::facedetection ALIAS diffractor_facedetection)
