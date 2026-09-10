#pragma once

// dlss5-feed-host64.cfg from the add-on's side (stage 26.16).
//
// In the 32-bit kits the Neural Rendering model runs in dlss5-feed-host64.exe, and that host
// computes the motion vectors itself with the driver's optical flow engine. Which source it uses,
// and how the engine is set up, lives in dlss5-feed-host64.cfg next to the host exe. The host
// re-reads that file while it runs (ofa::PollConfig), so writing it is all the tab has to do to
// change the setting live.
//
// This add-on is loaded by the ReShade inside that same host process, so the file is simply next
// to our own module. Everything here is plain Win32 file work on the runtime thread; nothing is
// touched when the add-on runs in an ordinary game, where the file does not exist.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pw_ofa {

enum Source : int { SourceUnset = 0, SourceOfa = 1, SourceShader = 2 };

struct Settings {
    int source = SourceOfa; // SourceOfa / SourceShader
    int grid = 2;           // 1, 2 or 4
    int perf = 20;          // 5 slow, 10 medium, 20 fast
};

namespace detail {

inline std::string &CfgPathRef()
{
    static std::string path;
    return path;
}

inline bool &AvailableRef()
{
    static bool available = false;
    return available;
}

inline FILETIME &StampRef()
{
    static FILETIME stamp{};
    return stamp;
}

inline std::string DirectoryOf(const std::string &file)
{
    const std::size_t cut = file.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : file.substr(0, cut + 1);
}

inline std::string ModulePath(HMODULE module)
{
    char buffer[MAX_PATH]{};
    const DWORD written = GetModuleFileNameA(module, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) return std::string();
    return std::string(buffer, written);
}

inline void Trim(std::string &s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    s = s.substr(b, e - b);
}

// 26.21: a missing file and a file that could not be read are different answers. A failed ReadFile
// used to end the loop and report success with whatever had been read, so a following Save rewrote
// the file from a truncated - or empty - image and lost every key the host owns.
enum class ReadOutcome { Ok, Missing, Error };

inline ReadOutcome ReadFileOutcome(const std::string &path, std::string *out)
{
    out->clear();
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ReadOutcome::Missing
                                                                                : ReadOutcome::Error;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) { CloseHandle(file); return ReadOutcome::Error; }
    char chunk[4096];
    unsigned long long total = 0;
    for (;;) { // ReadFile may answer with less than asked for: read on until the whole file is in
        DWORD got = 0;
        if (!ReadFile(file, chunk, sizeof(chunk), &got, nullptr)) {
            CloseHandle(file);
            out->clear();
            return ReadOutcome::Error;
        }
        if (got == 0) break;
        out->append(chunk, got);
        total += got;
    }
    CloseHandle(file);
    if (total < static_cast<unsigned long long>(size.QuadPart)) { out->clear(); return ReadOutcome::Error; }
    return ReadOutcome::Ok;
}

inline bool ReadFileText(const std::string &path, std::string *out)
{
    return ReadFileOutcome(path, out) == ReadOutcome::Ok;
}

inline bool WriteFileText(const std::string &path, const std::string &text)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = text.empty() ||
                    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == static_cast<DWORD>(text.size());
}

inline bool WriteTime(const std::string &path, FILETIME *out)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return false;
    *out = data.ftLastWriteTime;
    return true;
}

// Splits into lines, keeping the line ending of each so a rewrite preserves the file byte for byte
// apart from the values it replaces.
inline std::vector<std::string> SplitLines(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start + 1));
        start = nl + 1;
    }
    return lines;
}

// The key of a `key=value` line, empty for comments, blanks and anything without an `=`.
inline std::string KeyOf(const std::string &line)
{
    std::string body = line;
    Trim(body);
    if (body.empty() || body[0] == '#' || body[0] == ';') return std::string();
    const std::size_t eq = body.find('=');
    if (eq == std::string::npos) return std::string();
    std::string key = body.substr(0, eq);
    Trim(key);
    for (char &c : key) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return key;
}

inline std::string ValueOf(const std::string &line)
{
    std::string body = line;
    Trim(body);
    const std::size_t eq = body.find('=');
    if (eq == std::string::npos) return std::string();
    std::string value = body.substr(eq + 1);
    Trim(value);
    return value;
}

} // namespace detail

// Is this the 64-bit Feed host? Two independent signs, either of which is enough: the config file
// sits next to our own module (the add-on is deployed into host64\ with the exe), or the process
// is dlss5-feed-host64.exe itself (then the file is next to the exe, and the host creates it at
// start-up if it is missing). Call once, from DllMain / the first present.
inline void Init(HMODULE module)
{
    if (!detail::CfgPathRef().empty()) return;

    const std::string moduleDir = detail::DirectoryOf(detail::ModulePath(module));
    if (!moduleDir.empty()) {
        const std::string candidate = moduleDir + "dlss5-feed-host64.cfg";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            detail::CfgPathRef() = candidate;
            detail::AvailableRef() = true;
            return;
        }
    }

    const std::string exePath = detail::ModulePath(nullptr);
    std::string exeName = exePath;
    const std::size_t cut = exeName.find_last_of("\\/");
    if (cut != std::string::npos) exeName = exeName.substr(cut + 1);
    if (_stricmp(exeName.c_str(), "dlss5-feed-host64.exe") == 0) {
        detail::CfgPathRef() = detail::DirectoryOf(exePath) + "dlss5-feed-host64.cfg";
        detail::AvailableRef() = true;
        return;
    }

    // Not the host. The empty path keeps every call below a no-op, but a second Init() must be
    // able to find the file if the host writes it later, so only the "found" case latches.
}

