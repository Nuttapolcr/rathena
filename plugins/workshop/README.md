# workshop — Lua mod loader for rAthena map-server

Workshop เป็น map-server plugin ที่โหลดและรัน Lua mods จากโฟลเดอร์ `mods/`
แต่ละ mod คือ subfolder ที่มีไฟล์ `modinfo.yml` กำกับ และไฟล์สคริปต์ Lua ที่
plugin จะ execute ตามลำดับที่กำหนดด้วย dependencies + load_order

## Build & install

หนึ่งครั้งแรก โหลด Lua 5.4 source:

```sh
plugins/workshop/fetch_lua.sh
```

Build:

```sh
make plugin                 # จาก rAthena root
# หรือ
make -C plugins/workshop    # build เฉพาะ workshop
```

**Windows / Visual Studio:**

```bat
rem one-time, ใช้ tar + PowerShell ที่มากับ Windows 10 1803+
plugins\workshop\fetch_lua.bat
```

จากนั้นเปิด `rAthena.sln` ใน Visual Studio — workshop project อยู่ใต้
solution folder `plugins`. Build configuration ใด ๆ ก็ได้ (Debug/Release
x86/x64) จะวาง `workshop.dll` ใน `plugins\workshop\` ให้พร้อมอ้างจาก
`conf\plugins.conf`

เปิดใน [conf/plugins.conf](../../conf/plugins.conf):

```
plugins/workshop/workshop
```

ตอน start map-server ครั้งแรก plugin จะสร้าง `plugins/workshop/mods/`
และ scaffold mod ตัวอย่างไว้ — เปิดใช้งานโดยแก้ `enabled: true` ในไฟล์
`modinfo.yml`

## Mod layout

```
plugins/workshop/mods/<name>/
├── modinfo.yml          # required
└── scripts/
    └── *.lua            # source หรือ pre-compiled .luac
```

Plugin auto-detect bytecode (`luac`-compiled `.luac`) จาก byte แรก
(`0x1B`) — ส่ง encoded scripts ได้โดยไม่ต้องเขียน decoder เพิ่ม

### modinfo.yml schema

| field          | type         | required | default | คำอธิบาย                                                |
|----------------|--------------|----------|---------|----------------------------------------------------------|
| `name`         | string       | yes      |         | ชื่อ mod (ใช้อ้างอิงใน dependencies)                       |
| `enabled`      | bool         |          | `true`  | `false` = อ่าน modinfo แต่ไม่โหลด scripts                  |
| `version`      | string       |          | "0.0.0" |                                                          |
| `author`       | string       |          | ""      |                                                          |
| `description`  | string       |          | ""      |                                                          |
| `scripts`      | list[string] | yes      | `[]`    | path สัมพัทธ์จาก mod folder (โหลดเรียงตามลำดับในรายการ)     |
| `dependencies` | list[string] |          | `[]`    | mod ที่ต้องโหลดก่อนตัวนี้                                   |
| `load_order`   | int          |          | `100`   | ตัวเลขน้อย = โหลดก่อน                                       |
| `on_init`      | string       |          | ""      | ชื่อ Lua function ที่จะถูกเรียกหลังโหลด scripts ครบทุกไฟล์ |
| `db`           | list         |          | `[]`    | rAthena-style YAML DB files — โหลดก่อน scripts (ดู [DB store](#db-store)) |
| `rathena_scripts` | list[string] |       | `[]`    | classic `.txt` NPC scripts — queue ให้ engine (ดู [rAthena scripts](#rathena-scripts)) |
| `disable_scripts` | list[string] |       | `[]`    | path ที่จะลบออกจาก `scripts_main.conf` queue                |

ลำดับโหลดถูกแก้ด้วย topological sort: dependencies → ใครชนเสมอกัน
ตัดสินด้วย load_order → แล้วค่อยตัดสินด้วยชื่อ mod

## DB store

แต่ละ mod สามารถ ship ไฟล์ YAML รูปแบบเดียวกับ rAthena (`Header.Type` +
`Body` sequence) ใน folder ของตัวเอง Workshop รวมไฟล์เหล่านี้เป็น store
กลางที่ Lua mods อ่านได้ผ่าน `db_get` / `db_each` / `db_count` / `db_has` /
`db_types`

**modinfo.yml — db section:**

```yaml
db:
  - db/items.yml                      # bare path: key=Id, override=replace
  - path: db/balance_overrides.yml    # detailed form
    key:      Id                      # primary-key field (default "Id")
    override: merge                   # collision policy
  - path: db/my_status.yml            # status.yml shape — keyed by name
    key:      Status
