// Test hygiene -- a suite run by hand, outside CTest, keeps away from the
// user's own state, as it does under CTest. CMakeLists.txt gives every test
// these variables through CTest (ENVIRONMENT); this file, linked into every
// test executable, sets the same ones before main() when they are unset, so
// a variable a caller sets still wins. Found on 8 October 2026: suites run
// by hand had put 1 177 captures of their fixture projects, 30 MB, in the
// user's project archive (DONNEES-2026-10 DB2 archives every save).
// DEC44 -- and the archive's daily copy: a suite that turns the archive on
// in a folder of its own must not copy it into the folder the user's
// settings file names (a synchronized OneDrive library).

#include <cstdlib>

#ifndef CUTMACHINE_TEST_BINARY_DIR
#error "CUTMACHINE_TEST_BINARY_DIR names the build tree of the tests"
#endif

namespace {

struct TestEnvironment {
    TestEnvironment() {
        const auto unlessSet = [](const char* name, const char* value) {
            ::setenv(name, value, 0);
        };
        unlessSet("CUTMACHINE_PROJECT_ARCHIVE", "off");
        unlessSet("CUTMACHINE_ARCHIVE_BACKUP_DIR", "off");
        unlessSet("CUTMACHINE_LOG_DIR",
                  CUTMACHINE_TEST_BINARY_DIR "/test-logs");
        unlessSet("CUTMACHINE_MOTION_LIBRARY",
                  CUTMACHINE_TEST_BINARY_DIR "/test-motion-library");
        unlessSet("CUTMACHINE_MODELS", "off");
        // PA16 -- found on 13 November 2026: a suite that calls every tool
        // ran the Resolve bridge for real, and wrote its descriptors in the
        // user's data folder. The bridge named here does not exist.
        unlessSet("CUTMACHINE_RESOLVE_BRIDGE",
                  CUTMACHINE_TEST_BINARY_DIR "/test-no-resolve-bridge.py");
        unlessSet("CUTMACHINE_RESOLVE_SEND_DIR",
                  CUTMACHINE_TEST_BINARY_DIR "/test-resolve-send");
    }
};

const TestEnvironment kTestEnvironment;

}  // namespace
