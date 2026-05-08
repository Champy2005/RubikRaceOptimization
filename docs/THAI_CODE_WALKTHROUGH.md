# คำอธิบายโค้ด Solver ภาษาไทย

เอกสารนี้ใช้ประกอบการอ่าน `src/ara_solver.cpp` และทำวิดีโอพรีเซนต์ได้โดยตรง จุดประสงค์คืออธิบายว่าแต่ละส่วนของโค้ดทำอะไร ทำไมต้องมี และสำคัญแค่ไหนกับ solver เวอร์ชันปัจจุบัน

ระดับความสำคัญที่ใช้ในเอกสาร:

- **KEY** = แกนหลักของ solver ตอนนี้ ควรอธิบายในวิดีโอ
- **SUPPORT** = โค้ดสนับสนุน เช่น validate, hash, checkpoint, metadata
- **CLEANUP** = ช่วยลด move หลังจากได้คำตอบหลักแล้ว แต่ไม่ใช่ตัวทำคะแนนหลัก
- **LEGACY** = โค้ดเก่า/ไว้เทียบผล/debug ไม่ใช่ default path หลัก
- **TEST** = โค้ดทดสอบ

หมายเหตุ: เลขบรรทัดอาจขยับได้ถ้าแก้ไฟล์ แต่ชื่อ function/class ใช้อ้างอิงได้เสถียรกว่า

---

## ภาพรวม Pipeline ปัจจุบัน

```text
อ่าน input
-> สร้าง Problem
-> apply_constructor_first_defaults()
-> paired seed constructor
-> ConstructorFirstSolver
   -> small-board search ถ้า N <= 5
   -> multi-seed monotone constructor ถ้า N >= 6
   -> elite checkpoint repair
   -> sweep/backtrack compression
   -> bounded local patch cleanup
-> validate
-> เขียน solution.out / meta.json / checkpoint
```

### สิ่งที่เป็นหัวใจจริง ๆ

1. **paired seed** สร้างคำตอบ valid เร็วเสมอ เพื่อไม่ให้ run ล้มเหลวเพราะยังไม่มีคำตอบ
2. **AnytimeConstructiveSolver แบบ monotone multi-seed** คือ engine หลักสำหรับ testcase 2-6
3. **elite checkpoint repair** เอาจุดกลางทางจากคำตอบที่ดีมาเริ่ม repair suffix ใหม่
4. **publish only if valid and shorter** ทุกคำตอบที่เขียนออกไฟล์ต้อง replay ผ่านเต็ม ๆ
5. **patch cleanup** ตอนนี้เป็น polish เท่านั้น ไม่ใช่ตัวหลักในการกระโดดคะแนน

### สิ่งที่ไม่ใช่หัวใจแล้ว

- macro transport / strip แบบเก่า
- large band-v2/v1 constructor แบบเก่า
- 5x5 patch batches ยาว ๆ
- patch-only plateau ที่ลดทีละ `20 -> 18`

---

## Utility พื้นฐานด้านเวลา, hash, packing

### `Timer` — SUPPORT

เก็บเวลาเริ่ม run และมี `elapsed_ms()` สำหรับถามว่าใช้เวลาไปกี่ millisecond แล้ว

ใช้แทบทุก phase เพื่อหยุดตาม `--time-ms`, per-config deadline, cleanup cap และ patch stall

### `Hash128`, `Hash128Hasher` — SUPPORT

เก็บ hash 128-bit เป็น `lo` และ `hi`

ใช้แทน board state hash ที่ชนกันยากกว่า 64-bit ธรรมดา เหมาะกับการเช็ค board / center hash / checkpoint ว่าตรง input เดิมไหม

### `splitmix64()` — SUPPORT

ฟังก์ชัน random hash แบบ deterministic

ใช้สร้าง seed, hash ของ cell, hash ของ job signature และ tie-break ต่าง ๆ ข้อดีคือเร็วและให้ค่ากระจายดี

### `value_key()`, `cell_hash()`, `xor_hash()` — SUPPORT

ใช้สร้าง hash ของค่า tile ในตำแหน่งหนึ่ง ๆ

แนวคิดคือ board hash = XOR ของ hash ทุก cell ถ้ามีการ swap tile กับ blank ก็ update hash ได้เร็ว โดยไม่ต้อง hash board ใหม่ทั้งกระดาน