```

**Override modes** (เลือกเมื่อ key ของ entry ชนกับ mod ก่อนหน้า):

| mode      | พฤติกรรม                                                    |
|-----------|-------------------------------------------------------------|
| `replace` | newer wins (default; เหมือน `db/import/` ของ rAthena)         |
| `skip`    | first-seen wins; ที่มาทีหลังโดน drop                          |
| `error`   | log error + skip — ใช้ตอน debug ว่า mod ไหนชนกัน               |
| `merge`   | deep-merge YAML maps key by key (later mod tweaks fields)   |

**YAML file shape** (เหมือน rAthena DB):

```yaml
Header:
  Type: ITEM_DB
  Version: 1
Body:
  - Id: 90001
    AegisName: Workshop_Token
    Name: Workshop Token
    Type: Etc
    ...
```

`Header.Type` คือชื่อ bucket (เช่น `ITEM_DB`, `MOB_DB`, `STATUS_DB`) ที่ Lua
ใช้อ้างอิง — Workshop ไม่ตีความ Type เป็นอย่างอื่น เก็บ entry ตามโครงสร้าง
YAML ตรง ๆ

**Key field** — ค่า `key` เลือก field ที่ใช้เป็น primary key ของ bucket
นั้น default คือ `Id` (item_db / mob_db / skill_db style) แต่บางไฟล์ใช้
field อื่น เช่น `db/re/status.yml` ที่ entries keyed ด้วย `Status:` name
(ไม่มี numeric Id) — ตั้ง `key: Status` แล้ว query ด้วย name string ได้:

```yaml
db:
  - path: ../../../db/re/status.yml    # path สัมพัทธ์จาก mod folder
    key:  Status
```

Key เก็บเป็น string เสมอ — numeric id เก็บในรูป decimal literal ("512"),
name key เก็บตรง ๆ ("Stone"). ฝั่ง Lua `db_get` รับทั้ง number และ string
(number จะถูกแปลงเป็น decimal form ก่อน lookup)

**ลำดับการโหลด:** ทุก mod ในลำดับ topological+load_order — สำหรับ mod
แต่ละตัว DB files โหลด **ก่อน** scripts จึงสามารถเรียก `db_get` / `db_each`
ใน top-level Lua code ได้

**Lua API:**

```lua
db_count('ITEM_DB')                   -- → int
db_has  ('ITEM_DB', 90001)            -- → bool (key: number or string)
db_get  ('ITEM_DB', 90001)            -- → table or nil
db_get  ('STATUS_DB', 'Stone')        -- name-keyed lookup
db_types()                            -- → list of bucket names

db_each('ITEM_DB', function(key, entry)
    -- key arrives as a number for numeric-keyed DBs, a string otherwise
    -- entry คือ Lua table ที่ recursively converted จาก YAML
    print(key, entry.AegisName, entry.Type)
end)
```

YAML scalar → Lua: int → number → bool (เฉพาะ `true`/`false`/`yes`/`no`
literals) → string fallback. Sequences กลายเป็น 1-indexed table; maps
กลายเป็น keyed table

**ข้อจำกัด:** ระบบนี้เป็น store แยกของ workshop เอง ไม่ได้ inject เข้า
DB ของ engine — engine ยังโหลด `db/<region>/item_db.yml` ปกติ Workshop DB
เหมาะกับ mod logic (lookups, balancing tables, custom config) ที่ Lua
script ต้องการอ่าน ไม่ได้แทน item_db จริง

## rAthena scripts

แต่ละ mod ship ไฟล์ `.txt` script ของ rAthena ใน folder ตัวเอง แล้ว
declare ใน modinfo.yml — workshop จะ queue ให้ engine ผ่าน
`npc_addsrcfile` ตอน `plugin_init` (ก่อน `do_init_npc` ที่ parse script ทั้งหมด)
เป็นช่วงเวลาเดียวกับที่ `scripts_main.conf` ถูกประมวลผลพอดี

**modinfo.yml — rathena_scripts:**

```yaml
rathena_scripts:
  - npc/myshop.txt          # path สัมพัทธ์จาก folder ของ mod
  - npc/quests/dailies.txt
