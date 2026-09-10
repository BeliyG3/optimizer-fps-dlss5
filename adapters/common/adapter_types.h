#pragma once

#include <cstddef>
#include <cstdint>

namespace pw {

enum class AdapterStatus : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    NotInitialized,
    UnsupportedFormat,
    ShaderBytecodeMissing,
    DeviceError,
    ResourceMismatch,
};

struct ShaderBytecode {
    const void *data;
    std::size_t size;
};

struct ShaderSet {
    ShaderBytecode fullscreenVertex;
    ShaderBytecode packPixel;
    ShaderBytecode unpackPixel;
    // Optional. ABI-v1 callers that initialize only the first three fields keep
    // their previous behavior and simply cannot draw the diagnostic outline.
    ShaderBytecode outlinePixel{};
};

// Transient display-only diagnostics. These bits are supplied for an individual
// Unpack/outline recording call; they are never stored in ConfigV1/ConfigV2.
enum DiagnosticOutlineFlags : std::uint32_t {
    DiagnosticOutlineNone = 0,
    DiagnosticOutlineCenter = 1u << 0,
    DiagnosticOutlineRawWork = 1u << 1,
};

inline constexpr std::uint32_t kDiagnosticOutlineMask =
    DiagnosticOutlineCenter | DiagnosticOutlineRawWork;

[[nodiscard]] inline constexpr DiagnosticOutlineFlags operator|(
    DiagnosticOutlineFlags left, DiagnosticOutlineFlags right) noexcept
{
    return static_cast<DiagnosticOutlineFlags>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] inline constexpr bool ValidDiagnosticOutlineFlags(
    DiagnosticOutlineFlags flags) noexcept
{
    return (static_cast<std::uint32_t>(flags) & ~kDiagnosticOutlineMask) == 0;
}

[[nodiscard]] inline bool IsValid(ShaderBytecode bytecode) noexcept
{
    return bytecode.data != nullptr && bytecode.size != 0;
}

[[nodiscard]] inline const char *AdapterStatusString(AdapterStatus status) noexcept
{
    switch (status) {
    case AdapterStatus::Ok: return "ok";
    case AdapterStatus::InvalidArgument: return "invalid argument";
    case AdapterStatus::NotInitialized: return "adapter is not initialized";
    case AdapterStatus::UnsupportedFormat: return "unsupported resource format";
    case AdapterStatus::ShaderBytecodeMissing: return "shader bytecode is missing";
    case AdapterStatus::DeviceError: return "graphics device operation failed";
    case AdapterStatus::ResourceMismatch: return "resource description does not match the layout";
    }
    return "unknown adapter status";
}

} // namespace pw
