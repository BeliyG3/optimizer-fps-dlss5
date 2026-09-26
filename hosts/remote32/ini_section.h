#pragma once

namespace ofps::remote {

// The x86 tab only reads its own ReShade.ini; migration is owned by the x64 host.
// Reads an int from [OptimizerFPS], falling back to the legacy [PeripheralWarp].
// Goes through ReShade's config API: some games cannot open ReShade.ini directly.
bool GetConfigInt(const char *key, int &value);

} // namespace ofps::remote
