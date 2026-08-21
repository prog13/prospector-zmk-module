# The touch chip

Everything measured about the capacitive touch controller on the Waveshare 1.83" Touch LCD
(rev 2), the panel this module's `prospector_adapter` shield drives.

## Identity

The part is a **CST816D**, not the CST816S the Zephyr driver is named for.

| register | value | |
|---|---|---|
| `0xA7` ChipID | `0xB6` | `CST816S_CHIP_ID3` in the driver's header |
| `0xA8` ProjID | `0x27` | |
| `0xA9` FwVersion | `0x01` | |

Waveshare's own product page for the module names CST816D, and `0xB6` is that part. Two
consequences:

- The **CST816D datasheet has no register map at all** — twelve pages, none of them registers.
- The **CST816S register declaration is substantially correct for this part** and is the real
  map — its register addresses are right and every register tested was writable and readable at
  the documented address. But it does **not** describe this part in two ways, both measured: its
  stated *defaults* are wrong (see below), and one documented control, `MotionSlAngle` @0xEF, has
  no behaviour at all. Treat an address as reliable and a *behaviour* as unverified until tested.
- **It documents 27 registers, not the whole part.** `0x01`–`0x06`, `0xA7`–`0xA9`, `0xB0`–`0xB3`
  and `0xEC`–`0xFE`, plus 3 of `MotionMask`'s 8 bits. The factory scan below found four unnamed
  nonzero regions outside that range, so a register absent from the map may still exist.
- Do **not** trust `github.com/max22-/cst816d`. It writes MotionMask at `0xA9` (FwVersion) and
  "DisAutoSleep" at `0xFD` — which is IOCtl, where bit 0 is `En1v8` and switches the IIC/IRQ
  pins to 1.8 V levels on a 3.3 V system.

## Bus and wiring

| | |
|---|---|
| I2C address | `0x15` on `i2c1` (`cst816s@15`, `compatible = "hynitron,cst816s"`) |
| bus speed | **100 kHz** — no `clock-frequency` in the overlay, so `I2C_BITRATE_STANDARD` |
| controller | nRF **TWI**, not TWIM (`xiao_ble_common.dtsi`), so byte-interrupt driven |
| shares the bus with | `apds9960@39`, whose `sensor_sample_fetch` is compiled out unless `PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR` |
| IRQ | `irq-gpios` = xiao D0, `GPIO_ACTIVE_LOW | GPIO_PULL_UP` |
| reset | `rst-gpios` = xiao D1, `GPIO_ACTIVE_LOW` |
| report rate | **~80/s (12.45 ms period)**, measured; unaffected by enabling gestures |

The node ships `status = "disabled"` in the module overlay and is enabled by the config repo.

## Register reference

Every register the vendor documents, merged with a full `0x00`–`0xFF` read of an untouched chip.
The vendor text is tracked alongside this file as
[`cst816s-register-declaration.txt`](cst816s-register-declaration.txt) — 27 registers over 5 pages,
extracted verbatim from Waveshare's CST816S register declaration PDF. `—` in the factory column
means the register reads `00`.

Touch data — the driver reads these for you and emits Zephyr input events:

| addr | name | documented | factory |
|---|---|---|---|
| `0x01` | GestureID | `00` none, `01`–`04` slide up/down/left/right, `05` single click, `0B` double click, `0C` long press | — |
| `0x02` | FingerNum | 0 = no finger, 1 = one finger. **Single-touch part** | — |
| `0x03` | XposH | X coordinate, bits [11:8] | — |
| `0x04` | XposL | X coordinate, bits [7:0] | — |
| `0x05` | YposH | Y coordinate, bits [11:8] | — |
| `0x06` | YposL | Y coordinate, bits [7:0] | — |

Identity and calibration:

| addr | name | documented | factory |
|---|---|---|---|
| `0xA5` | PowerMode | not in the map; **never write** | — |
| `0xA7` | ChipID | | `b6` |
| `0xA8` | ProjID | | `27` |
| `0xA9` | FwVersion | | `01` |
| `0xB0`–`0xB1` | BPC0 H/L | 16-bit BPC0 value | `01 54` = 340 |
| `0xB2`–`0xB3` | BPC1 H/L | 16-bit BPC1 value | `01 6b` = 363 |

Configuration. **Every one of these is writable and reads back what you write** (verified on
`0xED` `0xEE` `0xEF` `0xFB` `0xFC`):

