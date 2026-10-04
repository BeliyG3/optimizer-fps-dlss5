// Frame pacing (--fps-cap, --fps-jitter) and the Present-to-Present statistics (--frametime).
#pragma once

// Spin-waits until this frame's CPU budget (1/fpsCap plus the random jitter) has passed.
void PaceFrame(double fpsCap, double fpsJitterMs);
// Records the interval since the previous Present (frames 60..end); with --frametime prints the
// statistics after the last frame.
void RecordFrameTime(int frame, int frames, bool frametime);
