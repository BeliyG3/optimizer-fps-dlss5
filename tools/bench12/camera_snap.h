#pragma once
// The "snap" camera: a sharp in-place turn like a mouse flick. Pure function of the frame number.
// 64-bit arithmetic: the period is 2*(turnFrames+holdFrames), which overflows int for huge frame counts.
inline float SnapYawDegrees(int frame, float degrees, int turnFrames, int holdFrames)
{
    const long long half=(long long)turnFrames+holdFrames, period=2*half, f=((frame%period)+period)%period;
    const long long local=f%half;
    const float t=local<turnFrames ? float(local)/float(turnFrames) : 1.0f;
    return f<half ? degrees*t : degrees*(1.0f-t);
}