### `pack_cell()` / `unpack_cell()` — SUPPORT

แปลงค่า cell ระหว่าง format ปกติและ format compact:

- input ใช้ `-1` แทน blank
- packed board ใช้ `uint16_t`
- blank ใน packed board เป็นค่าพิเศษ `kBlankCell`

ช่วยประหยัด memory และทำให้ local patch solver ทำงานเร็วขึ้น

---

## Options และ CLI

### `Options` — KEY/SUPPORT

รวมค่าจาก command line ทั้งหมด

กลุ่มสำคัญตอนนี้:

- `constructor_engine`, `constructor_configs`, `constructor_ratio`, `constructor_seed`
- `constructor_threads`, `constructor_config_time_ms`, `constructor_repair_jobs`
- `cleanup_ms`, `no_cleanup`
- `constructive_mode`
- `checkpoint_path`, `resume_path`, `output_path`, `meta_path`

กลุ่ม legacy/debug:

- `large_engine`, `large_*`
- `macro_strategy` นอกจาก `sweep`
- patch options ลึก ๆ เช่น `patch_force_5x5_every`

### `parse_options()` — SUPPORT

อ่าน argument จาก CLI แล้วเติมค่าใน `Options`

รายละเอียดสำคัญ:

- รองรับทั้ง `--flag value` และ `--flag=value`
- `--improvement patch` ยังรับได้ แต่ถูก map เป็น `constructor-first`
- option ชื่อเก่า เช่น `--large-constructor-configs` ยัง map เข้า constructor-first defaults
- validate ค่า enum เช่น `constructor-engine`, `macro-strategy`, `large-engine`

### `apply_constructor_first_defaults()` — KEY

เป็นจุดเปลี่ยน architecture สำคัญ

หลังอ่าน input แล้ว function นี้ดูขนาด board แล้วตั้ง default ให้เหมาะ:

- `N < 6`: ไม่เน้น constructor race
- `N >= 6`: เปิด monotone constructor
- `N >= 30`: ให้ constructor budget สูงขึ้น, repair jobs มากขึ้น, cleanup สั้นลง

มันยังทำหน้าที่เชื่อม option เก่ากับ option ใหม่:

- `constructor_configs` -> `anytime_configs`
- `constructor_ratio` -> `anytime_ratio`
- `constructor_repair_jobs` -> `repair_jobs` หรือ `large_repair_from_elites`
- `cleanup_ms` -> `large_patch_cleanup_ms` สำหรับ board ใหญ่

นี่คือ function ที่ทำให้ testcase 2-6 ใช้ multi-seed constructor เป็น default จริง ๆ

---

## Problem และการเดินบนกระดาน

### `Problem` — KEY/SUPPORT

เก็บข้อมูล puzzle:

- `n` ขนาดกระดาน
- `initial` board เริ่มต้น
- `target_at` target เฉพาะ center cells
- `inner_positions` list ของตำแหน่ง center
- `initial_blank`
- `initial_hash`

function ภายใน:

- `index(r,c)` แปลง row/col เป็น index เดียว
- `rc(pos)` แปลง index กลับเป็น row/col
- `manhattan(a,b)` ระยะ Manhattan
- `in_bounds(r,c)` เช็คว่าอยู่ในกระดาน
- `is_inner(pos)` เช็คว่าเป็น center target area ไหม
- `is_goal(board)` เช็คเฉพาะ center ว่าตรง target หรือยัง
- `compute_hash(board)` hash ทั้ง board
- `read(in)` อ่าน input format

### `atomic_write()` — SUPPORT

เขียนไฟล์แบบปลอดภัย:

1. เขียนลง `path.tmp`
2. ลบไฟล์เก่า
3. rename tmp เป็นไฟล์จริง

เหตุผลคือถ้าโปรแกรมโดนหยุดกลางทาง จะลดโอกาสที่ output/meta เสียครึ่งไฟล์

### `valid_command_for_blank()` — SUPPORT

รับตำแหน่ง blank และ move `U/D/L/R`

คำนวณว่า blank จะไปช่องไหน ถ้าออกนอกกระดานให้ return false

สำคัญเพราะ move ใน output หมายถึง “blank move” ไม่ใช่ tile move

### `apply_move()` — SUPPORT

มี 2 overload:

