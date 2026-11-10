#include "runtime/autoplay/RuntimeAutoplayScript.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string trimmed(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parseInt(const std::string& text, int& value) {
    if (text.empty()) return false;
    std::size_t used = 0;
    try {
        value = std::stoi(text, &used);
    } catch (...) {
        return false;
    }
    return used == text.size();
}

bool yes(const std::string& value) {
    const std::string v = lowered(value);
    return v == "yes" || v == "true" || v == "1" || v == "on";
}

std::string gameValue(const RuntimeAutoplayScript& script, const std::string& key) {
    const auto it = script.game.find(key);
    return it == script.game.end() ? std::string() : it->second;
}

}  // namespace

bool loadRuntimeAutoplayScript(
    const std::string& path,
    RuntimeAutoplayScript& script,
    std::string& error
) {
    script = {};
    script.path = path;
    std::ifstream in(path);
    if (!in) {
        error = "cannot open autoplay script: " + path;
        return false;
    }
    std::string section;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        const auto comment = line.find(';');
        if (comment != std::string::npos) line.erase(comment);
        line = trimmed(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lowered(trimmed(line.substr(1, line.size() - 2)));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            error = "line " + std::to_string(lineNumber) + ": expected key=value";
            return false;
        }
        const std::string key = trimmed(line.substr(0, equals));
        const std::string value = trimmed(line.substr(equals + 1));
        if (section == "game") {
            script.game[lowered(key)] = value;
        } else if (section == "steps") {
            RuntimeAutoplayStep step;
            if (!parseInt(key, step.number)) {
                error = "line " + std::to_string(lineNumber) + ": step key must be a number";
                return false;
            }
            std::istringstream words(value);
            words >> step.verb;
            for (std::string word; words >> word;) step.args.push_back(word);
            if (step.verb.empty()) {
                error = "line " + std::to_string(lineNumber) + ": empty step";
                return false;
            }
            step.text = value;
            script.steps.push_back(std::move(step));
        }
    }
    std::stable_sort(script.steps.begin(), script.steps.end(),
        [](const RuntimeAutoplayStep& a, const RuntimeAutoplayStep& b) {
            return a.number < b.number;
        });
    if (script.steps.empty()) {
        error = "autoplay script has no [Steps]";
        return false;
    }
    return true;
}

int runtimeAutoplayGameInt(
    const RuntimeAutoplayScript& script,
    const std::string& key,
    int fallback
) {
    int value = 0;
    return parseInt(gameValue(script, key), value) ? value : fallback;
}

void applyRuntimeAutoplayGameSettings(
    const RuntimeAutoplayScript& script,
    RuntimeConfig& config
) {
    config.showMainMenu = false;
    config.showSkirmishMenu = false;
    config.gameMode = "Battle";

    const std::string map = gameValue(script, "map");
    if (!map.empty()) {
        // A bare file name means one of the game's multiplayer maps.
        const std::filesystem::path mapPath(map);
        config.defaultMapPath = mapPath.has_parent_path()
            ? map
            : (std::filesystem::path(config.localDataRootPath) / "resources"
                / "mix_exports" / "multimd" / mapPath).string();
    }
    const std::string country = gameValue(script, "country");
    config.playerCountry = country.empty() ? "Americans" : country;
    config.startingCredits = runtimeAutoplayGameInt(script, "credits", 10000);
    config.startingUnits = runtimeAutoplayGameInt(script, "units", config.startingUnits);
    config.gameSpeed = std::clamp(runtimeAutoplayGameInt(script, "speed", config.gameSpeed), 0, 6);
    if (!gameValue(script, "crates").empty()) config.crates = yes(gameValue(script, "crates"));
    if (!gameValue(script, "shortgame").empty()) config.shortGame = yes(gameValue(script, "shortgame"));

    const std::string enemy = gameValue(script, "enemy");
    const bool hasEnemy = !enemy.empty() && lowered(enemy) != "none";
    config.computerAiEnabled = gameValue(script, "ai").empty() || yes(gameValue(script, "ai"));

    config.playerSlots.clear();
    RuntimeConfig::PlayerSlot me;
    me.enabled = true;
    me.human = true;
    me.local = true;
    me.name = "Autoplay";
    me.country = config.playerCountry;
    me.color = gameValue(script, "color").empty() ? "Gold" : gameValue(script, "color");
    me.startPosition = runtimeAutoplayGameInt(script, "start", 1);
    config.playerSlots.push_back(me);
    if (hasEnemy) {
        RuntimeConfig::PlayerSlot ai;
        ai.enabled = true;
        ai.human = false;
        ai.name = "Enemy";
        ai.country = enemy;
        ai.color = "Red";
        ai.difficulty = gameValue(script, "enemydifficulty").empty()
            ? "Easy" : gameValue(script, "enemydifficulty");
        ai.startPosition = runtimeAutoplayGameInt(script, "enemystart", 2);
        config.playerSlots.push_back(ai);
    }
}