| addr | name | documented | factory |
|---|---|---|---|
| `0xEC` | MotionMask | `[2]` EnConLR, `[1]` EnConUD, `[0]` EnDClick. **Bits [7:3] undocumented** | `00` |
| `0xED` | IrqPluseWidth | IRQ low-pulse width. Unit 0.1 ms, range 1–200, default **10** | `01` |
| `0xEE` | NorScanPer | normal quick-scan period; affects LpAutoWakeTime and AutoSleepTime. Unit 10 ms, range 1–30, default **1** | `00` |
| `0xEF` | MotionSlAngle | gesture sliding-area angle, `angle = tan(c)*10` off the +x axis. **Inert on this part** | `00` |
| `0xF0`–`0xF1` | LpScanRaw1 H/L | low-power scan channel 1 reference | `40 40` |
| `0xF2`–`0xF3` | LpScanRaw2 H/L | low-power scan channel 2 reference | — |
| `0xF4` | LpAutoWakeTime | recalibration period in low power. Unit 1 min, range 1–5, default 5 | — |
| `0xF5` | LpScanTH | low-power wake threshold; smaller = more sensitive. Range 1–255, default 48 | — |
| `0xF6` | LpScanWin | low-power measurement range; greater = more sensitive, more power. Range 0–3, default 3 | — |
| `0xF7` | LpScanFreq | low-power scan frequency; smaller = more sensitive. Range 1–255, default 7 | — |
| `0xF8` | LpScanIdac | low-power scan current; smaller = more sensitive. Range 1–255 | — |
| `0xF9` | AutoSleepTime | enter low power after x seconds with no touch. Unit 1 s, default 2 s | `00` |
| `0xFA` | IrqCtl | `[7]` EnTest, `[6]` EnTouch, `[5]` EnChange, `[4]` EnMotion, `[0]` OnceWLP (one pulse for long press) | `70` |
| `0xFB` | AutoReset / DebounceTime | reset if there is touch but no valid gesture within x seconds. Unit 1 s, disable 0; **the PDF's default field is truncated — no value is documented**. **Never write** | `00` |
| `0xFC` | LongPressTime | auto reset after long press x seconds. Unit 1 s, disable 0, default 10 | `00` |
| `0xFD` | IOCtl | `[2]` SOFT_RST, `[1]` IIC_OD, `[0]` En1v8. **Never write** | `00` |
| `0xFE` | DisAutoSleep | 0 = auto-sleep enabled (documented default), nonzero = disabled | **`01`** |

Undocumented regions found by the full scan, named by nothing:

| addr | factory | |
|---|---|---|
| `0x29`–`0x33` | `ff` ×11 | the only nonzero block below `0xA7` |
| `0xAA` | `ff` | |
| `0xCC`–`0xD5` | `41 53 51 54 52 56 43 46 44 45` | ASCII `ASQTRVCFDE`, a vendor string |
| `0xEA` | `05` | inside the config block |

### Where the documented defaults are wrong

Four registers hold values the PDF does not predict: `IrqPluseWidth` is `01` where 10 is
documented, `NorScanPer` is `00` **outside** its documented range of 1–30, `LongPressTime` is `00`
where 10 s is documented, and `DisAutoSleep` is `01` where 0 is documented — so **auto-sleep is
disabled out of the box**, which is why a written configuration survives an idle stretch (measured:
9.9 minutes) with nothing to defend.

`EnMotion` (`0xFA` bit 4) is also **already set at power-on**: the driver writes only
`EnTouch | EnChange`, so bit 4 sits at its default, which is on.

### Driver naming, two discrepancies

The in-tree Zephyr driver (`zephyr/drivers/input/input_cst816s.c`) matches the map except:

- `CST816S_MOTION_EN_CON_UR` for `0xEC` bit 1 — the map calls it **EnConUD** (up-down). The
  driver's `UR` is a typo, not a different bit.
- `CST816S_REG_DEBOUNCE_TIME` for `0xFB` — the map names that register twice, `AutoReset` in the
  heading and `DebouceTime`/`DebounceTime` in the bit field, so the driver took the field name. Not
  a mistake; see the never-write list.

### The zero reads are real values

`0xED`, `0xEE`, `0xEF`, `0xFB` and `0xFC` are all **writable and readable**: write a distinctive
in-range value, read it straight back, restore. Twenty-five I2C transactions, no error, every
readback matching.

