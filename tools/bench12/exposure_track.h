#pragma once
#include "display.h"
#include <fstream>
#include <map>
#include <string>

// --exposure-record FILE / --exposure-replay FILE (options.h): the auto exposure each frame ends with, so a later run of the
// same view (a full one, or one from --start-frame S) applies exactly the recorded run's exposure. Only the adapted scale
// carries from frame to frame: Adapt (shaders/display.hlsl) moves it from the previous frame's value, and the renderers'
// firefly clamp divides by the previous frame's (FireflyLimit). The average and highlight targets are that frame's meter,
// kept for reading. --local-exposure has no history: its grid is rebuilt each frame from the frame and its scale.
// The file is text, one line per frame, "frame scale average highlight" with %.9g (which round-trips a float exactly).
class ExposureTrack {
public:
    void Record(const std::string &path); // truncates path; Write then appends a line per frame
    void Replay(const std::string &path); // loads path; throws when it cannot be read or holds no frame
    bool Recording() const { return out.is_open(); }
    bool Replaying() const { return !frames.empty(); }
    void Write(int frame, const ExposureReading &reading);
    // Replay: the reading frame ended with; before frame 0 none (FireflyLimit then clamps nothing, as on a run's first
    // frame). Throws when the file lacks the frame.
    ExposureReading At(int frame) const;
private:
    std::ofstream out;
    std::map<int,ExposureReading> frames;
};
std::string ExposureLine(int frame, const ExposureReading &reading);
// The frames of a track file's text; '#' lines are comments. Throws on a malformed line.
std::map<int,ExposureReading> ParseExposureTrack(const std::string &text);
