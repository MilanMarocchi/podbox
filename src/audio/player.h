#pragma once

#include <filesystem>
#include <memory>

namespace podbox {

enum class PlaybackState { Stopped, Playing, Paused };

// Plays a single decoded audio file through the system's default output.
// Backend-agnostic: the macOS implementation wraps AVFoundation; other
// platforms can provide their own. All times are in seconds.
class AudioPlayer {
public:
    static std::unique_ptr<AudioPlayer> create();
    virtual ~AudioPlayer() = default;

    // Loads a file and begins playing it. Returns false if it can't be
    // opened/decoded. Replaces whatever was playing.
    virtual bool open(const std::filesystem::path& file) = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(double seconds) = 0;

    virtual PlaybackState state() const = 0;
    virtual double position() const = 0;
    virtual double duration() const = 0;

    virtual void setVolume(float v) = 0;  // 0..1
    virtual float volume() const = 0;

    // True once the current file has played to its end (cleared by open()).
    virtual bool reachedEnd() const = 0;
};

}  // namespace podbox