- board แบบ `vector<int>`
- board แบบ `PackedBoard`

ทำงานเหมือนกัน:

1. เช็ค move ว่า blank เดินได้
2. swap blank กับ tile ข้าง ๆ
3. update blank position
4. ถ้ามี hash pointer ก็ update hash ไปพร้อมกัน

### `command_for_blank_step()` — SUPPORT

แปลงการเดินจากช่องหนึ่งไปช่องติดกันเป็น command ของ blank

เช่น blank จาก `from` ไป `to` ถ้า `to` อยู่ด้านบน จะได้ `U`

### `command_for_tile_step()` — SUPPORT

แปลงทิศการขยับ tile เป็น command ที่ต้องสั่ง blank

ใช้ตอนควบคุม tile ให้เดินเข้าหา target เพราะถ้า tile จะขยับไปทางหนึ่ง blank ต้องยืนอยู่ฝั่งตรงข้ามก่อน

### `inverse_command()` — SUPPORT/CLEANUP

คืนคำสั่งตรงข้าม เช่น `U <-> D`, `L <-> R`

ใช้ลบ backtrack ง่าย ๆ เช่น `LR`, `RL`, `UD`, `DU`

### `validate_solution()` — KEY/SUPPORT

replay move string ทั้งหมดจาก initial board

ถ้าเจอ `S` จะหยุด แล้วเช็คว่า center ตรง target ไหม

นี่คือ safety gate สำคัญที่สุด ทุก candidate ที่จะ publish ต้องผ่าน function นี้

---

## `ConstructiveSolver` seed constructor

สถานะ: **KEY สำหรับ fallback**, แต่ไม่ใช่ engine ทำคะแนนหลักแล้ว

หน้าที่คือสร้างคำตอบ valid เริ่มต้นเร็ว ๆ โดยเฉพาะ mode `paired`

### `solve()` — KEY

เลือกว่าจะใช้ constructor mode ไหน:

- `paired` default seed
- `greedy` เก่า
- `macro`, `strip`, `order-*` legacy/experimental

ถ้า solve สำเร็จจะได้ move string ที่ validate ได้

### `bfs_path()` — SUPPORT

หาเส้นทางสั้นสุดของ blank จากจุดหนึ่งไปอีกจุด โดยหลบ locked cells และ tile ที่กำลังควบคุม

เป็น BFS บน grid ธรรมดา

### `move_blank_to()` — SUPPORT

เรียก `bfs_path()` แล้ว apply move ทีละตัว เพื่อย้าย blank ไปตำแหน่ง support ที่ต้องการ

### `find_source()` — SUPPORT/LEGACY

หา tile ที่มีค่าตรงกับ target value

เวอร์ชัน seed นี้ค่อนข้างง่ายกว่า scoring ของ monotone constructor

### `transport_tile()` — SUPPORT

ย้าย tile จาก source ไป target โดย:

1. หา step ที่ทำให้ tile เข้าใกล้ target
2. ย้าย blank ไป support cell
3. swap เพื่อดัน tile
4. ทำซ้ำจน tile ถึง target

### `transport_tile_macro()` — LEGACY

เวอร์ชัน route-changing เก่า ใช้กับ macro constructor

ตอนนี้ไม่ใช่ default ที่สำคัญ เพราะ monotone multi-seed ทำหน้าที่นี้ดีกว่า

### `solve_row_suffix()`, `finish_remaining_center()` — SUPPORT/LEGACY

ช่วยจบเคสท้าย ๆ ตอนเหลือ target ไม่มาก

### `solve_pending_targets()` — SUPPORT

รับ list target ที่ยังไม่เสร็จ แล้วค่อย ๆ solve ทีละช่อง

### `make_strip_bands()`, `solve_band_targets()`, `solve_strip()` — LEGACY

แนวคิด band/strip constructor เก่า

เก็บไว้เพื่อ A/B หรือ debug แต่ไม่ใช่เส้นหลัก

### `solve_dynamic_order()` — LEGACY

ลอง target order แบบ dynamic

ปัจจุบันแนวคิด order หลายแบบถูกทำดีกว่าใน `AnytimeConstructiveSolver::make_configs()`

---

## `AnytimeConstructiveSolver` multi-seed constructor

สถานะ: **KEY ที่สุดสำหรับ testcase 2-6**

