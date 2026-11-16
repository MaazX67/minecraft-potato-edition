#pragma once
#include <cstdint>

// Java 26.3 worldgen randoms, reimplemented from the decompiled sources:
//   Xoroshiro128++  -> net.minecraft.world.level.levelgen.Xoroshiro128PlusPlus
//   SplitMix64      -> java.util.SplittableRandom / PositionalRandomFactory
// These are the exact bit-level algorithms Java uses for seed parity.
namespace potato {

class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed = 0) : mSeed(seed) {}
    uint64_t NextLong() {
        uint64_t z = mSeed += 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    uint32_t NextInt() { return uint32_t(NextLong() >> 32); }
    int32_t NextIntBounded(int32_t bound) {
        // Java SplittableRandom.nextLong(bound): rejection sampling.
        uint64_t r = uint64_t(NextLong()) & 0x7FFFFFFFFFFFFFFFULL;
        return int32_t(r % uint64_t(bound));
    }
private:
    uint64_t mSeed;
};

class Xoroshiro128PlusPlus {
public:
    Xoroshiro128PlusPlus(uint64_t lo = 0, uint64_t hi = 0) : mS0(lo), mS1(hi) {}
    uint64_t NextLong() {
        uint64_t s0 = mS0;
        uint64_t s1 = mS1;
        uint64_t result = Rotl(s0 + s1, 17) + s0;
        s1 ^= s0;
        mS0 = Rotl(s0, 49) ^ s1 ^ (s1 << 21);
        mS1 = Rotr(s1, 28);
        return result;
    }
    uint32_t NextInt() { return uint32_t(NextLong() >> 32); }
    int32_t NextIntBounded(int32_t bound) {
        uint64_t r = uint64_t(NextLong()) & 0x7FFFFFFFFFFFFFFFULL;
        return int32_t(r % uint64_t(bound));
    }
    double NextDouble() { return double(NextLong() >> 11) * (1.0 / 9007199254740992.0); }
private:
    static uint64_t Rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static uint64_t Rotr(uint64_t x, int k) { return (x >> k) | (x << (64 - k)); }
    uint64_t mS0, mS1;
};

} // namespace potato
