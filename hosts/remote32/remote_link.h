#pragma once

#include "ipc.h"

namespace ofps::remote {

enum class Connection { Missing, OtherVersion, Live, Disconnected };

void Poll();
void Close();
Connection State();
bool Snapshot(WireSnapshot &out);
bool PublishEdit(const OfpsSettingsValues &values, std::uint32_t *generation = nullptr);
bool PublishFeeder(std::uint32_t source, std::uint32_t grid, std::uint32_t perf,
                   std::uint32_t *generation = nullptr);
bool RetryAvailable();
void RequestRetry();

} // namespace ofps::remote
