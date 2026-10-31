include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    # [gta-recomp] clang only (the PS5's cross build and the build machine's recompiler; the top-level CMakeLists.txt
    # refuses other compilers). LTO is opt-in (PS2X_LTO=THIN|FULL; tools/ps5.py build --lto): with full LTO every relink
    # of the 150 MB game re-codegens everything, which kills iteration time.
    # -ffp-contract=off: the PS2's FMACs (VU and EE FPU) round the product and the sum separately, so clang must not
    # fuse `a + b * c` into one rounding. -fno-strict-aliasing: the runtime and generated code read guest memory through
    # casts. No stack protector / CET. -march: the PS5's Zen 2 (VCS_PS5), x86-64-v3 for the build machine's tools.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_ps2x_march -march=x86-64-v3)
        if(VCS_PS5)
            set(_ps2x_march -march=znver2)
        endif()
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:-O2 -DNDEBUG ${_ps2x_march} -ffp-contract=off -fno-math-errno -fno-strict-aliasing
                                -fno-stack-protector -fcf-protection=none>
        )
        get_target_property(_type ${TargetName} TYPE)
        if(PS2X_LTO STREQUAL "THIN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin
                                    -Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache
                                    -Wl,--thinlto-cache-policy=cache_size_bytes=1g:prune_after=72h>)
                # ThinLTO's backends run in parallel; each needs memory for a share of the 150 MB game
                if(PS2X_LTO_JOBS)
                    target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-Wl,--thinlto-jobs=${PS2X_LTO_JOBS}>)
                endif()
            endif()
        elseif(PS2X_LTO STREQUAL "FULL" AND IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
        # [gta-recomp] Profile-guided optimisation (PS2X_PGO): GEN = instrumented build that writes a .profraw when the
        # game exits; a .profdata path = optimise with that profile. Functions the profile doesn't match (code changed
        # since) are optimised as without it.
        if(PS2X_PGO STREQUAL "GEN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            target_compile_definitions(${TargetName} PRIVATE $<$<CONFIG:Release>:PS2X_PGO_GEN=1>)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            endif()
        elseif(PS2X_PGO)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-use=${PS2X_PGO}
                                   -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing
                                   -Wno-backend-plugin>)
        endif()
        return()
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()