```

ไฟล์ที่ list ไว้จะถูกอ่านโดย engine เหมือน NPC ทั่วไป — ใช้งาน buildin
script command ที่ Lua mod ลงทะเบียนได้ตรง ๆ (เช่น `shop_intro`,
`workshop_async_roll`)

**ปิด script จาก `scripts_main.conf`** ผ่าน `disable_scripts`:

```yaml
disable_scripts:
  - npc/airports/airships.txt   # path ตามที่เขียนใน scripts_main.conf
  - npc/cities/prontera.txt
```

Workshop จะเรียก `npc_delsrcfile` ดึงรายการเหล่านี้ออกจาก queue ก่อนที่
engine จะ parse — จึงไม่ถูกโหลดในรอบนี้ (ครั้งหน้าที่ map-server boot
ก็จะถูก disable อีก เพราะ workshop รันใหม่ทุกรอบ)

**Timing:** queue ถูก populate ตอน `map_config_read` (อ่าน
`scripts_main.conf`) — ก่อน plugin_init runs — ก่อน `do_init_npc` parse
ของจริง การ add/del ของเราจึง take effect 100% ก่อน script จะถูก
compile ครั้งเดียวในรอบ boot นั้น

**ข้อจำกัด:** ใช้ได้เฉพาะตอน boot — `workshop_reload` ใน runtime ไม่
re-trigger NPC parsing (ต้องใช้ `@reloadnpcfile` หรือ restart map-server
เพื่อให้รายการ rathena_scripts ใหม่/ถูกลบมีผล)

## Lua API reference

### Logging

```lua
log_info(msg)    log_status(msg)    log_warn(msg)    log_error(msg)
print(...)                       -- routed to server log
```

### Player lookups

```lua
get_player(aid)               -- returns nil หรือ player table
get_player_by_name("Iris")
```

Player table มี field: `aid`, `name`, `base_level`, `job_level`, `map`, `x`, `y`

### Inventory & money

```lua
getitem(player, item_id [, amount])
delitem(player, slot, amount [, type, reason])
countitem(player, item_id)              -- รวม stack ทั้งหมดของ item นี้

payzeny(player, amount)                  -- หักเงิน
give_zeny(player, amount)                -- จ่ายเงิน
```

### Item / skill database

```lua
item_exists(id)         item_internal_name(id)        -- "Apple"
item_name(id)           -- display name
item_type(id)           -- IT_HEALING=0, IT_USABLE=2, IT_ETC=3, ...

skill_id("MG_FIREBOLT") -- name → numeric id (0 ถ้าไม่พบ)
skill_name(id)          -- numeric id → AEGIS name
skill_inf(id)           -- INF flags
```

### Stats & combat

```lua
read_param(player, sp_type)       -- SP_STR=13, SP_AGI=14, ...
get_equip_id(player, eqi_slot)    -- EQI_HEAD_TOP=0 ฯลฯ; 0 = ว่าง
get_skill_lv(player, skill_id)

bonus (player, sp_type, val)
bonus2(player, sp_type, v1, v2)
bonus3(player, sp_type, v1, v2, v3)
bonus4(player, sp_type, v1, v2, v3, v4)
bonus5(player, sp_type, v1, v2, v3, v4, v5)

gainexp(player, base_exp [, job_exp])
heal  (player, hp [, sp])
damage(src, target, hp [, sp, walkdelay, flag, skill_id])
use_skill(player, skill_id, lvl [, target_aid])
```

### Status changes (SC_*)

`type` is either an sc_type number or a constant name — `'FREEZE'` or
`'SC_FREEZE'` both work. `sc_id()` resolves a name once so loops don't
re-look-up. Durations are milliseconds. From Lua the rate is always 100%
(`sc_start` applies the status unconditionally).

```lua
sc_id("FREEZE")                       -- → numeric sc_type, -1 if unknown

