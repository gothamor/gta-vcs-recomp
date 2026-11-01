# macOS (experimental)

The Apple Silicon build uses SDL3 and Vulkan through MoltenVK. It requires your own **PS2 US SLUS-21590 v1.00** disc image; the PSP release is a different game build. The Windows launcher is not available on macOS.

Install Xcode command line tools and Homebrew, then install the dependencies:

```sh
brew install cmake ninja glslang molten-vk vulkan-loader ffmpeg pkgconf git
python3 tools/build.py --check-deps
python3 tools/build.py --recompile /path/to/your-ps2-disc.iso --quick
python3 tools/run.py 0 play
```

The first build downloads its source dependencies. It saves the disc path in `build/vcs.ini`; the disc image remains necessary when playing. `build/vcs` can also be run directly, with the ISO path as an argument. Saves and caches are stored under `~/Library/Application Support/VCSRecomp`; set `VCS_DATA_DIR` to isolate a test run. The default macOS build does not use LTO.

On an Apple M4 Pro with MoltenVK 1.4.2, a hidden new-game run reached the first mission outdoors and a simulated W key moved the player 5.21 units. A visible launch created a Vulkan swapchain and played intro video frames. In a 45-second reference run, VU0 checks reported no differences in the exercised routines, while the world verification reported 11 packets off by more than one unit among 1,168,999 packets (plus 11 within one). Longer gameplay, audio, graphics parity and other Macs still need verification. The recompiler reported unhandled instructions during generation; their impact has not been established.
