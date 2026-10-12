# Compiler settings applied to Meridian's own targets (never to third-party code).

# IEEE-754 semantics are a hard requirement: the compensated summing bus relies on
# the compiler NOT re-associating floating-point additions. -ffast-math or /fp:fast
# would silently delete the error-compensation terms. src/core/dsp/SimdConfig.h
# turns that mistake into a compile error.
function(meridian_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:__cplusplus /utf-8 /fp:precise)
        if(MERIDIAN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
            -ffp-contract=off)
        if(MERIDIAN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    meridian_configure_simd(${target})
endfunction()

# SIMD baseline: SSE2 is guaranteed on every x86-64 CPU and NEON on every AArch64
# CPU, so those paths are always on. AVX2 is opt-in because it would make the
# binary crash on older machines.
function(meridian_configure_simd target)
    if(NOT MERIDIAN_ENABLE_AVX2)
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /arch:AVX2)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(${target} PRIVATE -mavx2)
    endif()
endfunction()
