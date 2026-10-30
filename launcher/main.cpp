// VCSLauncher: from "downloaded the release" to playing, without a terminal (docs/LAUNCHER.md).
//
//   VCSLauncher.exe                     the window
//   VCSLauncher.exe --check             tools/doctor.py's report in the terminal
//   VCSLauncher.exe --build [--iso X]   tools/build.py --recompile-if-needed, output in the terminal
//   VCSLauncher.exe --play [--iso X]    start the game and wait for it; its exit code
//   VCSLauncher.exe --self-test [--iso X]   the launcher's own checks (CI): repository, Python, doctor, disc check
//   ... --out FILE                      the output to a file instead (scripts, CI)
//   VCSLauncher.exe --screenshot F.png [--step N] [--demo running|error] [--action build --every S] [--iso X]
//                                       [--pick-build-dir PATH] (as if chosen on the Folder step)
//                                       the window rendered hidden and saved (tests, docs)
//
// The command-line forms print into the terminal that started them (a GUI exe: in PowerShell use
// `Start-Process -Wait -NoNewWindow`, in cmd `start /wait`).
#include "disc_check.h"
#include "system.h"
#include "tasks.h"
#include "ui.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

namespace
{
    std::string argValue(const std::vector<std::string> &args, const char *name)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name)
                return args[i + 1];
        return {};
    }

    bool hasArg(const std::vector<std::string> &args, const char *name)
    {
        for (const auto &a : args)
            if (a == name)
                return true;
        return false;
    }

    int runInConsole(const std::vector<std::wstring> &argv, const fs::path &cwd)
    {
        Process p;
        Process::Options options;
        options.cwd = cwd;
        options.inheritConsole = true;
        options.env = {{"PYTHONUTF8", "1"}};
        std::string error;
        if (!p.start(argv, options, error))
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        return p.wait();
    }

    int commandLine(const std::vector<std::string> &args)
    {
        if (const std::string out = argValue(args, "--out"); !out.empty())
            redirectOutput(pathFrom(out));
        else
            attachParentConsole();
        App app;
        std::string error;
        // init() runs the doctor in the background and scans for a disc; the command-line forms do their own
        if (!app.init(error))
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        const std::string iso = argValue(args, "--iso");
        if (!iso.empty())
            app.chooseDisc(pathFrom(iso));
        auto tool = [&](const char *name) {
            std::vector<std::wstring> argv = app.python;
            argv.insert(argv.end(), {L"-I", (app.root / "tools" / name).wstring()});
            return argv;
        };
        if (hasArg(args, "--check"))
            return runInConsole(tool("doctor.py"), app.root);
        if (hasArg(args, "--build"))
        {
            auto argv = tool("build.py");
            argv.push_back(L"--recompile-if-needed");
            if (app.disc.ok)
                argv.push_back(app.iso.wstring());
            return runInConsole(argv, app.root);
        }
        if (hasArg(args, "--play"))
        {
            if (!app.disc.ok)
            {
                std::fprintf(stderr, "error: no disc: %s\n", app.disc.message.c_str());
                return 2;
            }
            if (!app.play(error))
            {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 1;
            }
            std::printf("playing; the game's output: %s\n", pathText(app.gameLog).c_str());
            return app.game->wait();
        }
        if (hasArg(args, "--self-test"))
        {
            int failed = 0;
            auto check = [&](bool ok, const std::string &what) {
                std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
                failed += ok ? 0 : 1;
            };
            check(fs::is_regular_file(app.root / "tools" / "build.py"), "repository: " + pathText(app.root));
            check(!app.python.empty(), "python: " + app.pythonText);
            check(app.toolsProblem.empty(), app.toolsProblem.empty() ? "scripts: all in tools\\" : app.toolsProblem);
            for (int i = 0; i < 6000 && (app.doctorRunning || !app.doctorDone); ++i)
            {
                app.update(i * 0.01);
                SDL_Delay(10);
            }
            check(app.doctor.isObject(), "doctor.py --json" + (app.doctorError.empty() ? "" : ": " + app.doctorError));
            check(!app.doctor["next"].str().empty(), "doctor's next step: " + app.doctor["next"].str());
            if (!iso.empty())
                check(app.disc.ok, "disc: " + app.disc.message);
            const DiscResult missing = checkDisc(app.root / "no-such-disc.iso");
            check(!missing.ok && missing.code == "not_found", "disc check refuses a missing file");
            const DiscResult text = checkDisc(app.root / "README.md");
            check(!text.ok && text.code == "not_iso", "disc check refuses a text file: " + text.message);
            std::printf("%s\n", failed ? "self-test FAILED" : "self-test ok");
            return failed ? 1 : 0;
        }
        std::fprintf(stderr, "usage: VCSLauncher [--check | --build | --play | --self-test] [--iso PATH] [--out FILE]\n");
        return 1;
    }

    ImFont *loadFont(const char *file)
    {
        // Segoe UI is on every Windows; the built-in font otherwise
        const fs::path path = fs::path(L"C:\\Windows\\Fonts") / file;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
            return nullptr;
        return ImGui::GetIO().Fonts->AddFontFromFileTTF(pathText(path).c_str(), 18.0f);
    }
}

