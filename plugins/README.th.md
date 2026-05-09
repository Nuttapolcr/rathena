# ระบบปลั๊กอิน rAthena

> **ภาษา:** ไทย · [English](README.md)

ระบบปลั๊กอินแบบไดนามิกที่ให้ไฟล์ DLL / SO ภายนอกสามารถ hook เข้ากับ
event ของ map-server, ลงทะเบียน script command และ `@command` แบบกำหนดเอง,
รวมถึงรับส่ง packet ที่ออกแบบขึ้นเอง — โดยไม่ต้องแก้ไข source ของ rAthena

---

## สารบัญ

- [ภาพรวม](#ภาพรวม)
- [เริ่มต้นใช้งาน](#เริ่มต้นใช้งาน)
- [การ build ปลั๊กอิน](#การ-build-ปลั๊กอิน)
- [วงจรชีวิตของปลั๊กอิน](#วงจรชีวิตของปลั๊กอิน)
- [Hooks](#hooks)
- [API Surface](#api-surface)
  - [`script` — ตัวช่วย script state](#script--ตัวช่วย-script-state)
  - [`pc` — การจัดการผู้เล่น](#pc--การจัดการผู้เล่น)
  - [`mob` — มอนสเตอร์](#mob--มอนสเตอร์)
  - [`map` — แมพ / iteration](#map--แมพ--iteration)
  - [`status` — HP / SP](#status--hp--sp)
  - [`bl` — Block list](#bl--block-list)
  - [`item_api` — Item & Item DB](#item_api--item--item-db)
  - [`atcmd` — @command แบบกำหนดเอง](#atcmd--command-แบบกำหนดเอง)
  - [`quest` — เควสต์](#quest--เควสต์)
  - [`npc` — NPC events](#npc--npc-events)
  - [`skill` — สกิล](#skill--สกิล)
  - [`storage` — กล่องเก็บของ](#storage--กล่องเก็บของ)
  - [`clif` — Packet สำเร็จรูป](#clif--packet-สำเร็จรูป)
  - [`timer` — Timer](#timer--timer)
  - [`log` — Logging](#log--logging)
  - [`packet` — Packet แบบกำหนดเอง](#packet--packet-แบบกำหนดเอง)
- [Pattern การใช้งาน](#pattern-การใช้งาน)
- [Example Plugin](#example-plugin)
- [เคล็ดลับและการแก้ปัญหา](#เคล็ดลับและการแก้ปัญหา)

---

## ภาพรวม

ปลั๊กอินคือ shared library (`.so` บน Linux/macOS, `.dll` บน Windows)
ที่ export สัญลักษณ์ C-linkage 3 ตัว map-server จะโหลดทุกปลั๊กอินที่
ระบุไว้ใน `conf/plugins.conf` ตอนเริ่มต้น แล้วส่ง struct
[`plugin_api_t`](../src/map/plugin.hpp) ที่เต็มไปด้วย function pointer
ให้ปลั๊กอินใช้ลงทะเบียน hook callback, script command, @command
หรือ packet handler

**ทำไมถึงควรใช้ปลั๊กอินแทนการ build เอง?**

- Reload เฉพาะปลั๊กอินเพื่อทดลอง logic — ไม่ต้อง rebuild server ทั้งตัว
- กระจายฟีเจอร์เป็นไฟล์ `.so` / `.dll` แบบ drop-in
- อัปเกรด server ได้สะอาด — โค้ดของคุณอยู่นอก `src/`

API ของปลั๊กอินถูกออกแบบมาให้แคบโดยตั้งใจ: ปลั๊กอิน include เพียง
`map/plugin.hpp` ไม่ต้อง include header ตัวอื่นของ server ทุก type
ของ server (`map_session_data`, `mob_data`, `block_list`, …)
ถูก forward-declare ไว้และเข้าถึงผ่าน accessor function ที่อยู่บน
struct API ทำให้ ABI ของปลั๊กอินไม่ขึ้นกับ field layout ภายในของ server

---

## เริ่มต้นใช้งาน

1. **เปิดใช้ปลั๊กอิน** โดยเพิ่ม path ลงใน `conf/plugins.conf`:

   ```text
   plugins/example/example_plugin
   ```

   นามสกุลไฟล์ (`.so` / `.dll`) จะถูกเติมให้อัตโนมัติตามแพลตฟอร์ม

2. **Build** ตัวอย่าง:

   ```bash
   make plugin
   ```

   จะได้ `plugins/example/example.so` (Linux) หรือ
   `example_plugin.dll` (Windows)

3. **เริ่ม server** จะเห็น log:

   ```text
   [Status]: plugin: Loaded 'Example Plugin' v3.0.0 by rAthena Dev Team
   [Status]: plugin: 1 plugin(s) loaded.
   ```

4. **ลองเรียกใช้** ใน NPC script:

   ```c
   prontera,150,150,4    script    PluginTest    4_F_KAFRA1,{
       mes "สวัสดีจากปลั๊กอิน!";
       plugin_announce "ข้อความประกาศจากปลั๊กอิน!";
       .@n = plugin_async_roll(6);
       mes "คุณทอยได้: " + .@n;
       close;
   }
   ```

---

## การ build ปลั๊กอิน

ปลั๊กอินคือไฟล์ C++ 1 หรือหลายไฟล์ที่ include `map/plugin.hpp`
และ export สัญลักษณ์ 3 ตัว:

```cpp
PLUGIN_API plugin_info_t* plugin_info();
PLUGIN_API bool           plugin_init(plugin_api_t* api);
PLUGIN_API void           plugin_final();
```

### Linux / macOS

target `make plugin` ที่มีอยู่จะ build ทุกโฟลเดอร์ใต้ `plugins/`
เพิ่มปลั๊กอินของคุณเป็น sibling ของ `example/`:

```text
plugins/
├── example/
│   └── example_plugin.cpp
└── my_feature/
    └── my_feature.cpp     ← ค้นพบและ build อัตโนมัติ
```

`make plugin` จะรัน `g++ -std=c++17 -shared -fPIC -fvisibility=hidden`
กับทุกไฟล์ `.cpp` ในแต่ละโฟลเดอร์ปลั๊กอิน แล้วสร้าง `.so` ที่ชื่อ
ตรงกับชื่อโฟลเดอร์

### Windows (Visual Studio)

ใช้ [`plugins/example/example_plugin.vcxproj`](example/example_plugin.vcxproj)
เป็นต้นแบบ การตั้งค่าสำคัญ:

- **Configuration Type:** Dynamic Library (`.dll`)
- **Additional Include Directories:** `<rathena>/src`
- **Output Name:** ตรงกับที่ระบุใน `conf/plugins.conf`

### CMake (cross-platform)

ไฟล์ [`plugins/CMakeLists.txt`](CMakeLists.txt) เดินทุกโฟลเดอร์ย่อย
และ build ให้ ใช้แบบ standalone ได้:

```bash
cd plugins
mkdir build && cd build
cmake -DRATHENA_SRC=../../src ..
cmake --build .
```

---

## วงจรชีวิตของปลั๊กอิน

```text
        ┌──────────────┐
        │ map-server   │
        │ เริ่มทำงาน    │
        └──────┬───────┘
               │
               ▼
   อ่าน conf/plugins.conf
               │
               ▼
   สำหรับแต่ละ path:
       dlopen / LoadLibrary
       เรียก plugin_info()      ← คืน metadata
       เรียก plugin_init(api)   ← ลงทะเบียน hook / command
                │
                ▼
       ┌──── server รัน ────┐
       │                    │
       │   hooks ทำงาน      │
       │   commands ถูกเรียก│
       │   packets ถูก dispatch │
       │                    │
       └────────────────────┘
                │
                ▼
        server ปิด
                │
                ▼
   สำหรับทุกปลั๊กอินที่โหลด:
       เรียก plugin_final()     ← คืน state ที่ปลั๊กอินจองไว้
       dlclose / FreeLibrary
```

ก่อนปิด DLL แต่ละตัว plugin manager จะ:

- เคลียร์ `@command` ทุกตัวที่ปลั๊กอินลงทะเบียนไว้ ป้องกันไม่ให้
  มีการ lookup ไปยัง memory ที่ถูก free แล้ว
- เคลียร์ packet handler ทุกตัวที่ปลั๊กอินลงทะเบียนไว้ใน `packet_db`
- ลบ token ของ script ที่ค้างอยู่ทุกตัว เพื่อให้ `resume()`
  ที่หลงเหลือเป็น no-op ที่ปลอดภัย

โดยทั่วไปคุณแค่ต้องลบ hook ของตัวเองใน `plugin_final()`

### สัญลักษณ์ที่ต้อง export

```cpp
static plugin_info_t info = {
    "My Plugin",                 // name
    "Author",                    // author
    "1.0.0",                     // version
    "อะไรที่ปลั๊กอินนี้ทำ"          // description
};

PLUGIN_API plugin_info_t* plugin_info() { return &info; }

static plugin_api_t* g_api = nullptr;

PLUGIN_API bool plugin_init(plugin_api_t* api) {
    g_api = api;
    // … ลงทะเบียน hook, command, packet …
    return true;
}

PLUGIN_API void plugin_final() {
    // … ลบ hook ที่ลงทะเบียนไว้ …
}
```

---

## Hooks

Hook ช่วยให้ปลั๊กอินสังเกต — และอาจ veto — gameplay event ได้

### การ subscribe

```cpp
api->hook_add(HOOK_PC_LOGIN, on_login, /*user_data=*/nullptr, /*priority=*/100);
```

priority น้อยกว่ารันก่อน หลายปลั๊กอินสามารถ subscribe hook เดียวกันได้
และจะถูกเรียกตามลำดับ priority

### Signature ของ callback

```cpp
static int on_login(void* data, void* user_data) {
    auto* d = static_cast<plugin_pc_login_t*>(data);
    // … ใช้ d->sd …
    return HOOK_CONTINUE;        // หรือ HOOK_STOP เพื่อ veto
}
```

`HOOK_STOP` จะมีผลกับ hook ที่ fire *ก่อน* action เช่น
`HOOK_PC_DEAD`, `HOOK_ITEM_USE`, `HOOK_TRADE_REQUEST` ส่วน hook
แบบ informational เช่น `HOOK_PC_BASELEVELUP` จะไม่สนใจค่า return

### รายการ hook

| หมวด        | Hook                                                            | Veto ได้? |
|------------|-----------------------------------------------------------------|:--------:|
| Mob        | `HOOK_MOB_KILL`, `HOOK_MOB_SPAWN`                               |    ·     |
| ผู้เล่น     | `HOOK_PC_LOGIN`, `HOOK_PC_LOGOUT`                               |    ·     |
|            | `HOOK_PC_BASELEVELUP`, `HOOK_PC_JOBLEVELUP`                     |    ·     |
|            | `HOOK_PC_DEAD`                                                  |    ✓     |
| Inventory  | `HOOK_ITEM_USE`, `HOOK_ITEM_PICKUP`, `HOOK_ITEM_DROP`, `HOOK_ITEM_EQUIP` | ✓ |
| สกิล        | `HOOK_SKILL_USE`                                                |    ✓     |
| แชต         | `HOOK_PC_CHAT`, `HOOK_PC_WHISPER`, `HOOK_PC_PARTYCHAT`, `HOOK_PC_GUILDCHAT` | ✓ |
| NPC        | `HOOK_NPC_CLICK`                                                |    ✓     |
| เทรด       | `HOOK_TRADE_REQUEST`, `HOOK_TRADE_COMMIT`                       |    ✓     |
| ปาร์ตี้      | `HOOK_PARTY_CREATE`, `HOOK_PARTY_LEAVE`                         |    ✓     |
| กิลด์        | `HOOK_GUILD_CREATE`, `HOOK_GUILD_JOIN`, `HOOK_GUILD_LEAVE`      |   ✓*    |
| Status     | `HOOK_STATUS_CHANGE_START`, `HOOK_STATUS_CHANGE_END`            |   ✓*    |
| แผงค้า      | `HOOK_VENDING_OPEN`, `HOOK_VENDING_BUY`                         |    ✓     |
| Storage    | `HOOK_STORAGE_OPEN`                                             |    ✓     |
| เควสต์      | `HOOK_QUEST_ADD`, `HOOK_QUEST_COMPLETE`                         |   ✓*    |
| Companion  | `HOOK_PET_BORN`, `HOOK_PET_CATCH`, `HOOK_HOMUN_CALL`, `HOOK_HOMUN_LEVELUP` | ✓* |
| Commands   | `HOOK_ATCMD_EXECUTE`                                            |    ·     |

*✓\* = veto ได้เฉพาะ event ตัวแรกในแต่ละแถว ดูรายละเอียด struct
และความหมายที่แน่ชัดได้ใน [`plugin.hpp`](../src/map/plugin.hpp)*

---

## API Surface

ทุก sub-struct ใน `plugin_api_t` จัดกลุ่ม operation ที่เกี่ยวข้องกัน
ดู declaration ฉบับเต็มได้ที่
[`src/map/plugin.hpp`](../src/map/plugin.hpp)

### `script` — ตัวช่วย script state

ใช้ภายใน script command ที่ลงทะเบียนผ่าน `script_addcommand`

| Function | หน้าที่ |
|---|---|
| `hasdata(st, n)` | argument `n` มีค่าไหม? |
| `getnum(st, n)` | อ่าน argument `n` เป็น integer |
| `getstr(st, n)` | อ่าน argument `n` เป็น string |
| `pushint(st, v)` / `pushstr(st, s)` | push ค่า return |
| `rid2sd(st)` | คืน session ของผู้เล่นที่ผูกอยู่ หรือ `nullptr` |
| `suspend(st)` | หยุด script ไว้, คืน opaque token (ดู [Async script command](#async-script-command)) |
| `resume(token)` | รัน script ที่หยุดไว้ต่อ; no-op ถ้า token ไม่ valid แล้ว |

### `pc` — การจัดการผู้เล่น

| Function | หน้าที่ |
|---|---|
| `message(fd, msg)` | ส่งข้อความ chat ไปยังผู้เล่น 1 คน |
| `additem(sd, &it, amount, log_type)` | ให้ของ |
| `delitem(sd, idx, amount, type, reason, log_type)` | เอาของออก |
| `gainexp(sd, src, base, job, flag)` | ให้ EXP |
| `payzeny(sd, z, log_type)` / `getzeny(sd, z, log_type)` | หัก / เพิ่มเงิน |
| `setpos(sd, mapindex, x, y, clrtype)` | warp |
| `get_fd / get_aid / get_name` | ข้อมูล session |
| `get_blv / get_jlv / get_mapid / get_pos_x / get_pos_y` | ข้อมูล stat |
| `as_bl(sd)` | cast เป็น `block_list*` |

### `mob` — มอนสเตอร์

| Function | หน้าที่ |
|---|---|
| `once_spawn(...)` | spawn มอน N ตัวที่ตำแหน่ง |
| `get_id / get_x / get_y / get_name` | accessor ของ mob |

### `map` — แมพ / iteration

| Function | หน้าที่ |
|---|---|
| `name2id(name)` / `id2name(idx)` | แปลง index แมพ ↔ ชื่อ |
| `id2sd(id)` / `charid2sd(cid)` / `nick2sd(name, allow_partial)` | หา session |
| `foreachinmap(cb, user, m, type_mask)` | iterate ทุก block-list ตามชนิดในแมพ `m` |
| `foreachinarea(cb, user, m, x0, y0, x1, y1, type_mask)` | เหมือนกัน แต่ในกรอบสี่เหลี่ยม |
| `get_mapflag(m, flag)` | อ่าน `e_mapflag` |

`type_mask` คือ bitmask: `1 << PLUGIN_BL_PC` สำหรับผู้เล่น,
`1 << PLUGIN_BL_MOB` สำหรับมอน, ฯลฯ

### `status` — HP / SP

| Function | หน้าที่ |
|---|---|
| `heal(bl, hp, sp, flag)` | ฟื้น HP/SP |
| `damage(src, target, hp, sp, walkdelay, flag, skill_id)` | ทำดาเมจ |

### `bl` — Block list

| Function | หน้าที่ |
|---|---|
| `get_type(bl)` | เทียบกับ `e_plugin_bl_type` (`PLUGIN_BL_PC`, `PLUGIN_BL_MOB`, …) |
| `as_sd(bl)` | cast เป็น `map_session_data*` ถ้าเป็นผู้เล่น มิฉะนั้น `nullptr` |

### `item_api` — Item & Item DB

| Function | หน้าที่ |
|---|---|
| `get_nameid(item*)` | อ่าน nameid จาก struct `item` |
| `db_exists(nameid)` | id นี้มีอยู่ไหม? |
| `db_get_name(nameid)` | ชื่อภายใน (`Apple`) |
| `db_get_ename(nameid)` | ชื่อแสดงผล (`Apple`) |
| `db_get_type(nameid)` | `IT_HEALING`, `IT_USABLE`, … |

### `atcmd` — @command แบบกำหนดเอง

```cpp
api->atcmd.register_cmd("myhello", /*level=*/0, on_atcmd_myhello);
```

`level` คือ group ID ขั้นต่ำที่จำเป็นในการรัน command

### `quest` — เควสต์

| Function | หน้าที่ |
|---|---|
| `add(sd, quest_id)` | เพิ่มเข้า quest log |
| `update_status(sd, quest_id, state)` | `Q_INACTIVE / Q_ACTIVE / Q_COMPLETE` |
| `check(sd, quest_id, type)` | `HAVEQUEST / PLAYTIME / HUNTING` |

### `npc` — NPC events

| Function | หน้าที่ |
|---|---|
| `event(sd, "NpcExname::OnLabel", ontouch)` | trigger event label ของ NPC |

### `skill` — สกิล

| Function | หน้าที่ |
|---|---|
| `get_lv(sd, skill_id)` | ระดับสกิลที่ผู้เล่นมี |
| `use_id(sd, skill_id, lv, target_id)` | ร่ายสกิลใส่เป้าหมาย |
| `get_name(skill_id)` | ชื่อ AEGIS (`MG_FIREBOLT`) |
| `get_inf(skill_id)` | flag `INF_*` |
| `name2id(name)` | reverse lookup |

### `storage` — กล่องเก็บของ

| Function | หน้าที่ |
|---|---|
| `open(sd)` | เปิดกล่องส่วนตัวของผู้เล่น |

### `clif` — Packet สำเร็จรูป

ฟังก์ชันส่ง packet สำเร็จรูปสำหรับ effect ทั่วไปที่ส่งให้ client

| Function | หน้าที่ |
|---|---|
| `displaymessage(fd, msg)` | ข้อความ chat 1 บรรทัด |
| `emotion(bl, emote)` | bubble emote (ดู `emotion_type`) |
| `specialeffect(bl, id, target)` / `specialeffect_single(bl, id, fd)` | visual effect |
| `progressbar(sd, color, seconds)` / `progressbar_abort(sd)` | cast bar |
| `broadcast(bl, msg, type, target)` | ประกาศทั้ง server / map |
| `messagecolor(bl, color, msg, rgb2bgr, target)` | chat สี |

ค่า `color` เป็น `0xRRGGBB` ส่วน `target` ตรงกับ `enum send_target`:
`ALL_CLIENT=0`, `AREA=2`, `SELF=24`, …

### `timer` — Timer

| Function | หน้าที่ |
|---|---|
| `gettick()` | tick ปัจจุบัน (ms) |
| `add_timer(when_tick, func, id, data)` | one-shot |
| `add_timer_interval(when_tick, func, id, data, interval_ms)` | ซ้ำ |
| `delete_timer(tid, func)` | ยกเลิก |

callback ของ timer มี signature
`int32_t (*)(int32_t tid, int64_t tick, int32_t id, intptr_t data)`

### `log` — Logging

wrapper ของ `ShowInfo`/`ShowStatus`/`ShowWarning`/`ShowError`/`ShowDebug`
format string เองก่อน (เช่นใช้ `snprintf`) แล้วส่งผลลัพธ์ไป —
format string แบบ variadic ไม่ปลอดภัยข้าม DLL boundary

### `packet` — Packet แบบกำหนดเอง

ลงทะเบียน handler บน packet ID ที่ยังไม่ใช้ และ push packet ออกไปยัง client

| Function | หน้าที่ |
|---|---|
| `register_handler(cmd, length, func)` | map packet ขาเข้าไปยัง handler ของคุณ ใช้ `length=-1` สำหรับ variable-length |
| `unregister_handler(cmd)` | ลบ handler |
| `read_b/w/l(fd, off)` | อ่านค่าจาก recv buffer |
| `read_str(fd, off)` / `read_rest(fd)` | pointer string / จำนวน byte ที่เหลือ |
| `send_self(fd, buf, len)` | push packet ที่ประกอบเสร็จแล้วไปยัง 1 fd |
| `send_target(bl, buf, len, target)` | broadcast ผ่าน `clif_send` |

signature ของ handler คือ
`void (*)(int32_t fd, struct map_session_data* sd)` โดย `sd`
อาจเป็น `nullptr` สำหรับ packet ก่อน login

> **เลือก ID ที่ยังไม่ใช้** ช่วงที่ valid คือ `0x064`–`0xCFF`
> ตรวจให้แน่ใจว่า ID ไม่ชนกับสิ่งที่ client build ของคุณใช้อยู่แล้ว

---

## Pattern การใช้งาน

### Script command แบบกำหนดเอง

```cpp
// plugin_hello "<name>";  →  print log, return "hello!"
static int32_t buildin_plugin_hello(script_state* st) {
    const char* name = g_api->script.getstr(st, 2);
    char buf[128];
    snprintf(buf, sizeof(buf), "[plugin] hello, %s", name ? name : "world");
    g_api->log.info(buf);
    g_api->script.pushstr(st, "hello!");
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}

// ใน plugin_init:
api->script_addcommand("plugin_hello", "s", buildin_plugin_hello);
```

arg-string ใช้รูปแบบเดียวกับ rAthena: `s` = string, `i` = integer,
`?` = optional, `*` = variadic ฯลฯ

### @command แบบกำหนดเอง

```cpp
static int32_t on_atcmd_heal(map_session_data* sd, const char* cmd, const char* msg) {
    g_api->status.heal(g_api->pc.as_bl(sd), 99999, 99999, 0);
    g_api->pc.message(g_api->pc.get_fd(sd), "Fully healed.");
    return 0;
}

// ใน plugin_init:
api->atcmd.register_cmd("plugin_heal", /*level=*/0, on_atcmd_heal);
```

### Async script command

Script command ของปลั๊กอินสามารถพักการทำงานแล้วกลับมารันต่อภายหลังได้
เหมาะกับการรอ timer, network call หรือ event ภายนอก

```cpp
static int32_t on_tick(int32_t, int64_t, int32_t, intptr_t data) {
    void* token = reinterpret_cast<void*>(data);
    auto* st = static_cast<script_state*>(token);

    g_api->script.pushint(st, 42);     // push ค่า return ก่อน
    g_api->script.resume(token);       // ค่อย resume
    return 0;
}

static int32_t buildin_plugin_async(script_state* st) {
    void* token = g_api->script.suspend(st);
    g_api->timer.add_timer(g_api->timer.gettick() + 1000,
                           on_tick, 0, reinterpret_cast<intptr_t>(token));
    return PLUGIN_SCRIPT_CMD_SUCCESS;
}
```

ผู้เล่นจะยังถูกผูกกับ NPC ระหว่างที่ script ค้างไว้ (ตรงกับ semantics
ของ `close` / `select`) ถ้าผู้เล่น logout หรือ NPC ถูก reload, engine
จะ free state ให้อัตโนมัติ และ `resume()` ที่ตามมาจะกลายเป็น no-op

### Packet client แบบกำหนดเอง

```cpp
static constexpr uint16_t MY_PACKET = 0x0CFE;     // เลือกค่าที่ยังไม่ใช้

static void on_my_packet(int32_t fd, map_session_data* sd) {
    if (!sd) return;
    uint32_t value = g_api->packet.read_l(fd, 2);  // ข้าม 2-byte cmd

    // Echo กลับเป็น value+1 บน packet 0x0CFF
    struct __attribute__((packed)) { uint16_t cmd; uint32_t v; } reply;
    reply.cmd = 0x0CFF;
    reply.v   = value + 1;
    g_api->packet.send_self(g_api->pc.get_fd(sd), &reply, sizeof(reply));
}

// ใน plugin_init:
api->packet.register_handler(MY_PACKET, /*length=*/6, on_my_packet);
```

---

## Example Plugin

ไฟล์ [`plugins/example/example_plugin.cpp`](example/example_plugin.cpp)
ที่มาพร้อมกับโปรเจกต์สาธิตทุกหมวด ที่น่าสนใจ:

| Script command | สาธิต |
|---|---|
| `plugin_hello "x"` | command เบื้องต้น + ค่า return |
| `plugin_give_item id, n` | `pc.additem` |
| `plugin_spawn_mob id, n` | `mob.once_spawn` |
| `plugin_warp "map", x, y` | `pc.setpos` |
| `plugin_announce "msg"` | `clif.broadcast` |
| `plugin_count_mobs` | `map.foreachinmap` |
| `plugin_delayed_give id, secs` | `timer` + `clif.progressbar` |
| `plugin_async_roll(max)` | `script.suspend` / `script.resume` |
| `plugin_send_ping` | packet ขาออกแบบกำหนดเอง |

นอกจากนี้ยังลงทะเบียน handler สำหรับ packet `0x0CFE` ขาเข้าและส่ง
reply กลับใน `0x0CFF`

ทดลอง:

1. ลบเครื่องหมาย comment ในบรรทัดนี้ของ `conf/plugins.conf`:
   ```text
   plugins/example/example_plugin
   ```
2. `make plugin`
3. รีสตาร์ท map-server

---

## เคล็ดลับและการแก้ปัญหา

- **หา symbol ไม่เจอ / ขาด export** — ตรวจให้แน่ใจว่า export ทั้ง 3
  ใช้ `PLUGIN_API` (ซึ่งจะ expand เป็น
  `extern "C" __attribute__((visibility("default")))` บน Linux และ
  `__declspec(dllexport)` บน Windows)
- **โหลดปลั๊กอินได้ แต่ command ไม่ทำงาน** — ตรวจสอบว่า path ใน
  `conf/plugins.conf` เป็น relative กับ working directory ของ
  map-server และไม่มีนามสกุลไฟล์ (loader จะเติมให้) ดู log ตอน start
  ว่ามี `plugin: Loaded ...` ไหม
- **Crash ตอนปิด server** — มักเกิดจาก hook callback ที่ไม่ได้ลบใน
  `plugin_final()` ตรวจให้ทุก `hook_add` มีคู่ `hook_remove`
- **`printf` แบบ variadic จากปลั๊กอินแสดงผลเพี้ยน** — ใช้ sub-struct
  `log` แทน และ format string เองก่อน อย่าส่ง format specifier
  ข้าม DLL boundary
- **Packet ID ชนกัน** — `register_handler` จะเขียนทับสิ่งที่อยู่ใน
  `packet_db[cmd]` เลือกค่าที่ client ของคุณไม่ได้ใช้
- **Hot reload** — `plugin_manager_init` กับ `plugin_manager_final`
  เป็น entry point เดียว build ปัจจุบันยัง reload ได้เฉพาะตอนรีสตาร์ท
  server เท่านั้น

---

ดู ABI ฉบับเต็ม, struct ของ hook payload และ constant ทั้งหมดได้ที่
[`src/map/plugin.hpp`](../src/map/plugin.hpp)
