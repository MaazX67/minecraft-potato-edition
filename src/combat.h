#pragma once
// Java 26.3 combat math for the new tiered weapons (spear, mace, trident).
// Pure functions only: no D3D, no asset dependency, so the headless selftest
// pins every constant. Sources are cited per function; values without a JAR
// or wiki citation are marked UNVERIFIED in PARITY.md.
#include <string>

namespace potato {

enum class WeaponKind { None, Spear, Mace, Trident };

struct WeaponDef {
    WeaponKind kind = WeaponKind::None;
    int tier = 0;            // 0 wood/gold, 1 stone/copper, 2 iron, 3 diamond, 4 netherite
    int durability = 0;
    float jabDamage = 0;     // total attack damage (includes the player's base 1)
    float attackSpeed = 4.0f;
    float chargeMult = 0;    // spear charge-attack velocity multiplier
};

// ---- Mace (MaceItem.class bytecode, 26.3 JAR) -----------------------------
// canSmashAttack: fallDistance > 1.5 && !isFallFlying.
inline bool MaceCanSmash(float fallDistance, bool fallFlying) {
    return fallDistance > 1.5f && !fallFlying;
}
// getAttackDamageBonus: d<=3 -> 4d ; d<=8 -> 12+2(d-3) ; else 22+(d-8).
// Added on top of the mace's base 6 attack damage. Enchantment term omitted.
inline float MaceSmashBonus(float d) {
    if (d <= 3.0f) return 4.0f * d;
    if (d <= 8.0f) return 12.0f + 2.0f * (d - 3.0f);
    return 22.0f + (d - 8.0f);
}
// getKnockbackPower: (3.5 - dist) * 0.7 * (fallDistance>5 ? 2 : 1) * (1 - resist).
inline float MaceKnockbackPower(float dist, float fallDistance, float resistance) {
    const float scale = fallDistance > 5.0f ? 2.0f : 1.0f;
    return (3.5f - dist) * 0.7f * scale * (1.0f - resistance);
}

// ---- Spear (minecraft.wiki/w/Spear + 26.3 JAR data files) -----------------
// Attack range: minimum 2 blocks, maximum 4.5 blocks (hitbox margin 0.125).
inline bool SpearInRange(float dist) { return dist >= 2.0f && dist <= 4.5f; }
// Charge attack damage = multiplier * relative velocity, only when the
// attacker and target close at >= 4.6 blocks/second.
inline float SpearChargeDamage(float chargeMult, float relativeSpeed) {
    return relativeSpeed >= 4.6f ? chargeMult * relativeSpeed : 0.0f;
}
// Charge attack also knocks back when the user moves at >= 5.1 blocks/second.
inline bool SpearChargeKnockback(float userSpeed) { return userSpeed >= 5.1f; }
// Charge multipliers map cleanly by tier: 0->0.7, 1->0.82, 2->0.95,
// 3->1.075, 4->1.2 (wood/gold share 0.7; stone/copper share 0.82).
inline float SpearChargeMultiplier(int tier) {
    switch (tier) {
        case 0: return 0.7f;
        case 1: return 0.82f;
        case 2: return 0.95f;
        case 3: return 1.075f;
        default: return 1.2f;
    }
}

// Resolve a weapon by its registry name. Spears are data-driven in 26.3 (no
// SpearItem class), so the tier comes from the name prefix. Jab damage, attack
// speed and durability are the Java Edition values from the wiki stat table.
inline WeaponDef WeaponFromName(const std::string& name) {
    WeaponDef w;
    if (name == "mace") {
        // MaceItem.createAttributes: +5 damage (total 6), -3.4 speed (0.6).
        // Durability 500 is the wiki value (UNVERIFIED against the JAR component).
        w.kind = WeaponKind::Mace; w.durability = 500;
        w.jabDamage = 6.0f; w.attackSpeed = 0.6f;
        return w;
    }
    if (name == "trident") {
        // Trident: 9 melee damage, 1.1 attack speed, 250 durability (wiki).
        w.kind = WeaponKind::Trident; w.durability = 250;
        w.jabDamage = 9.0f; w.attackSpeed = 1.1f;
        return w;
    }
    const std::string suffix = "_spear";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const std::string tier = name.substr(0, name.size() - suffix.size());
        w.kind = WeaponKind::Spear;
        if (tier == "wooden")      { w.tier = 0; w.durability = 59;   w.jabDamage = 1; w.attackSpeed = 1.54f; }
        else if (tier == "golden") { w.tier = 0; w.durability = 32;   w.jabDamage = 1; w.attackSpeed = 1.05f; }
        else if (tier == "stone")  { w.tier = 1; w.durability = 131;  w.jabDamage = 2; w.attackSpeed = 1.33f; }
        else if (tier == "copper") { w.tier = 1; w.durability = 190;  w.jabDamage = 2; w.attackSpeed = 1.18f; }
        else if (tier == "iron")   { w.tier = 2; w.durability = 250;  w.jabDamage = 3; w.attackSpeed = 1.05f; }
        else if (tier == "diamond"){ w.tier = 3; w.durability = 1561; w.jabDamage = 4; w.attackSpeed = 0.95f; }
        else if (tier == "netherite"){ w.tier = 4; w.durability = 2031; w.jabDamage = 5; w.attackSpeed = 0.87f; }
        else { w.kind = WeaponKind::None; return w; }
        w.chargeMult = SpearChargeMultiplier(w.tier);
        return w;
    }
    return w;
}

} // namespace potato