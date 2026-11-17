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

// Java java.util.Random / LegacyRandomSource: 48-bit LCG, seed scrambled with
// 0x5DEECE66D. Vanilla worldgen structure placement draws from this RNG in a
// fixed order (frequency float, then two bounded ints), so it must be exact.
struct JavaRandom {
    uint64_t state = 0;
    void SetSeed(int64_t seed) {
        state = (uint64_t(seed) ^ 0x5DEECE66Dull) & ((1ull << 48) - 1);
    }
    int32_t Next(int bits) {
        state = (state * 0x5DEECE66Dull + 0xBull) & ((1ull << 48) - 1);
        return int32_t(state >> (48 - bits));
    }
    int32_t NextInt(int32_t bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound)
            return int32_t((int64_t(bound) * int64_t(Next(31))) >> 31);
        for (;;) {
            const int32_t bits = Next(31);
            const int32_t val = bits % bound;
            if (bits - val + (bound - 1) >= 0) return val;
        }
    }
    float NextFloat() { return float(Next(24)) / float(1 << 24); }
    // WorldgenRandom.setLargeFeatureWithSalt / setSeed(region formula).
    void SetRegionSeed(int64_t regionX, int64_t regionZ, int64_t worldSeed, int64_t salt) {
        SetSeed(regionX * 341873128712LL + regionZ * 132897987541LL + worldSeed + salt);
    }
};

} // namespace potato