So the block is neither write-only nor read-as-zero, and the zeros above are values the chip is
genuinely holding. `NorScanPer` really is 0 — outside its documented range of 1–30 — on a chip
demonstrably scanning at ~80/s. **On this part, 0 means "vendor default"; the documented ranges
do not describe it.** Do not infer that a register is unimplemented from a zero read, and do not
infer a behaviour from a documented default.

### Registers never to write

- **`0xFD` IOCtl.** Bit 0 `En1v8` moves the IIC and IRQ pins to 1.8 V levels on a 3.3 V system.
- **`0xA5` POWER_MODE.**
- **`0xFB` must stay `00`.** Documented as "automatically reset if there is touch but no valid
  gesture within x seconds" — a literal description of a finger held still, so any screen where
  resting a finger is an interaction will reset the chip under it. Zero by luck of the vendor's
  firmware, not by design.

## Gestures

`CONFIG_INPUT_CST816S_EV_DEVICE=y` makes the driver emit an `INPUT_EV_DEVICE` event whose `code`
is the chip's gesture id. The chip classifies on its own; no LVGL layer is involved.

The codes are the values of `GestureID` @`0x01`, which the driver reads for you.

| code | gesture | arrives untouched? |
|---|---|---|
| `0x01`–`0x04` | slide up / down / left / right | yes |
| `0x05` | single click | yes |
| `0x0B` | double click | **no** — needs `EnDClick` |
| `0x0C` | long press | yes |

- **`MotionMask` (`0xEC`) does not gate swipe classification at all.** Swipes arrive with the
  register at its factory `00`. The driver reads the gesture register on every touch/change IRQ,
  so no motion IRQ is ever required.
- **`EnMotion` (`0xFA` bit 4) gates the *click* codes, not the swipes.** Clearing it killed 5/5
  taps while swipes kept working.
- **`EnDClick` (`0xEC` bit 0) is required for `0x0B`**, and the chip then *arbitrates*: a double
  tap reports one `0x0B` and no `0x05` at all, rather than two singles plus a double. The cost is
  ~200 ms added to every `0x05`, which the chip spends waiting out the double-click window.
- `EnConLR` / `EnConUD` were never needed for anything.

### The structural rule

**At most one gesture per contact, whichever condition fires first.** A still hold gives `0x0C`
at ~1.27 s and then nothing for the next 28 seconds; a drifting hold gives a swipe and then no
`0x0C` at all. A gesture therefore cannot double-fire inside one touch — and equally, **every
long contact emits something**.

Swipes report **mid-stroke**, 86–321 ms after press and *before* the release. Clicks report on
**lift** — 10 ms after release without `EnDClick`, ~208 ms with it.

The driver's extra per-report I2C read starves nothing: ~2300 coordinate reports still arrive
across a 29 s hold.

### Direction mapping

The panel keeps its portrait orientation while the image is rotated a quarter turn, so the chip's
axes are rotated 90° from the screen's. With `ROTATE_DISPLAY_180=n`:

| screen direction | chip code | chip's own name |
|---|---|---|
| up | `0x04` | SWIPE_RIGHT |
| down | `0x03` | SWIPE_LEFT |
| left | `0x01` | SWIPE_UP |
| right | `0x02` | SWIPE_DOWN |

**A binding must name the screen direction, not the chip's label.**

## What each gesture collides with

The classifier is generous and has no tunable threshold, so every code overlaps some ordinary
touch. Measured trip points, and what each one means for a binding.

### Swipes `0x01`–`0x04`

- **Trips on ~31 counts of travel.** A slow drag netting as little as 7 counts has classified as a
  swipe; deliberate swipes travel 157–248 counts. No distance or velocity threshold exists in the
  register map.
- **Reported mid-stroke**, 50–321 ms after press and before release. The finger's net displacement
  at release is therefore *not* what the chip classified on, and cannot be used to explain or
  predict a classification.
