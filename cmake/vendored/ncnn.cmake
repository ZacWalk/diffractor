# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build the vendored ncnn as diffractor::ncnn.
#
# This is the one vendored library whose own build is driven rather than restated. ncnn generates
# layer_declaration.h, layer_registry.h and layer_type_enum.h at configure time, and on x86 it
# generates a second and third copy of each SIMD source so one binary can dispatch to AVX2 or
# AVX-512 at run time. An explicit source list cannot describe any of that, and a hand-maintained
# transcription of it would be wrong the first time a layer moved.
#
# What Diffractor owns instead is the configuration below, which is the whole reason the vendored
# tree is 20 MB rather than 90: no Vulkan, no OpenMP, no tools, and only the layers MobileFaceNet
# and the face pipeline actually execute.

include_guard(GLOBAL)

set(_ncnn_dir "${CMAKE_SOURCE_DIR}/third-party/ncnn")

if (NOT EXISTS "${_ncnn_dir}/CMakeLists.txt")
    return()
endif ()

# ncnn declares its switches with option(), under a cmake_minimum_required that predates CMP0077,
# so a plain variable would be ignored and the default taken. They have to be cached.
function(_diffractor_ncnn_set name value)
    set(${name} ${value} CACHE BOOL "" FORCE)
endfunction()

# Vulkan is the single largest thing excluded, and it is excluded structurally: src/layer/vulkan and
# every .comp shader are not vendored at all, so this is not a switch that can be turned back on
# without re-vendoring. Diffractor already owns a Direct3D device and its lifetime rules
# (docs/rendering.md); a second, independently managed GPU device inside an inference library is a
# device-loss and memory-budget problem with no owner.
_diffractor_ncnn_set(NCNN_VULKAN OFF)
_diffractor_ncnn_set(NCNN_SIMPLEVK OFF)
_diffractor_ncnn_set(NCNN_SYSTEM_GLSLANG OFF)

# OpenMP would give ncnn a thread pool of its own, sized to the machine rather than to what
# Diffractor has decided to spend. Face work is queued per item on a worker that is already one of
# several, so the parallelism belongs at that level: ncnn runs single-threaded on whichever worker
# called it. This is also what keeps the library inside the threading rules in AGENTS.md.
_diffractor_ncnn_set(NCNN_OPENMP OFF)
_diffractor_ncnn_set(NCNN_SIMPLEOMP OFF)

# Nothing below is reachable from an embedding model, and each one costs build time and binary size.
_diffractor_ncnn_set(NCNN_BUILD_TOOLS OFF)
_diffractor_ncnn_set(NCNN_BUILD_EXAMPLES OFF)
_diffractor_ncnn_set(NCNN_BUILD_BENCHMARK OFF)
_diffractor_ncnn_set(NCNN_BUILD_TESTS OFF)
_diffractor_ncnn_set(NCNN_PYTHON OFF)
_diffractor_ncnn_set(NCNN_INSTALL_SDK OFF)
_diffractor_ncnn_set(NCNN_SHARED_LIB OFF)
_diffractor_ncnn_set(NCNN_SIMPLEOCV OFF)
_diffractor_ncnn_set(NCNN_SIMPLESTL OFF)
_diffractor_ncnn_set(NCNN_SIMPLEMATH OFF)
_diffractor_ncnn_set(NCNN_BENCHMARK OFF)
_diffractor_ncnn_set(NCNN_C_API OFF)

# MobileFaceNet ships as float32. Turning the quantized paths off also removes the reason to build
# Quantize, Dequantize and Requantize, which is why they are absent from the layer list below.
_diffractor_ncnn_set(NCNN_INT8 OFF)
_diffractor_ncnn_set(NCNN_BF16 OFF)
_diffractor_ncnn_set(NCNN_WEIGHT_QUANT OFF)

