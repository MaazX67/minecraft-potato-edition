#pragma once
#include "assets.h"
#include "engine.h"
#include <objbase.h>
#include <xaudio2.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace potato {

enum SoundCat {
    SND_STONE, SND_GRASS, SND_DIRT, SND_WOOD, SND_SAND, SND_GRAVEL,
    SND_HIT, SND_CRIT, SND_CLICK, SND_HURT, SND_SPLASH,
    SND_STEP_GRASS, SND_STEP_STONE, SND_STEP_WOOD, SND_STEP_GRAVEL, SND_STEP_SAND,
    SND_STEP_SNOW, SND_STEP_WOOL,
    SND_GLASS, SND_WOOL, SND_SNOW, SND_TOOL_BREAK,
    SND_ZOMBIE_SAY, SND_ZOMBIE_HURT, SND_ZOMBIE_DEATH,
    SND_COUNT
};

// Native audio architecture: every vanilla sound is located through the
// launcher's SHA-1 object index, decoded to PCM at boot, and packed into one
// contiguous arena. Playback requests enter a rigid ring queue and are served
// by a background thread driving a pool of pre-created source voices — the
// render thread never touches XAudio2, disk, or a decoder.
class SoundEngine {
public:
    bool Init(AssetStore& store); // caller must CoInitializeEx first
    void Shutdown();
    void Play(SoundCat cat);                    // non-positional (at listener)
    void PlayAt(SoundCat cat, float x, float y, float z, float volume = 1, float pitch = 1); // 3D positional
    void SetListener(float px, float py, float pz,
                     float fwdX, float fwdY, float fwdZ, float rightX, float rightZ);
    void SetEnabled(bool on);
    void SetVolume(float v);
    static SoundCat BlockSound(uint16_t block);
    static SoundCat StepSound(uint16_t block);
    uint64_t Played() const { return mPlayed.load(std::memory_order_relaxed); }
    size_t Variants(SoundCat cat) const { return mSounds[cat].size(); }

private:
    struct PlayRequest {
        SoundCat cat = SND_STONE;
        float x = 0, y = 0, z = 0;
        bool positional = false;
        float volume = 1, pitch = 1;
    };
    struct Sound {
        std::vector<IXAudio2SourceVoice*> voices; // pool: overlapping plays
        XAUDIO2_BUFFER buffer = {};               // points into mPcmArena
        int channels = 1;
        size_t nextVoice = 0;
    };
    struct Listener {
        float px = 0, py = 0, pz = 0;
        float fx = 0, fy = 0, fz = -1;
        float rx = 1, rz = 0;
    };

    void Submit(const PlayRequest& req);
    void WorkerLoop();
    void Dispatch(const PlayRequest& req);

    IXAudio2* mEngine = nullptr;
    IXAudio2MasteringVoice* mMaster = nullptr;
    UINT32 mMasterChannels = 2;
    std::vector<Sound> mSounds[SND_COUNT];
    size_t mNext[SND_COUNT] = {};
    std::vector<short> mPcmArena; // contiguous PCM storage for every buffer

    RingQueue<PlayRequest, 64> mRequests;
    std::mutex mWorkMutex;
    std::condition_variable mCv;
    bool mStop = false;
    std::thread mWorker;
    std::atomic<bool> mEnabled{true};
    std::atomic<uint64_t> mPlayed{0};
    float mVolume = 1.0f;
    std::mutex mListenerMutex;
    Listener mListener;
};

} // namespace potato