sc_start(player, "FREEZE", 5000)              -- 5s freeze
sc_start(player, "BLESSING", 60000, 10)       -- Blessing lv10, val1=10
sc_start(player, scid, dur, v1, v2, v3, v4 [, flag])
                                              -- flag: SCSTART_* bitmask
                                              -- (NOAVOID 0x1, NOTICKDEF 0x2,
                                              --  LOADED 0x4, NORATEDEF 0x8,
                                              --  NOICON 0x10), default 0

sc_start_from(src, target, type, dur [, v1..v4 [, flag]])
                                              -- attribute to a source bl

sc_end   (player, type)               -- → 1 if a status was removed
sc_clear (player [, all])             -- all=true drops permanent ones too
sc_active(player, type)               -- → bool
sc_val   (player, type, which)        -- val1..val4 of an active SC (which 1..4)
```

#### Custom status changes — `register_sc`

Define a brand-new SC that the engine recalculates the way a built-in one
does. `register_sc` returns an id; every `sc_*` wrapper above accepts it
exactly like an engine `sc_type` (workshop routes on the id). For plugin
SCs `sc_start`'s `flag` arg is ignored and a duration `<= 0` means
permanent.

```lua
-- register_sc(name, calc_flag, calc_fn [, icon]) -> id (>=0) or -1
--   calc_flag — bitmask, or a table of stat names:
--               'STR' 'AGI' 'VIT' 'INT' 'DEX' 'LUK' 'MAXHP' 'MAXSP' 'SPEED'
--   calc_fn(ctx) — runs once per affected stat during status recalc;
--                  ctx = { player?, sc_id, stat (a PLUGIN_SCB_* bit),
--                          cur (value so far), val1..val4 }
--                  return the new value for ctx.cur.
--   icon — optional EFST_* status-bar icon (0 = none)

HYPER = register_sc('Workshop_Hyper', {'STR', 'AGI'}, function(ctx)
    return ctx.cur + (ctx.val1 or 0)        -- +val1 to STR and AGI
end)

sc_start(player, HYPER, 30000, 25)          -- +25 STR/AGI for 30s
sc_active(player, HYPER)                     -- → bool
sc_val(player, HYPER, 1)                     -- → 25
sc_end(player, HYPER)
```

Base-stat flags cascade — a STR-affecting SC also makes the engine
recompute batk/matk/etc. `register_sc` is a load-time call; on
`workshop_reload` the previous registrations go inert (their calc
callbacks die with the old Lua VM) and re-running the mod registers
fresh ones — a small engine-side leak, harmless in practice.

### World / map

```lua
warp(player, "mapname", x, y)
monster("map", x, y, "name", mob_id [, amount])

mapindex("prontera")        -- → uint16
mapname(idx)                -- uint16 → "prontera"
mapflag("prontera", flag)   -- mapflag value

announce("text" [, color])              -- server-wide
mapannounce("map", "text" [, color])    -- เฉพาะ map เดียว
message(player, "text")                 -- chat ส่วนตัว
messagecolor(player, color, "text" [, target])

emotion(player, emote_id)
specialeffect(player, effect_id [, target])
specialeffect_single(player, effect_id)
progressbar(player, color_rgb, seconds)
progressbar_abort(player)
```

### Map iteration

```lua
for_each_player_in_map("prontera", function(p)
    -- return truthy เพื่อนับเป็น "matched"
end)

for_each_player_in_area("map", x0, y0, x1, y1, fn)

count_players_in_map("prontera")
count_mobs_in_map("prontera")
```

### Storage & quests

```lua
open_storage      (player)              -- คลังส่วนตัว (Kafra) → 1 ถ้าเปิดได้
open_guild_storage(player)              -- คลังกิลด์ → 1 ถ้าเปิดได้
open_storage2     (player, id [, mode]) -- premium/extended storage (storage.yml)
                                        --   mode: "get" | "put" | "all" (default) | "none"
                                        --         หรือ e_storage_mode bitmask
