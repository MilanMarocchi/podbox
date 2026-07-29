#include "audio/player.h"

#import <AVFoundation/AVFoundation.h>

namespace podbox {
namespace {

// Delegate that flags when a track plays to its natural end, so the app can
// auto-advance to the next song.
class AvfPlayer;

}  // namespace
}  // namespace podbox

@interface PBPlayerDelegate : NSObject <AVAudioPlayerDelegate> {
@public
    bool finished;
}
@end

@implementation PBPlayerDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player
                       successfully:(BOOL)flag {
    (void)player;
    (void)flag;
    finished = true;
}
@end

namespace podbox {
namespace {

class AvfPlayer : public AudioPlayer {
public:
    AvfPlayer() { delegate_ = [[PBPlayerDelegate alloc] init]; }

    bool open(const std::filesystem::path& file) override {
        NSURL* url = [NSURL fileURLWithPath:@(file.c_str())];
        NSError* err = nil;
        AVAudioPlayer* p = [[AVAudioPlayer alloc] initWithContentsOfURL:url
                                                                  error:&err];
        if (!p) return false;
        p.delegate = delegate_;
        p.volume = volume_;
        delegate_->finished = false;
        [p prepareToPlay];
        [p play];
        player_ = p;
        state_ = PlaybackState::Playing;
        return true;
    }

    void play() override {
        if (player_ && state_ != PlaybackState::Playing) {
            [player_ play];
            state_ = PlaybackState::Playing;
        }
    }

    void pause() override {
        if (player_ && state_ == PlaybackState::Playing) {
            [player_ pause];
            state_ = PlaybackState::Paused;
        }
    }

    void stop() override {
        if (player_) {
            [player_ stop];
            player_ = nil;
        }
        state_ = PlaybackState::Stopped;
    }

    void seek(double seconds) override {
        if (!player_) return;
        const double d = player_.duration;
        if (seconds < 0) seconds = 0;
        if (d > 0 && seconds > d) seconds = d;
        player_.currentTime = seconds;
    }

    PlaybackState state() const override { return state_; }
    double position() const override {
        return player_ ? player_.currentTime : 0.0;
    }
    double duration() const override {
        return player_ ? player_.duration : 0.0;
    }

    void setVolume(float v) override {
        volume_ = v < 0 ? 0 : (v > 1 ? 1 : v);
        if (player_) player_.volume = volume_;
    }
    float volume() const override { return volume_; }

    bool reachedEnd() const override { return delegate_->finished; }

private:
    AVAudioPlayer* player_ = nil;
    PBPlayerDelegate* delegate_ = nil;
    PlaybackState state_ = PlaybackState::Stopped;
    float volume_ = 0.8f;
};

}  // namespace

std::unique_ptr<AudioPlayer> AudioPlayer::create() {
    return std::make_unique<AvfPlayer>();
}

}  // namespace podbox
