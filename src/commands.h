#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace potato {

// World operations a slash command may perform. The game shell implements
// this so the parser and all Java-syntax handling stay headless-testable.
class CommandHost {
public:
    virtual ~CommandHost() = default;
    virtual int GameMode() const = 0; // 0 survival, 1 creative
    virtual void SetGameMode(int mode) = 0;
    virtual void PlayerPos(float& x, float& y, float& z) const = 0;
    virtual void Teleport(float x, float y, float z) = 0;
    virtual uint64_t TimeTicks() const = 0;
    virtual void SetTimeTicks(uint64_t ticks) = 0;
    virtual uint32_t WorldSeed() const = 0;
    // 0 clear, 1 rain, 2 thunder. durationTicks 0 means the Java default.
    virtual void SetWeather(int weather, int durationTicks) = 0;
    virtual int Weather() const = 0;
    virtual bool GiveItem(const std::string& item, int count, std::string& givenName) = 0;
    virtual bool LocateBiome(const std::string& biome, int& x, int& z, int& distance) = 0;
    // Returns the number spawned, -1 for an unknown entity id.
    virtual int Summon(const std::string& entity, int count) = 0;
    // Returns entities killed, -1 for an unknown target.
    virtual int KillEntities(const std::string& target) = 0;
    virtual int ClearInventory() = 0;
    virtual bool GetGameRule(const std::string& rule, std::string& value) const = 0;
    virtual bool SetGameRule(const std::string& rule, const std::string& value) = 0;
    virtual std::string PlayerName() const = 0;
};

struct CommandResult {
    bool ok = false;
    bool error = false;
    std::vector<std::string> lines; // one chat line per entry
};

// line may be "/cmd ..." or "cmd ...". Text without '/' is chat, handled by
// the caller, and returns an error here.
CommandResult ExecuteCommand(const std::string& line, CommandHost& host);

// Tab completion over Java's command/argument names. Returns full replacement
// strings for the whole input line.
std::vector<std::string> CommandCompletions(const std::string& input);
std::string CommonPrefix(const std::vector<std::string>& options);

// Session history behind Up/Down; the cursor sits at size() while editing a
// fresh line.
class ChatHistory {
public:
    void Push(const std::string& line);
    bool Prev(std::string& out);
    bool Next(std::string& out);
    void Reset() { mCursor = mLines.size(); }
    size_t Size() const { return mLines.size(); }
private:
    std::vector<std::string> mLines;
    size_t mCursor = 0;
};

} // namespace potato
