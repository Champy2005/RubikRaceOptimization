# Sliding Puzzle Solver Manual

This document explains how the current solver works, how to run it, what the
main options mean, and how to explain the algorithm in a video.

For a Thai code-by-code walkthrough, see `docs/THAI_CODE_WALKTHROUGH.md`.

The solver is an anytime heuristic solver for a partial-goal sliding puzzle.
Only the center `(N - 2) x (N - 2)` area must match the target. The border is
free workspace and can end in any arrangement unless the target explicitly
places the blank in the center.

The important design rule is:

```text
Always keep one valid solution.
Only publish a new solution if it fully validates and is shorter.
```

That is why the solver can be stopped or resumed safely.

## Quick Mental Model

The current solver is constructor-first, especially for large boards.

```text
paired fallback seed
-> constructor race / anytime greedy search
-> elite checkpoint repair
-> macro sweep compression
-> short patch cleanup
-> publish best valid solution
```

For small boards, it can use a weighted search to reach very short answers.
For medium boards, the multi-start constructor is the main route-changing
engine. For large boards such as testcase 6, the constructor race is now the
main optimization phase, while patching is only polish.

The big testcase 6 breakthrough came from this phase:

```text
[Large constructor improvement: best=422775 config=cfg186/seed5/o2/s3/p1]
```

That means a completely different valid route was generated. It was not a tiny
local patch.

## File Layout

```text
src/ara_solver.cpp       main solver source
build/ara_solver.exe     compiled executable
testdata/*.in            input testcases
runs/*.out               generated solution files
runs/*.json              metadata for runs
runs/*.bin               resumable checkpoints
docs/                    documentation
```

## Input And Output

Input format:

```text
N
N x N initial board values
(N - 2) x (N - 2) target center values
```

The blank is `-1` in input. Internally, some code stores the blank as a special
packed value, and the friend's reference code stores it as `0`.

Output format:

```text
sequence_of_moves ending_with_S
```

The move letters are blank moves:

```text
U means the blank swaps with the tile below it
D means the blank swaps with the tile above it
L means the blank swaps with the tile to its right
R means the blank swaps with the tile to its left
S ends the solution
```

Validation replays the whole move string from the original board and then checks
that every center cell matches the target.

## Recommended Commands

Build:

```powershell
g++ -std=c++17 -O2 -pipe -pthread .\src\ara_solver.cpp -o .\build\ara_solver.exe
```

Self-test:

```powershell
.\build\ara_solver.exe --self-test
```

Normal run:

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --time-ms 600000 `
  --out .\runs\solution_6.out `
  --meta .\runs\meta_6.json `
  --checkpoint .\runs\patch_6.bin `
  --constructive-mode paired `
  --constructor-configs 2048 `
  --constructor-ratio 85 `
  --constructor-config-time-ms 0 `
  --constructor-repair-jobs 128 `
  --cleanup-ms 5000 `
  --patch-stats
```

Resume:

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --resume .\runs\patch_6.bin `
  --checkpoint .\runs\patch_6.bin `
  --out .\runs\solution_6.out `
  --meta .\runs\meta_6.json `
  --time-ms 600000 `
  --patch-stats
```

Validate:

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --validate-solution .\runs\solution_6.out
```

Expected validation output:

```text
valid length=...
```

## Main Architecture

The solver is mostly contained in `src/ara_solver.cpp`.

Important components:

```text
Problem
  Stores board size, initial board, target cells, inner positions, hashes.

ConstructiveSolver
  Builds the guaranteed fallback seed. Default mode is paired.

AnytimeConstructiveSolver
  Runs the constructor race: many greedy configs, monotone tile movement,
  rollout, elite archive, and checkpoint repair.

PatchImprover
  Owns the best valid solution, checkpoint/resume, snapshots, macro passes,
  large-board scheduling, and patch cleanup.

SolverApp
  Parses mode and connects input, construction, improvement, output, and meta.
```

The central owner after the seed is `PatchImprover`. Even when the solver is not
mostly doing patches, this class manages publishing, validation, metadata, and
checkpointing.

## Phase 1: Paired Seed

The first real solution usually comes from:

```text
--constructive-mode paired
```

This constructor is deterministic and reliable. Its job is not to be optimal.
Its job is to make sure the solver always has a valid answer quickly.

Why this matters:

```text
If constructor race fails, output is still valid.
If macro fails, output is still valid.
If patch fails, output is still valid.
```

The paired seed is the fallback, not the final optimization strategy.

## Phase 2: Small-Board Search

For very small boards, especially `N == 5`, the solver can run a weighted search
and reach very short paths. This is why testcase 1 can reach length `30`.

This phase is now the main route-changing phase for testcase 2-6. On large
boards it receives most of the run budget before cleanup starts.

## Phase 3: Constructor Race

