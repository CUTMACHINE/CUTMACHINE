// STRUCTURE-2026-11 ST1 -- docs/roadmap/STRUCTURE-2026-11.md. The entry of
// `cutmachine` and of the app, kept out of the objects they share with the
// window test, which links the same ones under an entry of its own
// (tests/ui_smoke/UiSmokeMain.mm). It installs no test hooks.

#include "AppTestHooks.h"

int main(int argc, char* argv[]) {
    return cutmachine_app::RunCutmachine(argc, argv, nullptr);
}
