#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>

// Echo of the NR runtime's own log. nvngx_dlssnr.dll has no app log callback when it is hosted
// without the NGX core; it writes nvngx_dlssnr_<version>.log into the application data path once
// the __NGX_LOG_LEVEL environment variable is set. The variable is set only around the runtime's
// load and initialization (the NGX core reads it too and then logs to the console), and new lines
// are printed as "[nr log] ...", each distinct line once.
class NrLogTail {
public:
    // Before the runtime is loaded: level 0 leaves logging off; an existing variable is kept.
    void Arm(const std::filesystem::path &directory, int level);
    // After the runtime is initialized: restores the environment and locates the log file.
    void Attach();
    void Echo();
    void Summary() const;
private:
    std::filesystem::path directory, file;
    std::map<std::filesystem::path,std::uintmax_t> before;
    std::uintmax_t offset=0;
    bool armed=false, restore=false;
    std::string partial;
    std::unordered_set<std::string> seen;
    unsigned repeated=0;
};