inline bool Available() { return detail::AvailableRef(); }
inline const std::string &CfgPath() { return detail::CfgPathRef(); }

// Reads the file into `out`, leaving keys it does not mention alone. False when there is no file.
inline bool Load(Settings *out)
{
    if (!Available()) return false;
    std::string text;
    if (!detail::ReadFileText(CfgPath(), &text)) return false;
    for (const std::string &line : detail::SplitLines(text)) {
        const std::string key = detail::KeyOf(line);
        if (key.empty()) continue;
        const std::string value = detail::ValueOf(line);
        if (key == "mv_source")
            out->source = _stricmp(value.c_str(), "shader") == 0 ? SourceShader : SourceOfa;
        else if (key == "ofa_grid")
            out->grid = std::atoi(value.c_str());
        else if (key == "ofa_perf")
            out->perf = std::atoi(value.c_str());
    }
    if (out->grid != 1 && out->grid != 2 && out->grid != 4) out->grid = 2;
    if (out->perf != 5 && out->perf != 10 && out->perf != 20) out->perf = 20;
    detail::WriteTime(CfgPath(), &detail::StampRef());
    return true;
}

// Re-reads only when the file's last-write time moved since the last Load/Refresh/Save. Cheap
// enough for every frame; called once every few dozen presents anyway.
inline bool Refresh(Settings *out)
{
    if (!Available()) return false;
    FILETIME now{};
    if (!detail::WriteTime(CfgPath(), &now)) return false;
    const FILETIME &stamp = detail::StampRef();
    if (now.dwLowDateTime == stamp.dwLowDateTime && now.dwHighDateTime == stamp.dwHighDateTime)
        return false;
    return Load(out);
}

// Rewrites the three keys in place. Every other line -- the header comment, ofa_log, ofa_pipeline,
// anything a later host version adds -- is copied through unchanged; a key the file does not have
// is appended. Returns false when the file could not be written (it is open elsewhere, or the
// folder is read-only), and then nothing was changed.
inline bool Save(const Settings &settings)
{
    if (!Available()) return false;
    std::string text;
    const detail::ReadOutcome outcome = detail::ReadFileOutcome(CfgPath(), &text);
    // A file that is there but could not be read is never rewritten: the keys the host owns
    // (ofa_log, ofa_pipeline, anything a later version added) would be lost.
    if (outcome == detail::ReadOutcome::Error) return false;

    char sourceValue[16];
    std::snprintf(sourceValue, sizeof(sourceValue), "%s",
                  settings.source == SourceShader ? "shader" : "ofa");
    char gridValue[16], perfValue[16];
    std::snprintf(gridValue, sizeof(gridValue), "%d", settings.grid);
    std::snprintf(perfValue, sizeof(perfValue), "%d", settings.perf);

    struct Pair { const char *key; const char *value; bool seen; };
    Pair pairs[3] = { { "mv_source", sourceValue, false },
                      { "ofa_grid", gridValue, false },
                      { "ofa_perf", perfValue, false } };

    std::vector<std::string> lines = detail::SplitLines(text);
    std::string out;
    for (std::string &line : lines) {
        const std::string key = detail::KeyOf(line);
        bool replaced = false;
        for (Pair &pair : pairs) {
            if (key != pair.key) continue;
            pair.seen = true;
            replaced = true;
            const bool crlf = line.size() >= 2 && line[line.size() - 2] == '\r';
            const bool nl = !line.empty() && line.back() == '\n';
            out += std::string(pair.key) + "=" + pair.value + (crlf ? "\r\n" : nl ? "\n" : "");
            break;
        }
        if (!replaced) out += line;
    }
    bool appended = false;
    for (const Pair &pair : pairs) {
        if (pair.seen) continue;
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += std::string(pair.key) + "=" + pair.value + "\n";
        appended = true;
    }
    (void)appended;

    // Through a temporary and a replace, not straight over the file: the host polls the same file
    // from another thread of another process and must never see a half-written one.
    const std::string temp = CfgPath() + ".pwtmp";
    if (!detail::WriteFileText(temp, out)) return false;
    if (!MoveFileExA(temp.c_str(), CfgPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp.c_str());
        return false;
    }
    detail::WriteTime(CfgPath(), &detail::StampRef());
    return true;
}

} // namespace pw_ofa
