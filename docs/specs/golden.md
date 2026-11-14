# Probe and golden-file format

Our equivalent of Horizon's `tools/test_*_parity.py`: a probe inside `PotatoSelfTest.exe` prints one machine-readable
line, and CI (or the user's machine) compares it to a committed golden.

## Rules

1. A probe prints exactly one line prefixed with the area tag, e.g.:
   `PARITY-REGISTRY: jar=yes blocks=136 items=66 recipes=64 loot=... names=202 dup=0 hash=1a2b3c4d`
2. Probes never throw. Missing oracle data (no JAR on a CI runner) prints `SKIP (<reason>)` and does not fail.
3. Goldens live in `tests/golden/<area>.txt` and contain the probe line only — no Mojang data, just counts and hashes.
4. A goldens diff is reviewed like code: a changed hash means behaviour changed, and needs a stated reason.
5. Never commit the generated `tools/parity/ledger.json`, extracted JAR files, or assets.

## Current probes

| Tag | Source | Oracle | Gate |
| --- | --- | --- | --- |
| `PARITY-REGISTRY` | `selftest.cpp` `TestRegistryParity` | the user's 26.3 client JAR | counts >= v2.0.0 baseline, `dup=0` |
| `PARITY-UI` (planned, M2) | `selftest.cpp` | JAR GUI sprites + font providers | sprite/font table counts and hash |
| `PARITY-CHUNK` (planned, M4) | `selftest.cpp` | vanilla chunks generated locally | per-seed chunk hash |

## Baseline

| Metric | v2.0.0 baseline |
| --- | ---: |
| Mapped blocks | 136 |
| Mapped items | 66 |
| Recipes | 64 |

These numbers rise as the JAR registry work lands; they must never fall.
