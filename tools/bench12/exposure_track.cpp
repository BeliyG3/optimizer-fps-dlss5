#include "exposure_track.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>

std::string ExposureLine(int frame, const ExposureReading &r)
{
    char line[96];
    std::snprintf(line,sizeof(line),"%d %.9g %.9g %.9g\n",frame,double(r.scale),double(r.average),double(r.highlight));
    return line;
}
std::map<int,ExposureReading> ParseExposureTrack(const std::string &text)
{
    std::map<int,ExposureReading> frames;
    std::istringstream lines(text); std::string line;
    while(std::getline(lines,line)) {
        if(line.empty() || line[0]=='#') continue;
        int frame=0; ExposureReading r; char rest=0;
        if(std::sscanf(line.c_str(),"%d %f %f %f %c",&frame,&r.scale,&r.average,&r.highlight,&rest)!=4 || frame<0)
            throw std::runtime_error("Malformed exposure track line: "+line);
        r.valid=true; frames[frame]=r;
    }
    return frames;
}
void ExposureTrack::Record(const std::string &path)
{
    out.open(path,std::ios::binary|std::ios::trunc);
    if(!out) throw std::runtime_error("--exposure-record: cannot write "+path);
    out<<"# pw_bench12 exposure track: frame scale average highlight\n";
}
void ExposureTrack::Replay(const std::string &path)
{
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("--exposure-replay: cannot read "+path);
    std::ostringstream text; text<<in.rdbuf();
    frames=ParseExposureTrack(text.str());
    if(frames.empty()) throw std::runtime_error("--exposure-replay: no frames in "+path);
    std::printf("[info] exposure replay: %zu frames (%d..%d) from %s\n",frames.size(),frames.begin()->first,frames.rbegin()->first,path.c_str());
}
void ExposureTrack::Write(int frame, const ExposureReading &reading)
{
    out<<ExposureLine(frame,reading); out.flush();
}
ExposureReading ExposureTrack::At(int frame) const
{
    if(frame<0) return {};
    const auto found=frames.find(frame);
    if(found==frames.end()) throw std::runtime_error("--exposure-replay: the track has no frame "+std::to_string(frame));
    return found->second;
}
