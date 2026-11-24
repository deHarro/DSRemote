# DSRemote — Windows 11 / Rigol DS2000 LAN + RAW Edition

**A Windows 11 development fork of DSRemote with direct Rigol LAN control, live waveform display, high-resolution RAW snapshots, interactive zoom and measurement cursors.**

This fork builds on the [Windows port by pvvx](https://github.com/pvvx/DSRemote), which itself is based on [DSRemote by Teunis van Beelen (Teuniz)](https://www.teuniz.net/DSRemote/). It focuses on making a Rigol DS2000-series oscilloscope useful as a high-resolution desktop measurement instrument.

> **Current development state:** 24 November 2026 — LAN/NORM recovery, RUN/SINGLE and horizontal overview V3c (locally tested; Git tag pending)  
> **Earlier stable RAW reference:** `dsremote-raw-v1.8` (13 November 2026)  
> **Tested instrument:** Rigol DS2202 upgraded to DS2302, firmware `00.03.06`  
> **Tested environment:** Windows 11 · Qt 5.15.2 · MinGW 8.1, 64-bit

## What's different in this fork?

### Direct LAN communication and live acquisition

DSRemote connects **directly** to the oscilloscope's LAN interface from its Qt/C++ application. The current DS2302 path sends SCPI commands and receives waveform responses using a **TCP socket on port 5555 of the oscilloscope** (`osc_lan.cpp`). The former separate Python/VXI-11 bridge is **no longer needed** for normal operation.

- Live **NORM/BYTE** waveform display, normally up to 1,400 displayed points per enabled channel.
- **RUN, STOP and SINGLE** handling, including live display during a SINGLE acquisition and the corresponding button states.
- **NORM recovery** for occasional malformed or unexpectedly terminated SCPI binary blocks, limiting disruption to subsequent reads.
- **Connect/Disconnect** menu items enable and disable according to connection state.

**Known limitation:** At slow time bases, especially around 200 ms/div, the waveform made available via SCPI may update only every second acquisition. Standalone NORM/MAX probes exhibit similar delays; the root cause is not yet resolved. This does not imply the scope's own screen stops updating.

### High-resolution RAW acquisition

The original, normal waveform view typically provides **1,400 samples per channel** for the displayed trace. That is useful for a live overview, but it cannot reveal the details present in the oscilloscope's deeper acquisition memory.

This fork can read the stopped DS2302's **RAW acquisition memory** and display the actual recorded samples:

| Tested configuration | Sample rate | RAW depth | Sample interval |
| --- | ---: | ---: | ---: |
| CH1 only | 2 GSa/s | 280,000 points | 500 ps |
| CH1 + CH2 | 1 GSa/s | 280,000 points per channel | 1 ns |

These are **observed test configurations**, not fixed limits or a guarantee for every DS2000 model. Available depth and rate depend on the instrument settings.

Both enabled channels can be captured from the same stopped acquisition. They are transferred sequentially, then displayed together as a consistent snapshot.

### Improved waveform display

- Antialiased traces for a clearer desktop view.
- Mouse-wheel **1–2–5 horizontal display zoom**, centered around the mouse position.
- Local zoom up to **16× in NORM** and **2048× in RAW**.
- Middle-mouse drag to pan; double-click the plot to reset the local view.
- A visible `Ansicht: .../div` label for the effective *on-screen* time scale.
- Zoom and pan change the **display only**, not the oscilloscope's acquisition time base.
- Trigger-position handling and cursor positions remain consistent when zooming or changing the instrument's time base.
- A **horizontal acquisition overview (V3c)** shows a schematic zigzag for the retained acquisition range, blue background bars for areas outside the visible window, and a small orange trigger-position marker. The zigzag is a *symbol*, not a miniature copy of the recorded waveform. The overview responds to instrument time-base and H-position changes, including the stop/edge cases observed on the DS2302.

RAW rendering uses the received samples and an overview/detail drawing strategy. **Zooming does not increase the scope's physical sample rate or create extra measured samples.**

### Interactive X and Y cursors

The `curs` control cycles through cursor display modes: **off → X → Y → X+Y**.

- **X cursors:** independently movable time markers, with absolute times, Δt and `1 / |Δt|`.
- **Y cursors:** independently movable voltage levels, with voltage and ΔV measurements.
- Drag cursor lines or their handles with the left mouse button.
- The compact measurement panel can be moved within the plot or detached as a Qt dock window.
- Measurements remain tied to time/voltage coordinates instead of screen pixels, so zooming does not change their meaning.

### RAW / SNAPSHOT controls

The lower black status strip contains the existing CH1/CH2 setting fields plus:

| Control or indication | Behaviour |
| --- | --- |
| **RAW** | Toggle between the live NORM view and a previously cached RAW snapshot; **does not download again** |
| **RAW laden** | Load a new snapshot from the oscilloscope; enabled **only when the instrument is in STOP** |
| **Ctrl+Shift+R** | Keyboard shortcut for the RAW/NORM view toggle |
| **LIVE CH1+CH2 / NORM** | Current NORM view, showing the enabled live channels |
| **SNAPSHOT CH1+CH2 / ... P/K / ... ps** | Cached RAW view, explicitly listing the captured channels, sample count and sampling interval |
| **SNAPSHOT ... (ALT)** | The instrument has run since the snapshot was taken; the cached data is still available but no longer current |

Only channels belonging to the cached snapshot are displayed in SNAPSHOT mode. A frozen CH1 waveform is **never silently mixed** with a live NORM CH2 waveform.

When loading a new snapshot, the previous valid cache remains available until the new transfer completes successfully. An unsuccessful refresh does not overwrite it.

### Responsive loading indicator

The lower status strip shows a progress bar immediately after **RAW laden** is clicked. Because the DS2302 can take some time before returning its first data block, the bar uses a **time-based estimate** rather than waiting for each block to arrive.

- The estimate adapts to the measured duration of successful transfers.
- A `~` indicates that the displayed percentage is **estimated**.
- The bar stays below 100% until the actual acquisition transfer has completed.
- The underlying transfer remains driven by real data; the estimated bar changes **only the UI**, not acquisition logic.

## Build and run

### Requirements

- Windows 11 (tested)
- Qt Creator with **Qt 5.15.2 / MinGW 8.1, 64-bit**
- A compatible Rigol oscilloscope reachable over the local network, with its SCPI/TCP service accessible on port **5555**

**Python is not required** to build or run DSRemote. Optional standalone Python scripts are used only for development or diagnostics.

The Windows project links against Winsock with `win32:LIBS += -lws2_32` in `dsremote.pro`.

### Build DSRemote

1. Clone this fork and open **`dsremote.pro`** in Qt Creator.
2. Select the **Desktop Qt 5.15.2 MinGW 64-bit** kit.
3. Configure the desired Debug or Release build and build the project.
4. Start DSRemote. Open **Settings** and select the **LAN** connection; enter the **oscilloscope's own IP address or hostname**.
5. Select **Device → Connect**. No external bridge process or localhost proxy is required.

Qt runtime DLLs, generated `build/` directories, executables and personal Qt Creator `*.pro.user*` settings are deliberately excluded from new commits by `.gitignore`. Build and runtime dependencies must be provided on the local machine.

### Current LAN architecture

The DS2000 connection is implemented directly inside DSRemote using C++/Qt and Winsock. For the tested DS2302, the application opens a normal **SCPI-over-TCP socket to the oscilloscope on port 5555**. This is **not** the previous Python VXI-11 bridge arrangement; no separate VXI-11 or proxy process needs to be running on the PC.

```text
 +--------------------------------+
 | DSRemote                       |
 | Windows 11 / Qt / C++          |
 |                                |
 | SCPI commands                  |
 | NORM and RAW waveform data     |
 +---------------+----------------+
                 |
                 | Direct LAN / TCP port 5555
                 | (scope IP address)
                 v
 +--------------------------------+
 | Rigol DS2202 / DS2302          |
 | Oscilloscope                   |
 +--------------------------------+

 No Python bridge. No localhost proxy.
```

Configure the **scope's LAN address** in DSRemote **Settings** and choose **Device → Connect**. Do **not** set the destination to `127.0.0.1` merely because old instructions mention a local bridge.

**Tip:** After switching the oscilloscope on, allow its startup and network initialization to finish before connecting. An early SCPI read may fail even if initial commands are accepted. This was observed with both the current and an earlier portable executable; it is not presently attributed to the GUI patch.

**Historical note:** Early development used `ds_bridge_ds2000_v4_quiet.py` and `python-vxi11` to connect a local `127.0.0.1:5555` listener to the scope. Those instructions have been retired. Python probes may still be useful as independent diagnostic tools.

## How to use RAW snapshots

1. Connect DSRemote and enable the desired oscilloscope channels.
2. Acquire a waveform on the oscilloscope, then put it in **STOP**.
3. Click **RAW laden**. The estimated progress bar should begin moving immediately.
4. When loading finishes, verify the green **SNAPSHOT** status and channel names.
5. Zoom and pan to inspect details, optionally using X/Y cursors.
6. Click **RAW** to return to **LIVE/NORM**. Click **RAW** again to revisit the same cached snapshot without another transfer.
7. To capture newer data, stop the oscilloscope again and click **RAW laden**.

A snapshot is intentionally **not live**. Pressing RUN or SINGLE does not update the cached RAW waveform. The status marks the cache as old after the oscilloscope runs; a new capture requires **RAW laden**.

## Technical notes

The DS2302 may return a zero-length RAW block while its status is still `READ`. That is not necessarily an error. A verified 280,000-point transfer, for example, arrived as:

```text
READ,0       ->       0 bytes
READ,126945  -> 126,945 bytes
IDLE,153055  -> 153,055 bytes
                -----------
Total           280,000 bytes
```

The RAW reader therefore supports multiple blocks, tolerates a limited number of empty `READ` blocks, and validates the complete expected byte count before publishing a snapshot.

After a RAW transfer, the waveform interface is restored to the verified **NORM / BYTE / 1..1400** settings, including `:WAV:POIN 1400`, `:WAV:STAR 1` and `:WAV:STOP 1400`. This avoids the truncated or empty NORM traces observed in early experiments.

The current implementation has a software safety limit of **1,000,000 RAW points per channel**. This is an implementation limit, not a claim about the hardware's maximum memory depth.

## Development milestones

The names below document successive development steps. `dsremote-raw-v1.8` is a **historical RAW reference**, not the current GUI/LAN state. The 24 November 2026 changes can be recorded in a separate Git commit and tag.

| Milestone | Main change |
| --- | --- |
| Display / cursor work | Antialiasing, local zoom/pan, X/Y cursors, movable measurement panel, trigger-position correction |
| RAW V1.3 | Larger RAW depth handling, including the observed 280,000-point single-channel capture |
| RAW V1.4 | Fixed zero-length first-block handling and completed multi-block RAW transfer |
| RAW V1.5 | Extended RAW display zoom up to 2048×; retained 16× NORM limit |
| RAW V1.6 | Added RAW buttons, explicit loading, cache and status controls |
| RAW V1.7 | Clear LIVE/SNAPSHOT channel separation, cache preservation and stale-state indication |
| **RAW V1.8** | Restored CH1/CH2 settings in the bottom strip and added smooth, learning-based progress indication |
| DS2000 direct LAN | Replaced the external Python bridge with a native Qt/C++ TCP/SCPI connection to scope port 5555 |
| NORM Recovery / RUN-SINGLE | Better handling of irregular binary blocks; live RUN/SINGLE display and button states |
| GUI connectivity | Correct Connect/Disconnect menu enable/disable behavior |
| **Horizontal overview V3c** | Correct visible-window bars, symbolic zigzag width, H-position, small trigger marker and drawing order (zigzag in front of blue bars) |

**Historical RAW reference:** `dsremote-raw-v1.8` (13 November 2026).  
**Development state:** 24 November 2026; see [`DS2302_Entwicklungsstand_2026-11-24.md`](DS2302_Entwicklungsstand_2026-11-24.md).

## Known limitations and roadmap

**Current limitations**

- The tested RAW path requires the instrument to be in **STOP**; it is not continuous RAW streaming.
- At slow time bases, external NORM waveform data may skip a sweep or update late; independent Python tests reproduce the behavior, and the cause remains unresolved.
- Device communication may be unreliable briefly after power-on; ensure the oscilloscope has finished initialization.
- Detailed behaviour has been tested on **one DS2302**; other models and firmware versions are not guaranteed.
- The RAW cache is an **in-memory working snapshot**, not a persistent waveform archive or two-slot comparison system.
- The visual zoom can show individual recorded samples but cannot recover detail beyond the acquired sample interval.

**Planned improvements**

- Investigate the **missing/late sweep updates** at slow time bases without compromising the current NORM recovery.
- Improve robustness for **larger RAW transfers** and occasional LAN errors.
- Consolidate disconnect/close handling and tidy up legacy log text mentioning a TCP "bridge".
- Consider small **Fine / Coarse** indicators next to the horizontal and vertical scale knobs.
- Continue refining cursor targeting, recorder integration and DS2000-specific usability while preserving the working acquisition paths.

## Credits and license

- Original DSRemote: [Teunis van Beelen (Teuniz)](https://www.teuniz.net/DSRemote/)
- Windows port and upstream for this fork: [pvvx/DSRemote](https://github.com/pvvx/DSRemote)
- Windows 11 / DS2000 extensions documented here: this fork

This fork of DSRemote is coded mainly by ChatGPT, guided carefully and intensely by the owner of this repository.

The project retains the upstream **GNU General Public License v3**; see the repository's `LICENSE` file. Existing copyright notices and author credits remain applicable.

---

*This README describes the Windows 11 / DS2302 development state as of 24 November 2026, including features added since `dsremote-raw-v1.8`. The newer commit/tag is a proposed checkpoint until actually published; compatibility with other Rigol models and firmware is not guaranteed.*

---

## Original README (upstream project)

The following documentation originates from the
original DSRemote project and is preserved for reference.

# DSRemote
DSRemote Windows 10 x64 (Desktop_Qt_5_13_2_MinGW_64_bit)
Operate your Rigol oscilloscope from your Windows desktop.
Only LAN connection supported.

Clone from:<br>
[DSRemote written by Teuniz](https://gitlab.com/Teuniz/DSRemote)<br>
[DSRemote](https://www.teuniz.net/DSRemote/index.html)<br>

Supported devices
---------------

DS6000 and DS1000Z series oscilloscopes.

### MSO4000/DS4000 series

There is some basic support like capturing screenshots.

Not tested yet:

* Record & Playback function
* Wave Inspector
* serial decoding.


DS1000Z series:
---------------
DSRemote assumes that, on your DS1054Z, all options are installed.
If that is not the case it's possible that the software does not work correctly.
On your scope, go to the menu Utility -> Options -> Installed and make sure you have the following options installed:

* DECODER RS232,I2C,SPI Official
* MEM_DEPTH 24M/12M/6M Official
* TRIGGER Timeout,Runt,Window,Delay,Setup,NthEdge,RS232,I2C,SPI Official
* RECORDER Record/Re-play Official
* BANDWIDTH 100M Official

[Готовые исполняемые файлы для Win10x64](https://github.com/pvvx/DSRemote/blob/master/bin/DsRemote.zip)
