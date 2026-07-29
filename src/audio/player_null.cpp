#include "audio/player.h"

// Fallback backend for platforms without an audio implementation yet (e.g.
// Linux, until a miniaudio/ALSA backend is added). Reports as stopped and
// ignores playback requests so the rest of the app runs unchanged.

namespace podbox {
namespace {

class NullPlayer : public AudioPlayer {
public:
    bool open(const std::filesystem::path&) override { return false; }
    void play() override {}
    void pause() override {}
    void stop() override {}
    void seek(double) override {}
    PlaybackState state() const override { return PlaybackState::Stopped; }
    double position() const override { return 0.0; }
    double duration() const override { return 0.0; }
    void setVolume(float v) override { volume_ = v; }
    float volume() const override { return volume_; }
    bool reachedEnd() const override { return false; }

private:
    float volume_ = 0.8f;
};

}  // namespace

std::unique_ptr<AudioPlayer> AudioPlayer::create() {
    return std::make_unique<NullPlayer>();
}

}  // namespace podbox
