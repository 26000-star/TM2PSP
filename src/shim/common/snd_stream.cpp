#include "snd_stream.h"

#include <atomic>
#include <string.h>

#include "snd_driver.h"

namespace SndStream {

namespace {

s16 s_ring[kCapacity * 2];
std::atomic<int> s_write(0), s_read(0);   // frame counts, wrapping at 2^31 (only their difference is used)
std::atomic<int> s_underruns(0);
std::atomic<bool> s_started(false);
int s_dropped = 0;
int s_frac = 0;
s64 s_fracUs = 0;
std::vector<s16> *s_capture = nullptr;

}

void Reset()
{
    s_write = 0;
    s_read = 0;
    s_underruns = 0;
    s_started = false;
    s_dropped = 0;
    s_frac = 0;
    s_fracUs = 0;
}

int FrameSamples(int targetFill, int frameUs)
{
    int n;
    if (frameUs < 0) {
        s_frac += SndDriver::kRate;
        n = s_frac / 30;
        s_frac %= 30;
    } else {
        s_fracUs += (s64)SndDriver::kRate * frameUs;
        n = (int)(s_fracUs / 1000000);
        s_fracUs %= 1000000;
    }
    if (targetFill >= 0) {
        int d = (targetFill - Fill()) / 8;
        d = d < -256 ? -256 : d > 256 ? 256 : d;
        n += d;
        if (n < 0)
            n = 0;
    }
    return n;
}

int Fill() { return s_write.load(std::memory_order_acquire) - s_read.load(std::memory_order_acquire); }

void Produce(int frames)
{
    static s16 buf[512 * 2];
    int w = s_write.load(std::memory_order_relaxed);
    while (frames > 0) {
        int n = frames < 512 ? frames : 512;
        SndDriver::Render(buf, n);
        if (s_capture)
            s_capture->insert(s_capture->end(), buf, buf + 2 * n);
        int room = kCapacity - (w - s_read.load(std::memory_order_acquire));
        int k = n < room ? n : room;
        for (int i = 0; i < k;) {   // in at most two pieces around the end of the ring
            int at = (w + i) & (kCapacity - 1), run = kCapacity - at < k - i ? kCapacity - at : k - i;
            memcpy(&s_ring[2 * at], &buf[2 * i], sizeof(s16) * 2 * run);
            i += run;
        }
        w += k;
        s_write.store(w, std::memory_order_release);
        s_dropped += n - k;
        frames -= n;
    }
}

int Consume(s16 *out, int frames)
{
    int r = s_read.load(std::memory_order_relaxed);
    int avail = s_write.load(std::memory_order_acquire) - r;
    int k = frames < avail ? frames : avail;
    for (int i = 0; i < k;) {
        int at = (r + i) & (kCapacity - 1), run = kCapacity - at < k - i ? kCapacity - at : k - i;
        memcpy(&out[2 * i], &s_ring[2 * at], sizeof(s16) * 2 * run);
        i += run;
    }
    s_read.store(r + k, std::memory_order_release);
    if (k > 0)
        s_started = true;
    if (k < frames) {
        memset(&out[2 * k], 0, sizeof(s16) * 2 * (frames - k));
        if (s_started)
            s_underruns += frames - k;
    }
    return k;
}

int Dropped() { return s_dropped; }
int Underruns() { return s_underruns; }
void SetCapture(std::vector<s16> *out) { s_capture = out; }

}
