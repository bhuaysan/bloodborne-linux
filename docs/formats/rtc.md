# libSceRtc dates and ticks

What the eboot's ten libSceRtc imports do, worked out only from the eboot's own call sites. Under the earlier policy the library was in its exposure log ([clean-room.md](../clean-room.md#exposure-log)), so this note and the code (`src/hle/system/date_time.{hpp,cpp}`) give NIDs and neutral descriptions, not function names; #226 lifts that. All imports are called by the eboot only. Addresses are eboot link addresses.

Facts the code shows are marked with their addresses. Where the code doesn't show something, the value is **our choice**, marked as such.

## The date

A 16-byte struct. The game builds one at 0x20ee3c5–0x20ee3e0 and reads one back at 0x20ee23d–0x20ee302; a 16-byte `memcmp` at 0x20ee520 compares two.

| Offset | Size | Field | Evidence |
|---|---|---|---|
| 0 | 2 | year, full (e.g. 2015) | the reader accepts `year - 0x641 <= 0x9be`, years 1601..4095 (0x20ee240) |
| 2 | 2 | month, 1..12 | months are normalised to 1..12 with a carry into the year (0x20ee368–0x20ee3a7) |
| 4 | 2 | day, from 1 | the builder starts from day 1 and adds `(day - 1) * 86400e6` (0x20ee3af, 0x20ee41d–0x20ee42a) |
| 6 | 2 | hour | the builder stores it at 6 (0x20ee3d2–0x20ee3dc) and scales it by 3,600,000,000 µs (0x20ee439–0x20ee46a); the reader packs it into bits 34+ (0x20ee2c8–0x20ee302) |
| 8 | 2 | minute | the builder stores it at 8 (0x20ee3d2–0x20ee3dc) and scales it by 60,000,000 µs (0x20ee439–0x20ee46a); the reader packs it into bits 39+ (0x20ee2c8–0x20ee302) |
| 10 | 2 | second | the builder stores it at 10 (0x20ee3d2–0x20ee3dc) and scales it by 1,000,000 µs (0x20ee439–0x20ee46a); the reader packs it into bits 45+ (0x20ee2c8–0x20ee302) |
| 12 | 4 | microsecond | the builder stores milliseconds × 1000, the reader divides by 1000 (0x20ee3c5–0x20ee3e0, 0x20ee23d–0x20ee302) |

The field order is also the order the game prints: `%04d%02d%02d%02d%02d%02d_%d_%04d` (0x493a7e3, used at 0x1fe5c56) and the save-name format at 0x493af66 (fed at 0x2ba0da1–0x2ba0df2).

## The tick

A u64 counting **microseconds**. The game takes a date's tick and adds days × 86,400,000,000, hours × 3,600,000,000, minutes × 60,000,000, seconds × 1,000,000 and milliseconds × 1,000 (0x20ee420–0x20ee4e5), then turns the sum back into a date (0x20ee535). A date's tick and the current tick share one scale and origin: the game subtracts one from the other and clamps a negative result to 0 (0xcb70d8–0xcb70e8). A tick of 0 means "no date" (0x145b77e).

The **epoch is not shown**: no epoch constant appears in the text segment. **Our choice:** 1600-01-01 00:00:00 UTC, the start of the 400-year calendar cycle that holds the lowest year the game accepts (1601, 0x20ee240). Every date the game handles has a positive tick, and 0 stays free. Dates are valid from the epoch to 9999-12-31; if ticks ever have to match another system's (for example to read data written elsewhere), the epoch must be revisited.

## The imports

| NID | Call sites | Behaviour |
|---|---|---|
| `ZPD1YOKI+Kw` | 64, e.g. 0x145b724, 0x1fe5ade, 0x20ee580 | `(date* out)`: the current date. 0 is success; on failure the game uses a zero value (0x145b72b). Which clock (UTC or local) is not shown; **our choice:** UTC |
| `8w-H19ip48I` | 66, the same sites plus 0xcb70bd, 0x20ee3f2 | `(const date*, u64* tick)`: date to tick. Non-zero is failure at 0x20ee3f9 |
| `ueega6v3GUw` | 0x20ee22d, 0x20ee4f5 | `(date* out, const u64* tick)`: tick to date; with `8w-H19ip48I` it round-trips, which the game checks (0x20ee4f5–0x20ee52a) |
| `bDEVVP4bTjQ` | 0xcb70a6 | `(date* out, u64 value)`: a base-10 integer from the network header `X-RateLimit-Next-Available` (0xcb6fd7, 0xcb708f) to a date. Its unit and epoch are not shown; **our choice:** seconds since 1970-01-01 00:00:00 UTC. The network is offline (#91), so it is not reached |
| `zO9UL3qIINQ` | 0xc86f50, 0xcb70cf | `(u64* tick)`: the current tick |
| `jMNwqYr4R-k` | 0xcb70ea | no arguments; a positive int the game divides tick deltas by before multiplying by 1000 (0xcb70fb–0xcb7105): ticks per second, 1,000,000, consistent with the microsecond tick |
| `M1TvFst-jrM` | 11, e.g. 0x20ee626, 0x2ba0d58 | `(const u64* in, u64* out)`: applied when bit 51 of the game's packed date is set; the result is packed with the bit cleared (0x20ee5f0–0x20ee690) |
| `8Yr143yEnRo` | 0x15696ae, 0x20ee6e2 | the same form, applied when bit 51 is clear; the result is packed with the bit set (0x20ee6a0–0x20ee730) |
| `CyIK-i4XdgQ` | 0x20ee2a9 | `(year, month, day)` to the day of the week, stored in 3 bits (0x20ee2a2–0x20ee2b1). Which value is the first day is not shown; **our choice:** 0 = Sunday to 6 = Saturday, as C's `tm_wday` |
| `AxHBk3eat04` | 0x1fe5ca9 | `(char* buffer, 0)`: date text for a signed upload request, copied after `PUT\n\ntext/txt\n` (0x493a822, 0x1fe5f6f–0x1fe5fb9) and later sent as the value of the `Date` header (string 0x493a83b, used at 0x1fe64ac with the buffer read at 0x1fe64c6–0x1fe64d0). On failure the game uses an empty default. **Our choice:** fail, since uploads need the network |

What `M1TvFst-jrM` and `8Yr143yEnRo` shift (a time zone, DST, something else) is not shown: no scaling or offset is visible around them. **Our choice:** the runtime keeps one time zone, UTC, so both copy the tick unchanged. If the game turns out to show local times, they are where a zone offset belongs.

## The game's packed date

For reference, the game packs a date into a quadword (stores at 0x20ee274–0x20ee302, loads at 0x1fe5b9e–0x1fe5be1): year bits 0–11, millisecond 12–21, month 22–25, day of week 26–28, day 29–33, hour 34–38, minute 39–44, second 45–50, and a flag in bit 51 that selects which of the two tick conversions applies.