This is now the most important phase for large boards.

The idea is to generate many complete route structures instead of repairing one
bad route forever.

Each constructor config chooses:

```text
target order
source scoring weights
step scoring weights
candidate limit
rollout settings
random seed
max pass count
```

The solver tries many configs. Each successful config produces a complete valid
solution. If the new full route is shorter than the current best, it is
published.

### Target Orders

The constructor can fill target cells in different orders:

```text
serpentine rows top-down
serpentine rows bottom-up
serpentine columns left-right
serpentine columns right-left
outside-in rows
outside-in columns
diagonal order
center-out order
```

Serpentine orders are important because consecutive targets are spatially close,
so the blank and tile do not travel across the board as often.

### Source Tile Selection

For each target cell, the solver must choose a source tile with the required
value.

A bad solver only asks:

```text
Which matching tile is nearest to the target?
```

The current solver asks more:

```text
How far is the tile from the target through unlocked cells?
How hard is it for the blank to access/support that tile?
Is the tile already correctly placed somewhere useful?
Is the tile in the center or on the border workspace?
Does the chosen config prefer border sources or blank-accessible sources?
```

This is the same core idea that made the friend's solver strong:

```text
tile distance + blank support cost + penalties/bonuses
```

### Monotone Tile Movement

The constructor uses monotone movement for a controlled tile.

For each target:

1. Run BFS from the target through unlocked cells.
2. The tile may only move to a neighbor whose BFS distance is one smaller.
3. For each possible next tile step, route the blank to the support position.
4. Choose the best step by blank path length, center traffic, border preference,
   loop risk, and seeded noise.

In formula form:

```text
allowed next step:
  dist_to_target[next_tile_position] == dist_to_target[current_tile_position] - 1
```

This prevents giant wandering routes. The tile always makes progress toward the
target, even though the blank may move around to support it.

### Blank Routing

Before a tile can move one step, the blank must move to the tile's next
position. The blank path is found with BFS while avoiding:

```text
locked cells
the controlled tile
out-of-board positions
```

The solver scores blank paths by:

```text
blank path length
traffic through the center
whether it uses border workspace
small seeded randomness for diversity
```

### Move Cancellation

During construction, direct opposite moves are removed immediately:

```text
LR, RL, UD, DU
```

This keeps easy waste out of the route before patch cleanup even begins.

### Rollout

For each target, the solver does shallow lookahead:

```text
try top K source candidates
simulate placing them
estimate next few targets
choose the lowest estimated cost
```

Important options:

```text
--rollout-top-k
--rollout-depth
```

This is not exact search. It is cheap local simulation to avoid obviously bad
tile choices.

### Multi-Pass Solving

Large boards may not solve cleanly in one pass over the target order. The
constructor now supports multiple passes:

```text
pass 0: forward order
pass 1: reverse order
pass 2: forward order
...
```

Configs use pass counts such as:

```text
4, 6, 8, 10
```

This helps because a target that is hard early may become easy after nearby
targets are placed.

## Phase 4: Elite Archive And Repair

When a constructor produces a valid full solution, it can enter the elite
archive.

Each elite solution stores checkpoints at progress ratios:

```text
40%, 50%, 60%, 70%, 80%, 88%, 93%, 97%
```

Repair jobs restart from those checkpoints using modified configs:

```text
different target order
different source preference
larger candidate limits
slightly stronger randomness/rollout
```

This is powerful because it repairs suffixes from good route structures, not
from the paired fallback route.

For large boards, this is the intended flow:

```text
constructor produces full valid route
-> route enters elite archive
-> late checkpoints are used for suffix repair
-> full prefix + repaired suffix validates
-> publish if shorter
```

## Phase 5: Large-Board Scheduling

For `N >= 30`, the default behavior is constructor-first:

```text
paired seed
-> large constructor race
-> elite repair
-> macro/sweep compression
-> short patch cleanup
```

The paired seed is only the fallback. The main improvement should come from
large constructor commits like:

```text
[Large constructor improvement: best=422775 config=cfg186/seed5/o2/s3/p1]
```

That log means:

```text
old valid path was much longer
constructor config cfg186 generated a shorter full path
full replay validation passed
solver published it
```

## Phase 6: Macro/Sweep Compression

After constructor phases, the solver tries route compression.

Macro/sweep can remove:

```text
direct opposite moves
repeated full board states
safe bridgeable sweep segments
```

Every macro candidate is fully validated before publishing.

This phase can help, but it is not the main large-board engine.

## Phase 7: Patch Cleanup

Patch cleanup is the local optimizer. It looks for wasteful segments and tries
to replace them with shorter local repairs.

For a candidate segment:

```text
old route segment from move i to move j
start board at i
end board at j
small patch window around the affected area
try to find a shorter path from same start patch to same end patch
```

Accepted patches must satisfy:

