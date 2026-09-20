#pragma once

// The 32-bit tab's half of the shared block (pw_remote_ipc.h): finding the host's section,
// reading whichever protocol version it publishes, and sending an edit back. The drawing lives
// in producer_remote.cpp; this file knows nothing about ImGui.

#include "../pw_remote_ipc.h"

namespace pw_remote {

// Opens the section if it is not open yet (retried every 60 calls while the host is absent),
// then copies this frame's status and applied settings out of it. Call once per present.
void Poll();

// Releases the section: the host went away, or the add-on is unloading.
void Close();

// True once a block of any known version is mapped, regardless of the host's heartbeat.
bool Connected();

// True while the host's last present is recent (kPwRemoteHeartbeatTimeoutMs).
bool HostAlive();

// The protocol the host publishes: PW_REMOTE_VERSION, PW_REMOTE_VERSION_OFA or
// PW_REMOTE_VERSION_LEGACY, and 0 when nothing is connected.
unsigned int Version();

// The host's last published status, widened to this build's struct (fields an older host does
// not know stay zero).
const PwRemoteStatusV1 &Status();

// The values the controls show: adopted from the host once, then owned by the tab.
PwRemoteSettingsV1 &Local();
bool LocalValid();

// Throws away the tab's own values and takes the host's current ones again.
void ReloadFromHost();

// Publishes Local() to the host. A host on an older protocol gets the prefix it understands.
void Push();

} // namespace pw_remote
