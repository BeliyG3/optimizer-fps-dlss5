#include "core/frame/acc_dump.h"

#include <fstream>
#include <system_error>

namespace ofps::core {

bool WriteAccDump(const std::filesystem::path &path, const AccDumpHeader &header, std::span<const float> model,
                  std::span<const float> chain, std::span<const std::uint8_t> reason, std::span<const float> carry)
{
    const std::size_t texels = static_cast<std::size_t>(header.width) * header.height, floats = 2u * texels;
    if (floats == 0 || model.size() != floats || chain.size() != floats || reason.size() != texels) return false;
    if (!carry.empty() && carry.size() != floats) return false;
    AccDumpHeader head = header;
    head.version = carry.empty() ? 2u : 3u;
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    bool written = false;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const auto bytes = static_cast<std::streamsize>(floats * sizeof(float));
        f.write(reinterpret_cast<const char *>(&head), sizeof(head));
        f.write(reinterpret_cast<const char *>(model.data()), bytes);
        f.write(reinterpret_cast<const char *>(chain.data()), bytes);
        f.write(reinterpret_cast<const char *>(reason.data()), static_cast<std::streamsize>(texels));
        if (!carry.empty()) f.write(reinterpret_cast<const char *>(carry.data()), bytes);
        f.close();
        written = f.good();
    }
    std::error_code ec;
    if (written) {
        std::filesystem::rename(tmp, path, ec);
        if (!ec) return true;
    }
    std::filesystem::remove(tmp, ec);
    return false;
}

} // namespace ofps::core
