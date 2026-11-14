# DSRemote — Windows 11 / Rigol DS2000 RAW Edition

**A Windows 11 development fork of DSRemote with high-resolution, cached RAW waveform snapshots, interactive zoom and measurement cursors.**

This fork builds on the [Windows port by pvvx](https://github.com/pvvx/DSRemote), which itself is based on [DSRemote by Teunis van Beelen (Teuniz)](https://www.teuniz.net/DSRemote/). It focuses on making a Rigol DS2000-series oscilloscope useful as a high-resolution desktop measurement instrument.

> **Stable reference:** `dsremote-raw-v1.8` (13 November 2026)  
> **Tested instrument:** Rigol DS2302, firmware `00.03.06`  
> **Tested environment:** Windows 11 · Qt 5.15.2 · MinGW 8.1, 64-bit

## What's different in this fork?

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
- Python 3 with the `vxi11` module for the temporary bridge (commonly installed with `py -m pip install python-vxi11`)
- A compatible Rigol oscilloscope reachable over the local network

The Windows project links against Winsock with `win32:LIBS += -lws2_32` in `dsremote.pro`.

### Build DSRemote

1. Clone this fork and open **`dsremote.pro`** in Qt Creator.
2. Select the **Desktop Qt 5.15.2 MinGW 64-bit** kit.
3. Configure the desired Debug or Release build and build the project.
4. Run the Python bridge and then start DSRemote.

Qt runtime DLLs, generated `build/` directories, executables and personal Qt Creator `*.pro.user*` settings are deliberately excluded from new commits by `.gitignore`. Build and runtime dependencies must be provided on the local machine.

### Current LAN architecture

The current release still uses a **Python VXI-11 bridge**; native VXI-11 support in Qt is a future goal.

```text
DSRemote (Qt/C++ on Windows)
           |
           | local SCPI/TCP connection
           v
     127.0.0.1:5555
           |
           v
 ds_bridge_ds2000_v4_quiet.py
           |
           | VXI-11 over the LAN
           v
      Rigol DS2302
```

Configure the oscilloscope's LAN address in the bridge as appropriate for your installation. Start the bridge, for example, with:

```powershell
py ds_bridge_ds2000_v4_quiet.py
```

Then connect DSRemote to **`localhost`**, using the bridge listener on port **5555**. The bridge also handles certain DS2000 SCPI compatibility differences. It must remain running while DSRemote uses that connection.

**Note:** The script and its configuration reflect the tested local setup; this is not yet a standalone, universal Windows installer.

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

The names below document successive development steps; the published Git reference is the **V1.8 tag**.

| Milestone | Main change |
| --- | --- |
| Display / cursor work | Antialiasing, local zoom/pan, X/Y cursors, movable measurement panel, trigger-position correction |
| RAW V1.3 | Larger RAW depth handling, including the observed 280,000-point single-channel capture |
| RAW V1.4 | Fixed zero-length first-block handling and completed multi-block RAW transfer |
| RAW V1.5 | Extended RAW display zoom up to 2048×; retained 16× NORM limit |
| RAW V1.6 | Added RAW buttons, explicit loading, cache and status controls |
| RAW V1.7 | Clear LIVE/SNAPSHOT channel separation, cache preservation and stale-state indication |
| **RAW V1.8** | Restored CH1/CH2 settings in the bottom strip and added smooth, learning-based progress indication |

**Verified V1.8 reference:** `dsremote-raw-v1.8`.

## Known limitations and roadmap

**Current limitations**

- The tested RAW path requires the instrument to be in **STOP**; it is not continuous RAW streaming.
- The Python bridge remains necessary for the tested DS2302 LAN setup.
- Detailed behaviour has been tested on **one DS2302**; other models and firmware versions are not guaranteed.
- The RAW cache is an **in-memory working snapshot**, not a persistent waveform archive or two-slot comparison system.
- The visual zoom can show individual recorded samples but cannot recover detail beyond the acquired sample interval.

**Planned improvements**

- Replace the Python bridge with **native VXI-11 communication in Qt**.
- Add small, unobtrusive **Fine / Coarse** indicators next to the horizontal and vertical scale knobs.
- Add a **visual cursor target marker** at waveform intersections for more precise manual positioning — *without magnetic snapping or restricting free cursor movement*.
- Continue refining DS2000-specific usability while preserving the stable RAW acquisition path.

## Credits and license

- Original DSRemote: [Teunis van Beelen (Teuniz)](https://www.teuniz.net/DSRemote/)
- Windows port and upstream for this fork: [pvvx/DSRemote](https://github.com/pvvx/DSRemote)
- Windows 11 / DS2000 extensions documented here: this fork

This fork of DSRemote is coded mainly by ChatGPT, guided carefully and intensely by the owner of this repository.

The project retains the upstream **GNU General Public License v3**; see the repository's `LICENSE` file. Existing copyright notices and author credits remain applicable.

---

*This README describes the tested Windows 11 development state at tag `dsremote-raw-v1.8`, not a claim of general compatibility with every Rigol oscilloscope.*

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