namespace
{
    // --screenshot: the window rendered hidden into a texture and saved, for tests and the docs (no window on the
    // desktop: test runs stay hidden, AGENTS.md). --step N picks the step shown, --demo a sample task, --action build
    // runs the real build and saves a picture every --every seconds until it ends.
    struct Shots
    {
        std::string path;
        int step = -1;
        std::string demo, action, iso, pickBuildDir;
        double wait = 4.0, every = 0;
        int width = 1040, height = 680;
    };

    void demoTask(App &app, const std::string &demo)
    {
        app.task = std::make_unique<Task>();
        Task &t = *app.task;
        t.kind = "build";
        t.title = "Building the game";
        t.stages = {{"disc", "Extracting the executable from your disc", 0, 1},
                    {"recompiler", "Building the recompiler", 1, 40},
                    {"translate", "Translating MIPS to C++", 40, 130},
                    {"vu1", "Compiling VU1 microcode", 130, 134},
                    {"compile", "Compiling the game", 134, 0}};
        t.startTime = 0;
        t.done = 1180;
        t.total = 3400;
        t.eta = 1420;
        t.note = "compiling";
        for (int i = 0; i < 40; ++i)
            t.log.push_back("[build] ps2EntryRunner " + std::to_string(i * 30) + "/3400 ...");
        if (demo == "error")
        {
            t.finished = true;
            t.exitCode = 1;
            t.endTime = 900;
            t.stages.back().end = 900;
            t.errorCode = "disk_full";
            t.errorMessage = "building ps2EntryRunner failed";
            t.errorLog = "build/build.log";
            t.log.push_back("LLVM ERROR: IO failure on output stream: no space on device");
        }
    }

    int gui(const Shots *shots)
    {
        SDL_SetHint(SDL_HINT_APP_NAME, "VCS Launcher");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCS Launcher", SDL_GetError(), nullptr);
            return 1;
        }
        App app;
        std::string error;
        if (!app.init(error))
        {
            if (shots)
                std::fprintf(stderr, "error: %s\n", error.c_str());
            else
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCS Launcher", error.c_str(), nullptr);
            SDL_Quit();
            return 1;
        }