นี่คือ engine ที่ทำให้ solver กระโดดจากคำตอบแย่ ๆ ไปคำตอบดีขึ้นมาก โดยสร้าง route ใหม่ทั้งเส้น ไม่ใช่ซ่อม local endpoint

### Struct สำคัญข้างใน

#### `Stats` — KEY/SUPPORT

เก็บตัวเลขว่า constructor ทำอะไรไปบ้าง:

- ลองกี่ config
- ได้ valid solution กี่อัน
- elite update กี่ครั้ง
- repair jobs กี่งาน
- best config คือ config ไหน

ใช้ทั้งสำหรับ metadata และ debug performance

#### `Result` — KEY

ผลลัพธ์ของ constructor race:

- `best_path`
- `stats`

#### `PartialCheckpoint` — KEY

snapshot กลางทางจาก route ที่ valid ในท้ายที่สุด

เก็บ:

- board ตอนนั้น
- blank
- locked mask
- prefix moves
- fixed target count

ใช้ทำ suffix repair

#### `ExternalCheckpoint` — LEGACY/ADVANCED

checkpoint จากระบบ large band-v2 เก่า เอามา feed เข้า suffix repair ได้

ตอนนี้ไม่ใช่ default path หลัก

#### `Config` — KEY

ตัวแทน “บุคลิก” ของ constructor หนึ่งตัว

ค่าข้างในบอก:

- target order แบบไหน
- source preference แบบไหน
- step policy แบบไหน
- candidate limit
- weight scoring
- max passes
- seed
- id สำหรับ log

การลองหลาย config คือหัวใจของ multi-start

#### `RunState` — KEY

สถานะปัจจุบันของ constructor ระหว่างสร้าง solution:

- board
- blank
- locked cells
- moves
- fixed_count

ทุก config จะมี state ของตัวเอง ไม่ยุ่งกับ global best โดยตรง

#### `SourceCandidate` — KEY

candidate tile ที่มีค่าเดียวกับ target พร้อมคะแนน

คะแนนต่ำกว่าคือดีกว่า

#### `EliteSolution` — KEY

คำตอบ valid ที่ดี เก็บพร้อม config และ checkpoints

ใช้เลือกคำตอบที่สั้นที่สุด และใช้ทำ repair จากจุดกลางทาง

### `run()` — KEY

ตัว orchestrator ของ constructor race

ลำดับทำงาน:

1. สร้าง configs ด้วย `make_configs()`
2. ตั้ง per-config deadline
3. สร้าง worker threads
4. แต่ละ worker หยิบ config ไป solve
5. ถ้า path validate ผ่าน เก็บเป็น record
6. sort records แบบ deterministic
7. อัปเดต elite archive
8. ถ้าดีกว่า incumbent เก็บเป็น best
9. เอา checkpoints จาก elite ไปลอง repair suffix

ข้อสำคัญ: worker ไม่ publish เอง main reduction เป็นคนเลือก best ทีหลัง ทำให้ปลอดภัยกว่า

### `run_from_external_checkpoints()` — LEGACY/ADVANCED

ใช้ checkpoint จาก large band-v2 เก่ามาทำ suffix repair

ไม่ใช่ default หลัก แต่เก็บไว้เพราะเป็นเครื่องมือทดลองได้

### `make_configs()` — KEY

สร้าง config จำนวนมากจาก:

- order type หลายแบบ
- source preference หลายแบบ
- step policy หลายแบบ
- weight preset หลายชุด
- seed หลายค่า

default ปัจจุบัน:

- medium: 512 configs
- larger medium: 768 configs
- large: 2048 configs

นี่คือเหตุผลที่ testcase 2 ดีขึ้นเร็ว เพราะไม่ได้พึ่ง route เดียว

### `target_order()` — KEY

สร้างลำดับ target cells ตาม config:

- row order
- reverse row
- column
- reverse column
- serpentine
- center-out
- diagonal
- random/block shuffle

ผลของ order สำคัญมาก เพราะ sliding puzzle lock ผิดลำดับแล้ว route จะยาวทันที

### `bfs_path()` — SUPPORT

หา blank path ใน state ปัจจุบัน โดยหลบ locked cells และ controlled tile

นี่คือกลไกให้ tile movement ปลอดภัย ไม่ทำลาย target ที่ lock แล้ว