```text
replacement is shorter
endpoint is exact
boundary is unchanged
full solution validates
```

Patch sizes:

```text
3x3 exact BFS
4x4 bidirectional exact BFS
5x5 relaxed blank endpoint + exact rejoin
```

For large boards, patch cleanup is intentionally short:

```text
--cleanup-ms 5000
```

And 5x5 patches are disabled during default large cleanup. This avoids spending
minutes on tiny `20 -> 18` improvements.

## Resume And Checkpoints

Checkpoint files store the current best valid solution and optimization state.

Use:

```powershell
--checkpoint .\runs\patch_6.bin
```

to save.

Use:

```powershell
--resume .\runs\patch_6.bin
```

to continue.

The checkpoint is only updated after validated improvements. If the program is
stopped, the latest checkpoint still contains a valid best solution.

## Metadata

Each run writes a JSON file, usually:

```text
runs/meta_6.json
```

Important fields:

```text
best_length
  Current best move count, not including the final S.

event
  What caused the latest publish: seed, large_constructor, patch, final, etc.

best_source
  Source of the current best route.

elapsed_ms
  Total runtime used in the current run.

exit_reason
  Why the solver stopped.
```

Constructor fields:

```text
large_constructor_configs_attempted
  How many large constructor configs were attempted.

large_constructor_valid
  How many full valid constructor solutions were generated.

large_constructor_elites
  How many constructor routes entered the elite archive.

large_constructor_best_config
  Config id that produced the best large constructor result.

large_constructor_checkpoints
  Checkpoints stored from valid elite constructor routes.

large_elite_repair_jobs
  Repair jobs attempted from elite checkpoints.

large_elite_repair_valid
  Valid full solutions produced by elite repair.
```

Patch fields:

```text
batches
batches_5x5
jobs_attempted
large_patch_cleanup_ms
large_patch_cleanup_commits
```

If `large_constructor_valid` is `0`, the large route breakthrough did not
happen in that run. If it is positive, the solver found at least one complete
alternative route.

## Important Options

### Basic options

```text
--time-ms N
  Total run time budget in milliseconds.

--out PATH
  Output solution file.

--meta PATH
  Output metadata JSON file.

--checkpoint PATH
  Save resumable checkpoint.

--resume PATH
  Resume from checkpoint.

--validate-solution PATH
  Validate an output file and exit.

--self-test
  Run built-in tests and exit.
```

### Construction options

```text
--constructive-mode paired
  Default reliable seed constructor.

--constructive-mode greedy
  Older greedy seed mode.

--no-constructive
  Do not build a seed. Only useful when resuming from checkpoint.
```

### Constructor-first options

```text
--constructor-engine monotone|v1|off
  monotone is the current main engine. Default is monotone for N >= 6.

--constructor-seed N
  Base seed for config generation and tie-breaking.

--constructor-configs N
  Config count for testcase 2-6. Defaults are 512/768/2048 by board size.

--constructor-threads N
  Constructor worker threads. Default is CPU cores minus one.

--constructor-ratio N
  Percent of total time reserved for constructor race.

--constructor-config-time-ms N
  Per-config cap. 0 means auto/no tiny cap for large boards.

--constructor-repair-jobs N
  Repair attempts from elite constructor checkpoints.

--rollout-top-k N
  Number of candidate sources simulated per target.

--rollout-depth N
  Number of future targets estimated in rollout.

--elite-size N
  Number of valid elite routes retained.

--cleanup-ms N
  Maximum local cleanup time after constructor phases.

--no-cleanup
  Disable local patch cleanup entirely.
```

### Legacy aliases

```text
--anytime-engine / --anytime-configs / --anytime-ratio / --anytime-seed
  Old names for constructor-first options.

--large-constructor-configs N
  Alias for --constructor-configs on large boards.

--large-constructor-ratio N
  Alias for --constructor-ratio.

--large-config-time-ms N
  Alias for --constructor-config-time-ms.

--large-repair-from-elites N
  Alias for --constructor-repair-jobs.

--large-patch-cleanup-ms N
  Alias for --cleanup-ms.

--no-large-patch-cleanup
  Alias for --no-cleanup.
```

### Older large engine options

```text
--large-engine band-v2
  Legacy band-beam engine for A/B comparison. Not used by default.

--large-engine v1
  Old large engine for A/B comparison.

--large-engine off
  Default. Constructor-first plus sweep cleanup is preferred.

--large-engine off
  Disable the old large engine path.

--large-band-size N
--large-beam-width N
--large-band-candidates N
--large-suffix-jobs N
  Older band-v2 tuning knobs.
```

### Patch options

