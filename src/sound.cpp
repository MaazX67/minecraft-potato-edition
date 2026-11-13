#include "sound.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4244 4245 4456 4457 4701 4702 4706 4996)
#include "stb_vorbis.c"
#pragma warning(pop)

namespace potato {

static constexpr size_t VOICES_PER_SOUND = 3; // pool depth per variant
static constexpr float SOUND_RADIUS = 16.0f;  // vanilla default attenuation

static bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool SoundEngine::Init(AssetStore& store) {
    HRESULT hr = XAudio2Create(&mEngine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return false;
    hr = mEngine->CreateMasteringVoice(&mMaster);
    if (FAILED(hr)) {
        mEngine->Release();
        mEngine = nullptr;
        return false;
    }
    XAUDIO2_VOICE_DETAILS vd;
    mMaster->GetVoiceDetails(&vd);
    mMasterChannels = vd.InputChannels; // mastering voice: device channel count

    // Automated asset lookup: logical names -> SHA-1 objects via the index.
    struct Candidate { std::string name; SoundCat cat; };
    std::vector<Candidate> candidates;
    const std::vector<std::string> dig = store.ListPrefix("minecraft/sounds/dig/");
    for (const std::string& name : dig) {
        if (!EndsWith(name, ".ogg")) continue;
        SoundCat cat = SND_COUNT;
        if (name.find("stone") != std::string::npos) cat = SND_STONE;
        else if (name.find("gravel") != std::string::npos) cat = SND_GRAVEL;
        else if (name.find("grass") != std::string::npos) cat = SND_GRASS;
        else if (name.find("dirt") != std::string::npos) cat = SND_DIRT;
        else if (name.find("wood") != std::string::npos) cat = SND_WOOD;
        else if (name.find("sand") != std::string::npos) cat = SND_SAND;
        else if (name.find("glass") != std::string::npos) cat = SND_GLASS;
        else if (name.find("cloth") != std::string::npos) cat = SND_WOOL;
        else if (name.find("snow") != std::string::npos) cat = SND_SNOW;
        if (cat == SND_COUNT) continue;
        if (mNext[cat] >= 4) continue; // vanilla has 4 variants per material
        ++mNext[cat];
        candidates.push_back({name, cat});
    }
    const std::vector<std::string> atk = store.ListPrefix("minecraft/sounds/entity/player/attack/");
    for (const std::string& name : atk) {
        if (!EndsWith(name, ".ogg")) continue;
        const SoundCat cat = name.find("crit") != std::string::npos ? SND_CRIT : SND_HIT;
        if (mNext[cat] >= 4) continue;
        ++mNext[cat];
        candidates.push_back({name, cat});
    }
    const std::vector<std::string> step = store.ListPrefix("minecraft/sounds/step/");
    for (const std::string& name : step) {
        if (!EndsWith(name, ".ogg")) continue;
        SoundCat cat = SND_COUNT;
        if (name.find("grass") != std::string::npos) cat = SND_STEP_GRASS;
        else if (name.find("stone") != std::string::npos) cat = SND_STEP_STONE;
        else if (name.find("wood") != std::string::npos) cat = SND_STEP_WOOD;
        else if (name.find("gravel") != std::string::npos) cat = SND_STEP_GRAVEL;
        else if (name.find("sand") != std::string::npos) cat = SND_STEP_SAND;
        if (cat == SND_COUNT || mNext[cat] >= 4) continue;
        ++mNext[cat];
        candidates.push_back({name, cat});
    }
    const std::vector<std::string> hurt = store.ListPrefix("minecraft/sounds/entity/player/hurt");
    for (const std::string& name : hurt) {
        if (!EndsWith(name, ".ogg") || mNext[SND_HURT] >= 4) continue;
        ++mNext[SND_HURT];
        candidates.push_back({name, SND_HURT});
    }
    candidates.push_back({"minecraft/sounds/random/click.ogg", SND_CLICK});
    candidates.push_back({"minecraft/sounds/random/glass1.ogg", SND_GLASS});
    candidates.push_back({"minecraft/sounds/random/glass2.ogg", SND_GLASS});
    candidates.push_back({"minecraft/sounds/random/glass3.ogg", SND_GLASS});
    candidates.push_back({"minecraft/sounds/random/break.ogg", SND_TOOL_BREAK});
    candidates.push_back({"minecraft/sounds/liquid/splash.ogg", SND_SPLASH});
    memset(mNext, 0, sizeof(mNext)); // reuse as round-robin playback cursors

    // Decode pass: ogg -> PCM temps (freed immediately after arena copy).
    struct Decoded {
        SoundCat cat;
        std::vector<short> pcm;
        int channels, rate, samples;
    };
    std::vector<Decoded> decoded;
    size_t totalShorts = 0;
    for (const Candidate& c : candidates) {
        std::vector<uint8_t> bytes;
        if (!store.ReadFile(c.name, bytes)) continue;
        int channels = 0, rate = 0;
        short* pcm = nullptr;
        const int samples = stb_vorbis_decode_memory(bytes.data(), int(bytes.size()),
                                                     &channels, &rate, &pcm);
        if (samples <= 0 || !pcm) {
            if (pcm) free(pcm);
            continue;
        }
        Decoded d;
        d.cat = c.cat;
        d.channels = channels;
        d.rate = rate;
        d.samples = samples;
        d.pcm.assign(pcm, pcm + size_t(samples) * size_t(channels));
        free(pcm);
        totalShorts += d.pcm.size();
        decoded.push_back(std::move(d));
    }

    // Contiguous arena: reserve once, then every XAUDIO2_BUFFER points inside
    // it. No reallocation can ever invalidate a submitted pointer.
    mPcmArena.reserve(totalShorts);
    for (const Decoded& d : decoded) {
        const size_t offset = mPcmArena.size();
        mPcmArena.insert(mPcmArena.end(), d.pcm.begin(), d.pcm.end());
        Sound s;
        s.channels = d.channels;
        s.buffer.pAudioData = reinterpret_cast<const BYTE*>(mPcmArena.data() + offset);
        s.buffer.AudioBytes = UINT32(d.pcm.size() * sizeof(short));
        s.buffer.Flags = XAUDIO2_END_OF_STREAM;
        s.buffer.LoopCount = 0;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = WORD(d.channels);
        wf.nSamplesPerSec = DWORD(d.rate);
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = WORD(d.channels * 2);
        wf.nAvgBytesPerSec = DWORD(d.rate) * wf.nBlockAlign;
        wf.cbSize = 0;
        for (size_t i = 0; i < VOICES_PER_SOUND; ++i) {
            IXAudio2SourceVoice* voice = nullptr;
            if (SUCCEEDED(mEngine->CreateSourceVoice(&voice, &wf))) {
                s.voices.push_back(voice);
            }
        }
        if (!s.voices.empty()) mSounds[d.cat].push_back(std::move(s));
    }

    mWorker = std::thread([this] { WorkerLoop(); });
    return true; // sounds are optional; missing files degrade to silence
}

void SoundEngine::Submit(const PlayRequest& req) {
    if (!mEnabled.load(std::memory_order_relaxed) || !mEngine) return;
    if (mRequests.TryPush(req)) {
        mCv.notify_all();
    }
    // Ring full: drop. Audio is best-effort; the render thread never blocks.
}

void SoundEngine::Play(SoundCat cat) {
    PlayRequest req;
    req.cat = cat;
    req.positional = false;
    Submit(req);
}

void SoundEngine::PlayAt(SoundCat cat, float x, float y, float z, float volume, float pitch) {
    PlayRequest req;
    req.cat = cat;
    req.x = x; req.y = y; req.z = z;
    req.positional = true;
    req.volume = volume;
    req.pitch = pitch;
    Submit(req);
}

void SoundEngine::SetListener(float px, float py, float pz,
                                float fwdX, float fwdY, float fwdZ,
                                float rightX, float rightZ) {
    std::lock_guard<std::mutex> lk(mListenerMutex);
    mListener.px = px; mListener.py = py; mListener.pz = pz;
    mListener.fx = fwdX; mListener.fy = fwdY; mListener.fz = fwdZ;
    mListener.rx = rightX; mListener.rz = rightZ;
}

void SoundEngine::WorkerLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mWorkMutex);
            mCv.wait(lk, [this] { return mStop || mRequests.Size() > 0; });
            if (mStop) return;
        }
        PlayRequest req;
        while (mRequests.TryPop(req)) {
            Dispatch(req);
        }
    }
}