### `append_applied_move()` — SUPPORT/KEY

apply move ลง `RunState` และ append move เข้า path

ยังมี logic ลบ immediate opposite moves เช่น `LR`, `RL`, `UD`, `DU` เพื่อไม่ให้ route มี backtrack โง่ ๆ

### `move_blank_to()` — KEY/SUPPORT

พา blank ไป support cell ที่ต้องใช้ดัน tile

ถ้า blank ไปไม่ได้เพราะติด locked cells หรือ controlled tile จะ return false และ config นั้นต้องเลือกทางอื่นหรือ fail

### `build_dist_to_target()` — KEY

BFS จาก target ออกไปทั่ว board ผ่านช่องที่ไม่ locked

ได้ distance map ว่าแต่ละช่องห่างจาก target กี่ step

Monotone rule ใช้ map นี้บังคับว่า tile ต้องขยับไปช่องที่ distance ลดลงเท่านั้น

### `build_blank_dist()` — SUPPORT

BFS สำหรับ blank เพื่อประเมินว่า blank ไป support cell ได้ไกลแค่ไหน

ใช้ใน source scoring และ step scoring

### `blank_path_to()` — SUPPORT

reconstruct path ของ blank จาก BFS parent array

### `center_traffic()` — KEY/SUPPORT

วัดว่า path ของ blank ผ่าน center เยอะแค่ไหน

ถ้าผ่าน center มาก จะเสี่ยงไปรบกวน target cells จึงมี penalty ใน scoring

### `source_score()` — KEY

ให้คะแนน source tile ที่มีค่าตรงกับ target

score คิดจาก:

- ระยะ tile ไป target
- ระยะ blank ไป support cell
- ความเสี่ยงชน locked cells
- traffic ใน center
- source นั้นอยู่ในตำแหน่งที่ดีอยู่แล้วหรือเปล่า
- border/workspace access
- scarcity ของค่านั้น

นี่คือจุดที่ทำให้ solver “เลือก tile ฉลาดกว่า greedy ธรรมดา”

### `collect_sources()` — KEY

รวบรวม tile ทั้งหมดที่ value ตรง target แล้วใช้ `source_score()` จัดอันดับ

เก็บเฉพาะ near-best candidates เพื่อให้ rollout ไม่ระเบิด

### `future_score()` — KEY

ประเมินคร่าว ๆ ว่า state หลังเลือก candidate นี้จะทำให้ target ถัด ๆ ไปง่ายหรือยาก

ไม่ใช่ search ลึกเต็ม board แต่เป็น shallow rollout/heuristic lookahead

### `transport_tile_shortest()` — SUPPORT/LEGACY

ย้าย tile แบบ shortest/fallback

ใช้เมื่อ monotone policy ทำต่อไม่ได้

### `transport_tile_monotone()` — KEY

หัวใจของ constructor

หลักการ:

1. สร้าง BFS distance จาก target
2. tile ปัจจุบันอยู่ที่ `cur`
3. พิจารณา neighbor ที่ `dist[next] == dist[cur] - 1`
4. ให้ blank ไป support cell ที่ทำให้ดัน tile ไป `next`
5. เลือก step จาก cost เช่น blank path, center traffic, corridor preference, loop risk
6. ทำซ้ำจน tile ถึง target

นี่ทำให้ tile เคลื่อนที่แบบมีทิศทาง ไม่เดินวนง่ายเหมือน sweep เก่า

### `transport_tile()` — KEY

wrapper ที่เลือกว่าจะใช้ monotone หรือ fallback

ใน engine ปัจจุบันใช้ monotone เป็นหลัก

### `choose_source_with_rollout()` — KEY

เลือก source tile โดย:

1. collect near-best sources
2. simulate top-k candidates
3. ดู future score ตาม rollout depth
4. tie-break ด้วย seeded randomness

นี่คือส่วนที่คล้ายไอเดีย rank-2 friend solver มากที่สุด แต่เขียนใน style ของโปรเจกต์เราเอง

### `solve_config()` — KEY

ลอง solve ทั้ง puzzle ด้วย config เดียว

ลำดับ:

1. เริ่มจาก initial หรือ checkpoint
2. สร้าง target order
3. วนหลาย pass
4. เลือก target ที่ยังไม่ถูก
5. เลือก source ด้วย rollout
6. transport tile
7. lock target เมื่อถูกจริง
8. save partial checkpoints ตาม progress ratios
9. ถ้า center ตรง target ทั้งหมด return path

