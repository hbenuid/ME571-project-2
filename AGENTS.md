# ME571 Project 2

A soft pneumatic finger-rehab actuator with a soft optical bend sensor. An Arduino Uno runs the air pump and
exhaust valve, reads the sensors, works out the finger's bend angle and streams everything over USB serial.
`index.html` ("FlexSense") is a browser dashboard that shows the live angle, can inflate, hold and vent the
bladder, and exports recordings as CSV.

GitHub: `hbenuid/ME571-project-2`. This file is the reference for anyone, person or agent, working in the repo.
Update it when behaviour changes.

## Repository layout

| Path | What it is |
|---|---|
| `src/main.cpp` | The whole Arduino sketch (PlatformIO, Arduino framework). |
| `index.html` | The whole dashboard: one self-contained HTML file with inline CSS and JavaScript. No build step, no external dependencies. |
| `platformio.ini` | One environment, `uno` (atmelavr, Arduino Uno), serial monitor at 115200 baud, library dependencies. |
| `include/`, `lib/`, `test/` | Empty PlatformIO scaffolding (template READMEs only). There are no automated tests in the repo. |
| `.vscode/extensions.json` | Recommends the PlatformIO IDE extension. The other `.vscode` files are generated and git-ignored. |
| `.gitattributes` | `* text=auto`, so text files are stored with LF line endings. |
| `.gitignore` | Ignores `.pio/` build output, generated VS Code files and `.DS_Store`. |

## Hardware

