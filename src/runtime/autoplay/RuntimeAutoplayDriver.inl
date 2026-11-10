// Autoplay test harness: plays a real match from a script of player actions.
//
// Every action is delivered as the same Win32 messages a mouse or keyboard
// produces (WM_LBUTTONDOWN, WM_MOUSEMOVE, WM_KEYDOWN, ...) and sent to the game
// window, so the sidebar, placement, selection and command code under test is
// the code players use. Screen positions come from the game's own layout
// helpers (sidebar tab/button rects, cell and object projection).
//
// Included by RuntimeShell.cpp just before runRuntimeWindowSession; driven by
// startRuntimeAutoplay()/tickRuntimeAutoplay() from that function's loop.

#include "runtime/autoplay/RuntimeAutoplayScript.h"
#include "render/hd/RuntimeHdSprites.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {

struct RuntimeAutoplayRun {
    RuntimeAutoplayScript script;
    std::filesystem::path outDir;
    std::ofstream log;
    std::size_t step = 0;
    int phase = 0;              // progress inside a multi-message action
    int waitFrames = 0;         // loop passes left before the next phase
    int stepStartTick = -1;
    int targetId = -1;          // object chosen by the last AttackTarget
    int maxTicks = 54000;       // safety stop: 30 minutes of game time
    int maxSeconds = 900;       // safety stop in real time (ticks freeze at match end)
    bool matchOverLogged = false;
    struct Profile {
        std::chrono::steady_clock::time_point started;
        int tick = 0, samples = 0;
        std::uint64_t presented = 0;
        double paintMs = 0, mapMs = 0, overlayMs = 0, sidebarMs = 0, compileMs = 0, uploadMs = 0, presentMs = 0, gpuMs = 0, loopMs = 0;
        std::chrono::steady_clock::time_point last;
    } profile;
    int passedSteps = 0;
    std::map<std::size_t, int> loops;   // RepeatUntil step index -> jumps taken
    bool done = false;
    bool failed = false;
    std::string failure;
    std::chrono::steady_clock::time_point started;
};

std::unique_ptr<RuntimeAutoplayRun> gRuntimeAutoplay;