ถ้า config นี้แก้ไม่ได้ในเวลาหรือไปติด lock ก็ fail โดยไม่ทำลาย global best

### `add_elite()` — KEY

เพิ่ม valid solution เข้า elite archive ถ้าดีพอ

sort ตาม path length แล้วตัดให้เหลือ `elite_size`

elite archive คือฐานสำหรับ repair jobs

---

## `ConstructorFirstSolver` production controller

สถานะ: **KEY**

คลาสนี้คือ top-level pipeline ในทางปฏิบัติ ถึงแม้ข้างในยังเก็บ patch/macro legacy methods อยู่

### Struct ภายใน

#### `MoveInfo` — CLEANUP/SUPPORT

ข้อมูลของ move แต่ละตัวใน trajectory:

- move
- blank before/after
- touched cells
- moved tile
- local hash

ใช้ scoring patch segment

#### `TrajectorySnapshot` — CLEANUP/SUPPORT

เก็บ board เต็มทุก `snapshot_stride` moves

ทำให้ reconstruct board ที่ move index ใด ๆ ได้เร็วกว่า replay จากต้นทุกครั้ง

#### `Candidate` — CLEANUP

segment ที่น่าสงสัยว่าสั้นลงได้:

- start/end move index
- window position
- patch size
- score

#### `PatchJob` — CLEANUP

งานซ่อม local patch ที่ส่งให้ worker:

- patch board ตอน start/end
- boundary hash
- lower bound
- signature กันงานซ้ำ
- old length

#### `PatchResult` — CLEANUP

ผลจาก patch worker:

- success/fail
- replacement path
- delta ว่าสั้นลงกี่ move

### `load_checkpoint()` — SUPPORT

โหลด checkpoint format `PATCH01`

ถึงชื่อยังเป็น PATCH01 แต่ตอนนี้ใช้กับ constructor-first solver ด้วย เพื่อ backward compatibility

ขั้นตอน:

1. อ่าน magic/version
2. เช็คว่า input hash ตรงกับ checkpoint
3. โหลด best path
4. โหลด cursor/stats ถ้ามี
5. ถ้า field ใหม่ไม่มี ให้ default เป็น 0
6. validate best path
7. rebuild trajectory

### `set_seed()` — KEY

ตั้งคำตอบเริ่มต้นจาก paired constructor

ทำ:

1. set `best_path_`
2. validate seed
3. rebuild trajectory
4. publish event `seed`

นี่ทำให้ solver มี valid solution ตั้งแต่ต้น run

### `run()` — KEY

pipeline หลักของ solver

ลำดับ:

1. ถ้าไม่มี seed และยังไม่ goal ให้ error
2. small board search สำหรับ `N == 5`
3. ถ้า `N >= 6` เรียก `try_anytime_constructive()`
4. ถ้าเปิด large legacy engine ค่อยเรียก `try_large_board_resynthesis()`
5. เรียก macro sweep compression
6. เข้า bounded patch cleanup ตาม `cleanup_ms`
7. final validate
8. publish final
9. save checkpoint

ส่วนสำคัญคือ constructor มาก่อน cleanup และ cleanup ถูกจำกัดเวลา

### `try_small_board_search()` — KEY สำหรับ testcase 1

ใช้ weighted search บน board เล็ก

ทำให้ testcase 1 ลงถึง length 30 ได้ดี

### `weighted_small_board_search()` — KEY สำหรับ small board

ใช้ priority queue คล้าย weighted A*

ไม่ใช่ route หลักของ testcase 2-6 แต่สำคัญสำหรับ testcase 1

### `try_anytime_constructive()` — KEY

เรียก `AnytimeConstructiveSolver`

คำนวณ budget:

- board ใหญ่ใช้ `large_constructor_ratio`
- board กลางใช้ `anytime_ratio`
- reserve เวลาไว้ให้ cleanup เล็กน้อย

ถ้า constructor ได้ path ที่สั้นกว่าและ validate ผ่าน:

1. update best
2. set source เป็น `anytime_constructive` หรือ `large_constructor`
3. rebuild trajectory
4. publish
5. save checkpoint
6. log improvement

