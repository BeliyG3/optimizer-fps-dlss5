#pragma once

#include "core/api/ofps_ui_source.h"
#include "remote_link.h"

namespace ofps::remote {

class RemoteUiSource final : public ui::IOfpsUiSource {
public:
    bool Snapshot(ui::UiSnapshot &out) override;
    bool Commit(std::uint32_t id, OfpsSettingValue value) override;
    bool CommitFeeder(std::uint32_t source, std::uint32_t grid, std::uint32_t perf);
    std::uint32_t StatusLines(OfpsStatusRow *rows, std::uint32_t capacity) override;
    const WireShell &Shell() const { return snapshot_.shell; }

private:
    WireSnapshot snapshot_{};
    OfpsSettingsValues local_{};
    bool valid_ = false;
    bool pending_ = false;
    std::uint32_t pendingGeneration_ = 0;
};

} // namespace ofps::remote
