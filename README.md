# mod-mskill

An [AzerothCore](https://www.azerothcore.org/) module that adds an **`mskill`**
party / raid / whisper command for controlling the professions and class skills
of [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) bots.

It lets you ask your bots which professions they know, batch-learn everything a
profession or class trainer would teach them, and drop a profession you no
longer want — all from chat, without dragging each bot to a trainer.

---

## Requirements

* A working AzerothCore (WotLK 3.3.5a) server built from the
  **mod-playerbots fork** of the core
  (`mod-playerbots/azerothcore-wotlk`, `Playerbot` branch).
* The **[mod-playerbots](https://github.com/mod-playerbots/mod-playerbots)**
  module installed and working. This module depends on it: the `mskill`
  command only acts on characters that mod-playerbots is currently controlling
  as bots.
* Modules built with **static** linkage (the AzerothCore default). Because
  this module references mod-playerbots symbols directly, a fully dynamic
  module build can fail to link; if you build with `-DMODULES=dynamic` and hit
  unresolved-symbol errors, switch back to static.

---

## Commands

All commands are issued from chat. In **party / raid** chat they apply to every
bot in your group; as a **whisper** they apply only to the bot you whisper.
Each bot replies privately to you, prefixed with its name.

| Command | Channel | Effect |
| --- | --- | --- |
| `mskill` | party / raid / whisper | Each bot reports every profession it knows and its current skill level (e.g. `Mining: 285/300`). |
| `mskill profession learn` | party / raid / whisper | Each bot learns every spell its **already-known** professions' trainers would teach it, up to the bot's current profession level. Never learns anything gated above skill **300**, and never raises a profession's cap above **300**. |
| `mskill profession drop <name>` | **whisper only** | The whispered bot unlearns the named profession and all of its associated spells. |
| `mskill class learn` | party / raid / whisper | Each bot learns every available spell from its class trainer (class/race/level gated by the core). |

Examples:

```
mskill
mskill profession learn
mskill class learn
/w Lyndara mskill profession drop mining
/w Lyndara mskill profession drop first aid
```

Accepted profession names for `drop` (case-insensitive, common short forms
allowed): `alchemy`, `blacksmithing` (`blacksmith`), `enchanting` (`enchant`),
`engineering` (`engineer`), `herbalism` (`herb`), `inscription` (`scribe`),
`jewelcrafting` (`jc`), `leatherworking` (`lw`), `mining`, `skinning`,
`tailoring` (`tailor`), `cooking` (`cook`), `first aid` (`firstaid`),
`fishing`.

### Behaviour notes

* **`profession learn` only advances professions the bot already has.** It will
  not hand a bot a brand-new profession. Give the bot the profession first
  (e.g. at a trainer or via another tool), then run `mskill profession learn`
  to fill in the recipes/ranks it qualifies for.
* The **300 skill cap is a hard ceiling**. `Mskill.MaxProfessionSkill` can lower
  it, but values above 300 are clamped back to 300.
* Class learning leaves the core's normal class/race/level checks in place, so a
  bot only ever learns spells appropriate for its class and level.

---

## Configuration

After installing, copy / rename the dist config and edit if desired:

```
# from your runtime conf directory (where worldserver.conf lives)
cp mod_mskill.conf.dist mod_mskill.conf
```

| Setting | Default | Description |
| --- | --- | --- |
| `Mskill.Enable` | `1` | Master on/off switch for the module. |
| `Mskill.MaxProfessionSkill` | `300` | Cap used by `mskill profession learn`. Hard-limited to 300. |

---

## Installation

This module follows the standard AzerothCore
[module directory structure](https://www.azerothcore.org/wiki/directory-structure)
and is installed the same way as any other module
([installing a module](https://www.azerothcore.org/wiki/installing-a-module)).

1. Clone the repository into your AzerothCore source `modules/` directory:

   ```bash
   cd /path/to/azerothcore/modules
   git clone https://github.com/<your-account>/mod-mskill.git
   ```

   > If you download a ZIP instead, extract it into `modules/` and make sure the
   > folder is named exactly `mod-mskill` (remove any `-master`/`-main` suffix).

2. Re-run CMake to detect the new module, then rebuild the core:

   ```bash
   cd /path/to/azerothcore/build
   cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/azerothcore/env/dist/
   make -j$(nproc)
   make install
   ```

   On Windows, re-configure / re-generate in CMake, then build the
   `ALL_BUILD` target in Visual Studio.

   You can confirm the module was picked up in the CMake output under
   `* Modules configuration`, and in-game with `.server debug`.

3. Copy the config into your runtime conf directory and adjust if needed:

   ```bash
   cd /path/to/azerothcore/env/dist/etc
   cp mod_mskill.conf.dist mod_mskill.conf
   ```

4. Start `worldserver`. No SQL import is required by this module.

---

## Updating

```bash
cd /path/to/azerothcore/modules/mod-mskill
git pull

cd /path/to/azerothcore/build
cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/azerothcore/env/dist/
make -j$(nproc)
make install
```

After updating, re-check `mod_mskill.conf.dist` for any new settings and merge
them into your `mod_mskill.conf`. Restart `worldserver` to apply.

---

## How it works

The module registers a `PlayerScript` that listens on the group-chat and
private-chat hooks (`PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT` and
`PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT`). When a real player's message begins
with `mskill`, the module resolves the target bots (group members, or the
whispered bot), confirms each is a mod-playerbots bot via
`PlayerbotsMgr::GetPlayerbotAI`, and performs the requested action using the
core's `Trainer` data and the player skill API. The original chat message is
never blocked, so normal chat keeps working.

Profession and class spells are taught by iterating the relevant trainer
templates (cached on first use) and applying the core's own
`Trainer::CanTeachSpell` validation, mirroring how mod-playerbots itself learns
trainer spells — with added guards to honour the 300 cap and to avoid granting
new professions.

---

## Directory structure

```
mod-mskill/
├── conf/
│   └── mod_mskill.conf.dist        # module configuration
├── data/
│   └── sql/
│       └── db-world/               # reserved (no SQL needed)
├── src/
│   ├── mskill.cpp                  # module logic + script registration
│   └── mskill_loader.cpp           # Addmod_mskillScripts entry point
├── .editorconfig
├── .gitignore
├── LICENSE
└── README.md
```

---

## License

Released under the GNU AGPL v3, consistent with AzerothCore. See
[`LICENSE`](LICENSE).
