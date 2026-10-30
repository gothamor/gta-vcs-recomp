// The launcher's window: a sidebar (the four setup steps as a tracker, then Saves, Settings, Help), the selected page
// or the running task in the main column, and a footer whose one primary button is the next thing to do.
#pragma once

#include "tasks.h"

#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;
struct ImFont;

namespace launcher
{
    enum Page
    {
        kFolder,
        kDisc,
        kTools,
        kGame,
        kSaves,
        kSettings,
        kHelp,
        kPageCount
    };

    struct Ui
    {
        SDL_Window *window = nullptr;
        ImFont *body = nullptr, *bold = nullptr;
        float scale = 1.0f;
        int page = -1;        // the page shown; -1 = follow what's next
        bool showLog = false, showDrives = false;
        bool wantQuit = false, quitNow = false, confirmRestore = false;
        std::string restoreName, ripDrive;
        bool minimizedForGame = false;
        bool backupsAsked = false;
        bool awaitingBuildDir = false; // a folder was checked (tools/build_dir.py): confirm when the answer comes

        // file dialogs answer on another thread
        std::mutex dialogLock;
        std::vector<std::string> pickedIso, pickedRipTarget, pickedBuildDir;
    };

    void applyStyle(Ui &ui);
    void drawUi(App &app, Ui &ui, double now);
    void openIsoDialog(Ui &ui);
}
