# HackOS IR Code File Formats for SD Card

This document describes how IR code files should be structured on the
SD card so that the StorageManager and IRToolsApp process them efficiently.

## Directory Layout

```
/ext/assets/ir/
├── tv_bgone.csv          # TV-B-Gone universal power-off database
└── saved/                # User-saved IR codes (DB Manager)
    ├── living_room_ac.csv
    ├── bedroom_tv.csv
    └── projector.csv
```

## TV-B-Gone Database (`tv_bgone.csv`)

CSV format with one IR code per line.  Comment lines start with `#`.

```
# brand,protocol_id,hex_code,bits
Samsung,7,0xE0E040BF,32
Sony,5,0xA90,12
LG,10,0x20DF10EF,32
```

### Fields

| Column       | Description                                          |
|------------- |------------------------------------------------------|
| brand        | Human-readable brand name (no commas)                |
| protocol_id  | IRremoteESP8266 `decode_type_t` numeric value        |
| hex_code     | IR code value in hexadecimal (with `0x` prefix)      |
| bits         | Bit length of the code (e.g. 12, 15, 20, 32, 48)    |

### Protocol IDs (common)

| ID | Protocol  |
|----|-----------|
| 3  | NEC       |
| 4  | RC5       |
| 5  | SONY      |
| 6  | RC6       |
| 7  | SAMSUNG   |
| 8  | JVC       |
| 9  | PANASONIC |
| 10 | LG        |
| 12 | SANYO     |
| 15 | SHARP     |

## User-Saved Codes (DB Manager)

Each saved code is stored as a single CSV file under `/ext/assets/ir/saved/`.
The file name is chosen by the user when saving.

```
# name,protocol_id,hex_code,bits
Power,7,0xE0E040BF,32
```

### Fields

| Column       | Description                                          |
|------------- |------------------------------------------------------|
| name         | User-assigned name for the signal                    |
| protocol_id  | IRremoteESP8266 `decode_type_t` numeric value        |
| hex_code     | IR code value in hexadecimal (with `0x` prefix)      |
| bits         | Bit length of the code                               |

## Processing Notes

- The app reads the CSV line-by-line using `fs::File::readBytesUntil`.
- Lines starting with `#` are skipped (comments/headers).
- Fields are split on `,` and parsed with `strtoul` (hex) / `strtol`.
- The TV-B-Gone function iterates all entries sequentially with a 200 ms
  delay between transmissions.
- Maximum line length is 128 characters.