```text
--patch-top-k N
  Number of high-scoring patch candidates per pass.

--patch-attempt-ms N
  Per-job local solver cap.

--patch-threads N
  Patch worker threads.

--patch-batch-size N
  Number of patch jobs per batch.

--patch-batch-timeslice-ms N
  Max time to wait for a patch batch.

--patch-commit-policy best-batch|first|deterministic
  How patch results are selected.

--patch-force-5x5-every N
  How often to force 5x5 patch attempts on non-large boards.

--patch-stats
  Print patch batch statistics.

--patch-max-commits N
  Stop after N accepted patch improvements.
```

### Macro options

```text
--macro-strategy auto|transport|strip|sweep|all
  Select macro strategy.

--macro-stall-ms N
  Retry macro after patch stall.

--macro-time-ratio N
  Percent of run budget allowed for macro pass.

--no-macro-resynthesis
  Disable macro passes.
```

## Suggested Recipes

### Testcase 1

```powershell
.\build\ara_solver.exe .\testdata\1.in `
  --time-ms 10000 `
  --out .\runs\solution_1.out `
  --meta .\runs\meta_1.json `
  --checkpoint .\runs\patch_1.bin `
  --constructive-mode paired
```

Expected: length around `30`.

### Testcase 2

```powershell
.\build\ara_solver.exe .\testdata\2.in `
  --time-ms 60000 `
  --out .\runs\solution_2.out `
  --meta .\runs\meta_2.json `
  --checkpoint .\runs\patch_2.bin `
  --constructive-mode paired
```

Expected: the monotone constructor should be the main improvement source.

### Testcase 6

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --time-ms 600000 `
  --out .\runs\solution_6.out `
  --meta .\runs\meta_6.json `
  --checkpoint .\runs\patch_6.bin `
  --constructive-mode paired `
  --constructor-configs 2048 `
  --constructor-ratio 85 `
  --constructor-config-time-ms 0 `
  --constructor-repair-jobs 128 `
  --cleanup-ms 5000 `
  --patch-stats
```

If you want more exploration:

```powershell
--constructor-configs 4096
--constructor-ratio 90
--constructor-seed 2
```

Try several seeds:

```text
--constructor-seed 1
--constructor-seed 2
--constructor-seed 3
--constructor-seed 7
--constructor-seed 11
```

## How To Explain It In A Video

Suggested video structure:

1. Explain the puzzle:
   only the center area must match, border is workspace.

2. Explain the safety invariant:
   the solver always keeps a valid best solution and only publishes validated
   shorter paths.

3. Explain the old problem:
   patching can only remove local waste, so large boards plateau with tiny
   improvements like `20 -> 18`.

4. Explain the constructor-first solution:
   instead of patching one route, generate many full route structures and keep
   the best.

5. Explain monotone tile movement:
   BFS distance to the target must decrease every tile step.

6. Explain source scoring:
   choose source tiles using both tile distance and blank accessibility.

7. Explain rollout:
   simulate a few candidate choices before committing.

8. Explain elite repair:
   save checkpoints from good full routes and repair suffixes.

9. Explain large testcase behavior:
   testcase 6 improved from around `796k` to `422775` because a large
   constructor config found a new full route.

10. Explain validation:
    every candidate is replayed from the original board before publishing.

Short narration:

```text
The main trick is that I stopped spending most of the time on local repair.
For large boards, local repair can only remove a few moves at a time. Instead,
the solver now runs many greedy constructors with different target orders,
weights, and seeds. Each constructor tries to build a full valid solution using
monotone tile movement, where each controlled tile must move closer to its
target according to BFS distance. Valid full solutions become elites, and late
checkpoints from those elites are used for repair. Patching is still there, but
only as a short cleanup phase.
```

## Troubleshooting

If output barely improves:

```text
Check constructor_valid.
```

If it is `0`, no alternative full route finished. Try:

```powershell
--constructor-configs 4096
--constructor-ratio 90
--constructor-config-time-ms 0
--constructor-seed 2
```

If patching dominates:

```powershell
--cleanup-ms 2000
```

or:

```powershell
--no-cleanup
```

If you want deterministic debugging:

```powershell
--patch-commit-policy deterministic
--patch-threads 1
--anytime-threads 1
```

If resume behaves oddly, validate the current output:

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --validate-solution .\runs\solution_6.out
```

## Current Strengths And Weaknesses

Strengths:

```text
valid fallback seed
safe checkpoint/resume
strong constructor-first large improvement
good testcase 2 behavior
short bounded patch cleanup
metadata makes plateaus visible
```

Weaknesses:

```text
large constructor success is still seed/config dependent
some options are experimental
old band-v2 path is less important after constructor-first scheduling
rank-1 quality still needs more tuning and more constructor diversity
```

## One-Sentence Summary

The solver is an anytime constructor-first sliding puzzle solver: it keeps a
valid fallback route, races many monotone greedy constructors to find better
full routes, repairs elites from checkpoints, and only uses local patches as a
short final cleanup.
