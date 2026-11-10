# ME571 Project 2

A soft pneumatic finger-rehab actuator with a soft optical bend sensor. An Arduino Uno runs the air pump and
exhaust valve, reads the sensors, works out the finger's bend angle and streams everything over USB serial.
`index.html` ("FlexSense") is a browser dashboard that shows the live angle, runs three exercise modes (an
automatic timed cycle, manual control with the arrow keys and a placeholder spaceship game), and exports recordings
as CSV.

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
3. It prints `Venting for 5 seconds. Leave the FSR released.`, waits 5 s, then records atmospheric pressure with
   the bladder vented (`pressureBaseline`).
4. It loads the waveguide baseline from EEPROM and prints it, for example
   `Waveguide baseline: 410 counts (2.00 V), saved on the Arduino`, or `…, default in src/main.cpp` when none has
   been saved. Then it prints `Ready. Press and hold the FSR to inflate.`

Opening the serial port normally resets an Uno, so every dashboard connection or serial-monitor start re-runs this
and re-zeros the pressure. The bladder should be empty at that moment. The waveguide baseline is **not** measured at
startup, so the loss and angle mean the same in every session. It's saved in the Uno's EEPROM (address 0: a magic
number `0xB5E1`, then the counts), which keeps it through restarts and uploads. You set it from the dashboard's Debug
view, which sends `B<counts>`. `DEFAULT_WAVEGUIDE_BASELINE` (787) is used only while nothing valid is saved, so
changing it in the sketch does nothing once a baseline has been saved: save the new value from the Debug view.

### Measurements

- **Bladder pressure** (`Press(psi)`): MPRLS hPa ÷ 68.9476, minus `pressureBaseline`. This is gauge pressure, about 0
  when vented.
- **Optical loss** (`Loss(dB)`): `10·log10(waveguideBaseline / reading)`. `waveguideBaseline` is the saved
  straight-finger reading (default 787 counts, 3.85 V), so the loss is near 0 dB straight and rises with bending. It
  goes negative if the light is brighter than the baseline. `--` when the reading is 0. Because
  the baseline is fixed, anything that changes the light level (LED, potentiometer, wiring, sleeve position) shifts
  every loss and angle until a new baseline is saved and the angle recalibrated.
- **Angle** (`Angle(deg)`): `ANGLE_A·loss² + ANGLE_B·loss + ANGLE_C`, floored at 0. The coefficients come from the
  second 2026-11-10 calibration (5 points, 1.64 to 5.32 dB and 0 to 90°, RMS error 7.4°), measured against the
  787-count baseline. They hold only while that baseline is saved: a straight finger read 1.64 dB, not 0, so even
  re-saving the straight-finger reading as the baseline shifts every angle. After changing the baseline,
  recalibrate. `--` when the loss is `--`.
- **Waveguide voltage** (`Wave(V)`): the raw A1 reading × 5/1023.
- **Raw waveguide reading** (`Wave(raw)`): the same A1 reading in ADC counts, 0 to 1023, for debugging the light
  sensor at full resolution.

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

- Dashboard inflation stops at `MAX_DASHBOARD_PSI`, set to **30 psi**. That is above the roughly 10 psi (gauge) the
  MPRLS can read, so a valid reading never reaches it: in practice only a failed (NaN) reading stops dashboard
  inflation, and holding Bend or Inflate keeps the pump running.
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
 Force(g)  Pump  Mode  Press(psi)  Loss(dB)  Angle(deg)  Wave(V)  Wave(raw)
        0   OFF   FSR        0.01      0.00         1.1     3.50        716
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
| `Wave(raw)` | 11 | Waveguide reading in ADC counts (0 to 1023), no decimals |

The startup messages are plain text lines.

### Serial commands (computer → Arduino)

Single ASCII characters with no line ending needed, except `B`. Anything else is ignored.

| Char | Action |
|---|---|
| `I` | Inflate (or `MAX` if already at the limit) |
| `H` | Hold |
| `V` | Vent |
| `K` | Keep-alive: refreshes the 1.5 s timeout and nothing else |
| `X` | Vent and hand control back to the FSR |
| `B<counts>` + newline | Save the waveguide baseline, 1 to 1023 counts, for example `B410\n`. The sketch answers `Waveguide baseline: 410 counts (2.00 V), saved`, or `Baseline not changed: it must be 1 to 1023 counts.` Any other character before the newline cancels it. It doesn't change the mode. |

`I`, `H` or `V` sent in `FSR` mode puts the dashboard in control. You can type these into the PlatformIO serial
monitor to test, but without keep-alives the sketch vents again after 1.5 s.

