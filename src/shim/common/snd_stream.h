/* The sound driver's output on its way to the platform's audio device: the main thread renders the driver
 * (SndDriver::Render) after each game frame into a ring buffer, the device's thread or callback takes from it. The
 * driver and the game's sound calls therefore stay on the main thread (no lock), and a scripted run renders a fixed
 * number of samples per frame, so its sound is the same on every platform (Capture).
 * One producer (main thread) and one consumer (audio thread): the two positions are atomics. */
#ifndef TM_SND_STREAM_H
#define TM_SND_STREAM_H

#include <vector>

#include "nitro.h"

namespace SndStream {

enum { kCapacity = 16384 };   // stereo frames (0.5 s at 32 kHz)

void Reset();                 // empties the ring and the frame counter (no consumer running)
// Samples to render for one game frame: kRate / 30 with the remainder carried, or, with frameUs >= 0, the frame's
// measured length (a late frame lasts 50 ms or more: the device plays that long before the next Produce). With
// targetFill >= 0, corrected by an eighth of the distance between the ring's fill and the target (at most 256 either
// way), so that the producer follows the device's clock.
int FrameSamples(int targetFill, int frameUs = -1);
void Produce(int frames);     // renders into the ring; what does not fit is dropped (counted)
int Fill();
int Consume(s16 *out, int frames); // copies up to `frames`, zero-fills the rest; returns the frames copied

// Statistics: frames dropped by Produce, frames of silence written by Consume after the first sample (underruns).
int Dropped();
int Underruns();
// Capture: every produced frame is also appended here while it is set (scripted runs write it as a WAV file).
void SetCapture(std::vector<s16> *out);

}

#endif
