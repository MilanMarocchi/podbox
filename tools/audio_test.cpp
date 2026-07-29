// Plays an audio file for a few seconds via the AudioPlayer backend and
// reports duration/position, verifying the decode + output pipeline.
#include "audio/player.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: audio_test <audiofile> [seconds]\n");
        return 2;
    }
    const double seconds = argc >= 3 ? std::atof(argv[2]) : 3.0;
    auto player = podbox::AudioPlayer::create();
    if (!player->open(argv[1])) {
        std::fprintf(stderr, "error: could not open/decode %s\n", argv[1]);
        return 1;
    }
    const double duration = player->duration();
    std::printf("playing: duration=%.1fs\n", duration);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
               .count() < seconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("  position=%.1fs playing=%d\n", player->position(),
                    player->state() == podbox::PlaybackState::Playing);
    }
    player->stop();
    return duration > 0 ? 0 : 1;
}