        Ui ui;
        const float displayScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        ui.scale = shots ? 1.0f : displayScale > 0 ? displayScale : 1.0f;
        const int width = shots ? shots->width : int(1040 * ui.scale), height = shots ? shots->height : int(680 * ui.scale);
        ui.window = SDL_CreateWindow("GTA: Vice City Stories - Launcher", width, height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
        SDL_Renderer *renderer = ui.window ? SDL_CreateRenderer(ui.window, nullptr) : nullptr;
        if (!renderer)
        {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VCS Launcher", SDL_GetError(), nullptr);
            return 1;
        }
        SDL_Texture *target = nullptr;
        if (shots)
        {
            target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height);
            SDL_SetRenderTarget(renderer, target);
        }
        else
        {
            SDL_SetRenderVSync(renderer, 1);
            SDL_SetWindowMinimumSize(ui.window, int(820 * ui.scale), int(560 * ui.scale));
            SDL_SetWindowPosition(ui.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            SDL_ShowWindow(ui.window);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ui.body = loadFont("segoeui.ttf");
        ui.bold = loadFont("seguisb.ttf");
        if (!ui.bold)
            ui.bold = ui.body;
        ImGui::GetStyle().FontSizeBase = 18.0f;
        applyStyle(ui);
        ImGui_ImplSDL3_InitForSDLRenderer(ui.window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);

        if (shots)
        {
            if (!shots->iso.empty())
                app.chooseDisc(pathFrom(shots->iso));
            ui.page = shots->step;
            if (!shots->demo.empty())
                demoTask(app, shots->demo);
        }
        bool startedAction = false;
        int shotIndex = 0;
        double lastShot = -1e9;

        const Uint64 start = SDL_GetTicks();
        bool checkedUpdates = shots != nullptr;
        while (!ui.quitNow)
        {
            // idle: wake for events or every quarter second; busy: ~20 times a second
            const bool active = app.busy() || app.doctorRunning || app.gameRunning;
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, shots ? 30 : active ? 50 : 250))
            {
                do
                {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    if (event.type == SDL_EVENT_QUIT ||
                        (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(ui.window)))
                        ui.wantQuit = true;
                    if (event.type == SDL_EVENT_DROP_FILE && event.drop.data)
                    {
                        app.chooseDisc(pathFrom(event.drop.data));
                        ui.page = kDisc;
                    }
                } while (SDL_PollEvent(&event));
            }
            const double now = double(SDL_GetTicks() - start) / 1000.0;
            const bool wasRunning = app.gameRunning;
            app.update(now);
            if (wasRunning && !app.gameRunning && ui.minimizedForGame)
            {
                // back from the game (always after an error, which the launcher explains)
                SDL_RestoreWindow(ui.window);
                SDL_RaiseWindow(ui.window);
                ui.minimizedForGame = false;
            }
            if (!checkedUpdates && app.settings.checkUpdates && app.doctorDone && !app.busy())
            {
                checkedUpdates = true;
                app.startUpdateCheck(true);
            }
            if (shots && !shots->pickBuildDir.empty() && !startedAction && app.doctorDone)
            {
                startedAction = true; // as if the folder dialog had answered
                std::lock_guard<std::mutex> lock(ui.dialogLock);
                ui.pickedBuildDir.push_back(shots->pickBuildDir);
            }
            if (shots && shots->action == "build" && !startedAction && app.doctorDone)
            {
                startedAction = true;
                app.startBuild();
            }
            if (!shots && (SDL_GetWindowFlags(ui.window) & SDL_WINDOW_MINIMIZED))
                continue;

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            if (shots)
                io.DisplaySize = ImVec2(float(width), float(height));
            ImGui::NewFrame();
            drawUi(app, ui, now);
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 23, 18, 33, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

            if (shots)
            {
                SDL_FlushRenderer(renderer); // nothing is presented here: without this SDL's command queue only grows
                const bool settled = app.doctorDone && !app.doctorRunning && now >= shots->wait;
                const bool actionDone = shots->action.empty() ? settled : startedAction && !app.busy() && settled;
                const bool periodic = shots->every > 0 && now - lastShot >= shots->every;
                if (periodic || actionDone)
                {
                    std::string path = shots->path;
                    if (shots->every > 0 && !actionDone)
                    {
                        char suffix[16];
                        std::snprintf(suffix, sizeof(suffix), "-%03d", shotIndex++);
                        const size_t dot = path.rfind('.');
                        path = path.substr(0, dot) + suffix + (dot == std::string::npos ? "" : path.substr(dot));
                    }
                    if (SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr))
                    {
                        SDL_SavePNG(surface, path.c_str());
                        SDL_DestroySurface(surface);
                        std::printf("screenshot: %s (%.0f s)\n", path.c_str(), now);
                    }
                    lastShot = now;
                    if (actionDone)
                        ui.quitNow = true;
                }
            }
            else
                SDL_RenderPresent(renderer);
            if (shots && ui.wantQuit)
                ui.quitNow = true;
        }

        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        const int code = app.task && app.task->finished && !app.task->ok() ? 1 : 0;
        app.task.reset(); // a task cancelled on the way out: its job ends the tree
        if (target)
            SDL_DestroyTexture(target);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(ui.window);
        SDL_Quit();
        return shots ? code : 0;
    }
}

int main(int argc, char *argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (hasArg(args, "--screenshot"))
    {
        if (const std::string out = argValue(args, "--out"); !out.empty())
            redirectOutput(pathFrom(out));
        Shots shots;
        shots.path = argValue(args, "--screenshot");
        if (const std::string step = argValue(args, "--step"); !step.empty())
            shots.step = std::atoi(step.c_str());
        shots.demo = argValue(args, "--demo");
        shots.action = argValue(args, "--action");
        shots.iso = argValue(args, "--iso");
        shots.pickBuildDir = argValue(args, "--pick-build-dir");
        if (const std::string wait = argValue(args, "--wait"); !wait.empty())
            shots.wait = std::atof(wait.c_str());
        if (const std::string every = argValue(args, "--every"); !every.empty())
            shots.every = std::atof(every.c_str());
        return gui(&shots);
    }
    if (!args.empty())
        return commandLine(args);
    return gui(nullptr);
}
