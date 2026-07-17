# `sce_sys/sdslot.dat` layout

Vita retail saves keep per-slot metadata (the title/subtitle/description and last-modified time the
system shows in the LiveArea save browser) in `sce_sys/sdslot.dat` inside the save folder. Save
Keeper reads this file to name backups after the real save time and to show per-slot details.
`parse_sdslot_data` in `src/core/SaveSlotMetadata.cpp` is the single decoder; the constants there
are the source of truth and this document explains them.

## Structure

- `0x000` magic `"SDSL"` (`0x4c534453`, little-endian). A file that does not start with it is not
  a slot file and is ignored.
- `0x200` active-slot table: one byte per slot index, non-zero when that slot holds a save. Up to
  256 slots.
- `0x400` header size; each slot record is `0x400` bytes, so slot `n` starts at `0x400 + n*0x400`.

Within a slot record (offsets relative to the record start):

| Offset | Size    | Field                                                    |
| ------ | ------- | -------------------------------------------------------- |
| `0x04` | `0x40`  | title (UTF-8, NUL-terminated, may fill the whole field)  |
| `0x44` | `0x80`  | subtitle                                                 |
| `0xc4` | `0x200` | details / description                                    |
| `0x30c`| 12      | modified time: six little-endian `uint16` fields, `year, month, day, hour, minute, second` |

Strings that fill their whole capacity with no terminator are truncated at capacity rather than
treated as corruption, so a maxed-out title never discards an otherwise-valid slot.

## Time zone (load-bearing assumption)

The six date fields are **UTC calendar values**, not device-local. This was determined empirically
by saving in-game at a known local time and comparing against the raw bytes: the stored value was
ahead of the wall clock by the console's UTC offset. `parse_sdslot_data` converts each slot time
from UTC to device-local exactly once, at the parser boundary (`local_datetime_from_utc`), so every
downstream consumer - backup file names, the JSON sidecar, list rows, and the details screen - sees
consistent local wall-clock time.

If this assumption is ever wrong for some firmware or region, the symptom is uniform: every
displayed save time is shifted by the local UTC offset. Version 1 metadata sidecars were written
before this was understood; `parse_save_metadata_json` migrates them (see `kSaveMetadataJsonVersion`).