std::string autoplayLower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool autoplayInt(const std::string& text, int& value) {
    try {
        std::size_t used = 0;
        value = std::stoi(text, &used);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

void autoplayLog(RuntimeAutoplayRun& run, const RuntimeWindowState& state, const std::string& text) {
    run.log << "[tick " << state.tick << "] " << text << "\n";
    run.log.flush();
}

// ---------------------------------------------------------------- input
void autoplaySend(HWND window, UINT message, WPARAM wParam, POINT point) {
    // Points are logical; the window host divides real client coordinates by
    // the battle window scale, so send what a real mouse would report.
    const int scale = concord::window::runtimeWindowScaleFor(window);
    SendMessageW(window, message, wParam, MAKELPARAM(point.x * scale + scale / 2, point.y * scale + scale / 2));
}

void autoplayMove(HWND window, POINT point) {
    autoplaySend(window, WM_MOUSEMOVE, 0, point);
}

void autoplayLeftClick(HWND window, POINT point) {
    autoplaySend(window, WM_MOUSEMOVE, 0, point);
    autoplaySend(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    autoplaySend(window, WM_LBUTTONUP, 0, point);
}

void autoplayRightClick(HWND window, POINT point) {
    autoplaySend(window, WM_MOUSEMOVE, 0, point);
    autoplaySend(window, WM_RBUTTONDOWN, MK_RBUTTON, point);
    autoplaySend(window, WM_RBUTTONUP, 0, point);
}

void autoplayDrag(HWND window, POINT from, POINT to) {
    autoplaySend(window, WM_MOUSEMOVE, 0, from);
    autoplaySend(window, WM_LBUTTONDOWN, MK_LBUTTON, from);
    for (int i = 1; i <= 8; ++i) {
        const POINT p{from.x + (to.x - from.x) * i / 8, from.y + (to.y - from.y) * i / 8};
        autoplaySend(window, WM_MOUSEMOVE, MK_LBUTTON, p);
    }
    autoplaySend(window, WM_LBUTTONUP, 0, to);
}

void autoplayKey(HWND window, UINT vk) {
    SendMessageW(window, WM_KEYDOWN, vk, 1);
    SendMessageW(window, WM_KEYUP, vk, (1u << 30) | (1u << 31) | 1);
}

POINT autoplayCenter(const RECT& rect) {
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

RECT autoplayClient(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    return client;
}

// ---------------------------------------------------------------- game queries
bool autoplayIsMine(const RuntimeWindowState& state, const WorldObjectInstance& object) {
    return autoplayLower(object.owner) == autoplayLower(state.economy.owner);
}

bool autoplayIsEnemy(const RuntimeWindowState& state, const WorldObjectInstance& object) {
    const std::string owner = autoplayLower(object.owner);
    return !owner.empty() && !autoplayIsMine(state, object) && owner != "neutral"
        && owner != "special" && owner != "civilian";
}

bool autoplayIsMobile(const WorldObjectInstance& object) {
    return object.kind == WorldObjectKind::Unit || object.kind == WorldObjectKind::Infantry;
}

// Harvesters and MCVs: mobile, but not fighting units.
bool autoplayIsSupportUnit(const WorldObjectInstance& object) {
    const std::string t = autoplayLower(object.type);
    if (t.size() >= 3 && t.compare(t.size() - 3, 3, "mcv") == 0) return true;
    return t == "cmin" || t == "harv" || t == "smin" || t == "pcv";
}

int autoplayEnemiesLeft(const RuntimeWindowState& state) {
    int count = 0;
    for (const auto& o : state.world.getObjects())
        if (o.alive && !o.rubble && autoplayIsEnemy(state, o)) ++count;
    return count;
}

const RuntimeBuildCatalogItem* autoplayCatalogItem(const RuntimeWindowState& state, const std::string& typeId, int* index = nullptr) {
    for (std::size_t i = 0; i < state.economy.catalog.size(); ++i) {
        if (autoplayLower(state.economy.catalog[i].typeId) == autoplayLower(typeId)) {
            if (index) *index = static_cast<int>(i);
            return &state.economy.catalog[i];
        }
    }
    return nullptr;
}

int autoplayCountMine(const RuntimeWindowState& state, const std::string& typeId, bool structures) {
    int count = 0;
    for (const auto& object : state.world.getObjects()) {
        if (!object.alive || object.rubble || !autoplayIsMine(state, object)) continue;
        if (structures != (object.kind == WorldObjectKind::Structure)) continue;
        if (typeId.empty() || autoplayLower(object.type) == autoplayLower(typeId)) ++count;
    }
    return count;
}

const WorldObjectInstance* autoplayObjectById(const RuntimeWindowState& state, int id) {
    for (const auto& object : state.world.getObjects())
        if (object.instanceId == id) return &object;
    return nullptr;
}

// Finds a screen point inside the map viewport that the game itself maps to
// `cell` (the inverse of approximateCellFromScreenPoint, by local search).
// Terrain height of a cell (0 when unknown); cells on hills project higher.
int autoplayCellLevel(const RuntimeWindowState& state, RuntimeCellPoint cell) {
    const auto it = state.terrainByCell.find(runtimeCellKey(cell.x, cell.y));
    return (it != state.terrainByCell.end() && it->second) ? it->second->level : 0;
}

// Screen estimate for a cell, including its terrain height.
POINT autoplayCellEstimate(const RuntimeWindowState& state, const RECT& viewport, RuntimeCellPoint cell) {
    return runtimeScreenPointForCell(state, viewport, cell, autoplayCellLevel(state, cell));
}

// Exact hit when one exists; otherwise (`allowNearest`) the screen point whose
// picked cell is closest to `cell` (shroud can hide the exact surface).
std::optional<POINT> autoplayScreenPointForCell(const RuntimeWindowState& state, const RECT& viewport,
    RuntimeCellPoint cell, bool allowNearest = false, int* nearestError = nullptr) {
    const POINT guess = autoplayCellEstimate(state, viewport, cell);
    std::optional<POINT> nearest;
    int nearestDist = INT_MAX;
    for (int radius = 0; radius <= 180; radius += 3) {
        for (int dy = -radius; dy <= radius; dy += 3) {
            for (int dx = -radius; dx <= radius; dx += 3) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const POINT p{guess.x + dx, guess.y + dy};
                if (p.x <= viewport.left || p.x >= viewport.right || p.y <= viewport.top || p.y >= viewport.bottom) continue;
                const POINT c = approximateCellFromScreenPoint(state, viewport, p);
                if (c.x == cell.x && c.y == cell.y) { if (nearestError) *nearestError = 0; return p; }
                const int d = std::max(std::abs(c.x - cell.x), std::abs(c.y - cell.y));
                if (d < nearestDist) { nearestDist = d; nearest = p; }
            }
        }
    }
    if (nearestError) *nearestError = nearestDist;
    return allowNearest ? nearest : std::nullopt;
}

// ---------------------------------------------------------------- conditions
// Evaluates a condition word + arguments; `detail` explains the result.
bool autoplayCondition(RuntimeAutoplayRun& run, const RuntimeWindowState& state,
    const std::vector<std::string>& words, std::string& detail) {
    if (words.empty()) { detail = "missing condition"; return false; }
    const std::string what = autoplayLower(words[0]);
    const auto arg = [&](std::size_t i) { return i < words.size() ? words[i] : std::string(); };
    int n = 0;
    if (what == "baseready") {
        const int structures = autoplayCountMine(state, "", true);
        detail = "my structures=" + std::to_string(structures) + " catalog=" + std::to_string(state.economy.catalog.size());
        return structures > 0 && !state.economy.catalog.empty();
    }
    if (what == "buildingexists") {
        const int want = autoplayInt(arg(2), n) ? n : 1;
        const int have = autoplayCountMine(state, arg(1), true);
        detail = arg(1) + " count=" + std::to_string(have) + " want>=" + std::to_string(want);
        return have >= want;
    }
    if (what == "unitexists") {
        const int want = autoplayInt(arg(2), n) ? n : 1;
        const int have = autoplayCountMine(state, arg(1), false);
        detail = arg(1) + " count=" + std::to_string(have) + " want>=" + std::to_string(want);
        return have >= want;
    }
    if (what == "buildready") {
        const RuntimeBuildCatalogItem* item = autoplayCatalogItem(state, arg(1));
        const bool ready = item && runtimeReadyProductionForItem(state, *item) != nullptr;
        detail = !item ? arg(1) + " not in catalog" : (ready ? arg(1) + " finished" : arg(1) + " not finished");
        return ready;
    }
    if (what == "placing") {
        detail = runtimeSelectedPlacement(state).has_value() ? "placement active" : "no placement";
        return runtimeSelectedPlacement(state).has_value();
    }
    if (what == "creditsatleast") {
        autoplayInt(arg(1), n);
        detail = "credits=" + std::to_string(state.economy.credits);
        return state.economy.credits >= n;
    }
    if (what == "selected") {
        const int want = autoplayInt(arg(1), n) ? n : 1;
        detail = "selected=" + std::to_string(state.selectedObjectIds.size());
        return static_cast<int>(state.selectedObjectIds.size()) >= want;
    }
    if (what == "unitsnear") {
        int x = 0, y = 0, r = 3, want = 1;
        autoplayInt(arg(1), x); autoplayInt(arg(2), y);
        if (!arg(3).empty()) autoplayInt(arg(3), r);
        if (!arg(4).empty()) autoplayInt(arg(4), want);
        int have = 0;
        for (const auto& object : state.world.getObjects())
            if (object.alive && autoplayIsMine(state, object) && autoplayIsMobile(object)
                && std::abs(object.x - x) <= r && std::abs(object.y - y) <= r) ++have;
        detail = "units within " + std::to_string(r) + " of " + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(have);
        return have >= want;
    }
    if (what == "targetdestroyed") {
        const WorldObjectInstance* target = autoplayObjectById(state, run.targetId);
        detail = target ? "target " + target->type + " health=" + std::to_string(target->health) : "target gone";
        return run.targetId >= 0 && (!target || !target->alive || target->rubble);
    }
    if (what == "targetdamaged") {
        const WorldObjectInstance* target = autoplayObjectById(state, run.targetId);
        autoplayInt(arg(1), n);
        detail = target ? "target health=" + std::to_string(target->health) : "target gone";
        return !target || target->health < n;
    }
    if (what == "enemiesleft") {
        autoplayInt(arg(1), n);
        const int left = autoplayEnemiesLeft(state);
        detail = "enemies left=" + std::to_string(left) + " want<=" + std::to_string(n);
        // A won match counts as cleared: defeated players' leftovers don't fight.
        return left <= n || (state.skirmishMatchFinished && state.skirmishVictory);
    }
    if (what == "hdcapture") {
        // HdCapture NAME: the full-size HD back buffer NAME was written.
        std::error_code error;
        const auto path = run.outDir / arg(1);
        const auto size = std::filesystem::is_regular_file(path, error) ? std::filesystem::file_size(path, error) : 0;
        detail = arg(1) + (size ? " written (" + std::to_string(size) + " bytes)" : " missing");
        return size > 0;
    }
    if (what == "matchover") {
        const std::string want = autoplayLower(arg(1));
        detail = state.skirmishMatchFinished ? "match over: " + state.skirmishResult : "match running";
        if (!state.skirmishMatchFinished) return false;
        if (want == "victory") return state.skirmishVictory;
        if (want == "defeat") return !state.skirmishVictory;
        return true;
    }
    detail = "unknown condition " + words[0];
    return false;
}

// ---------------------------------------------------------------- screenshots
bool autoplayScreenshot(HWND window, const std::filesystem::path& path) {
    const RECT client = autoplayClient(window);
    const int realScale = concord::window::runtimeWindowScaleFor(window);   // capture the real (scaled) client
    const int w = (client.right - client.left) * realScale, h = (client.bottom - client.top) * realScale;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(window);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h;                 // bottom-up, as BMP stores it
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(window, memory, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */);
    if (!printed) BitBlt(memory, 0, 0, w, h, screen, 0, 0, SRCCOPY);
    GdiFlush();
    bool ok = false;
    std::ofstream out(path, std::ios::binary);
    if (out && bits) {
        const std::uint32_t imageBytes = static_cast<std::uint32_t>(w) * h * 4;
        BITMAPFILEHEADER file{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + imageBytes;
        out.write(reinterpret_cast<const char*>(&file), sizeof(file));
        out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
        out.write(static_cast<const char*>(bits), imageBytes);
        ok = static_cast<bool>(out);
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    return ok;
}

// ---------------------------------------------------------------- lifecycle
void finishRuntimeAutoplay(RuntimeAutoplayRun& run, const RuntimeWindowState& state) {
    run.done = true;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
    std::ostringstream summary;
    summary << (run.failed ? "FAIL" : "PASS") << "\n"
            << "script=" << run.script.path << "\n"
            << "steps_passed=" << run.passedSteps << "/" << run.script.steps.size() << "\n"
            << "ticks=" << state.tick << "\n"
            << "seconds=" << seconds << "\n";
    if (run.failed) summary << "failure=" << run.failure << "\n";
    std::ofstream(run.outDir / "result.txt") << summary.str();
    autoplayLog(run, state, std::string("RESULT ") + (run.failed ? "FAIL: " + run.failure : "PASS"));
    std::cout << "Autoplay " << (run.failed ? "FAIL: " + run.failure : std::string("PASS")) << "\n";
}

void failRuntimeAutoplay(RuntimeAutoplayRun& run, const RuntimeWindowState& state, const std::string& why) {
    run.failed = true;
    const RuntimeAutoplayStep& step = run.script.steps[std::min(run.step, run.script.steps.size() - 1)];
    run.failure = "step " + std::to_string(step.number) + " (" + step.text + "): " + why;
    finishRuntimeAutoplay(run, state);
}

void nextRuntimeAutoplayStep(RuntimeAutoplayRun& run, const RuntimeWindowState& state, const std::string& note) {
    const RuntimeAutoplayStep& step = run.script.steps[run.step];
    autoplayLog(run, state, "OK   " + std::to_string(step.number) + " " + step.text + (note.empty() ? "" : "  -- " + note));
    ++run.passedSteps;
    ++run.step;
    run.phase = 0;
    run.waitFrames = 0;
    run.stepStartTick = -1;
}

}  // namespace

// Called once before the window loop; false = the script could not be loaded.
bool startRuntimeAutoplay(RuntimeWindowState& state) {
    if (state.config.autoplayScriptPath.empty()) return true;
    auto run = std::make_unique<RuntimeAutoplayRun>();
    std::string error;
    if (!loadRuntimeAutoplayScript(state.config.autoplayScriptPath, run->script, error)) {
        std::cerr << "Autoplay: " << error << "\n";
        return false;
    }
    run->outDir = state.config.autoplayOutputDir.empty()
        ? std::filesystem::path("build") / "autoplay"
        : std::filesystem::path(state.config.autoplayOutputDir);
    std::error_code ec;
    std::filesystem::create_directories(run->outDir, ec);
    run->log.open(run->outDir / "autoplay.log");
    run->maxTicks = runtimeAutoplayGameInt(run->script, "maxticks", run->maxTicks);
    run->maxSeconds = runtimeAutoplayGameInt(run->script, "maxseconds", run->maxSeconds);
    run->started = std::chrono::steady_clock::now();
    autoplayLog(*run, state, "START " + run->script.path + " steps=" + std::to_string(run->script.steps.size())
        + " owner=" + state.economy.owner);
    gRuntimeAutoplay = std::move(run);
    return true;
}

// Called every pass of the window loop, after messages are drained. Returns
// false when the run is over and the window should close.
bool tickRuntimeAutoplay(RuntimeWindowState& state, HWND window) {
    if (!gRuntimeAutoplay) return true;
    RuntimeAutoplayRun& run = *gRuntimeAutoplay;
    if (run.done) return false;
    if (state.tick > run.maxTicks) {
        failRuntimeAutoplay(run, state, "MaxTicks reached");
        return false;
    }
    if (std::chrono::steady_clock::now() - run.started > std::chrono::seconds(run.maxSeconds)) {
        failRuntimeAutoplay(run, state, "MaxSeconds reached (real time)");
        return false;
    }
    if (state.skirmishMatchFinished && !run.matchOverLogged) {
        run.matchOverLogged = true;
        autoplayLog(run, state, "MATCH OVER: " + state.skirmishResult);
    }
    if (run.waitFrames > 0) { --run.waitFrames; return true; }
    if (run.step >= run.script.steps.size()) {
        finishRuntimeAutoplay(run, state);
        return false;
    }

    const RuntimeAutoplayStep& step = run.script.steps[run.step];
    if (run.stepStartTick < 0) run.stepStartTick = state.tick;
    const std::string verb = autoplayLower(step.verb);
    const auto& a = step.args;
    const RECT client = autoplayClient(window);
    const RECT viewport = runtimeMapViewport(client);
    const RECT sidebar = runtimeSidebarRect(client);
    std::string detail;
    int n = 0;

    // Moves the camera, then waits two passes so painting/projection catch up.
    const auto lookAt = [&](RuntimeCellPoint cell) {
        centerRuntimeCameraOnCell(state, client, cell);
        state.repaintRequested = true;
        run.waitFrames = 2;
    };
    // Clicks the sidebar button of a catalog item, switching tabs first.
    // Returns: 0 = clicked, 1 = waiting (tab switched), -1 = failed.
    const auto clickCatalogItem = [&](const RuntimeBuildCatalogItem& item, int index) -> int {
        if (state.activeBuildTab != item.kind) {
            autoplayLeftClick(window, autoplayCenter(runtimeBuildTabRect(sidebar, runtimeBuildTabIndex(item.kind))));
            run.waitFrames = 2;
            return 1;
        }
        const std::vector<int> visible = runtimeVisibleBuildCatalogIndices(state, sidebar);
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (visible[i] == index) {
                autoplayLeftClick(window, autoplayCenter(runtimeBuildButtonRect(sidebar, static_cast<int>(i))));
                return 0;
            }
        }
        detail = item.typeId + " is not on the visible sidebar page (" + std::to_string(visible.size()) + " visible)";
        return -1;
    };

    if (verb == "waitticks") {
        autoplayInt(a.empty() ? "0" : a[0], n);
        if (state.tick - run.stepStartTick >= n) nextRuntimeAutoplayStep(run, state, "");
        else if (state.skirmishMatchFinished) nextRuntimeAutoplayStep(run, state, "match over, ticks stopped");
        return true;
    }
    if (verb == "waitframes") {
        autoplayInt(a.empty() ? "0" : a[0], n);
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = n;
        return true;
    }
    if (verb == "waituntil" || verb == "expect") {
        std::vector<std::string> words = a;
        int timeout = 0;
        if (verb == "waituntil" && !words.empty() && autoplayInt(words.back(), timeout)) words.pop_back();
        if (autoplayCondition(run, state, words, detail)) {
            nextRuntimeAutoplayStep(run, state, detail + " after " + std::to_string(state.tick - run.stepStartTick) + " ticks");
        } else if (verb == "expect" || state.tick - run.stepStartTick > timeout) {
            failRuntimeAutoplay(run, state, (verb == "expect" ? "expectation false: " : "timeout: ") + detail);
            return false;
        } else if (state.skirmishMatchFinished) {
            // Ticks have stopped, so the wait can't finish. A won match ends the
            // fight being waited on; a lost one is a failure.
            if (state.skirmishVictory) {
                nextRuntimeAutoplayStep(run, state, "match won while waiting (" + detail + ")");
                return true;
            }
            failRuntimeAutoplay(run, state, "match ended (" + state.skirmishResult + ") while waiting: " + detail);
            return false;
        }
        return true;
    }
    if (verb == "repeatuntil") {
        // RepeatUntil STEP MAXLOOPS CONDITION...: done when CONDITION holds,
        // otherwise jump back to step number STEP (at most MAXLOOPS times).
        int target = 0, maxLoops = 0;
        if (a.size() < 3 || !autoplayInt(a[0], target) || !autoplayInt(a[1], maxLoops)) {
            failRuntimeAutoplay(run, state, "RepeatUntil STEP MAXLOOPS CONDITION...");
            return false;
        }
        const std::vector<std::string> words(a.begin() + 2, a.end());
        if (autoplayCondition(run, state, words, detail)) {
            nextRuntimeAutoplayStep(run, state, detail + " after " + std::to_string(run.loops[run.step]) + " loop(s)");
            return true;
        }
        std::size_t jump = run.script.steps.size();
        for (std::size_t i = 0; i < run.step; ++i)
            if (run.script.steps[i].number == target) { jump = i; break; }
        if (jump >= run.step) { failRuntimeAutoplay(run, state, "loop target step " + a[0] + " is not an earlier step"); return false; }
        int& taken = run.loops[run.step];
        if (taken >= maxLoops) { failRuntimeAutoplay(run, state, "still false after " + std::to_string(taken) + " loops: " + detail); return false; }
        ++taken;
        autoplayLog(run, state, "LOOP " + std::to_string(step.number) + " -> step " + a[0] + " (" + std::to_string(taken) + "/" + a[1] + "): " + detail);
        run.passedSteps -= static_cast<int>(run.step - jump);   // those steps run again
        run.step = jump;
        run.phase = 0;
        run.stepStartTick = -1;
        return true;
    }
    if (verb == "clicktab") {
        const std::string tab = autoplayLower(a.empty() ? "" : a[0]);
        const RuntimeBuildKind kind = tab == "defense" ? RuntimeBuildKind::Defense
            : tab == "infantry" ? RuntimeBuildKind::Infantry
            : tab == "unit" || tab == "units" ? RuntimeBuildKind::Unit : RuntimeBuildKind::Building;
        autoplayLeftClick(window, autoplayCenter(runtimeBuildTabRect(sidebar, runtimeBuildTabIndex(kind))));
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "clicksidebar") {
        int index = -1;
        const RuntimeBuildCatalogItem* item = autoplayCatalogItem(state, a.empty() ? "" : a[0], &index);
        if (!item) { failRuntimeAutoplay(run, state, "not in build catalog"); return false; }
        const int result = clickCatalogItem(*item, index);
        if (result < 0) { failRuntimeAutoplay(run, state, detail); return false; }
        if (result == 0) { nextRuntimeAutoplayStep(run, state, "clicked " + item->typeId); run.waitFrames = 2; }
        return true;
    }
    if (verb == "placebuilding") {
        int index = -1;
        const RuntimeBuildCatalogItem* item = autoplayCatalogItem(state, a.empty() ? "" : a[0], &index);
        if (!item) { failRuntimeAutoplay(run, state, "not in build catalog"); return false; }
        if (run.phase == 0) {                              // enter placement mode
            if (!runtimeSelectedPlacement(state).has_value()) {
                const int result = clickCatalogItem(*item, index);
                if (result < 0) { failRuntimeAutoplay(run, state, detail); return false; }
                if (result == 1) return true;
                run.waitFrames = 2;
            }
            run.phase = 1;
            return true;
        }
        if (!runtimeSelectedPlacement(state).has_value()) {
            failRuntimeAutoplay(run, state, "placement mode did not start (is it built?)");
            return false;
        }
        std::optional<RuntimeCellPoint> cell;
        int x = 0, y = 0;
        if (a.size() >= 3 && autoplayInt(a[1], x) && autoplayInt(a[2], y)) {
            // The nearest legal cell to X,Y (by the game's own placement rules).
            const auto occupied = buildRuntimeOccupancyLookup(state);
            const auto rubble = runtimeBuildingRubblePlacementCells(state);
            for (int radius = 0; radius <= 10 && !cell; ++radius)
                for (int dy = -radius; dy <= radius && !cell; ++dy)
                    for (int dx = -radius; dx <= radius && !cell; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                        const RuntimeCellPoint candidate{x + dx, y + dy};
                        if (runtimeCellCanPlaceSidebarBuilding(state, *item, candidate, &occupied, {}, &rubble)) cell = candidate;
                    }
            if (cell && run.phase == 1 && (cell->x != x || cell->y != y))
                autoplayLog(run, state, "     " + std::to_string(x) + "," + std::to_string(y) + " not legal; using "
                    + std::to_string(cell->x) + "," + std::to_string(cell->y));
        } else cell = findRuntimePlacementCellNearBase(state, *item);
        if (!cell) { failRuntimeAutoplay(run, state, "no legal placement cell near base"); return false; }
        if (run.phase == 1) { lookAt(*cell); run.phase = 2; return true; }
        // Find a screen point whose placement preview lands exactly on `cell`.
        const RuntimeStructureFootprint footprint = runtimeStructureFootprintForBuildItem(state, *item);
        const POINT guess = autoplayCellEstimate(state, viewport, *cell);
        std::optional<POINT> hit;
        for (int radius = 0; radius <= 120 && !hit; radius += 3)
            for (int dy = -radius; dy <= radius && !hit; dy += 3)
                for (int dx = -radius; dx <= radius && !hit; dx += 3) {
                    if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                    const POINT p{guess.x + dx, guess.y + dy};
                    if (p.x <= viewport.left || p.x >= viewport.right || p.y <= viewport.top || p.y >= viewport.bottom) continue;
                    const POINT target = approximateCellFromScreenPoint(state, viewport, p);
                    const RuntimeCellPoint topLeft = runtimePlacementTopLeftForPreviewTarget({target.x, target.y}, footprint);
                    if (topLeft.x == cell->x && topLeft.y == cell->y) hit = p;
                }
        if (!hit) { failRuntimeAutoplay(run, state, "no screen point maps to cell"); return false; }
        if (run.phase == 2) { autoplayMove(window, *hit); run.waitFrames = 1; run.phase = 3; return true; }
        autoplayLeftClick(window, *hit);
        nextRuntimeAutoplayStep(run, state, "clicked map at " + std::to_string(hit->x) + "," + std::to_string(hit->y)
            + " for cell " + std::to_string(cell->x) + "," + std::to_string(cell->y));
        run.waitFrames = 2;
        return true;
    }
    if (verb == "selectunits") {
        // SelectUnits [All|Combat|TYPE]: drag a box around my mobile units
        // (Combat = without harvesters and MCVs).
        const std::string type = a.empty() ? "" : autoplayLower(a[0]);
        std::vector<const WorldObjectInstance*> units;
        for (const auto& object : state.world.getObjects())
            if (object.alive && autoplayIsMine(state, object) && autoplayIsMobile(object)
                && (type.empty() || type == "all" || (type == "combat" && !autoplayIsSupportUnit(object))
                    || autoplayLower(object.type) == type))
                units.push_back(&object);
        if (units.empty()) { failRuntimeAutoplay(run, state, "no units of mine to select"); return false; }
        if (run.phase == 0) {
            long sx = 0, sy = 0;
            for (const auto* u : units) { sx += u->x; sy += u->y; }
            lookAt({static_cast<int>(sx / static_cast<long>(units.size())), static_cast<int>(sy / static_cast<long>(units.size()))});
            run.phase = 1;
            return true;
        }
        RECT box{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        int onScreen = 0;
        for (const auto* u : units) {
            const POINT p = runtimeOverlayCenterForObject(state, *u, viewport);
            if (p.x <= viewport.left || p.x >= viewport.right || p.y <= viewport.top || p.y >= viewport.bottom) continue;
            box.left = std::min(box.left, p.x); box.top = std::min(box.top, p.y);
            box.right = std::max(box.right, p.x); box.bottom = std::max(box.bottom, p.y);
            ++onScreen;
        }
        if (onScreen == 0) { failRuntimeAutoplay(run, state, "units not on screen"); return false; }
        const POINT from{std::max(viewport.left + 2, box.left - 24), std::max(viewport.top + 2, box.top - 24)};
        const POINT to{std::min(viewport.right - 2, box.right + 24), std::min(viewport.bottom - 2, box.bottom + 24)};
        autoplayDrag(window, from, to);
        nextRuntimeAutoplayStep(run, state, std::to_string(onScreen) + " units in box");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "deploymcv") {
        // Click the MCV to select it, then click it again to deploy (RA2 controls).
        const WorldObjectInstance* mcv = nullptr;
        for (const auto& o : state.world.getObjects()) {
            const std::string t = autoplayLower(o.type);
            if (o.alive && autoplayIsMine(state, o) && autoplayIsMobile(o)
                && t.size() >= 3 && t.compare(t.size() - 3, 3, "mcv") == 0) { mcv = &o; break; }
        }
        if (!mcv) {
            if (run.phase > 0 || autoplayCountMine(state, "", true) > 0) {
                nextRuntimeAutoplayStep(run, state, "MCV deployed");
                return true;
            }
            failRuntimeAutoplay(run, state, "no MCV of mine");
            return false;
        }
        if (run.phase == 0) { lookAt({mcv->x, mcv->y}); run.phase = 1; return true; }
        const POINT p = runtimeOverlayCenterForObject(state, *mcv, viewport);
        if (run.phase == 1) { autoplayLeftClick(window, p); run.waitFrames = 3; run.phase = 2; return true; }
        autoplayLeftClick(window, p);
        nextRuntimeAutoplayStep(run, state, "clicked MCV twice at " + std::to_string(p.x) + "," + std::to_string(p.y));
        run.waitFrames = 2;
        return true;
    }
    if (verb == "moveto" || verb == "rightmoveto") {
        int x = 0, y = 0;
        if (!a.empty() && autoplayLower(a[0]) == "enemybase") {
            // A cell `distance` cells short of the nearest enemy structure (or
            // any enemy object), on the side facing my selection.
            int distance = 6;
            if (a.size() >= 2) autoplayInt(a[1], distance);
            long sx = 0, sy = 0; int count = 0;
            for (const int id : state.selectedObjectIds)
                if (const auto* o = autoplayObjectById(state, id)) { sx += o->x; sy += o->y; ++count; }
            const double cx = count ? double(sx) / count : 0.0, cy = count ? double(sy) / count : 0.0;
            const WorldObjectInstance* best = nullptr;
            double bestD = 1e18;
            for (int pass = 0; pass < 2 && !best; ++pass)
                for (const auto& o : state.world.getObjects()) {
                    if (!o.alive || o.rubble || !autoplayIsEnemy(state, o)) continue;
                    if (pass == 0 && o.kind != WorldObjectKind::Structure) continue;
                    const double d = (o.x - cx) * (o.x - cx) + (o.y - cy) * (o.y - cy);
                    if (d < bestD) { bestD = d; best = &o; }
                }
            if (!best) { failRuntimeAutoplay(run, state, "no enemy found"); return false; }
            const double len = std::max(1.0, std::sqrt(bestD));
            x = best->x + static_cast<int>(std::lround((cx - best->x) / len * distance));
            y = best->y + static_cast<int>(std::lround((cy - best->y) / len * distance));
        } else if (a.size() < 2 || !autoplayInt(a[0], x) || !autoplayInt(a[1], y)) {
            failRuntimeAutoplay(run, state, "MoveTo X Y or MoveTo EnemyBase [distance]");
            return false;
        }
        if (run.phase == 0) {
            autoplayLog(run, state, "     move target cell " + std::to_string(x) + "," + std::to_string(y));
            lookAt({x, y});
            run.phase = 1;
            return true;
        }
        int error = 0;
        std::optional<POINT> p = autoplayScreenPointForCell(state, viewport, {x, y}, true, &error);
        if (p && error > 0)
            autoplayLog(run, state, "     exact cell not pickable; nearest pick is " + std::to_string(error) + " cell(s) off");
        if (!p) {
            const POINT guess = autoplayCellEstimate(state, viewport, {x, y});
            const POINT back = approximateCellFromScreenPoint(state, viewport, guess);
            std::ostringstream why;
            why << "cell " << x << "," << y << " not found on screen; estimate " << guess.x << "," << guess.y
                << " maps back to " << back.x << "," << back.y << "; viewport " << viewport.left << "," << viewport.top
                << "-" << viewport.right << "," << viewport.bottom;
            if (guess.x > viewport.left && guess.x < viewport.right && guess.y > viewport.top && guess.y < viewport.bottom) {
                autoplayLog(run, state, "     " + why.str() + "; clicking the estimate");
                p = guess;
            } else {
                failRuntimeAutoplay(run, state, why.str());
                return false;
            }
        }
        if (verb == "moveto") autoplayLeftClick(window, *p); else autoplayRightClick(window, *p);
        nextRuntimeAutoplayStep(run, state, "clicked " + std::to_string(p->x) + "," + std::to_string(p->y));
        run.waitFrames = 2;
        return true;
    }
    if (verb == "attacktarget") {
        // AttackTarget [Structure|Unit|Any|TYPE]: nearest enemy to my selection.
        if (run.phase == 0) {
            const std::string want = autoplayLower(a.empty() ? "any" : a[0]);
            long sx = 0, sy = 0; int count = 0;
            for (const int id : state.selectedObjectIds)
                if (const auto* o = autoplayObjectById(state, id)) { sx += o->x; sy += o->y; ++count; }
            const double cx = count ? double(sx) / count : 0.0, cy = count ? double(sy) / count : 0.0;
            const WorldObjectInstance* best = nullptr;
            double bestD = 1e18;
            for (const auto& o : state.world.getObjects()) {
                if (!o.alive || o.rubble || !autoplayIsEnemy(state, o)) continue;
                const bool structure = o.kind == WorldObjectKind::Structure;
                if (want == "structure" && !structure) continue;
                if (want == "unit" && structure) continue;
                if (want != "any" && want != "structure" && want != "unit" && autoplayLower(o.type) != want) continue;
                const double d = (o.x - cx) * (o.x - cx) + (o.y - cy) * (o.y - cy);
                if (d < bestD) { bestD = d; best = &o; }
            }
            if (!best) {
                if (run.targetId >= 0 && autoplayEnemiesLeft(state) == 0) {
                    // Everything died since the last attack (e.g. inside a loop).
                    nextRuntimeAutoplayStep(run, state, "no enemy left to attack");
                    return true;
                }
                failRuntimeAutoplay(run, state, "no enemy target found");
                return false;
            }
            run.targetId = best->instanceId;
            autoplayLog(run, state, "     target " + best->type + " #" + std::to_string(best->instanceId)
                + " owner=" + best->owner + " at " + std::to_string(best->x) + "," + std::to_string(best->y));
            lookAt({best->x, best->y});
            run.phase = 1;
            return true;
        }
        const WorldObjectInstance* target = autoplayObjectById(state, run.targetId);
        if (!target || !target->alive || target->rubble) {
            // Killed by auto-fire before the click: pick the next nearest enemy.
            autoplayLog(run, state, "     target died before the click; picking another");
            run.phase = 0;
            return true;
        }
        const POINT p = runtimeOverlayCenterForObject(state, *target, viewport);
        if (run.phase == 1) { autoplayMove(window, p); run.waitFrames = 1; run.phase = 2; return true; }
        autoplayLeftClick(window, p);
        nextRuntimeAutoplayStep(run, state, "clicked target at " + std::to_string(p.x) + "," + std::to_string(p.y));
        run.waitFrames = 2;
        return true;
    }
    if (verb == "profile") {
        // Profile N: average the frame cost parts over N loop passes.
        int frames = 60;
        if (!a.empty()) autoplayInt(a[0], frames);
        auto& p = run.profile;
        const auto now = std::chrono::steady_clock::now();
        if (run.phase == 0) {
            p = {};
            p.started = p.last = now;
            p.tick = state.tick;
            p.presented = state.d3d11Presenter ? state.d3d11Presenter->report().presented : 0;
            run.phase = 1;
            return true;
        }
        p.loopMs = std::max(p.loopMs, std::chrono::duration<double, std::milli>(now - p.last).count());
        p.last = now;
        p.paintMs += state.perf.lastPaintMs;
        p.mapMs += state.perf.lastMapDrawMs;
        p.overlayMs += state.perf.lastOverlayDrawMs;
        p.sidebarMs += state.perf.lastSidebarDrawMs;
        p.compileMs += state.mapComposition.native().report().compileMs;
        if (state.d3d11Presenter && state.d3d11Presenter->timingCount()) {
            const auto& t = state.d3d11Presenter->timings()[state.d3d11Presenter->timingCount() - 1];
            p.uploadMs += t.uploadCpuMs; p.presentMs += t.presentCpuMs; p.gpuMs += t.gpuValid ? t.gpuUploadMs : 0.0;
        }
        if (++p.samples < frames) return true;
        const double seconds = std::chrono::duration<double>(now - p.started).count();
        const auto presented = state.d3d11Presenter ? state.d3d11Presenter->report().presented - p.presented : 0;
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        out << "loop=" << p.samples / seconds << "/s frames=" << presented / seconds << "/s ticks=" << (state.tick - p.tick) / seconds
            << "/s worstLoopMs=" << p.loopMs << " | avg ms: paint=" << p.paintMs / p.samples << " mapPrep=" << p.mapMs / p.samples
            << " unitOverlay=" << p.overlayMs / p.samples << " sidebar=" << p.sidebarMs / p.samples
            << " overlayCompile=" << p.compileMs / p.samples << " upload=" << p.uploadMs / p.samples
            << " present=" << p.presentMs / p.samples << " gpu=" << p.gpuMs / p.samples;
        nextRuntimeAutoplayStep(run, state, out.str());
        return true;
    }
    if (verb == "hover") {
        // Hover X Y: move the mouse (no click) to window coordinates.
        int x = 0, y = 0;
        if (a.size() < 2 || !autoplayInt(a[0], x) || !autoplayInt(a[1], y)) { failRuntimeAutoplay(run, state, "Hover X Y"); return false; }
        autoplayMove(window, {x, y});
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "sethealth") {
        // SetHealth TYPE PERCENT: test-only, sets every object of mine of TYPE
        // to PERCENT of its strength (to look at damage states).
        int percent = 100;
        if (a.size() < 2 || !autoplayInt(a[1], percent)) { failRuntimeAutoplay(run, state, "SetHealth TYPE PERCENT"); return false; }
        const std::string type = autoplayLower(a[0]);
        int changed = 0;
        for (auto& o : state.world.accessObjects()) {
            if (!o.alive || !autoplayIsMine(state, o) || autoplayLower(o.type) != type) continue;
            o.health = std::max(1, runtimeMaxHealthForObject(state, o) * std::clamp(percent, 1, 100) / 100);
            ++changed;
        }
        if (!changed) { failRuntimeAutoplay(run, state, "no object of mine of type " + type); return false; }
        state.repaintRequested = true;
        nextRuntimeAutoplayStep(run, state, std::to_string(changed) + " set to " + std::to_string(percent) + "%");
        run.waitFrames = 3;
        return true;
    }
    if (verb == "cameraon") {
        // CameraOn TYPE [N]: centre the view on my N-th (1-based) object of TYPE.
        int want = 1;
        if (a.size() >= 2) autoplayInt(a[1], want);
        const std::string type = autoplayLower(a.empty() ? "" : a[0]);
        int seen = 0;
        for (const auto& o : state.world.getObjects()) {
            if (!o.alive || !autoplayIsMine(state, o) || autoplayLower(o.type) != type) continue;
            if (++seen < want) continue;
            lookAt({o.x, o.y});
            nextRuntimeAutoplayStep(run, state, "centred on " + o.type + " #" + std::to_string(o.instanceId)
                + " at " + std::to_string(o.x) + "," + std::to_string(o.y));
            run.waitFrames = 3;
            return true;
        }
        failRuntimeAutoplay(run, state, "no object of mine of type " + type + " (#" + std::to_string(want) + ")");
        return false;
    }
    if (verb == "camera") {
        int x = 0, y = 0;
        if (a.size() >= 2 && autoplayInt(a[0], x) && autoplayInt(a[1], y)) lookAt({x, y});
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "key") {
        const std::string key = autoplayLower(a.empty() ? "" : a[0]);
        UINT vk = 0;
        if (key == "esc" || key == "escape") vk = VK_ESCAPE;
        else if (key == "enter") vk = VK_RETURN;
        else if (key == "space") vk = VK_SPACE;
        else if (key.size() == 1) vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(key[0])));
        if (!vk) { failRuntimeAutoplay(run, state, "unknown key " + key); return false; }
        autoplayKey(window, vk);
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "click" || verb == "rightclick") {
        int x = 0, y = 0;
        if (a.size() < 2 || !autoplayInt(a[0], x) || !autoplayInt(a[1], y)) { failRuntimeAutoplay(run, state, "Click X Y"); return false; }
        if (verb == "click") autoplayLeftClick(window, {x, y}); else autoplayRightClick(window, {x, y});
        nextRuntimeAutoplayStep(run, state, "");
        run.waitFrames = 2;
        return true;
    }
    if (verb == "screenshot") {
        const std::string base = a.empty() ? "shot" + std::to_string(step.number) : a[0];
        if (run.phase == 0) {
            // At an HD render scale the window shows a downscaled copy; also save
            // the full-size back buffer of the next map frame as NAME_hd.bmp.
            if (concord::render::hd::hdRenderScale() > 1)
                concord::render::hd::requestHdCapture((run.outDir / (base + "_hd.bmp")).string());
            state.repaintRequested = true; run.waitFrames = 2; run.phase = 1; return true;
        }
        const std::string name = base + ".bmp";
        const bool ok = autoplayScreenshot(window, run.outDir / name);
        nextRuntimeAutoplayStep(run, state, (ok ? "saved " + name : "screenshot FAILED " + name)
            + (concord::render::hd::hdRenderScale() > 1 ? " (+ " + base + "_hd.bmp)" : ""));
        return true;
    }
    if (verb == "log") {
        nextRuntimeAutoplayStep(run, state, "");
        return true;
    }
    if (verb == "end") {
        run.step = run.script.steps.size();
        finishRuntimeAutoplay(run, state);
        return false;
    }
    failRuntimeAutoplay(run, state, "unknown action " + step.verb);
    return false;
}

// Exit code of a finished run (0 pass, 1 fail), or -1 when autoplay is off.
int runtimeAutoplayExitCode() {
    if (!gRuntimeAutoplay || !gRuntimeAutoplay->done) return -1;
    return gRuntimeAutoplay->failed ? 1 : 0;
}
