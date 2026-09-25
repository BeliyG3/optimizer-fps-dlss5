#pragma once

namespace ofps::remote {

// The x86 tab only reads its own ReShade.ini. Migration is owned by the x64 host.
const char *ActiveIniSectionReadOnly();

} // namespace ofps::remote