void SoundEngine::Dispatch(const PlayRequest& req) {
    if (!mEnabled.load(std::memory_order_relaxed)) return;
    SoundCat cat = req.cat;
    if (mSounds[cat].empty() && cat == SND_DIRT) cat = SND_GRASS;
    if (mSounds[cat].empty()) {
        switch (cat) {
            case SND_STEP_GRASS: cat = SND_GRASS; break;
            case SND_STEP_STONE: cat = SND_STONE; break;
            case SND_STEP_WOOD: cat = SND_WOOD; break;
            case SND_STEP_GRAVEL: cat = SND_GRAVEL; break;
            case SND_STEP_SAND: cat = SND_SAND; break;
            default: break;
        }
    }
    if (mSounds[cat].empty()) cat = SND_STONE;
    std::vector<Sound>& v = mSounds[cat];
    if (v.empty()) return;
    Sound& s = v[mNext[cat]++ % v.size()];
    IXAudio2SourceVoice* voice = s.voices[s.nextVoice++ % s.voices.size()];

    float volume = 1.0f, pan = 0.0f;
    if (req.positional) {
        Listener l;
        {
            std::lock_guard<std::mutex> lk(mListenerMutex);
            l = mListener;
        }
        const float dx = req.x - l.px, dy = req.y - l.py, dz = req.z - l.pz;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        volume = 1.0f - dist / SOUND_RADIUS;
        if (volume <= 0.0f) return; // beyond audible radius: skip the voice
        if (dist > 0.0001f) {
            pan = (dx * l.rx + dz * l.rz) / dist;
            if (pan > 1.0f) pan = 1.0f;
            if (pan < -1.0f) pan = -1.0f;
        }
    }

    volume *= req.volume;
    voice->SetFrequencyRatio(req.pitch);
    // Equal-power pan into the mastering voice's stereo field.
    const float theta = (pan + 1.0f) * 0.7853982f; // (pan+1) * pi/4
    const float left = volume * cosf(theta);
    const float right = volume * sinf(theta);
    if (mMasterChannels == 2) {
        if (s.channels == 1) {
            const float matrix[2] = {left, right};
            voice->SetOutputMatrix(nullptr, 1, 2, matrix);
        } else {
            const float matrix[4] = {left, 0.0f, 0.0f, right};
            voice->SetOutputMatrix(nullptr, 2, 2, matrix);
        }
    } else {
        voice->SetVolume(volume);
    }

    // Stop+Flush+Submit gives instant retrigger with zero decode latency.
    voice->Stop(0);
    voice->FlushSourceBuffers();
    if (SUCCEEDED(voice->SubmitSourceBuffer(&s.buffer)) && SUCCEEDED(voice->Start(0)))
        mPlayed.fetch_add(1, std::memory_order_relaxed);
}

