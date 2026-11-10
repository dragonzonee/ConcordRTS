#pragma once

#include "runtime/config/RuntimeConfig.h"

#include <map>
#include <string>
#include <vector>

// Autoplay scripts: an INI file with a [Game] section (match setup) and a
// [Steps] section (numbered player actions). See tests/autoplay/README.md.
//
//   [Game]
//   Map=2peaks.map
//   Country=Americans
//   [Steps]
//   1=WaitUntil BaseReady 300
//   2=ClickSidebar GAPOWR

struct RuntimeAutoplayStep {
    int number = 0;                 // key in [Steps]
    std::string verb;               // first word, e.g. "ClickSidebar"
    std::vector<std::string> args;  // remaining words
    std::string text;               // the whole value, for logs
};

struct RuntimeAutoplayScript {
    std::string path;
    std::map<std::string, std::string> game;  // lower-case keys
    std::vector<RuntimeAutoplayStep> steps;   // ordered by number
};

bool loadRuntimeAutoplayScript(
    const std::string& path,
    RuntimeAutoplayScript& script,
    std::string& error
);

// Applies [Game] to the match configuration (map, countries, credits, speed,
// AI) so the match starts directly, without the menus.
void applyRuntimeAutoplayGameSettings(
    const RuntimeAutoplayScript& script,
    RuntimeConfig& config
);

// Integer value of a [Game] key, or the fallback when absent/invalid.
int runtimeAutoplayGameInt(
    const RuntimeAutoplayScript& script,
    const std::string& key,
    int fallback
);
