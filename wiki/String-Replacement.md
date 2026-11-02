# String Replacement

Dump the game's text to TOML and replace it with your own, for translations or text mods. Source: `src/mc_engine/string_dumper.h` and `.cpp`, `src/mc_engine/string_table.cpp`.

## Options

| Option | Type | Default | Effect |
|---|---|---|---|
| `string_dump_enabled` | bool | `false` | Capture encountered strings to TOML. Hot reload. |
| `string_replace_enabled` | bool | `false` | Inject replacement strings from disk when available. Hot reload. |
| `string_folder` | string | `""` | Override the strings folder. Empty means `strings/` under the game data folder. Needs a restart. |
| `string_language` | string | `""` | Language code for per-language replacements, for example `pt_BR` or `es_ES`. Hot reload. |

Source of all four: `src/mc_engine/string_dumper.cpp`. When the SDK in use provides its own dumper with the same cvar names, those are used instead; the file formats are the same.

## Folder layout

```text
<strings folder>/
  dump/strings.toml        what was seen
  replace/*.toml           what to show instead
```

Only files with the `.toml` extension in `replace/` are read.

## Dump format

Entries are appended to `dump/strings.toml`. A string already in the file is not written again. The file starts with a comment saying to edit strings in `replace/`, not in the dump.

```toml
[[strings]]
hash = "0123456789abcdef"
context = "..."
original = "..."
address = "0x82000000"
```

`address` is written only when known. The `hash` is FNV-1a over the context, a separator and the text, as 16 hex digits; replacements are matched by recomputing it from `context` and `original`.

## Replacement format

Each file in `replace/` holds a `[[strings]]` array. An entry needs `context` and `original` exactly as in the dump, plus one of:

```toml
# every language
[[strings]]
context = "..."
original = "Hello"
text = "Ola"

# per language, chosen by string_language
[[strings]]
context = "..."
original = "Hello"
[strings.pt_BR]
text = "Ola"
```

Rules from the code:

- A plain `text` field applies in every language.
- Otherwise the sub-table named after `string_language` is used. With `string_language` empty, only plain `text` entries load.
- Entries without `context` or `original` are skipped.
- A file that fails to parse is skipped with `StringDumper: failed to parse <file>`; other files still load.

Log lines: `StringDumper: N replacement(s) indexed from <dir>`, `loaded N replacement(s) from <file>`.

## Not documented in the source

- Which `context` values exist; they are what the dump records.
- Whether replacement text may be longer than the original; the code does not state a limit here.