นี่คือ function ที่ทำให้เกิด jump ใหญ่ เช่น testcase 6 ได้ `422775`

### Large-board compression functions — LEGACY/CLEANUP

กลุ่มนี้ยังอยู่แต่ไม่ใช่ default engine หลัก:

- `try_large_center_hash_compression()`
- `try_large_sweep_compression()`
- `try_large_corridor_constructor()`
- `try_large_band_v2_constructor()`
- `try_large_board_resynthesis()`

แนวคิดเดิมคือหา loop หรือสร้าง band route ใหม่ แต่ตอนนี้ default `large_engine = off` เพราะ multi-seed constructor ให้ผลดีกว่า

ยังเก็บไว้เพื่อ A/B testing หรือเปิดเองด้วย `--large-engine`

### Macro functions — CLEANUP/LEGACY

#### `try_sweep_macro()` — CLEANUP

ใช้จริงใน default ตอนนี้

หน้าที่หลัก:

- ลบ backtrack ง่าย ๆ
- ลอง compress sweep pattern บางส่วน
- validate ก่อน commit

#### `try_transport_macro()` — LEGACY

ลองสร้าง route ใหม่ด้วย macro transport แบบเก่า

ไม่ใช่ default เพราะ constructor race ดีกว่า

#### `try_strip_macro()` — LEGACY

ลอง strip/order variants แบบเก่า

ไม่ใช่ default

#### `try_macro_resynthesis()` — CLEANUP/LEGACY dispatcher

เลือก macro strategy ตาม option

default ตอนนี้เป็น `sweep` เท่านั้น เพื่อไม่ให้เสียเวลาใน legacy route builders

### Trajectory / snapshot functions — CLEANUP/SUPPORT

#### `rebuild_trajectory()`

replay best path แล้วสร้าง:

- `moves_`
- snapshots ทุก stride
- snapshot 0 เสมอ

#### `state_at()`

หา board state ที่ move index หนึ่ง

ใช้ snapshot ก่อนหน้าที่ใกล้สุด แล้ว replay delta สั้น ๆ

สำคัญต่อ patch cleanup เพราะไม่ต้อง replay จากต้นทุก candidate

### Patch candidate scoring — CLEANUP

#### `segment_touched()`

ดูว่า segment นั้นแตะ cell อะไรบ้าง

#### `choose_window()`

เลือก local patch window แบบ permissive:

สนใจว่า move ส่วนใหญ่กระทบ window ไม่ได้บังคับว่า path ต้องอยู่ใน window ตลอด

#### `score_segment()`

ให้คะแนน segment ที่น่าซ่อม:

- length
- backtrack
- repeated blank
- repeated touched cells
- loopiness
- congestion
- overlap penalty

#### `gather_candidates()`

สร้าง candidate list จาก best path ปัจจุบัน

### Local patch solvers — CLEANUP

#### `solve_exact_patch()`

ใช้กับ 3x3/4x4 exact endpoint patch

ต้องถึง endpoint เดิมพอดี

#### `solve_relaxed_5x5()`

ใช้กับ 5x5 แบบ relax blank endpoint แล้วค่อย rejoin exact endpoint

ตอนนี้สำหรับ large boards ถูกปิดช่วง cleanup default เพราะช้าและให้กำไรน้อย

#### `patch_lower_bound()`

คำนวณ lower bound ราคาถูกจาก Manhattan + blank reposition

ถ้า old segment ใกล้ lower bound แล้วก็ skip ไม่เสียเวลา solve

#### `prepare_jobs()`

เปลี่ยน candidate เป็น immutable patch jobs และ dedupe ด้วย signature

#### `run_batch()`

รัน patch jobs แบบ parallel และมี timeslice

#### `select_result()` / `better_result()`

เลือก patch ที่ดีที่สุด:

1. delta มากสุด
2. score density ดีกว่า
3. start เร็วกว่า
4. patch size เล็กกว่า

#### `commit_result()`

splice replacement เข้า best path ถ้า:

- สั้นกว่า
- endpoint ถูก
- boundary safe
- validate ผ่าน

### Publishing / checkpoint — SUPPORT/KEY

#### `publish()`

เขียน:

- solution output
- meta JSON

metadata ตอนนี้มีทั้ง field ใหม่และเก่า:

- ใหม่: `constructor_*`, `elite_repair_*`, `cleanup_*`
- เก่า: `patch_*`, `macro_*`, `large_*`

