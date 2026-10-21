# Local changes to vendored PS2Recomp

Upstream: https://github.com/ran-j/PS2Recomp at the commit in
`third_party/PS2Recomp/UPSTREAM_COMMIT`. Every local change is marked in the
source with a `[gta-recomp]` comment and listed here.

| File | Change | Why |
|---|---|---|
| `ps2xRuntime/cmake/ReleaseMode.cmake` | clang-cl branch: drop `/Qspectre-`, `/GL`, `/LTCG`, `/fp:fast`; LTO only with lld-link | clang-cl rejects `/Qspectre-`; `/GL` needs MSVC's linker. `/fp:fast` dropped for clang because it lets the compiler reorder/assume-no-NaN, which fights the runtime's PS2 float emulation. |
