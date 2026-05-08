# Constructor-First Sliding Puzzle Solver

This project solves the partial-goal sliding puzzle format in `testdata/*.in`.
Only the center `(N - 2) x (N - 2)` region must match the target; the border is
free workspace.

The default pipeline is now constructor-first:

```text
paired seed
-> multi-seed monotone constructor race
-> elite checkpoint repair
-> sweep/backtrack compression
-> short local patch cleanup
-> atomic publish + checkpoint
```

The paired constructor is kept as a guaranteed valid fallback. For testcase 2-6,
the multi-seed constructor is the main improvement engine. Patch repair is only
bounded cleanup, so long runs do not get stuck spending minutes on tiny `20 -> 18`
endpoint repairs.

## Build

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic .\src\ara_solver.cpp -o .\build\ara_solver.exe
```

## Run

The defaults are already tuned for constructor-first solving:

```powershell
.\build\ara_solver.exe .\testdata\2.in `
  --time-ms 60000 `
  --out .\runs\solution_2.out `
  --meta .\runs\meta_2.json `
  --checkpoint .\runs\patch_2.bin
```

For testcase 6, give the constructor race a longer budget:

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --time-ms 600000 `
  --out .\runs\solution_6.out `
  --meta .\runs\meta_6.json `
  --checkpoint .\runs\patch_6.bin `
  --constructor-configs 2048 `
  --constructor-ratio 85 `
  --constructor-config-time-ms 0 `
  --constructor-repair-jobs 128 `
  --cleanup-ms 5000 `
  --patch-stats
```

Old large-option names such as `--large-constructor-configs` and
`--large-patch-cleanup-ms` still work as aliases.

## Resume

```powershell
.\build\ara_solver.exe .\testdata\6.in `
  --resume .\runs\patch_6.bin `
  --checkpoint .\runs\patch_6.bin `
  --out .\runs\solution_6.out `
  --meta .\runs\meta_6.json `
  --time-ms 600000
```

Old `PATCH01` checkpoints remain loadable. Missing newer constructor-first
metadata uses safe defaults.

## Validate

```powershell
.\build\ara_solver.exe .\testdata\6.in --validate-solution .\runs\solution_6.out
```

Expected output:

```text
valid length=...
```

## Main Options

- `--improvement constructor-first|none`: default `constructor-first`. The old
  value `patch` is accepted as an alias.
- `--constructor-engine monotone|v1|off`: default `monotone` for `N >= 6`.
- `--constructor-configs N`: number of multi-start configs. Defaults are `512`
  for medium boards, `768` for larger medium boards, and `2048` for `N >= 30`.
- `--constructor-ratio N`: percent of total time reserved for constructor race;
  default `80`, or `85` for `N >= 30`.
- `--constructor-seed N`: deterministic seed for config generation and
  near-best tie-breaking.
- `--constructor-threads N`: constructor worker threads; default is CPU cores
  minus one.
- `--constructor-config-time-ms N`: per-config cap. `0` means auto/no tiny cap
  for large boards.
- `--constructor-repair-jobs N`: elite checkpoint suffix repair attempts.
- `--cleanup-ms N`: max local cleanup time after constructor phases. Defaults to
  `10000` for testcase 2-5 and `5000` for testcase 6-style large boards.
- `--no-cleanup`: skip local patch cleanup completely.
- `--macro-strategy sweep|auto|transport|strip|all`: default `sweep`. Transport
  and strip are legacy/debug route builders.
- `--large-engine off|v1|band-v2`: default `off`. Old large-board engines remain
  selectable for A/B testing.
- `--patch-stats`: print cleanup batch/job diagnostics.
- `--constructive-mode greedy|paired|macro|strip`: seed constructor, default
  `paired`.
- `--self-test`: run built-in tests.

## Metadata

The `.json` meta file now reports constructor-first fields:

- `constructor_configs_attempted`
- `constructor_valid`
- `constructor_elites`
- `constructor_best_config`
- `constructor_checkpoints`
- `elite_repair_jobs`
- `elite_repair_valid`
- `cleanup_time_ms`
- `cleanup_commits`
- `best_source`

Legacy patch, macro, and large-engine counters are still emitted so old analysis
scripts do not break.

## Project Layout

- `src/`: solver source.
- `build/`: compiled binaries.
- `runs/`: active solutions, metadata, and checkpoints.
- `samples/solutions/`: preserved valid sample outputs.
- `samples/meta/`: preserved sample metadata.
- `docs/`: assignment PDFs and solver manual.
- `docs/THAI_CODE_WALKTHROUGH.md`: Thai code walkthrough for presentations.
- `testdata/`: input cases.