เก็บ field เก่าไว้เพื่อไม่ให้ script วิเคราะห์ผลเดิมพัง

#### `save_checkpoint()`

เขียน binary checkpoint

ยังใช้ magic `PATCH01` เพื่อ load checkpoint เก่าได้

---

## `SolverApp`, self-test, main

### `SolverApp` — KEY/SUPPORT

เป็น app-level entrypoint หลัง parse input แล้ว

ถ้า `--improvement constructor-first`:

1. สร้าง `ConstructorFirstSolver`
2. ลอง resume checkpoint
3. ถ้า resume ไม่ได้ สร้าง paired seed
4. เรียก `run()`
5. print summary

ถ้า `--improvement none`:

สร้าง seed อย่างเดียว ไม่ทำ improvement

### `make_problem_for_test()` — TEST

สร้าง `Problem` ขนาดเล็กใน memory สำหรับ self-test

### `run_self_tests()` — TEST

ทดสอบพื้นฐาน:

- apply move ถูกไหม
- goal check ถูกไหม
- paired constructor validate ผ่านไหม
- patch cleanup ลด path ง่าย ๆ ได้ไหม

### `main()` — SUPPORT

ลำดับจริงตอนเปิดโปรแกรม:

1. parse options
2. ถ้า `--self-test` run test
3. อ่าน input file หรือ stdin
4. apply constructor-first defaults
5. ถ้า `--validate-solution` ให้ validate แล้วจบ
6. สร้าง `SolverApp`
7. run
8. catch exception แล้ว print error

---

## สรุปว่าอะไรควรพูดในวิดีโอ

ควรพูด:

- puzzle ต้อง match เฉพาะ center
- paired seed เป็น fallback
- multi-seed constructor คือหัวใจ
- monotone BFS distance ทำให้ tile เดินเข้าหา target ไม่วนมั่ว
- source scoring ใช้ทั้ง tile distance และ blank accessibility
- rollout ช่วยเลือก source ที่ดีต่ออนาคต
- elite checkpoint repair คือการเริ่มใหม่จากจุดกลางทางที่ดี
- patch cleanup เป็นแค่ polish
- ทุก commit ต้อง validate ทั้ง solution

พูดสั้น ๆ หรือข้ามได้:

- details ของ `Hash128`
- binary checkpoint field layout
- exact BFS internals ของ patch solver
- legacy large band-v2
- macro transport/strip

ไม่ควรขายเป็น main idea:

- 5x5 patch
- patch-only optimization
- old large corridor constructor

---

## Mapping สำหรับอธิบายไฟล์แบบเร็ว

```text
บรรทัด ~1-160      constants, hash, Options
บรรทัด ~170-490    CLI parsing
บรรทัด ~493-760    Problem, move application, validation
บรรทัด ~770-1490   paired/legacy ConstructiveSolver
บรรทัด ~1500-2720  AnytimeConstructiveSolver หลัก
บรรทัด ~2730-6000  ConstructorFirstSolver + cleanup/legacy/checkpoint/meta
บรรทัด ~6010-6070  SolverApp
บรรทัด ~6077-6135  self-test
บรรทัด ~6143+      main()
```

ถ้าต้องอธิบาย “ทั้งโค้ดใน 1 นาที”:

```text
โค้ดนี้อ่าน puzzle แล้วสร้างคำตอบ valid เร็วด้วย paired constructor ก่อน
จากนั้น engine หลักคือ multi-seed monotone constructor ซึ่งลองหลาย config หลาย seed
แต่ละ config แก้ center target ทีละช่องโดยเลือก source tile ด้วย heuristic และ rollout
ตอนย้าย tile จะใช้ BFS distance บังคับให้ tile ขยับเข้าใกล้ target แบบ monotone
คำตอบ valid ที่ดีจะถูกเก็บเป็น elite และ checkpoints ของมันใช้ repair suffix ต่อ
ถ้าเจอคำตอบสั้นกว่า จะ replay validate ทั้งหมดก่อน publish
หลังจากนั้น cleanup จะลองลบ sweep/backtrack และ patch local segment แบบจำกัดเวลา
ดังนั้นคะแนนหลักมาจาก constructor route ใหม่ ไม่ใช่ patch ซ่อมทีละนิด
```
