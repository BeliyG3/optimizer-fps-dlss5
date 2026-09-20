# Two independent version sources.
#
# PW_SDK_VERSION     - the semantic version of the PeripheralWarp SDK/library (the CMake
#                      `project(... VERSION ...)`, the installed package version, the ABI story).
#                      It moves only when the public API/ABI or the library itself changes.
# PW_RELEASE_VERSION - the user-facing release number of the ReShade add-on ("the mod"). Since
#                      2026.9 it is the year and month of the release: a second release inside one
#                      month adds a third component (2026.9.1). Releases up to 26.29 carried the
#                      number of the work stage they came out of; that line ended there.
#
# The add-on's exported NAME is deliberately version-free (ReShade keys its DisabledAddons list on
# NAME, so a version inside the name resets the user's choice on every release); the version lives
# in DESCRIPTION and in the overlay banner instead.

set(PW_SDK_VERSION "0.5.0")
set(PW_RELEASE_VERSION "2026.9")
