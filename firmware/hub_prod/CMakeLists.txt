cmake_minimum_required(VERSION 3.16)

# The firmware version IS the git description - "a3cb57d", or "a3cb57d-dirty"
# when the tree has uncommitted changes, or "v1.2-4-ga3cb57d" once anything is
# tagged. It goes into the app descriptor, which is what RainMaker shows on the
# dashboard and what its OTA compares against.
#
# Why not a hand-maintained semver: RainMaker refuses an update whose version
# string equals the running one (esp_rmaker_ota.c, the version check around the
# memcmp of app_desc.version). A number somebody has to remember to bump is a
# number that eventually does not get bumped, and the symptom is an OTA that
# silently declines with "Current running version is same as the new". A commit
# hash cannot be forgotten, and it says exactly which code is on the box.
#
# Ordering is not needed: nothing here requires the new version to be GREATER,
# only different. So a hash is a complete answer and a semver is not required.
execute_process(
    COMMAND git describe --always --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE _git_ver
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_rc)
if(_git_rc EQUAL 0 AND _git_ver)
    set(PROJECT_VER "${_git_ver}")
else()
    # Left loud on purpose. Falling back silently is how two different builds
    # both end up reporting "1", and then OTA refuses the second one for
    # matching the first - with an error that blames the version, not the build.
    message(WARNING "git describe failed - firmware version will not identify the code on the device")
endif()

# No CMAKE_CONFIGURE_DEPENDS on .git/HEAD here: ESP-IDF already registers it (it
# stamps its own git description into the build), and adding it a second time
# makes ninja fail outright with ".git/HEAD is defined as an output multiple
# times" - which is how this was found. IDF's own dependency is what re-runs
# configure when the commit changes, so the behaviour is there; it just is not
# ours to add.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ro_hub)
