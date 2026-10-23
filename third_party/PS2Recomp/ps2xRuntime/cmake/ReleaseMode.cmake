include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    # [gta-recomp] clang-cl rejects /Qspectre- and /GL//LTCG need MSVC's own
    # linker; use plain optimisation flags there and leave LTO to IPO below.
    # LTO is opt-in (PS2X_LTO=THIN|FULL): with full LTO every relink of the
    # 150 MB game re-codegens everything, which kills iteration time.
    # -ffp-contract=off: /arch:AVX2 enables FMA and clang would fuse `a + b * c` into one rounding; the PS2's
    # FMACs (VU and EE FPU) round the product and the sum separately.
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:/O2 /Oi /Gy /Gw /GF /DNDEBUG /arch:AVX2 /GS- /clang:-fno-math-errno /clang:-ffp-contract=off>
        )
        if(PS2X_LTO STREQUAL "THIN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/lldltocache:${CMAKE_BINARY_DIR}/lto-cache>)
        elseif(PS2X_LTO STREQUAL "FULL" AND IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
        # [gta-recomp] Profile-guided optimisation (PS2X_PGO, tools/build.py --opt pgo): GEN = instrumented build that
        # writes a .profraw when the game exits; a .profdata path = optimise with that profile. Functions the profile
        # doesn't match (code changed since) are optimised as without it.
        if(PS2X_PGO STREQUAL "GEN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            target_compile_definitions(${TargetName} PRIVATE $<$<CONFIG:Release>:PS2X_PGO_GEN=1>)
            get_target_property(_type ${TargetName} TYPE)
            if(_type STREQUAL "EXECUTABLE")
                execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir OUTPUT_VARIABLE _resource
                                OUTPUT_STRIP_TRAILING_WHITESPACE)
                target_link_options(${TargetName} PRIVATE "${_resource}/lib/windows/clang_rt.profile-x86_64.lib")
            endif()
        elseif(PS2X_PGO)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-use=${PS2X_PGO}
                                   -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing
                                   -Wno-backend-plugin>)
        endif()
        return()
    endif()
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()