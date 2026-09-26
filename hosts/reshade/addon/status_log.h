#pragma once

namespace ofps::reshade {

// One line in ReShade.log every 30 s while the model is hooked: how many evaluates were warped,
// passed through or shown as plain colour, and why the last one did not warp. A session can then be
// read from the log alone, also in a 64-bit helper process whose tab is out of reach.
void LogStatusPeriodically();

} // namespace ofps::reshade