## Dashboard (`index.html`)

Open the file directly in **Chrome, Edge or Firefox 151+** on a desktop. It uses the Web Serial API, which Safari
doesn't have (Firefox added it in version 151, May 2026). `file://` pages count as a secure context, so no web server
is needed. Only one program can hold the serial port, so close the PlatformIO serial monitor first.

On Connect, Chrome and Edge open a port chooser. Firefox shows a popup, "Allow this local file to access your serial
ports?", with a "Select a serial port:" dropdown and **Allow** / **Block**. Pick the Uno (`usbmodem…`) and click
Allow. Cancelling Chrome's chooser, or Block in Firefox, shows "No port selected."

The page has three views: **Live session** (the default), **Calibration** (`#calibration`) and **Debug**
(`#debug`). A top bar, shared by all three, holds the FlexSense name, the view tabs (the current one highlighted),
the status, the one **Connect to Arduino** button, the Trial name box and **Download CSV**. All views share one
serial connection and recording. The
status reads `Simulator`, `Arduino · starting up` (connected, before the first data row), `Arduino · recording
m:ss`, or after a disconnect `Simulator · recorded m:ss, not downloaded` (amber) or `…, downloaded`. Its tooltip
counts the rows. The sketch's startup messages and connection problems appear on a line under the top bar. The
live session drives the bladder through the exercise modes below. The calibration and debug views have their own
Inflate, Hold and Vent buttons for setting a bend. Opening either of them stops the automatic cycle, releases held
keys and ends a game, because a capture needs a still finger. The arrow keys only work in the live session.

Explanations stay out of sight behind small **(i)** buttons next to headings and the top bar. Each `.info-btn` shows
the hidden `.info-text` right after it in one shared popup (`#infoPop`): hovering shows it, a click or tap keeps it
open until a click elsewhere or Escape, and changing view closes it. The popup stays inside the window. Only
short labels, live status and warnings stay on the page. To add an explanation, add a button and text in the same
way.

### Look

A plain light theme: off-white page, white panels with thin grey borders, the system font and one blue accent, with
no gradients, glows or shadows. There is no dark mode and no sidebar. The page colours are CSS variables in `:root`. The SVG finger,
the trend chart, the game and the calibration plot use hex values written into the markup, `drawChart()`,
`drawGame()` and `drawFitChart()`, so change those by hand to match. Each quantity keeps one colour everywhere: angle
`--angle` (blue), loss `--loss` (green) and pressure `--pressure` (orange), shown as a dot by each live-session
readout (the large angle badge on the finger, then loss and pressure under it) and as its line on the trend chart. Readout numbers stay in the text colour. The game's ship, whose height
is the angle, is blue too. In the calibration plot the measured points are dark, the new fit is blue, the current
sketch is a grey dashed line and the live loss is green. Warnings use `--warn` (dark amber).

### Exercise modes

Three tabs above the live session choose the mode. The live session puts the finger (or the game) on the left and
the Controls panel and trend chart on the right. The Controls panel shows only the current mode's controls, and its
heading says what the actuator is doing (the sketch's `Mode` column, or the simulator's). Switching modes stops the automatic cycle, releases any held key and
vents the bladder. It doesn't vent if the sketch is still in `FSR` mode and the page never took control.

| Mode | What it does | Commands |
|---|---|---|
| **Automatic** | Repeats inflate, hold, vent and rest for the times set in seconds, for a set number of reps (defaults 3, 2, 3, 2 s and 10 reps). **Start** begins and **Stop and vent** ends it early. | `I` at the start of inflate, `H` at hold, `V` at vent and rest |
| **Manual** | Hold **↑** to bend (inflate) and **↓** to straighten (vent). Let go to hold. While both keys are down, the last one pressed wins. The **Bend** and **Straighten** buttons work the same way when pressed and held. | `I`, `V`, then `H` on release |
| **Game** | A placeholder side-scrolling game. The ship's height follows the bend angle, from straight at the bottom to the "Top of screen at" angle (default 90°) at the top. Dodge asteroids to score one point for each one passed. One hit ends the game. **Start game** or **Space** starts it. | `V` at the start, so the finger moves under its own effort |

- The automatic cycle runs on the clock. With an Arduino it also advances on every data row, so a background tab
  doesn't stretch its phases. Inflation still stops at the sketch's `MAX_DASHBOARD_PSI`.
