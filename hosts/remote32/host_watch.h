#pragma once

// The 32-bit tab looks after the crash guard's marker of the Feed's 64-bit hosts that this game starts
// (host64\dlss5-feed-host64.exe, marker optimizer-fps-dlss5.session beside it). A host never unloads
// cleanly, so its add-on cannot remove the marker when the game stops it, and a stop within the grace
// used to read as a crash for every session after. The game side knows better, in two ways:
//
// * The game stopped a host and goes on (settings applied, a restart): the host's exit code is 0, where a
//   crash leaves an exception code and a removed device 3. The tab removes the marker of such a host,
//   unless the marker was written after the host ended (the next host's) or notes a removed device.
// * The game is shutting its Neural Rendering stack down. When the game destroys its device, the Feed
//   stops the host there and ReShade unloads the Feed before this tab: the tab settles the ended host on
//   its way out. At process exit the loader detaches this tab first and the Feed stops the host seconds
//   later, when the tab is gone: the tab signals (PW_REMOTE_LEAVING_EVENT_FORMAT_W, named after this
//   game's pid) and the host's add-on removes its own marker, unless its device was removed.
// Either way only the marker a host wrote itself goes; a crash's marker stays until Retry.

namespace ofps::remote {

// Once per present: checks the watched hosts; while none is watched, looks for one every 60 calls.
void WatchHosts();

// From DLL_PROCESS_DETACH (processExit: the DllMain reserved argument is not null).
void SignalLeaving(bool processExit);

} // namespace ofps::remote