storage_exists    (id)                  -- → bool: id เป็น premium storage ที่ตั้งไว้หรือไม่
register_storage  (id, name [, max_num [, sql_table]])  -- ลงทะเบียนคลัง → bool
storage_name      (id)                  -- → string: ชื่อ tab ที่ client เห็น ("Storage" ถ้าไม่มี)
```

`register_storage` ลง/แก้ entry ในตารางคลังของ map-server (ตารางเดียวกับที่
`storage.yml` ป้อน) — client แยก tab คลังด้วย `name` ที่ส่งไปกับ
`ZC_INVENTORY_START` (`INVTYPE_STORAGE`) ดังนั้นลงทะเบียน `id`/`name` ใหม่
แล้วเปิดด้วย `open_storage2` ก็ได้คลังใหม่ให้ player

- `id == 0` → เปลี่ยนชื่อ tab ของคลังส่วนตัว (Kafra)
- `id` 1..255 → premium storage ที่ `open_storage2(player, id)` เปิดได้
- `sql_table` default `"storage"`; ถ้าจะให้ของในคลัง **เซฟ/โหลดจริง**
  char-server ต้องรู้จัก id/table เดียวกันด้วย — ใส่ใน `db/(pre-)re/storage.yml`
- char-server ส่ง list คลังใหม่ทุกครั้งที่ map (re)connect → ทับ entry ที่ลงไว้
  ด้วย `register_storage` ลงทะเบียนใน hook `intif_connected` เพื่อให้ลงซ้ำเอง
  ทุกครั้ง (ดู section Hooks ด้านล่าง)

```lua
hook("intif_connected", function() register_storage(20, "Event Vault", 600, "event_storage") end)
open_storage2(player, 20)

quest_add   (player, quest_id)
quest_status(player, quest_id, status)   -- 0=Q_INACTIVE 1=Q_ACTIVE 2=Q_COMPLETE
quest_check (player, quest_id [, type])  -- 0=HAVEQUEST 1=PLAYTIME 2=HUNTING
```

### Client UI windows

เปิดหน้าต่าง UI ของ client ให้ player

```lua
open_ui(player, window [, data])
-- window: "bank" | "stylist" | "captcha" | "macro" | "tip" | "quest"
--         | "attendance" | "enchantgrade" | "enchant"  (หรือเลข out_ui_type)
-- data:   payload เฉพาะหน้าต่าง — quest id สำหรับ "quest", tip id สำหรับ "tip"
--         (default 0); หน้าต่างที่ client/PACKETVER ไม่รองรับจะถูกเมิน

open_ui(player, "bank")
open_ui(player, "quest", 12345)

