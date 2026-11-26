# The contract between the game and the launcher

The game says things about itself in JSON files, and the launcher reads them instead of hard-coding them. The files, their schemas
(`schemas/`), golden examples (`examples/`) and the validator (`validate_contract.py`) live here.

| File | Written by | Schema |
| --- | --- | --- |
| `launcher-recipe.json` (repo root) | by hand | `schemas/launcher-recipe.schema.json` |
| `cmake/deps.json` | by hand | `schemas/deps.schema.json` |
| `install-manifest.json` (next to `larecomp.exe`) | CMake at configure time | `schemas/install-manifest.schema.json` |
| `larecomp.exe --describe` (stdout) | CMake at configure time, embedded in the exe | `schemas/describe.schema.json` |
| `settings-ui.json` (`--dump_settings_ui`) | the exe | `schemas/settings-ui.schema.json` |
| `machine.json` (`--probe_machine`, and every start) | the exe | `schemas/machine.schema.json` |

Check files (exit 0 means no errors; `--strict` also fails on warnings):

    python -I documentation/contract/validate_contract.py --strict launcher-recipe.json documentation/contract/examples/*.json
    python -I documentation/contract/validate_contract.py --selftest

## Compatibility rules for launchers that are older than the game

A player can have a launcher that is older than the game version it is asked to set up or run. Two top-level fields in every contract
file say what an older launcher must do:

- `min_reader` (integer) is the contract-format gate: the oldest contract reader that renders the file correctly. A launcher whose
  reader version is lower stops and says to update the launcher.
- `min_launcher` (optional string, 1 to 4 dot-separated non-negative integers, for example `"1.0.0"` or `"1.336.600"`) is the program
  gate: the oldest launcher program (the app the player runs) that can set up or run this game version. A launcher whose own version is
  lower must not build or update to this game version. It shows a "get the newer launcher" screen and keeps the install it has. The
  file having no `min_launcher` means no requirement. A launcher that cannot parse the value ignores it.

The rules:

1. **Bump `min_launcher` only when a game change needs launcher code** (a new setup action, a new launcher feature the game relies
   on), and set it to the version of the first launcher that has that code. A game change that an old launcher already handles does
   not raise it.
2. **Bump `min_reader` only for a format break** (a renamed or removed field, a changed meaning of an existing field). Adding a field
   is not a break.
3. **Unknown fields MUST be ignored by readers.** The schemas do not forbid extra properties, and a reader never fails on one.
4. **An unknown action in `install-manifest.json`:** an action is optional when it has `"never_fails_setup": true` (every action
   today does). A launcher that does not know how to run an optional action (an unknown stdout `grammar`, an unknown fact in its
   `when`) MUST skip it and write one log line saying which action and why. A launcher that cannot run an action that is not
   optional is too old: it stops with the "get the newer launcher" screen and keeps the install it has.
5. **The value is the launcher's own version scheme**, `<major>.<days since 2026-01-01>.<minute of day>` (for example `1.332.480`).
   Versions are compared component by component as numbers, and a missing component counts as 0. `launcher-recipe.json` at the
   repo root is the only place to edit it: change its `min_launcher` and nothing else.
   - `install-manifest.json` and the `--describe` text take it from the recipe when CMake configures
     (`larecomp_recipe_min_launcher` in `cmake/InstallManifest.cmake`); a value that is not 1 to 4 numbers separated by dots stops the
     configure with a plain message.
   - `settings-ui.json` and `machine.json` take it from the same CMake value (`settings_ui_data.h` and a compile definition of
     `machine_facts.cpp`).
   - `cmake/deps.json` is read by hand-written CMake and is not generated: it carries a copy. `cmake/tests/install_manifest_test.py`
     fails when the copy differs from the recipe, so a bump that forgets it is caught; copy the value there.
   - The files in `examples/` are snapshots and carry the value of `examples/launcher-recipe.example.json`; they only change when the
     contract's own examples change. `validate_contract.py` fails when the files of one run (the recipe among them) disagree.
6. **A launcher reads `launcher-recipe.json` of a target commit before it checks that commit out** (`git show <commit>:launcher-recipe.json`),
   to apply the two gates above. So the recipe stays small, valid JSON, and `min_launcher` and `min_reader` stay top-level keys near
   the top of the file.

Which place the newer launcher comes from is not part of the contract; the launcher shows its own.