void SoundEngine::SetEnabled(bool on) {
    mEnabled.store(on, std::memory_order_relaxed);
    if (mMaster) mMaster->SetVolume(on ? mVolume : 0.0f);
}

void SoundEngine::SetVolume(float v) {
    mVolume = v;
    if (mMaster && mEnabled.load(std::memory_order_relaxed)) mMaster->SetVolume(v);
}

SoundCat SoundEngine::BlockSound(uint8_t block) {
    switch (block) {
        case BLOCK_GRASS: return SND_GRASS;
        case BLOCK_DIRT: return SND_DIRT;
        case BLOCK_PLANKS:
        case BLOCK_LOG: case BLOCK_SPRUCE_PLANKS: case BLOCK_BIRCH_PLANKS:
        case BLOCK_SPRUCE_LOG: case BLOCK_BIRCH_LOG: case BLOCK_CRAFTING_TABLE: case BLOCK_BOOKSHELF: return SND_WOOD;
        case BLOCK_SAND: return SND_SAND;
        case BLOCK_GRAVEL: return SND_GRAVEL;
        case BLOCK_GLASS: case BLOCK_ICE: return SND_GLASS;
        case BLOCK_SNOW: return SND_SNOW;
        case BLOCK_WHITE_WOOL: case BLOCK_RED_WOOL: case BLOCK_BLUE_WOOL:
        case BLOCK_LIME_WOOL: case BLOCK_YELLOW_WOOL: case BLOCK_BLACK_WOOL: return SND_WOOL;
        case BLOCK_OAK_LEAVES: case BLOCK_SPRUCE_LEAVES: case BLOCK_BIRCH_LEAVES: return SND_GRASS;
        default: return SND_STONE;
    }
}

SoundCat SoundEngine::StepSound(uint8_t block) {
    switch (block) {
        case BLOCK_GRASS:
        case BLOCK_DIRT: return SND_STEP_GRASS;
        case BLOCK_PLANKS:
        case BLOCK_LOG: case BLOCK_SPRUCE_PLANKS: case BLOCK_BIRCH_PLANKS:
        case BLOCK_SPRUCE_LOG: case BLOCK_BIRCH_LOG: case BLOCK_CRAFTING_TABLE: case BLOCK_BOOKSHELF: return SND_STEP_WOOD;
        case BLOCK_SAND: return SND_STEP_SAND;
        case BLOCK_GRAVEL: return SND_STEP_GRAVEL;
        case BLOCK_OAK_LEAVES: case BLOCK_SPRUCE_LEAVES: case BLOCK_BIRCH_LEAVES: return SND_STEP_GRASS;
        case BLOCK_SNOW: return SND_SNOW;
        case BLOCK_WHITE_WOOL: case BLOCK_RED_WOOL: case BLOCK_BLUE_WOOL:
        case BLOCK_LIME_WOOL: case BLOCK_YELLOW_WOOL: case BLOCK_BLACK_WOOL: return SND_WOOL;
        default: return SND_STEP_STONE;
    }
}

void SoundEngine::Shutdown() {
    if (mWorker.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mWorkMutex);
            mStop = true;
        }
        mCv.notify_all();
        mWorker.join();
    }
    for (int c = 0; c < SND_COUNT; ++c) {
        for (Sound& s : mSounds[c]) {
            for (IXAudio2SourceVoice* voice : s.voices) {
                voice->Stop(0);
                voice->DestroyVoice();
            }
            s.voices.clear();
        }
        mSounds[c].clear();
    }
    mPcmArena.clear();
    mPcmArena.shrink_to_fit();
    if (mMaster) {
        mMaster->DestroyVoice();
        mMaster = nullptr;
    }
    if (mEngine) {
        mEngine->Release();
        mEngine = nullptr;
    }
}

} // namespace potato