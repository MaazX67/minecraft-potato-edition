#include "commands.h"
#include "engine.h"
#include "registry.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace potato {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(uint8_t(c)));
    return s;
}

std::vector<std::string> Tokenize(const std::string& s) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

bool ParseInt(const std::string& token, long long& out) {
    if (token.empty()) return false;
    char* end = nullptr;
    const long long v = strtoll(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool ParseCoord(const std::string& token, float base, float& out) {
    if (token.empty()) return false;
    if (token[0] == '~') {
        if (token.size() == 1) { out = base; return true; }
        char* end = nullptr;
        const float v = strtof(token.c_str() + 1, &end);
        if (end == token.c_str() + 1 || *end != '\0') return false;
        out = base + v;
        return true;
    }
    char* end = nullptr;
    const float v = strtof(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

std::string DisplayName(uint8_t id) {
    if (id == BLOCK_GRASS) return "Grass Block";
    std::string n = ResourceNameForItem(id);
    const size_t colon = n.find(':');
    if (colon != std::string::npos) n = n.substr(colon + 1);
    std::string out;
    for (char c : n) {
        if (c == '_') out += ' ';
        else out += (out.empty() || out.back() == ' ') ? char(toupper(uint8_t(c))) : c;
    }
    return out;
}

CommandResult Ok(std::vector<std::string> lines) {
    CommandResult r;
    r.ok = true;
    r.lines = std::move(lines);
    return r;
}

CommandResult Ok(const std::string& line) { return Ok(std::vector<std::string>{line}); }

CommandResult Err(const std::string& line) {
    CommandResult r;
    r.error = true;
    r.lines.push_back(line);
    return r;
}

CommandResult Usage(const char* usage) {
    return Err(std::string("Usage: ") + usage);
}

const char* kGameModes[2] = {"Survival Mode", "Creative Mode"};

bool ParseGameMode(const std::string& token, int& mode) {
    const std::string t = Lower(token);
    if (t == "survival" || t == "s" || t == "0") { mode = 0; return true; }
    if (t == "creative" || t == "c" || t == "1") { mode = 1; return true; }
    return false;
}

bool ParseWeather(const std::string& token, int& weather) {
    const std::string t = Lower(token);
    if (t == "clear") { weather = 0; return true; }
    if (t == "rain") { weather = 1; return true; }
    if (t == "thunder") { weather = 2; return true; }
    return false;
}

bool ParseTimeValue(const std::string& token, uint64_t& ticks) {
    const std::string t = Lower(token);
    if (t == "day") { ticks = 1000; return true; }
    if (t == "noon") { ticks = 6000; return true; }
    if (t == "night") { ticks = 13000; return true; }
    if (t == "midnight") { ticks = 18000; return true; }
    long long v = 0;
    if (!ParseInt(t, v) || v < 0) return false;
    ticks = uint64_t(v);
    return true;
}

const char* kBiomes[] = {"plains", "forest", "birch_forest", "taiga", "desert", "snowy_plains",
                         "mountains", "river", "ocean", "cherry_grove"};

const char* kGameRules[] = {"keepInventory", "doDaylightCycle", "doMobSpawning", "doWeatherCycle"};

bool IsGameRule(const std::string& name) {
    for (const char* rule : kGameRules)
        if (name == rule) return true;
    return false;
}

std::vector<std::string> CommandNames() {
    return {"/gamemode", "/give", "/tp", "/time", "/weather", "/seed", "/locate",
            "/summon", "/kill", "/clear", "/gamerule", "/help"};
}

// Options for the token at index pos; done holds every earlier token.
std::vector<std::string> ArgOptions(const std::string& cmd, size_t pos,
                                    const std::vector<std::string>& done) {
    std::vector<std::string> out;
    if (cmd == "gamemode" && pos == 0) {
        out = {"survival", "creative", "s", "c"};
    } else if (cmd == "weather" && pos == 0) {
        out = {"clear", "rain", "thunder"};
    } else if (cmd == "time" && pos == 0) {
        out = {"set", "add", "query"};
    } else if (cmd == "time" && pos == 1 && !done.empty() && Lower(done[0]) == "set") {
        out = {"day", "noon", "night", "midnight"};
    } else if (cmd == "gamerule" && pos == 0) {
        for (const char* rule : kGameRules) out.push_back(rule);
    } else if (cmd == "gamerule" && pos == 1 && !done.empty() && IsGameRule(done[0])) {
        out = {"true", "false"};
    } else if (cmd == "summon" && pos == 0) {
        out = {"zombie"};
    } else if (cmd == "kill" && pos == 0) {
        out = {"player", "zombie", "@e"};
    } else if (cmd == "locate" && pos == 0) {
        out = {"biome"};
    } else if (cmd == "locate" && pos == 1 && !done.empty() && Lower(done[0]) == "biome") {
        for (const char* biome : kBiomes) out.push_back(biome);
    } else if (cmd == "give" && pos == 0) {
        for (int i = 1; i < 256; ++i) {
            const uint8_t id = uint8_t(i);
            if (!IsValidItem(id)) continue;
            const char* name = ItemName(id);
            if (name && name[0]) out.push_back(name);
        }
    }
    return out;
}

} // namespace

CommandResult ExecuteCommand(const std::string& line, CommandHost& host) {
    std::string body = line;
    if (!body.empty() && body[0] == '/') body = body.substr(1);
    std::vector<std::string> tokens = Tokenize(body);
    if (tokens.empty())
        return Err("Unknown or incomplete command, see below for error");
    const std::string cmd = Lower(tokens[0]);
    tokens.erase(tokens.begin());

    if (cmd == "gamemode") {
        if (tokens.empty()) return Usage("/gamemode <survival|creative>");
        int mode = 0;
        if (!ParseGameMode(tokens[0], mode))
            return Err("Unknown game mode '" + tokens[0] + "'");
        host.SetGameMode(mode);
        return Ok(std::string("Set own game mode to ") + kGameModes[mode]);
    }
    if (cmd == "give") {
        if (tokens.empty()) return Usage("/give <item> [amount]");
        long long count = 1;
        if (tokens.size() > 1 && (!ParseInt(tokens[1], count) || count < 1 || count > 6400))
            return Usage("/give <item> [amount]");
        std::string givenName;
        if (!host.GiveItem(tokens[0], int(count), givenName))
            return Err("Unknown item '" + tokens[0] + "'");
        return Ok("Gave " + std::to_string(count) + " [" + givenName + "] to " + host.PlayerName());
    }
    if (cmd == "tp" || cmd == "teleport") {
        float x, y, z;
        host.PlayerPos(x, y, z);
        if (tokens.size() != 3) return Usage("/tp <x> <y> <z>");
        if (!ParseCoord(tokens[0], x, x) || !ParseCoord(tokens[1], y, y) || !ParseCoord(tokens[2], z, z))
            return Usage("/tp <x> <y> <z>");
        host.Teleport(x, y, z);
        char msg[128];
        snprintf(msg, sizeof(msg), "Teleported %s to %.1f, %.1f, %.1f",
                 host.PlayerName().c_str(), double(x), double(y), double(z));
        return Ok(std::string(msg));
    }
    if (cmd == "time") {
        if (tokens.empty()) return Usage("/time <set|add|query> [value]");
        const std::string action = Lower(tokens[0]);
        if (action == "set" || action == "add") {
            if (tokens.size() < 2) return Usage("/time <set|add> <value>");
            uint64_t value = 0;
            if (!ParseTimeValue(tokens[1], value))
                return Err("Invalid time '" + tokens[1] + "'");
            uint64_t result = 0;
            if (action == "set") {
                result = value;
            } else {
                result = host.TimeTicks() + value;
            }
            host.SetTimeTicks(result);
            return Ok(std::string(action == "set" ? "Set the time to " : "Added ") +
                      std::to_string(action == "set" ? result : value) +
                      (action == "set" ? "" : " to the time"));
        }
        if (action == "query") {
            const uint64_t ticks = host.TimeTicks() % 24000u;
            return Ok("The time is " + std::to_string(ticks));
        }
        return Usage("/time <set|add|query> [value]");
    }
    if (cmd == "weather") {
        if (tokens.empty()) return Usage("/weather <clear|rain|thunder> [seconds]");
        int weather = 0;
        if (!ParseWeather(tokens[0], weather))
            return Err("Unknown weather '" + tokens[0] + "'");
        long long seconds = 0;
        if (tokens.size() > 1 && (!ParseInt(tokens[1], seconds) || seconds < 0))
            return Usage("/weather <clear|rain|thunder> [seconds]");
        host.SetWeather(weather, int(seconds * 20));
        if (weather == 0) return Ok("Changing to clear weather");
        if (weather == 1) return Ok("Changing to rainy weather");
        return Ok("Changing to thunder");
    }
    if (cmd == "seed") {
        return Ok("Seed: " + std::to_string(host.WorldSeed()));
    }
    if (cmd == "locate") {
        if (tokens.empty()) return Usage("/locate biome <biome>");
        const std::string what = Lower(tokens[0]);
        if (what == "biome") {
            if (tokens.size() < 2) return Usage("/locate biome <biome>");
            const std::string biome = Lower(tokens[1]);
            bool known = false;
            for (const char* name : kBiomes)
                if (biome == name) known = true;
            if (!known) return Err("Unknown biome '" + tokens[1] + "'");
            int x = 0, z = 0, distance = 0;
            if (!host.LocateBiome(biome, x, z, distance))
                return Err("Could not find a biome of type \"" + biome + "\" within a reasonable distance");
            char msg[160];
            snprintf(msg, sizeof(msg), "The nearest %s is at [%d, ~, %d] (%d blocks away)",
                     biome.c_str(), x, z, distance);
            return Ok(std::string(msg));
        }
        if (what == "structure" || what == "poi") {
            return Err("Structure locating is not available on this world");
        }
        return Usage("/locate biome <biome>");
    }
    if (cmd == "summon") {
        if (tokens.empty()) return Usage("/summon <entity> [count]");
        long long count = 1;
        if (tokens.size() > 1 && (!ParseInt(tokens[1], count) || count < 1 || count > 64))
            return Usage("/summon <entity> [count]");
        const std::string entity = Lower(tokens[0]);
        if (host.Summon(entity, int(count)) < 0)
            return Err("Unknown entity '" + tokens[0] + "'");
        return Ok("Summoned new Zombie");
    }
    if (cmd == "kill") {
        const std::string target = tokens.empty() ? "player" : Lower(tokens[0]);
        if (target != "player" && target != "@s" && target != "zombie" && target != "@e")
            return Err("Unknown target '" + tokens[0] + "'");
        const int killed = host.KillEntities(target);
        if (killed <= 0) return Ok("No entity was found");
        if (target == "player" || target == "@s")
            return Ok("Killed " + host.PlayerName());
        return Ok("Killed " + std::to_string(killed) + (killed == 1 ? " entity" : " entities"));
    }
    if (cmd == "clear") {
        const int removed = host.ClearInventory();
        if (removed <= 0)
            return Ok("No items were found on player " + host.PlayerName());
        return Ok("Removed " + std::to_string(removed) + " items from " + host.PlayerName());
    }
    if (cmd == "gamerule") {
        if (tokens.empty()) return Usage("/gamerule <rule> [value]");
        if (!IsGameRule(tokens[0])) return Err("Unknown game rule '" + tokens[0] + "'");
        if (tokens.size() < 2) {
            std::string value;
            if (!host.GetGameRule(tokens[0], value)) return Err("Unknown game rule '" + tokens[0] + "'");
            return Ok("Gamerule " + tokens[0] + " is currently set to: " + value);
        }
        const std::string value = Lower(tokens[1]);
        if (value != "true" && value != "false")
            return Err("Invalid game rule value '" + tokens[1] + "'");
        if (!host.SetGameRule(tokens[0], value))
            return Err("Unknown game rule '" + tokens[0] + "'");
        return Ok("Gamerule " + tokens[0] + " is now set to: " + value);
    }
    if (cmd == "help") {
        return Ok(std::vector<std::string>{
            "Commands: /gamemode /give /tp /time /weather /seed /locate /summon /kill /clear /gamerule",
            "Press Tab to complete commands, Up/Down for history."});
    }
    return Err("Unknown or incomplete command, see below for error");
}

std::string CommonPrefix(const std::vector<std::string>& options) {
    if (options.empty()) return {};
    std::string prefix = options[0];
    for (size_t i = 1; i < options.size(); ++i) {
        size_t n = 0;
        while (n < prefix.size() && n < options[i].size() && prefix[n] == options[i][n]) ++n;
        prefix.resize(n);
    }
    return prefix;
}

std::vector<std::string> CommandCompletions(const std::string& input) {
    std::vector<std::string> out;
    if (input.empty() || input[0] != '/') return out;
    const std::string body = input.substr(1);
    const size_t space = body.find(' ');
    if (space == std::string::npos) {
        const std::string partial = Lower(body);
        for (const std::string& name : CommandNames())
            if (name.size() > 1 && Lower(name.substr(1)).compare(0, partial.size(), partial) == 0)
                out.push_back(name);
        return out;
    }
    const std::string cmd = Lower(body.substr(0, space));
    const std::string rest = body.substr(space + 1);
    const std::vector<std::string> tokens = Tokenize(rest);
    const bool trailing = !rest.empty() && (rest.back() == ' ' || rest.back() == '\t');
    std::vector<std::string> done;
    std::string partial;
    if (trailing) {
        done = tokens;
    } else if (!tokens.empty()) {
        done.assign(tokens.begin(), tokens.end() - 1);
        partial = tokens.back();
    }
    const std::string lowerPartial = Lower(partial);
    std::string base = "/" + cmd;
    for (const std::string& token : done) base += " " + token;
    base += " ";
    for (const std::string& option : ArgOptions(cmd, done.size(), done))
        if (Lower(option).compare(0, lowerPartial.size(), lowerPartial) == 0)
            out.push_back(base + option);
    return out;
}

void ChatHistory::Push(const std::string& line) {
    if (line.empty()) return;
    if (!mLines.empty() && mLines.back() == line) { Reset(); return; }
    mLines.push_back(line);
    if (mLines.size() > 100) mLines.erase(mLines.begin());
    Reset();
}

bool ChatHistory::Prev(std::string& out) {
    if (mCursor == 0) return false;
    --mCursor;
    out = mLines[mCursor];
    return true;
}

bool ChatHistory::Next(std::string& out) {
    if (mCursor >= mLines.size()) return false;
    ++mCursor;
    out = mCursor == mLines.size() ? std::string() : mLines[mCursor];
    return true;
}

} // namespace potato
