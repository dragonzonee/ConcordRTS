# Autoplay test harness

Plays a real match from a script of player actions. Every action is sent to the
game window as real mouse/keyboard messages (`WM_LBUTTONDOWN`, `WM_MOUSEMOVE`,
`WM_KEYDOWN`, ...), so the sidebar, placement, selection and command code being
tested is exactly what a player uses. Screen positions come from the game's own
layout helpers (sidebar tab/button rectangles, cell and object projection).

```
bin\concord_runtime.exe --config local_data\runtime_config.ini --resource-root RedAlert2 ^
  --local-data-root local_data --autoplay tests\autoplay\build_and_attack.ini ^
  --autoplay-out build\autoplay\build_and_attack
```

Output folder: `autoplay.log` (every step with tick and details), `result.txt`
(`PASS`/`FAIL`, failing step and reason) and `*.bmp` screenshots.
Exit code: 0 = pass, 1 = fail.

## Script format

```ini
[Game]
Map=2peaks.map        ; bare name = multiplayer map from mix_exports/multimd
Country=Americans     ; your country
Enemy=Russians        ; or None
AI=no                 ; computer AI on/off
EnemyDifficulty=Easy
Credits=10000
Units=5               ; starting units
Speed=6               ; 0..6, 6 = fastest
Start=1               ; your start position
EnemyStart=2
MaxTicks=30000        ; safety stop (30 ticks = 1 second of game time)

[Steps]
1=DeployMCV
2=WaitUntil BaseReady 900
...
```

## Actions

| Action | What it does |
| --- | --- |
| `DeployMCV` | Clicks your MCV to select it, then clicks it again to deploy |
| `ClickTab Building\|Defense\|Infantry\|Unit` | Clicks a sidebar tab |
| `ClickSidebar TYPE` | Switches to the item's tab and clicks its build icon (e.g. `GAPOWR`, `E1`) |
| `PlaceBuilding TYPE [NearBase\|X Y]` | Enters placement for a finished building and clicks a legal map cell (the nearest legal one to X Y) |
| `SelectUnits [All\|Combat\|TYPE]` | Drags a selection box around your units (`Combat` skips harvesters and MCVs) |
| `MoveTo X Y` / `MoveTo EnemyBase [distance]` | Left-clicks the ground to move the selection |
| `RightMoveTo X Y` | Same with the right mouse button |
| `AttackTarget [Any\|Structure\|Unit\|TYPE]` | Clicks the enemy object nearest to the selection; remembered as the target |
| `Camera X Y` | Centres the view on a cell |
| `CameraOn TYPE [N]` | Centres the view on your N-th object of TYPE |
| `Hover X Y` | Moves the mouse to window coordinates without clicking |
| `SetHealth TYPE PERCENT` | Test-only: sets your objects of TYPE to PERCENT health (damage states) |
| `Profile N` | Logs loop/frame/tick rates and average ms per frame part over N frames |
| `Click X Y` / `RightClick X Y` | Raw click at window coordinates |
| `Key K` | Presses a key (`Esc`, `Enter`, `Space`, a letter) |
| `WaitTicks N` / `WaitFrames N` | Waits game ticks / loop passes |
| `WaitUntil CONDITION ... TIMEOUT` | Waits until true, fails after TIMEOUT ticks |
| `Expect CONDITION ...` | Fails now unless true |
| `RepeatUntil STEP MAXLOOPS CONDITION ...` | Moves on when true, otherwise jumps back to step number `STEP` (fails after `MAXLOOPS` jumps) |
| `Screenshot NAME` | Saves `NAME.bmp` |
| `End` | Stops with PASS |

## Conditions

`BaseReady`, `BuildingExists TYPE [N]`, `UnitExists TYPE [N]`, `BuildReady TYPE`,
`Placing`, `CreditsAtLeast N`, `Selected N`, `UnitsNear X Y [R] [N]`,
`TargetDestroyed`, `TargetDamaged HEALTH`, `EnemiesLeft N` (at most N enemy objects alive),
`MatchOver [Victory|Defeat]`, `HdCapture NAME` (the HD back-buffer image NAME was written).

`HdScale=2..4` in `[Game]` renders the map at that scale on the GPU (see
`src/render/hd/RuntimeHdSprites.h`); every `Screenshot NAME` then also saves
`NAME_hd.bmp`, the full-size frame.

Game ticks stop when the match ends, so once it is over `WaitTicks` finishes at
once and a pending `WaitUntil` passes on a victory and fails on a defeat.
`MaxSeconds=900` in `[Game]` is a real-time safety stop next to `MaxTicks`.

## Scripts

| Script | Covers |
| --- | --- |
| `build_and_attack.ini` | MCV deploy, Power Plant from the sidebar, select, move, attack one enemy |
| `build_base.ini` | Power Plant, Ore Refinery, Barracks, War Factory |
| `train_units.ini` | Base, then 3 GIs and 2 Grizzly tanks from the factories |
| `build_to_win.ini` | Base, 6 Grizzlies, march and attack until no enemy is left |
| `hd_power_plant.ini` | 3x HD render: two Power Plants drawn from their HD sprite pack |

Run them all: `powershell -File tools\autoplay\Run-Autoplay.ps1` (summary table,
exit code 1 if any fails). One script: `-Script build_base`.