- **Limit indicator.** While the `Mode` column (or the simulator) is `MAX`, the Bladder pressure readout turns
  amber with a **Limit reached** tag, and the Controls heading and the calibration view's actuator line read
  "Pressure limit reached, holding" in amber. It clears on the next row that isn't `MAX` (after Hold or Vent).
- If a data row shows `STOP` (FSR pressed) or `FSR` (the sketch timed out), the page ends the automatic cycle and
  forgets held keys, without sending anything. A held key needs a fresh press after that. Rows that arrive within
  400 ms of a command (`COMMAND_SETTLE_MS`) are ignored for this check, because they may have been printed before
  the command arrived.
- Arrow keys release when the window loses focus or the tab is hidden, and Manual mode then sends `H`. Arrow keys
  are ignored while typing in a text or number box.
- The page remembers the mode, the automatic timings, the game's top angle and the best score in `localStorage`.

### Without an Arduino: the simulator

The page starts on the simulator ("Simulator" in the status pill). A simulated bladder answers `I`, `H` and `V`
like the sketch does. It fills at 0.8 psi/s up to 30 psi (mirroring `MAX_DASHBOARD_PSI`), vents with a 0.5 s time
constant and bends the finger 30° per psi, up to 120°. So the Automatic and Manual modes can be tried without hardware. In the
Game, the bladder stays vented and **↑**/**↓** move the simulated finger directly, standing in for the patient's
own effort. Loss comes from the fields under **Simulator settings**, `L = L₀ + k(θ − θ₀)`. That section starts
closed and is hidden while an Arduino is connected. None of this is real data, and it is never recorded.

### With an Arduino

- **Connect to Arduino** opens the port at 115200 baud. Incoming text is split into lines on `\n` and trimmed. A line
  whose first cell is `Force(g)` is a header and sets the column order. Data rows are matched to columns **by header
  name**, so the sketch can reorder columns but can't rename them without updating the page. `DEFAULT_COLUMNS` is
  used until a header arrives. Other lines, such as the startup messages, appear under the top bar. Clicking
  Disconnect clears that line.
- Each data row updates the finger, the game, the readouts and the chart (angle, loss, pressure), the actuator
  status and the calibration view's live reading, and is recorded. `--` values are skipped, so the last good value
  stays on screen.
- The angle readouts show the real value. Only the finger drawing is capped at 120°.
- Connecting stops the simulator, so a dropped cable doesn't fall back to fake motion. It also stops a running
  automatic cycle. Disconnecting stops the cycle too, and the simulator carries on from the last real reading.
- If the connection ends without Disconnect being clicked (cable pulled, board unplugged), the page shows
  `Connection lost: …` under the top bar in amber, stops the automatic cycle and the pump commands, and keeps the
  recording. Chrome reports this as a stream error with its own message. Firefox ends the stream quietly, so the
  page tracks whether Disconnect was clicked (`userDisconnect`) and otherwise shows
  `Connection lost: The Arduino was disconnected.` The sketch vents on its own 1.5 s later either way.
- In the Game with an Arduino, the arrow keys do nothing. Only the real finger steers.

### Pump commands

Every `I`, `H` or `V` goes through `drive()`, which sends it to the Arduino or, without one, to the simulator. The
calibration view's **Inflate**, **Hold** and **Vent** send the character in each button's `data-command`, and the
one matching the Mode column lights up. Nothing that sends `I`, `H` or `V` (**Start**, the arrow keys, **Bend**,
**Straighten**, **Start game**, the calibration buttons) works until the first data row, so not during the startup
vent. The page answers **every data row with `K`**, which keeps the
sketch's 1.5 s timeout from firing. Because keep-alives are tied to incoming rows rather than a timer,
background-tab timer throttling shouldn't trip it. Disconnect sends `X` (waiting at most 500 ms) before closing the
port.

### Recording and CSV export

- Recording starts automatically on each Connect and covers every data row until Disconnect. Only Arduino data is
  recorded.
- **Download CSV** works during or after the connection.
- Columns: `Time(s)` (seconds since the first row, 3 decimals), `Clock time` (local, `YYYY-MM-DD HH:MM:SS.mmm`),
  `Trial` (the Trial name box at download time, repeated in every row), `Exercise` (`Automatic`, `Manual` or
  `Game`), `Rep` (the automatic cycle's rep while it runs), `Phase` (`Inflate`, `Hold`, `Vent` or `Rest` while the
  cycle runs, `Bend` or `Straighten` while a manual key is held, `Playing` during a game), then every Arduino column
  under its header name. The page's side of a row can be up to one row ahead of the sketch's `Mode` column.
  `--` becomes an empty cell. Fields use RFC 4180 quoting and CRLF line endings, in UTF-8 without a BOM.
- File name: `flexsense_<start date>_<start time>_<trial name>.csv`, for example
  `flexsense_2026-11-09_14-03-22_trial-3-50-grip.csv`.
- Times are when the browser received each row, not when the Arduino measured it: a few ms of jitter at 5 rows/s.
- Connecting again with rows that haven't been downloaded asks before discarding them. Closing the tab with
  undownloaded rows triggers the browser's leave-page warning.
- New columns added to the sketch's table appear in the CSV automatically.

### Calibration view

Fits the sketch's angle formula to measured points and prints the constants to paste into `src/main.cpp`. The page
never changes the angle itself: what's shown and recorded is always the sketch's `Angle(deg)`.

- **Capturing.** Connect with the bladder empty, because the sketch zeros the pressure as it starts. The loss is
  measured against the baseline saved on the Arduino, so keep the light level (LED, potentiometer, wiring) and the
  baseline the same from capturing to using the fit. Then bend the finger (by hand, or with Inflate and Hold), measure the angle with a protractor, type it in
  and press **Capture** (or Enter). The page averages the next 5, 10 or 25 data rows (about 1, 2 or 5 s) and saves a
  point: the measured angle, the mean `Loss(dB)` and its standard deviation ("spread"), the mean `Angle(deg)` the
  sketch printed, pressure, `Wave(V)`, time and which connection it came from. Rows with `--` loss are left out of
  the average. Capture works only after the first data row, and only on Arduino data, never the simulator. Cancel or
  Disconnect mid-capture saves nothing. It warns if the pump was inflating during a capture or the spread was over
  0.02 dB.
- **Fit.** Least squares of `angle = A·loss² + B·loss + C` (quadratic, the default) or `B·loss + C` (linear),
  optionally with `C = 0`. Predictions are floored at 0, as in the sketch. Shown: the equation and R², RMS and maximum
  error of the new fit, the RMS error of the sketch's current angle at the same points, and a plot of the measured
  points, the new fit (dotted where it extrapolates), the current sketch and the live loss. It warns when the fit
  doesn't keep rising across the points and when there are only as many points as terms. The live readouts show both angles and say when the live
  loss is outside the captured range.
- **Points.** Untick a point to leave it out of the fit, or remove it. Points are kept in the browser's
  `localStorage` (`flexsense.calibration.v1`) until **Clear all**, so a reload doesn't lose them. **Download points
  CSV** saves them as `flexsense_calibration_<date>_<time>.csv`, with the new fit's angle and error per point.
- **Applying it.** The "Paste into src/main.cpp" block replaces the sketch's lines from `// Angle calibration` down to
  `ANGLE_C`. Upload, reconnect, and the sketch prints the new angle.

### Debug view

For checking the light sensor and the serial link. It shows only Arduino data, never the simulator.

- **Light sensor on A1.** The latest `Wave(raw)` in counts, its voltage, the saved baseline (parsed from the
  sketch's `Waveguide baseline:` line) and the reading as a percentage of the baseline. A bar shows where the
  reading sits on 0 to 5 V, with the baseline marked in amber and 3 to 4 V shaded as a good straight-finger range.
  It warns when A1 reads 0 (no light, or not wired to A1) and when it's at full scale (turn the potentiometer down).
  With a sketch that has no `Wave(raw)`
  column, it estimates the counts from `Wave(V)` and says so.
- **Set the baseline.** A number box (filled with the current baseline), **Use current reading**, which fills in the
  mean `Wave(raw)` of the last 2 s, and **Save to Arduino**, which sends `B<counts>\n` and waits up to 2 s for the
  sketch's `…, saved` line. Without that answer it says to upload the current sketch. Saving needs the first data
  row, and the button stays off while the box is empty, out of range or equal to the saved value. The angle constants
  are fitted against one baseline, so set the baseline first, then calibrate, and recalibrate after changing it.
- **Inflate, Hold and Vent**, the same as in the calibration view, to see how the reading follows a bend.
- **Chart.** The raw reading over the last 20 s with the baseline dashed. Time when the pump ran (`Pump` is `ON` or
  `Mode` is `INFL`) is shaded, to show noise from the pump. **Zoom to the data** fits the scale to the readings,
  otherwise it's 0 to 1023. Min, max, mean and spread (standard deviation, the noise when the finger is still) are over the
  same 20 s.
- **Serial output.** The last 200 lines exactly as received, startup messages and headers included. It follows new
  lines unless you scroll up, and **Pause** freezes it. Each Connect clears it.

## Keeping the sketch and dashboard in sync

These are defined in both files, so change them together:

- **Header names.** The page reads `Angle(deg)`, `Loss(dB)`, `Press(psi)`, `Wave(V)`, `Wave(raw)`, `Pump` and `Mode`
  by name and lists every column in `DEFAULT_COLUMNS`.
- **Baseline message.** The debug view reads the number after `Waveguide baseline:` in the sketch's startup line
  and in its answer to `B`, treats a line ending in `, saved` as confirming a save, and shows a line starting
  `Baseline not changed` as a refusal.
- **Angle formula.** The calibration view fits `ANGLE_A·loss² + ANGLE_B·loss + ANGLE_C` floored at 0, and its paste
  block uses those constant names and the sketch's `// Angle calibration:` comment line. If the sketch's formula
  changes, change the fit in the page too.
- **Mode labels.** `MODE_LABELS` in the sketch, `MODE_TEXT` and `CONTROL_BUTTONS` in the page. The page also acts on
  `STOP` and `FSR` (ending the automatic cycle) in `handleSerialLine`, and on `VENT` in `showActuator`.
- **Pressure limit.** The simulator's `SIM.MAX_PSI` copies `MAX_DASHBOARD_PSI` so the simulator behaves like the
  real bladder. It only affects the simulator.
- **Command characters** `I`, `H`, `V`, `K`, `X`, and `B<counts>` + newline.
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
   empty `Wire.h`; an `EEPROM.h` whose `EEPROM.get()` and `put()` copy to and from a byte array; and an
   `Adafruit_MPRLS` whose `readPressure()` returns a test value. Then call `setup()` and
   `loop()` and check the pump and valve pins in each scenario: FSR mode, inflating to the limit, FSR stop, timeout
   handover, FSR re-arm, NaN pressure and `X`.
3. **Dashboard in headless Chromium.** Replace `navigator.serial` with a fake port whose `readable` replays text in
   the sketch's exact column widths (split mid-line) and whose `writable` records the commands sent. For the
   exercise modes, make the fake port a small stand-in sketch: it switches mode on `I`/`H`/`V`/`X`, prints a row
   every 200 ms with its current mode and a rising pressure while inflating, and can fake an FSR press. Check the
   readouts, mode switches, commands (including that nothing follows an FSR stop) and CSV, in the simulator and
   over the fake port. For the calibration view, replay rows at known losses and check the fitted constants against
   data generated from a known quadratic. Once rows are recorded, reloading the page opens the leave-page dialog,
   which a headless run has to accept or it hangs.
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

- **The angle calibration is rough and tied to one light level and baseline.** It has 5 points (RMS error 7.4°)
  against the 787-count baseline. More points would make the fit better. Any change to the light level (LED,
  potentiometer, wiring, sleeve) or the saved baseline needs a recalibration.
- **`MAX_DASHBOARD_PSI` is 30 psi, above what the MPRLS can measure** (about 10 psi gauge). Dashboard inflation
  therefore has no working pressure cut-off, only the FSR stop and the timeout. A limit that works must stay below
  about 10 psi.
- FSR-mode inflation has no pressure limit.
- Hold only stays steady if the pump doesn't leak air back.
- `arduino-libraries/Stepper` is in `lib_deps` but isn't used.
- **The game is a placeholder.** It has no levels, sounds or difficulty setting, and its "Top of screen at" angle
  is only as accurate as the angle calibration. The automatic cycle is timed, not closed-loop on angle or pressure.
- Unfixed display issues in `index.html`:
  - The trend chart plots loss (0–8 dB) and pressure (0–30 psi) on the angle's 0–120° scale with no axes of their
    own. It shows the last 20 s (`CHART_WINDOW_S`), placing each sample by when it arrived.
  - In the finger drawing, the grey sleeve completely covers the skin-coloured finger, and the angle arc at the first
    knuckle shows the full angle although that joint bends only 35% of it.

## Conventions

- Commit directly to `main`. Subjects are short imperative sentences ("Print the bending angle in the serial
  table"). Bodies explain what changed and why, wrapped at about 72 characters.
- No AI co-author trailers or "generated with" lines in commits or pull requests.
- Keep the sketch and the dashboard as single files. The dashboard has no build step or external dependencies.
- The safety behaviour (pressure limit, FSR stop, timeout, re-arm, valve ordering) lives in the sketch. Don't move it
  to the page or weaken it without the team agreeing and testing on hardware.
