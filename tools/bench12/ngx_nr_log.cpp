#include "ngx_nr_log.h"
#include <windows.h>
#include <cstdio>
#include <fstream>

namespace {
constexpr wchar_t Variable[]=L"__NGX_LOG_LEVEL";
bool RuntimeLog(const std::filesystem::path &path)
{
    return path.filename().wstring().rfind(L"nvngx_dlssnr",0)==0 && path.extension()==L".log";
}
// "[2026-09-21 23:12:29] [tid:50576][NGXCG2R::EvaluateFeature:1125] DLSSNR: ..." loses its time and
// thread so that a line repeated every frame compares equal.
std::string Strip(std::string line)
{
    if(!line.empty() && line.back()=='\r') line.pop_back();
    if(line.rfind("[",0)==0) { auto close=line.find("] "); if(close!=std::string::npos) line.erase(0,close+2); }
    if(line.rfind("[tid:",0)==0) { auto close=line.find(']'); if(close!=std::string::npos) line.erase(0,close+1); }
    return line;
}
}
void NrLogTail::Arm(const std::filesystem::path &path, int level)
{
    directory=path; armed=level>0;
    if(!armed) return;
    std::error_code error;
    for(const auto &entry:std::filesystem::directory_iterator(directory,error))
        if(entry.is_regular_file(error) && RuntimeLog(entry.path())) before[entry.path()]=entry.file_size(error);
    wchar_t value[64]{};
    const DWORD length=GetEnvironmentVariableW(Variable,value,64);
    if(length>0 && length<64) { std::printf("[nr log] keeping __NGX_LOG_LEVEL=%ls from the environment\n",value); return; }
    restore=SetEnvironmentVariableW(Variable,std::to_wstring(level).c_str())!=0;
}
void NrLogTail::Attach()
{
    if(!armed) return;
    if(restore) { SetEnvironmentVariableW(Variable,nullptr); restore=false; }
    std::error_code error; std::filesystem::file_time_type newest{};
    for(const auto &entry:std::filesystem::directory_iterator(directory,error)) {
        if(!entry.is_regular_file(error) || !RuntimeLog(entry.path())) continue;
        const auto written=entry.last_write_time(error);
        if(file.empty() || written>newest) { file=entry.path(); newest=written; }
    }
    if(file.empty()) {
        std::printf("[nr log] no nvngx_dlssnr*.log in %ls; the runtime log is not echoed\n",directory.c_str());
        armed=false; return;
    }
    // The runtime may append to an existing file or start it over.
    const auto size=std::filesystem::file_size(file,error);
    const auto old=before.find(file);
    offset=old!=before.end() && size>=old->second ? old->second : 0;
    std::printf("[nr log] runtime log file: %ls\n",file.c_str());
    Echo();
}
void NrLogTail::Echo()
{
    if(!armed || file.empty()) return;
    std::ifstream in(file,std::ios::binary);
    if(!in) return;
    in.seekg(0,std::ios::end);
    const auto end=static_cast<std::uintmax_t>(std::streamoff(in.tellg()));
    if(end<offset) offset=0;
    if(end==offset) return;
    std::string chunk(static_cast<size_t>(end-offset),'\0');
    in.seekg(std::streamoff(offset)); in.read(chunk.data(),std::streamsize(chunk.size()));
    offset=end; partial+=chunk;
    size_t start=0;
    for(size_t newline; (newline=partial.find('\n',start))!=std::string::npos; start=newline+1) {
        auto line=Strip(partial.substr(start,newline-start));
        if(line.empty()) continue;
        if(seen.insert(line).second) std::printf("[nr log] %s\n",line.c_str()); else ++repeated;
    }
    partial.erase(0,start);
}
void NrLogTail::Summary() const
{
    if(armed && repeated) std::printf("[nr log] %u repeated runtime log lines were not echoed again\n",repeated);
}