open_dressroom(player)                  -- หน้าต่าง dress room
open_roulette (player)                  -- หน้าต่าง roulette (ต้องเปิด feature_roulette)
open_mail     (player)                  -- หน้าต่างกล่องจดหมาย
```

### Battle config (conf/battle/*.conf)

Read or override any `conf/battle/*.conf` setting at runtime by its key
(`base_exp_rate`, `enable_pet_autofeed`, `max_walk_speed`, …). The engine
clamps a set value to the setting's declared `[min, max]`; changes are
not written back to the `.conf` files.

```lua
battle_get('base_exp_rate')          -- → int (0 if the name is unknown)
battle_has('base_exp_rate')          -- → bool (tells "0" from "unknown")
battle_set('base_exp_rate', 200)     -- → bool; value: number or boolean
battle_set('enable_pet_autofeed', true)
```

### NPC dialog

`mes()` ส่ง dialog line ไม่ block — เก็บไว้หลายบรรทัดได้ จากนั้นเรียก
หนึ่งใน blocking primitives ปิดท้าย script จะ park รอ player ตอบ

```lua
mes(player, "text")            -- ไม่ block; sequence ได้

next_dialog (player)            -- รอคลิก "Next"
close_dialog(player)            -- รอคลิก "Close"
menu(player, "Buy:Sell:Cancel") -- รอเลือก menu
input_int(player)               -- รอกรอกตัวเลข
input_str(player)               -- รอกรอกข้อความ
```

หลังจาก player ตอบสนอง engine จะรัน NPC script command **ถัดไป**
(ไม่ใช่ Lua function เดิม) — อ่านคำตอบใน buildin ตัวต่อมา:

```lua
npc_oid   (player)   -- bl id ของ NPC ที่ player คุยอยู่
npc_menu  (player)   -- 1-based ของ menu choice ล่าสุด
npc_amount(player)   -- input_int result
npc_str   (player)   -- input_str result
```

ดูตัวอย่าง multi-step ที่ [section ด้านล่าง](#patterns)

### NPC events

```lua
trigger_event(player, "NpcName::OnLabel" [, ontouch])
```

### Variable storage (setd / getd / array)

```lua
set_var(player, "$@count", 42)            -- integer
set_var(player, "$@name$", "Hero")        -- string (suffix $)
set_var(player, "@list",   100, 5)        -- array index 5
set_var(nil,    "$global_perm", 1)        -- global var (sd ใส่ nil)

get_var(player, "$@count")                -- → integer
get_var(player, "$@name$")                -- → string (เช็ค suffix)
get_var(player, "@list", 5)               -- array slot 5
```

Scope ระบุด้วย prefix ของชื่อ:

| prefix | scope                           |
|--------|---------------------------------|
| `.`    | NPC scope (ต่อ NPC instance)     |
| `.@`   | local stack frame               |
| `#`    | char-shared (per account)       |
| `##`   | account-wide                    |
| `@`    | temp char (clear ตอน logout)    |
| `$`    | global permanent                |
| `$@`   | global temporary                |
| `'`    | instance-scoped                 |

`set_var` / `get_var` ต้องเรียกใน `register_buildin` handler เท่านั้น (ต้องมี active script_state)

### Time

```lua
gettick()                    -- ms ตั้งแต่ server start (rAthena gettick)
getservertime()              -- unix timestamp
gettime(unit)                -- 1=sec 2=min 3=hour 4=wday 5=mday 6=mon 7=year 8=yday
gettimestr("%Y-%m-%d")       -- strftime
```

### Async / sleep / suspend

```lua
sleep(1000)                  -- pause NPC script + Lua coroutine 1s

local token = script_suspend()
-- ทำงาน async อะไรก็ได้
script_resume(token, return_value)
```

`sleep` และ `script_suspend` ใช้ได้เฉพาะใน `register_buildin` handlers
เพราะต้องมี script_state ให้ park

### Timers

#### One-shot

```lua
timer_after(5000, function(id) ... end [, id])    -- → tid
```

#### Named timers (mirror NPC `OnTimer<ms>`)

```lua
on_timer("boss",  5000, function() ... end)       -- ลงทะเบียน offset
on_timer("boss", 10000, function(name, offset)
    init_timer(name); start_timer(name)            -- loop เอง
end)

init_timer ("boss")    -- reset + cancel pending
start_timer("boss")    -- เริ่มนับ
stop_timer ("boss")    -- pause
get_timer_tick("boss") -- ms ที่ผ่านไป
```

### Clock events (rAthena-compatible)

ใช้ label name ตรง ๆ จาก rAthena (`OnClockHHMM`, `OnMinuteMM`, `OnHourHH`,
`OnDayMMDD`, `OnSun..OnSat<HHMM>`):

```lua
on_event("OnMinute00", function(name) ... end)
on_event("OnClock1300", function() ... end)
on_event("OnSun1500",  function() ... end)
```

หรือใช้ shortcut แบบ typed:

```lua
on_clock(13, 0,  function() ... end)
on_minute(30,    function() ... end)
on_hour(0,       function() ... end)
on_day(1, 1,     function() ... end)
```

Dispatcher ทำงาน 1s interval ตรวจ wall-clock; events จะ fire ตอน
minute/hour/day rollover (ตรงกับ behavior `npc_event_do_clock`)

### Hooks (server events)

```lua
hook("pc_login", function(ctx)
    if ctx.player then message(ctx.player, "Welcome!") end
end)
```

ชื่อ event ที่รองรับ:
`pc_login`, `pc_logout`, `pc_baselevelup`, `pc_joblevelup`, `pc_dead`,
`pc_chat`, `pc_whisper`, `mob_kill`, `mob_spawn`, `item_use`, `item_pickup`,
`item_drop`, `item_equip`, `skill_use`, `npc_click`, `atcmd_execute`,
`quest_add`, `quest_complete`, `storage_open`, `intif_connected`
(alias: `char_reconnect`)

`intif_connected` fire ทุกครั้งที่ map-server (เชื่อม/เชื่อมใหม่) กับ
char-server เสร็จ — `ctx.first` = `true` ครั้งแรก, `false` เมื่อ reconnect
ใช้ลงทะเบียนซ้ำของที่ char-server ทับตอน reconnect (เช่น `register_storage`)

```lua
hook("intif_connected", function(ctx)
    register_storage(20, "Event Vault", 600, "event_storage")
end)
```

Handler return `false` หรือ `"stop"` เพื่อ cancel action (เฉพาะ event ที่
รองรับ HOOK_STOP — ดู [src/map/plugin.hpp](../../src/map/plugin.hpp))

### Custom commands

```lua
-- @hello
register_atcmd("hello", 0, function(player, args)
    message(player, "Hi " .. player.name)
    return 1
end)

-- workshop_pinkpoke 1, 2;
register_buildin("workshop_pinkpoke", "ii", function(player, a, b)
    return a + b
end)
```

`argspec`: `i` = int, `s` = string. Return value (number/string) ถูก
push กลับเป็น script return

### Client packet hooks

```lua
-- on_packet(cmd, fn): intercept an *existing* client packet before the
-- engine's clif handler. ctx = { fd, cmd, player? }. Read the payload
-- with packet_read_b/w/l/str(ctx.fd, offset) — offsets are from the
-- 2-byte cmd word. Return false or "stop" to suppress the engine
-- handler; nil / true lets it run as normal.
on_packet(0x0090, function(ctx)                 -- "talk to NPC"
    local npc_id = packet_read_l(ctx.fd, 2)
    log_info('NPC click: npc_id=' .. npc_id)
    -- return false                              -- would block it
end)

-- register_packet(cmd, length, fn): handle a previously-unused packet
-- id (0x064..0xCFF — pick one your client build doesn't use). `length`
-- is the fixed size in bytes incl. the 2-byte cmd, or -1 for
-- variable-length packets (size read from offset 2).
register_packet(0x0CFD, 2, function(ctx)
    if ctx.player then message(ctx.player, 'pong!') end
end)

-- Read helpers (call inside on_packet / register_packet with ctx.fd):
packet_read_b(fd, off)    packet_read_w(fd, off)    packet_read_l(fd, off)
packet_read_str(fd, off)  packet_rest(fd)           -- bytes left in recv buf

-- Send helpers — `bytes` is a Lua string holding the raw packet
-- (cmd at offset 0..1):
packet_send_self(fd, bytes)                 -- push to one fd
packet_send(player|nil, bytes [, target])   -- clif_send; target:
                                            -- 0=ALL_CLIENT 1=ALL_SAMEMAP
                                            -- 2=AREA 3=AREA_WOS 24=SELF ...
                                            -- player may be nil only for
                                            -- target 0
```

One filter per cmd (re-registering swaps it); same for `register_packet`.
On `workshop_reload` the old registrations go inert (the engine keeps
them but the trampoline no-ops on the stale ref) and a re-run installs
fresh ones. Hooking core gameplay packets is powerful but easy to break
things with — start with log-only filters and a packet id you know your
client uses.

### Reload (runtime)

`workshop_reload;` — NPC script command ที่ shutdown Lua VM ครบรอบ
(ปิด timer, ปลด refs ของ event/timer/hook/packet), เปิดใหม่, รัน mod scripts
อีกครั้ง — buildin/atcmd ที่เคย register ไว้ engine ยังเรียกได้ ref ใหม่

## Patterns

### Multi-step NPC dialog

Lua coroutine ที่ yield ผ่าน dialog primitive จะ **ไม่** resume กลับมา —
flow ดำเนินต่อใน NPC script command ถัดไป จึงต้องแยก dialog ออกเป็น
หลาย buildin

```lua
-- mods/shop/scripts/shop.lua

register_buildin("shop_intro", "", function(player)
    mes(player, "Hello traveler!")
    mes(player, "Pick a service:")
    menu(player, "Buy potions:Sell items:Cancel")
end)

register_buildin("shop_handle", "", function(player)
    local choice = npc_menu(player)
    if     choice == 1 then mes(player, "Buying...")
    elseif choice == 2 then mes(player, "Selling...")
    else                    mes(player, "Bye!") end
    close_dialog(player)
end)
```

NPC script:

```
prontera,150,150,5	script	Shopkeeper	100,{
    shop_intro;
    shop_handle;
    end;
}
```

### Async work

```lua
register_buildin("roll", "i", function(player, max)
    if max < 1 then max = 1 end
    sleep(1000)                       -- script_state parked
    return math.random(1, max)
end)
```

NPC: `.@n = roll(6); mes "You rolled " + .@n;`

### Persistent counters

```lua
register_buildin("visit_track", "", function(player)
    local n = get_var(player, "#mod_visits") + 1
    set_var(player, "#mod_visits", n)
    mes(player, "Welcome back! Visit #" .. n)
end)
```

### Reactive loot announcer

```lua
hook("item_pickup", function(ctx)
    if ctx.item_id and ctx.item_id == 7227 then  -- TCG card
        announce(ctx.player.name .. " picked up a TCG!")
    end
end)
```

### Daily reward

```lua
on_clock(0, 0, function()                   -- ทุกเที่ยงคืน
    for_each_player_in_map("prontera", function(p)
        getitem(p, 512, 1)                   -- give Apple
        message(p, "Daily login reward delivered.")
        return true
    end)
end)
```

### Boss timer with phases

```lua
on_timer("boss_warn",  60000, function() announce("Boss in 4 minutes!") end)
on_timer("boss_warn", 240000, function() announce("Boss in 1 minute!") end)
on_timer("boss_warn", 300000, function()
    monster("guild_vs1", 50, 50, "Phreeoni", 1159, 1)
    announce("Phreeoni has spawned!")
    init_timer("boss_warn")
end)

register_atcmd("startboss", 99, function(player)
    start_timer("boss_warn")
    return 1
end)
```

## Limitations

- **Dialog flow ต้องแยก buildin** — Lua coroutine ที่ block ผ่าน
  `next_dialog` / `menu` / `input_*` จะไม่ resume; วาง flow ต่อใน buildin ตัวถัดไป
  (engine resume script_state เอง ผ่าน `npc_scriptcont`)
- **`bonus*` แบบนี้ไม่ persist ผ่าน status recalc** — ใช้ `sc_start` หรือ
  passive status สำหรับ buff ถาวร
- **ไม่มี shared library system** — function ที่ใช้ข้าม mod ต้อง share
  ผ่าน global table เอง (เช่น `_G.mylib = {...}`)
- **Hook context fields ไม่ครบทุก event** — ดู [lua_globals.cpp `hook_dispatch`](lua_globals.cpp)
  ว่าแต่ละ event push field อะไรลง ctx; ที่ไม่อยู่ใน switch case จะได้ table ว่าง

## Files

| file                                   | บทบาท                                      |
|----------------------------------------|---------------------------------------------|
| [workshop.cpp](workshop.cpp)           | plugin entry, lifecycle, `workshop_reload`  |
| [lua_bridge.cpp](lua_bridge.cpp)       | Lua VM mgmt, bytecode loading               |
| [lua_globals.cpp](lua_globals.cpp)     | global Lua wrappers — API surface ทั้งหมด    |
| [modloader.cpp](modloader.cpp)         | parse modinfo.yml, dependency resolution    |
| [Makefile](Makefile)                   | build (รวม Lua + yaml-cpp + plugin)         |
| [fetch_lua.sh](fetch_lua.sh)           | download Lua 5.4 source (ครั้งเดียว)         |