- **Any sustained drag along one axis is a swipe on that axis.** A full-travel brightness drag
  (420 counts along the chip's x-axis) fired an x-axis code 12/12. A drag and a swipe on the same
  axis are the same gesture.
- **A repeated push-move-lift stroke is a swipe**, 38 codes from 39 contacts (~97%). A screen whose
  touch interaction is pushing something around cannot bind a swipe code at all.
- **No latency gate separates a swipe from a stroke.** Strokes measured 50–387 ms against
  deliberate swipes at 74–321; flicks reach 50 ms, *below* the deliberate floor, so neither an upper
  nor a lower bound works. (A slow *position-control* drag does separate — 19 drag-swipes at
  ≥514 ms against 92 swipes at ≤321 — but that is a property of moving at reading speed, not of
  non-swipe touch generally.)
- **`MotionSlAngle` (`0xEF`) does not help.** Swept at 2/5/10/20 against a control at 0: deliberate
  swipes unaffected 56/56, straight drags fired x-axis codes at every value, wandering drags
  rejected no better than default. Inert on this part.

### Long press `0x0C`

Fires on every press held past ~1.27 s, including a deliberate hold and a slow drag in progress.
A consumer must ignore it explicitly. It is unusable wherever a resting finger is an interaction.

Its one useful property: under the one-gesture-per-contact rule a contact it claims is **spent**, so
a long rest-and-drift cannot go on to emit a swipe however far it wanders.

### Single click `0x05`

A bounced contact emits one — a touch of a few tens of milliseconds with no displacement, such as a
finger settling before a drag.

### Double click `0x0B`

The only code no single contact can produce, so no hold, drag, stroke or still finger reaches it.

- **The window is ~208 ms.** With `EnDClick` set, both `0x0B` and `0x05` arrive **207–210 ms after
  release** (13 samples) — that delay *is* the disambiguation wait, so the acceptance window is at
  most ~208 ms. Nothing documented configures it.
- **It collides with tapping twice in one spot**, at a measured **1 in 47** contacts of continuous
  deliberate touch play. The collision is on *position proximity*, not speed: 230 rapid taps roving
  over the panel produced zero.
- **The collision cannot be filtered.** Every `0x0B` observed, reconstructed from the press/release
  pairs that produced it:

  | | tap 1 dwell | gap | tap 2 dwell | taps apart |
  |---|---|---|---|---|
  | deliberate ×5 | 85–97 ms | 92, 102, 92, 102, 112 ms | 61–85 ms | 3–14 counts |
  | accidental ×1 | 36 ms | 112 ms | 97 ms | 8 counts |

  The accidental pair sits inside the deliberate distribution on gap, dwell and proximity. A tighter
  window would reject deliberate double taps too.

### Choosing a binding

Every channel the chip exposes overlaps some plain touch behaviour, so a binding is chosen by what
the collision **costs**, not by finding one without a collision:

| channel | code(s) | overlaps | measured false rate |
|---|---|---|---|
| position | coordinates | a fingertip that acts where it lands | — |
| duration | `0x0C` | a deliberate rest | fires on every hold >1.27 s |
| travel | `0x01`–`0x04` | a drag, or a push-move-lift stroke | ~97% of strokes |
| count | `0x05`, `0x0B` | tapping, twice in one spot | ~2% of touch play |

Measure the false-positive rate against the interaction the screen actually has, then weigh it
against what the bound action costs to undo. A zero-false-positive requirement rejects every channel
on this part.

**"Not in the map" is not evidence.** `MotionMask` documents only bits [2:0], leaving five
undocumented bits clear at the factory value beside `EnDClick`, and `0xEA` holds an undocumented `05`
in the same block. An undocumented gesture-timing control may exist.

## Notes for consumers

- **Guard on `INPUT_EV_DEVICE`.** The driver emits a sync'd event whose `code` is the gesture id
  and whose coordinates are stale. Callbacks that only check `INPUT_EV_ABS` / `INPUT_EV_KEY` fall
  through into a duplicate update with those stale coordinates. Idempotent in the code as it
  stands, but any new consumer must handle it.
- Several `INPUT_CALLBACK_DEFINE` consumers on the `cst816s` node is fine and is how
  `touch_brightness.c` and the layouts' touch code already coexist.
- **Undocumented codes exist, and a code can repeat.** Rapid drumming produced 12 events with
  `code=0xAA`, which is in no gesture table, and they repeated *after release* — `held=0` with the
  same value re-read on successive 12.45 ms reports. Not diagnosed. Two consequences for any
  consumer: switch on the codes you handle and **ignore everything else** rather than assuming the
  documented set is exhaustive, and do not rely on one-gesture-per-contact for correctness — it holds
  for the documented codes but did not here.
