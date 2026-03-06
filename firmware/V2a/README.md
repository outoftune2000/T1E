# T1E Firmware (ESP-IDF)

This firmware runs on the T1E device (XIAO ESP32-C3/C6 variants) with DS3231 RTC and SSD1681 e-paper.

## Power and Sleep Behavior

- Critical battery protection: if battery voltage is below `3.2V`, firmware enters indefinite deep sleep.
- Clock auto-sleep cycle: in normal clock mode (energy saver OFF), the device deep-sleeps for `60s` after a refresh grace period.
- Energy saver mode: when enabled, the clock shows a `ZZZ.....` screen and sleeps until Button A wakes it.
- RTC persistence is retained in RTC memory across deep sleep.

## RTC and Time Sync

- On DS3231 lost-power detection, time is restored from firmware build timestamp (`__DATE__` + `__TIME__`).
- Wi-Fi can be user-toggled and used to sync RTC from NTP.
- After each NTP sync attempt, Wi-Fi radio is explicitly powered down.

## Buttons

### Global long-press actions

- Hold `A` for `>= 5s`: toggle Wi-Fi ON/OFF.
  - If toggled ON, firmware immediately attempts Wi-Fi connect + NTP sync.
- Hold `B` for `>= 5s`: toggle Energy Saver ON/OFF.

### Clock mode tap behavior

- `A` single tap:
  - waits `1s` for a possible second tap;
  - if no second tap arrives, enters `ZZZ.....` screensaver mode.
- `A` double tap (two taps within `1s`): changes clock face.
- While screensaver is active, `A` tap wakes clock display again.
- In Energy Saver mode, waking with `A` allows normal clock updates for `2 minutes`, then returns to `ZZZ.....` and deep sleep.

### Other mode taps

- Dice: `A` rolls, `B` advances die type / exits dice sequence.
- Pomodoro: `A` start/cancel timer.
- Settings: `A` moves cursor, `B` confirms item.