# Kept on. NCNN_STRING is what makes a .param file loadable by layer name; NCNN_PIXEL_AFFINE is the
# warp that turns five landmarks into a 112x112 aligned face, and is the reason this pipeline needs
# no OpenCV. NCNN_RUNTIME_CPU is the dispatch that lets one binary use AVX2 where it exists while
# still starting on an SSE2 machine.
_diffractor_ncnn_set(NCNN_STDIO ON)
_diffractor_ncnn_set(NCNN_STRING ON)
_diffractor_ncnn_set(NCNN_THREADS ON)
_diffractor_ncnn_set(NCNN_PIXEL ON)
_diffractor_ncnn_set(NCNN_PIXEL_AFFINE ON)
_diffractor_ncnn_set(NCNN_PIXEL_ROTATE OFF)
_diffractor_ncnn_set(NCNN_PIXEL_DRAWING OFF)
_diffractor_ncnn_set(NCNN_RUNTIME_CPU ON)
_diffractor_ncnn_set(NCNN_DISABLE_RTTI OFF)
_diffractor_ncnn_set(NCNN_DISABLE_EXCEPTION OFF)

# The layers this build has. A .param naming anything else fails to load with a message rather than
# misbehaving, which is the failure mode worth having.
#
# Three groups, and the second is the one that is easy to get wrong:
#   the graph        convolution, convolutiondepthwise, innerproduct, pooling, prelu, binaryop,
#                    eltwise, batchnorm, scale, flatten, reshape, split, concat and their neighbours
#   created by ncnn  packing and cast are inserted by Net when a packed or half-storage layout is
#                    chosen; padding backs copy_make_border, which every convolution calls; crop,
#                    interp, flatten and bias back the Mat helpers in mat.cpp; convolution creates
#                    innerproduct, convolutiondepthwise creates convolution, innerproduct creates
#                    flatten, matmul creates gemm
#   cheap insurance  the activations and shape operators an exporter may emit for the same graph
#
# Anything not named here is switched off below and its sources are not vendored.
set(DIFFRACTOR_NCNN_LAYERS
        batchnorm bias binaryop cast clip concat convolution convolutiondepthwise crop dropout
        eltwise expanddims flatten gemm hardsigmoid hardswish innerproduct input interp matmul
        memorydata mish noop packing padding permute pooling prelu reduction relu reshape scale
        sigmoid slice softmax split squeeze swish tanh unaryop)

# Derived rather than listed, so that the set of layers switched off cannot drift from the set of
# layer sources the vendored tree still carries. ncnn errors at generate time on a layer that is on
# but missing, which is the check that keeps the two in step across an upgrade.
file(STRINGS "${_ncnn_dir}/src/CMakeLists.txt" _ncnn_layer_lines REGEX "^ncnn_add_layer\\(")

foreach (_line IN LISTS _ncnn_layer_lines)
    string(REGEX REPLACE "^ncnn_add_layer\\(([A-Za-z0-9_]+).*$" "\\1" _class "${_line}")
    string(TOLOWER "${_class}" _name)

    if (_name IN_LIST DIFFRACTOR_NCNN_LAYERS)
        _diffractor_ncnn_set(WITH_LAYER_${_name} ON)
    else ()
        _diffractor_ncnn_set(WITH_LAYER_${_name} OFF)
    endif ()
endforeach ()

# Not EXCLUDE_FROM_ALL: every other vendored library is built by a plain `dd build`, and one that
# is not would go on compiling only in the configuration nobody runs until something links it.
add_subdirectory("${_ncnn_dir}" "${CMAKE_BINARY_DIR}/vendored/ncnn")

if (NOT TARGET ncnn)
    return()
endif ()

# ncnn hard-codes /fp:fast for MSVC Release builds. Diffractor owns the floating-point mode for
# every target, so remove the upstream override and let DiffractorCompilerPolicy apply /fp:precise.
if (MSVC)
    get_target_property(_ncnn_compile_options ncnn COMPILE_OPTIONS)
    list(REMOVE_ITEM _ncnn_compile_options /fp:fast)
    set_target_properties(ncnn PROPERTIES COMPILE_OPTIONS "${_ncnn_compile_options}")
endif ()

# generate_export_header() writes ncnn_export.h with a dllimport attribute unless this is defined.
# ncnn sets it on its own compilation only, so without this every consumer of the static library
# fails to link.
target_compile_definitions(ncnn INTERFACE NCNN_STATIC_DEFINE)

diffractor_apply_vendored_policy(ncnn)

include("${CMAKE_CURRENT_LIST_DIR}/ncnn.local.cmake" OPTIONAL)

add_library(diffractor::ncnn ALIAS ncnn)