| Part | Connection | Notes |
|---|---|---|
| Air pump | D3 | HIGH runs the pump. |
| Solenoid exhaust valve | D4 | HIGH closes the exhaust, LOW opens it (checked on the team's valve). |
| FSR (force-sensitive resistor) | A0 | Voltage divider with a 3 kΩ resistor from A0 to GND (`R_DIV`). `readForce()` converts it to grams with the lab's piecewise formula. |
| Soft optical waveguide | A1 | Light reaching the detector. Bending the finger lowers the reading. |
| Adafruit MPRLS pressure sensor | I²C | Absolute pressure, 0 to 25 psi, so it can only see about 10 psi above atmosphere. |

## Firmware (`src/main.cpp`)

### Startup

1. Pump off, exhaust open, serial at 115200 baud.
2. If the MPRLS isn't found, it prints `MPRLS sensor not found. Check wiring and reset.` and stops.
3. It prints `Venting for 5 seconds. Leave the FSR released.`, waits 5 s, then records two baselines with the
   bladder vented: the waveguide reading (`waveguideBaseline`) and atmospheric pressure (`pressureBaseline`).
4. It prints `Ready. Press and hold the FSR to inflate.`

Opening the serial port normally resets an Uno, so every dashboard connection or serial-monitor start re-runs this
and re-zeros both baselines. The finger should be straight and the bladder empty at that moment.

### Measurements

- **Bladder pressure** (`Press(psi)`): MPRLS hPa ÷ 68.9476, minus `pressureBaseline`. This is gauge pressure, about 0
  when vented.
- **Optical loss** (`Loss(dB)`): `10·log10(waveguideBaseline / reading)`. 0 dB at rest, rising with bending. `--`
  when either reading is 0.
- **Angle** (`Angle(deg)`): `ANGLE_A·loss² + ANGLE_B·loss + ANGLE_C`, floored at 0. The coefficients are a
  **placeholder fit** (about 85° per dB) until the sleeve is calibrated. `--` when the loss is `--`.
- **Waveguide voltage** (`Wave(V)`): the raw A1 reading × 5/1023.

### Control modes

`mode` decides who drives the pump and valve. For the dashboard modes, `setMode()` is the only place outputs change.
It always closes the exhaust before starting the pump and stops the pump before opening the exhaust.

| Label | Meaning | Pump | Exhaust |
|---|---|---|---|
| `FSR` | Original behaviour: hold the FSR (> 600 g) to inflate, release it (≤ 300 g) to vent. | FSR | FSR |
| `INFL` | Dashboard: inflating | on | closed |
| `HOLD` | Dashboard: holding | off | closed |
| `VENT` | Dashboard: venting | off | open |
| `STOP` | The FSR was pressed while the dashboard was in control | off | open |
| `MAX` | Dashboard inflation reached `MAX_DASHBOARD_PSI` | off | closed |

Safety rules, all enforced in the sketch rather than the dashboard:

- Dashboard inflation stops at `MAX_DASHBOARD_PSI`, a **3 psi placeholder** to be set from the bladder's rating.
  While inflating, pressure is checked on every loop pass, not only at print time. A failed (NaN) pressure reading
  counts as over the limit, and `I` at or above the limit goes straight to `MAX`.
- In any dashboard mode, an FSR press (> `PRESS_FORCE`) switches to `STOP`, which lasts until the next dashboard
  command. While the FSR stays pressed, any command is overridden on the same loop pass.
- If no command, keep-alives included, arrives for `DASHBOARD_TIMEOUT_MS` (1.5 s), the sketch vents and returns to
  `FSR` mode.
- After returning to `FSR` mode, the FSR can't inflate until it has been released once (`fsrArmed`), so a held stop
  press can't turn into inflation.
- The pressure limit does **not** apply in `FSR` mode. FSR inflation is uncapped, as it always was.

### Serial output (Arduino → computer)

A fixed-width, whitespace-separated table, one row every `PRINT_INTERVAL_MS` (200 ms, so 5 rows/s). A blank line and
the header come before the first row and then every `HEADER_EVERY` (20) rows:

```
 Force(g)  Pump  Mode  Press(psi)  Loss(dB)  Angle(deg)  Wave(V)
        0   OFF   FSR        0.01      0.00         1.1     3.50
```

| Column | Width | Content |
|---|---|---|
| `Force(g)` | 9 | FSR force estimate, no decimals |
| `Pump` | 6 | `ON` or `OFF`, from the actual pin state |
| `Mode` | 6 | Mode label from the table above |
| `Press(psi)` | 12 | Bladder (gauge) pressure, 2 decimals |
| `Loss(dB)` | 10 | Optical loss, 2 decimals, or `--` |
| `Angle(deg)` | 12 | Bend angle, 1 decimal, or `--` |
| `Wave(V)` | 9 | Waveguide voltage, 2 decimals |

The startup messages are plain text lines.

### Serial commands (computer → Arduino)

Single ASCII characters with no line ending needed. Anything else is ignored.

| Char | Action |
|---|---|
| `I` | Inflate (or `MAX` if already at the limit) |
| `H` | Hold |
| `V` | Vent |
| `K` | Keep-alive: refreshes the 1.5 s timeout and nothing else |
| `X` | Vent and hand control back to the FSR |

`I`, `H` or `V` sent in `FSR` mode puts the dashboard in control. You can type these into the PlatformIO serial
monitor to test, but without keep-alives the sketch vents again after 1.5 s.

## Dashboard (`index.html`)

Open the file directly in **Chrome, Edge or Firefox 151+** on a desktop. It uses the Web Serial API, which Safari
doesn't have (Firefox added it in version 151, May 2026). `file://` pages count as a secure context, so no web server
is needed. Only one program can hold the serial port, so close the PlatformIO serial monitor first.

On Connect, Chrome and Edge open a port chooser. Firefox shows a popup, "Allow this local file to access your serial
ports?", with a "Select a serial port:" dropdown and **Allow** / **Block**. Pick the Uno (`usbmodem…`) and click
Allow. Cancelling Chrome's chooser, or Block in Firefox, shows "No port selected."

### Look

A plain light theme: off-white page, white panels with thin grey borders, the system font and one blue accent, with
no gradients, glows or shadows. There is no dark mode. The page colours are CSS variables in `:root`. The SVG finger
and the trend chart use hex values written into the markup and `drawChart()`, so change those by hand to match.
Each quantity keeps one colour everywhere: angle `--angle` (blue), loss `--loss` (green) and pressure `--pressure`
(orange), shown as a dot by each readout and as its line on the chart. Readout numbers stay in the text colour.

### Without an Arduino: the simulator

The page starts in simulator mode ("Manual simulator" in the panel header). The finger bends along a made-up sine
wave, or follows the Bending angle slider once you drag it. Loss comes from the "Simulator loss model" fields,
`L = L₀ + k(θ − θ₀)`. None of this is real data, and it is never recorded.

### With an Arduino

- **Connect to Arduino** opens the port at 115200 baud. Incoming text is split into lines on `\n` and trimmed. A line
  whose first cell is `Force(g)` is a header and sets the column order. Data rows are matched to columns **by header
  name**, so the sketch can reorder columns but can't rename them without updating the page. `DEFAULT_COLUMNS` is
  used until a header arrives. Other lines, such as the startup messages, appear next to the Connect button.
- Each data row updates the finger, the readouts and the chart (angle, loss, pressure) and the Mode text, and is
  recorded. `--` values are skipped, so the last good value stays on screen.
- The angle readouts show the real value. Only the finger drawing is capped at 120°.
- The simulation pauses on connect, so a dropped cable doesn't fall back to fake motion.
- If the connection ends without Disconnect being clicked (cable pulled, board unplugged), the page shows
  `Connection lost: …` next to the Connect button, turns the pump controls off and keeps the recording. Chrome
  reports this as a stream error with its own message. Firefox ends the stream quietly, so the page tracks whether
  Disconnect was clicked (`userDisconnect`) and otherwise shows `Connection lost: The Arduino was disconnected.`
  The sketch vents on its own 1.5 s later either way.

### Pump control

**Inflate**, **Hold** and **Vent** send `I`, `H` and `V`. They're enabled after the first data row, so not during
the startup vent. The page answers **every data row with `K`**, which keeps the sketch's 1.5 s timeout from firing.
Because keep-alives are tied to incoming rows rather than a timer, background-tab timer throttling shouldn't trip
it. Disconnect sends `X` (waiting at most 500 ms) before closing the port.

### Recording and CSV export

- Recording starts automatically on each Connect and covers every data row until Disconnect. Only Arduino data is
  recorded.
- **Download CSV** works during or after the connection.
- Columns: `Time(s)` (seconds since the first row, 3 decimals), `Clock time` (local, `YYYY-MM-DD HH:MM:SS.mmm`),
  `Trial` (the Trial name box at download time, repeated in every row), then every Arduino column under its header
  name. `--` becomes an empty cell. Fields use RFC 4180 quoting and CRLF line endings, in UTF-8 without a BOM.
- File name: `flexsense_<start date>_<start time>_<trial name>.csv`, for example
  `flexsense_2026-11-09_14-03-22_trial-3-50-grip.csv`.
- Times are when the browser received each row, not when the Arduino measured it: a few ms of jitter at 5 rows/s.
- Connecting again with rows that haven't been downloaded asks before discarding them. Closing the tab with
  undownloaded rows triggers the browser's leave-page warning.
- New columns added to the sketch's table appear in the CSV automatically.

## Keeping the sketch and dashboard in sync

These are defined in both files, so change them together:

- **Header names.** The page reads `Angle(deg)`, `Loss(dB)`, `Press(psi)` and `Mode` by name and lists every column
  in `DEFAULT_COLUMNS`.
- **Mode labels.** `MODE_LABELS` in the sketch, `MODE_TEXT` and `CONTROL_BUTTONS` in the page.
- **Command characters** `I`, `H`, `V`, `K`, `X`.
- **Baud rate** 115200.
- **Timeout.** The page's hint text quotes `DASHBOARD_TIMEOUT_MS` (1.5 s). Keep-alives go out once per data row, so
  the timeout must stay several times longer than `PRINT_INTERVAL_MS`.

## Building, uploading and monitoring

```sh
pio run                 # build
pio run -t upload       # build and upload to the Uno
pio device monitor      # serial monitor at 115200 baud (close it before using the dashboard)
```

If `pio` isn't on your PATH, PlatformIO's installer puts it at `~/.platformio/penv/bin/pio`. The PlatformIO VS Code
extension's build and upload buttons do the same thing.

Editor errors such as `'Arduino.h' file not found` from a plain clang setup are false alarms. `pio run` is the source
of truth.

## Verifying changes

There are no automated tests in the repo. Changes so far have been checked like this:

1. `pio run` builds cleanly.
2. **Firmware logic on a computer.** Compile `src/main.cpp` as ordinary C++ against small stand-in headers: an
   `Arduino.h` with fake pins, `millis()`, `delay()`, `analogRead()` and a `Serial` with input and output buffers; an
   empty `Wire.h`; and an `Adafruit_MPRLS` whose `readPressure()` returns a test value. Then call `setup()` and
   `loop()` and check the pump and valve pins in each scenario: FSR mode, inflating to the limit, FSR stop, timeout
   handover, FSR re-arm, NaN pressure and `X`.
3. **Dashboard in headless Chromium.** Replace `navigator.serial` with a fake port whose `readable` replays text in
   the sketch's exact column widths (split mid-line) and whose `writable` records the commands sent. Check the
   readouts, buttons, commands and CSV.
4. **Dashboard in headless Firefox (151+), on Firefox's real serial code.** Drive Firefox with WebDriver BiDi
   (`firefox --headless --profile <throwaway> --remote-debugging-port=0`, no extra install needed). With the pref
   `dom.webserial.testing.enabled` set to `true`, `requestPort()` picks a mock Arduino Uno (`test-device-1`, USB
   vendor 0x2341, product 0x0043) without a prompt, and the mock echoes back whatever is written to it. Write the
   sketch's output into the port through the page's `serialWriter` (a top-level `let`, reachable from
   `script.evaluate`) and record the page's own commands instead of writing them, or the echoed `K`s end up in the
   data. `navigator.serial.simulateDeviceDisconnection('test-device-1')` simulates a pulled cable. To see or answer
   the real permission popup instead, start Firefox with `MOZ_REMOTE_ALLOW_SYSTEM_ACCESS=1` and read
   `PopupNotifications` in the chrome context (`browsingContext.getTree` with `moz:scope: "chrome"`); the
   notification id is `webSerial-choosePort`.
5. Then test on the real hardware, with the pressure limit set correctly.

## Known gaps and open items

- **The angle calibration is a placeholder.** Measure angle against loss on the sleeve, fit a quadratic and update
  `ANGLE_A`, `ANGLE_B` and `ANGLE_C`. The dashboard needs no change.
- **`MAX_DASHBOARD_PSI` is a 3 psi placeholder.** It must stay below about 10 psi, where the MPRLS saturates.
- FSR-mode inflation has no pressure limit.
- Hold only stays steady if the pump doesn't leak air back.
- `arduino-libraries/Stepper` is in `lib_deps` but isn't used.
- Unfixed simulator and display issues in `index.html`:
  - Dragging the slider doesn't update the loss.
  - The loss box is overwritten while the simulation runs.
  - The trend chart plots loss and pressure on the angle's 0–120 scale with no axes of their own. It's labelled
    "Last 30 seconds · 10 Hz" but holds 120 samples (12 s at 10 Hz, 24 s at 5 rows/s), added per update rather than
    per time step.
  - In the finger drawing, the grey sleeve completely covers the skin-coloured finger, the "fingertip" label stays at
    the straight-finger position, and the angle arc at the first knuckle shows the full angle although that joint
    bends only 35% of it.
  - The sidebar's patient details and the "Right index · calibrated" label are static placeholders, and the sidebar
    links never change their highlighted item.

## Conventions

- Commit directly to `main`. Subjects are short imperative sentences ("Print the bending angle in the serial
  table"). Bodies explain what changed and why, wrapped at about 72 characters.
- No AI co-author trailers or "generated with" lines in commits or pull requests.
- Keep the sketch and the dashboard as single files. The dashboard has no build step or external dependencies.
- The safety behaviour (pressure limit, FSR stop, timeout, re-arm, valve ordering) lives in the sketch. Don't move it
  to the page or weaken it without the team agreeing and testing on hardware.
