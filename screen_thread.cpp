/*
***************************************************************************
*
* Author: Teunis van Beelen
*
* Copyright (C) 2015, 2016, 2017, 2018, 2019 Teunis van Beelen
*
* Email: teuniz@protonmail.com
*
***************************************************************************
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
***************************************************************************
*/


#include "screen_thread.h"
#include "ds2302_raw_shared.h"
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QByteArray>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <QElapsedTimer>


#define SPECT_LOG_MINIMUM (0.00000001)
#define SPECT_LOG_MINIMUM_LOG (-80)



void ds2_raw_progress(unsigned generation, int received, int expected);

// --- DS2302 STOP/RAW: shared capture state; all instrument I/O stays here ---
namespace {
QMutex ds2_raw_mutex;
DS2RawSnapshot ds2_raw_state;
bool ds2_raw_pending = false;
// Higher STOP/RAW cap. The current LAN path accepts up to 2 MiB per binary block;
// this cap keeps a single channel below 1 MiB of raw ADC data.
constexpr int DS2_RAW_SAFE_POINT_LIMIT = 1400000;

bool ds2_scpi_write(const QString &s)
{
    const QByteArray cmd = s.toLatin1();
    return tmc_write(cmd.constData()) == cmd.size();
}

bool ds2_scpi_query(struct tmcdev *device, const QString &command, QString &answer)
{
    if(!ds2_scpi_write(command)) return false;
    const int n = tmc_read();
    if(n <= 0 || n > 512 || !device || !device->buf) return false;
    answer = QString::fromLatin1(device->buf, n).trimmed();
    return !answer.isEmpty();
}

// DS2302_CURSOR_MODE_SYNC_V1: Nur LESEN des undokumentierten Setup-Blocks.
// Der Payload hat 2814 Byte. Byte 1630: X=0, Y=4, X-Y=8.
// Niemals :SYST:SET zum Zurueckschreiben verwenden.
static bool ds2_cursor_display_read(struct tmcdev *device, int &value)
{
    if(!device || !ds2_scpi_write(":SYST:SET?")) return false;
    const int n = tmc_read();
    if(n <= 0 || !device->buf) return false;

    const unsigned char *data =
        reinterpret_cast<const unsigned char *>(device->buf);
    int len = n;
    // Beide vom Projekt bereits bekannten Empfangsvarianten unterstuetzen:
    // C++ LAN-Pfad liefert Payload direkt; ein anderer Pfad inkl. #9-Header.
    if(len >= 11 && data[0] == '#' && data[1] == '9' &&
        std::memcmp(data + 2, "000002814", 9) == 0)
    {
        data += 11;
        len -= 11;
    }
    if(!((len == 2814) || (len == 2815 && data[2814] == '\n')) ||
        std::memcmp(data + 4, "DS2A1", 5) != 0)
        return false;

    value = int(data[1630]);
    return value == 0 || value == 4 || value == 8;
}

static bool ds2_cursor_key(const char *key, unsigned delay_ms)
{
    if(!ds2_scpi_write(QString(":SYST:KEY:PRES ") + key)) return false;
    usleep(delay_ms * 1000u);
    return true;
}

// Synchroner SCPI-Ablauf, aber AUSSCHLIESSLICH im Screen-Thread.
// Interne Moduskommandos aus der Queue werden niemals ans Scope gesendet.
static bool ds2_cursor_mode_set(struct tmcdev *device, const char *requested)
{
    QString mode;
    if(!ds2_scpi_query(device, ":CURS:MODE?", mode)) return false;

    if(std::strcmp(requested, "OFF") == 0)
    {
        if(!mode.startsWith("OFF", Qt::CaseInsensitive))
        {
            if(!ds2_scpi_write(":CURS:MODE OFF")) return false;
            usleep(400000);
            if(!ds2_scpi_query(device, ":CURS:MODE?", mode)) return false;
        }
        return mode.startsWith("OFF", Qt::CaseInsensitive);
    }

    int desired = -1;
    if(std::strcmp(requested, "X") == 0) desired = 0;
    else if(std::strcmp(requested, "Y") == 0) desired = 4;
    else if(std::strcmp(requested, "XY") == 0) desired = 8;
    if(desired < 0) return false;

    // MAN per SCPI festlegen, ohne die problematische CURSor-Taste zu toggeln.
    if(!mode.startsWith("MAN", Qt::CaseInsensitive))
    {
        if(!ds2_scpi_write(":CURS:MODE MAN")) return false;
        usleep(450000);
        if(!ds2_scpi_query(device, ":CURS:MODE?", mode) ||
            !mode.startsWith("MAN", Qt::CaseInsensitive)) return false;
    }

    int current = -1;
    if(!ds2_cursor_display_read(device, current)) return false;
    if(current == desired) return true; // Menues unveraendert lassen.

    // Der doppelte Menueaufruf ist am Rigol erprobt und verhindert MAN -> TRAC.
    if(!ds2_cursor_key("ACQuire", 300) ||
        !ds2_cursor_key("CURSor", 300)) return false;
    if(!ds2_scpi_query(device, ":CURS:MODE?", mode) ||
        !mode.startsWith("MAN", Qt::CaseInsensitive)) return false;

    bool list_open = false;
    bool matched = false;
    for(int attempt = 0; attempt < 6; ++attempt)
    {
        if(!ds2_cursor_key("F2", 950)) break;
        list_open = true;
        if(!ds2_cursor_display_read(device, current)) break;
        if(current == desired) { matched = true; break; }
    }

    // Nur wenn F2 die Auswahl geoeffnet hat, KFUNction betaetigen.
    // Auch nach einem fehlgeschlagenen Versuch die Auswahlliste schliessen.
    if(list_open && !ds2_cursor_key("KFUNction", 400)) return false;
    if(!matched) return false;

    if(!ds2_cursor_display_read(device, current) || current != desired) return false;
    if(!ds2_scpi_query(device, ":CURS:MODE?", mode)) return false;
    return mode.startsWith("MAN", Qt::CaseInsensitive);
}

bool ds2_capture_channel(struct tmcdev *device, int ch, int depth,
                         DS2RawTrace &result, QString &why,
                         unsigned generation, int completed_before, int expected_total)
{
    if(depth < 2 || depth > DS2_RAW_SAFE_POINT_LIMIT)
    {
        why = QString("Speichertiefe %1 ausserhalb des RAW-Limits 2..%2")
        .arg(depth).arg(DS2_RAW_SAFE_POINT_LIMIT);
        return false;
    }

    // Only the source needs preserving: DSRemote itself always uses
    // NORM/BYTE/1..1400 outside the STOP/RAW analysis mode.
    // IMPORTANT: Restoring the old :WAV:POIN? value can re-apply the
    // corrupt 0/1/900-point range left over from a previous RAW read.
    QString old_source;
    if(!ds2_scpi_query(device, ":WAV:SOUR?", old_source))
    {
        why = "Waveform-Quelle nicht lesbar";
        return false;
    }

    bool touched = false;
    bool good = false;
    do {
        touched = true; // Even an incomplete RAW setup requires NORM cleanup.
        if(!ds2_scpi_write(QString(":WAV:SOUR CHAN%1").arg(ch + 1)) ||
            !ds2_scpi_write(":WAV:FORM BYTE") ||
            !ds2_scpi_write(":WAV:MODE RAW") ||
            !ds2_scpi_write(QString(":WAV:POIN %1").arg(depth)) ||
            !ds2_scpi_write(":WAV:STAR 1") ||
            !ds2_scpi_write(QString(":WAV:STOP %1").arg(depth)))
        {
            why = "RAW-Konfiguration gescheitert";
            break;
        }

        QString mode, fmt, start, stop, pre;
        if(!ds2_scpi_query(device, ":WAV:MODE?", mode) ||
            !ds2_scpi_query(device, ":WAV:FORM?", fmt) ||
            !ds2_scpi_query(device, ":WAV:STAR?", start) ||
            !ds2_scpi_query(device, ":WAV:STOP?", stop) ||
            mode.compare("RAW", Qt::CaseInsensitive) != 0 ||
            fmt.compare("BYTE", Qt::CaseInsensitive) != 0 ||
            start.toInt() != 1 || stop.toInt() != depth)
        {
            why = "RAW-Konfiguration vom Geraet nicht bestaetigt";
            break;
        }
        if(!ds2_scpi_query(device, ":WAV:PRE?", pre))
        {
            why = "RAW-Preamble fehlt";
            break;
        }
        const QStringList fields = pre.split(',');
        if(fields.size() != 10 || fields[1].toInt() != 2)
        {
            why = QString("Unerwartete RAW-Preamble: %1").arg(pre);
            break;
        }
        bool ok[6] = {false, false, false, false, false, false};
        result.xincrement = fields[4].toDouble(&ok[0]);
        result.xorigin = fields[5].toDouble(&ok[1]);
        result.xreference = fields[6].toDouble(&ok[2]);
        result.yincrement = fields[7].toDouble(&ok[3]);
        result.yorigin = fields[8].toDouble(&ok[4]);
        result.yreference = fields[9].toDouble(&ok[5]);
        bool numbers_ok = true;
        for(int i = 0; i < 6; ++i) numbers_ok = numbers_ok && ok[i];
        if(!numbers_ok || !(result.xincrement > 0.0) ||
            !(result.yincrement > 0.0) ||
            !std::isfinite(result.xorigin) || !std::isfinite(result.yorigin))
        {
            why = "Ungueltige RAW-Zeit-/Spannungskalibrierung";
            break;
        }

        if(!ds2_scpi_write(":WAV:RESet") || !ds2_scpi_write(":WAV:BEGin"))
        {
            why = "RAW-Transfer konnte nicht gestartet werden";
            break;
        }
        result.bytes.reserve(depth);
        // DS2302 firmware may first reply READ,0 with an empty IEEE-488.2
        // binary block, then deliver the requested bytes in later blocks.
        // Observed with 280000 samples: READ,0 -> READ,126945 -> IDLE,153055.
        // A zero-length block is NOT a transfer error while status is READ.
        int consecutive_empty_blocks = 0;
        for(int block = 0; block < 128 && result.bytes.size() < depth; ++block)    {
            QString status;
            if(!ds2_scpi_query(device, ":WAV:STAT?", status) ||
                !ds2_scpi_write(":WAV:DATA?"))
            {
                why = "RAW-Blockstatus oder Datenabfrage gescheitert";
                break;
            }
            const int n = tmc_read();
            const bool still_reading = status.startsWith("READ", Qt::CaseInsensitive);
            const bool finished = status.startsWith("IDLE", Qt::CaseInsensitive);
            fprintf(stderr, "DS2302 RAW CH%d: Block %d, Status=%s, Bytes=%d, Summe=%d/%d\n",
                    ch + 1, block + 1, status.toLatin1().constData(), n,
                    result.bytes.size() + (n > 0 ? n : 0), depth);

            if(!still_reading && !finished)
            {
                why = QString("Unbekannter RAW-Blockstatus: %1").arg(status);
                break;
            }
            if(n < 0 || n > depth - result.bytes.size() || (n > 0 && !device->buf))
            {
                why = QString("Ungueltige RAW-Blocklaenge %1 (%2)").arg(n).arg(status);
                break;
            }
            if(n == 0)
            {
                ++consecutive_empty_blocks;
                if(finished)
                {
                    why = QString("RAW beendet nach %1/%2 Bytes (letzter Block leer)")
                    .arg(result.bytes.size()).arg(depth);
                    break;
                }
                if(consecutive_empty_blocks >= 20)
                {
                    why = "RAW: 20 leere READ-Bloecke hintereinander";
                    break;
                }
                usleep(100000);  // Give the stopped DS2302 time to prepare data.
                continue;
            }
            consecutive_empty_blocks = 0;
            const int old_size = result.bytes.size();
            result.bytes.resize(old_size + n);
            std::memcpy(result.bytes.data() + old_size, device->buf, n);
            ds2_raw_progress(generation, completed_before + result.bytes.size(), expected_total);
            if(finished) break;
        }
        if(result.bytes.size() != depth)
        {
            if(why.isEmpty())
                why = QString("RAW unvollstaendig: %1/%2 Bytes").arg(result.bytes.size()).arg(depth);
            break;
        }
        result.first_point = 1;
        good = true;
    } while(false);

    // Mandatory cleanup after EVERY RAW attempt, successful or not.
    // This exact order was verified on the DS2302 with two enabled channels:
    // :WAV:POIN 1400 changes both STAR and STOP to 1..1400.
    // The oscilloscope remains STOP; no acquisition settings are changed.
    if(touched)
    {
        const QStringList commands = QStringList()
        << ":WAV:END"
        << (QString(":WAV:SOUR ") + old_source)
        << ":WAV:MODE NORM"
        << ":WAV:FORM BYTE"
        << ":WAV:POIN 1400"
        << ":WAV:STAR 1"
        << ":WAV:STOP 1400";
        bool restored = true;
        // Do not short-circuit: even if one command fails, continue attempting
        // the remaining recovery commands.
        for(const QString &command : commands)
        {
            if(!ds2_scpi_write(command))
            {
                restored = false;
                fprintf(stderr, "DS2302 RAW: cleanup command failed: %s\n",
                        command.toLatin1().constData());
            }
        }

        if(restored)
        {
            QString mode, format, points, start, stop;
            restored = ds2_scpi_query(device, ":WAV:MODE?", mode) &&
                       ds2_scpi_query(device, ":WAV:FORM?", format) &&
                       ds2_scpi_query(device, ":WAV:POIN?", points) &&
                       ds2_scpi_query(device, ":WAV:STAR?", start) &&
                       ds2_scpi_query(device, ":WAV:STOP?", stop) &&
                       mode.compare("NORM", Qt::CaseInsensitive) == 0 &&
                       format.compare("BYTE", Qt::CaseInsensitive) == 0 &&
                       points.toInt() == 1400 && start.toInt() == 1 &&
                       stop.toInt() == 1400;
            if(!restored)
                fprintf(stderr, "DS2302 RAW: NORM 1..1400 verification failed\n");
        }
        if(!restored)
        {
            why += " (WARNUNG: NORM 1..1400 konnte nicht bestaetigt werden)";
            return false;
        }
    }
    return good;
}
} // namespace

DS2RawSnapshot ds2_raw_snapshot()
{
    QMutexLocker lock(&ds2_raw_mutex);
    return ds2_raw_state;
}

// All RAW-state changes are serialized; the Qt GUI never accesses SCPI directly.
static bool ds2_has_cached_locked()
{
    for(int c=0; c<4; ++c)
        if(ds2_raw_state.traces[c].valid()) return true;
    return false;
}

void ds2_raw_toggle()
{
    QMutexLocker lock(&ds2_raw_mutex);
    if(ds2_raw_state.busy) return;
    if(ds2_raw_state.enabled)
    {
        // NORM: keep the RAW cache intact, never re-fetch on a simple toggle.
        ds2_raw_state.enabled = false;
        ds2_raw_state.message = "NORM (RAW gespeichert)";
    }
    else if(ds2_has_cached_locked())
    {
        ds2_raw_state.enabled = true; // read only; no communication
        ds2_raw_state.message = ds2_raw_state.stale
                                    ? "RAW gespeichert (Aufnahme inzwischen geaendert)" : "RAW gespeichert";
    }
    // No cache: do not fetch implicitly. Only "RAW laden" starts I/O.
    // This also prevents turning on a misleading empty SNAPSHOT mode.
}

bool ds2_raw_reload()
{
    QMutexLocker lock(&ds2_raw_mutex);
    if(ds2_raw_state.busy) return false;
    ++ds2_raw_state.generation;
    ds2_raw_state.enabled = true;
    ds2_raw_state.busy = true;
    ds2_raw_state.progress_done = 0;
    ds2_raw_state.progress_total = 0;
    ds2_raw_state.message = "RAW lade Daten ...";
    ds2_raw_state.load_failed = false;
    // Keep the old cache until a successful replacement has arrived.
    ds2_raw_pending = true;
    return true;
}

void ds2_raw_progress(unsigned generation, int received, int expected)
{
    QMutexLocker lock(&ds2_raw_mutex);
    if(ds2_raw_state.generation != generation || !ds2_raw_state.busy) return;
    ds2_raw_state.progress_done = received;
    ds2_raw_state.progress_total = expected;
}

void ds2_raw_cancel_if_running()
{
    QMutexLocker lock(&ds2_raw_mutex);
    // RUN does not destroy a captured STOP waveform. It only marks it as old.
    if(ds2_has_cached_locked()) ds2_raw_state.stale = true;
}

bool ds2_raw_take_request(unsigned &generation)
{
    QMutexLocker lock(&ds2_raw_mutex);
    if(!ds2_raw_pending) return false;
    ds2_raw_pending = false;
    generation = ds2_raw_state.generation;
    return true;
}

void ds2_raw_publish(unsigned generation, const DS2RawSnapshot &state)
{
    QMutexLocker lock(&ds2_raw_mutex);
    if(ds2_raw_state.generation != generation) return;
    const bool visible = ds2_raw_state.enabled;
    bool any_new = false;
    for(int i=0; i<4; ++i) any_new |= state.traces[i].valid();
    if(state.complete && any_new)
    {
        // Atomic replacement: all requested channels must be present.
        // A new CH1-only capture also removes a CH2 trace from the old cache.
        for(int i=0; i<4; ++i) ds2_raw_state.traces[i] = state.traces[i];
        ds2_raw_state.complete = true;
        ds2_raw_state.stale = false;
        ds2_raw_state.load_failed = false;
    }
    else
    {
        // A failed refresh does not destroy or partly replace a good snapshot.
        ds2_raw_state.load_failed = true;
    }
    // Failed refresh does not destroy a previously successful cache.
    ds2_raw_state.message = state.message;
    ds2_raw_state.enabled = visible && ds2_has_cached_locked();
    ds2_raw_state.busy = false;
    ds2_raw_state.progress_done = ds2_raw_state.progress_total;
}

void screen_thread::set_device(struct tmcdev *tmdev)
{
    params.cmd_cue_idx_in = 0;
    params.cmd_cue_idx_out = 0;
    params.connected = 0;
    params.ds2_last_scope_activechannel = -1;
    params.ds2_pending_activechannel = -1;
    params.ds2_timescale_last_valid = 0;
    params.ds2_timescale_change_pending = 0;

    device = tmdev;

    ds2_trigger_sync_timer.start();
    ds2_position_sync_timer.start();
}


screen_thread::screen_thread()
{
    int i;

    device = NULL;

    h_busy = 0;

    for(i=0; i<MAX_CHNS; i++)
    {
        params.wavebuf[i] = (short *)malloc(WAVFRM_MAX_BUFSZ);
    }

    params.cmd_cue_idx_in = 0;
    params.cmd_cue_idx_out = 0;
    params.connected = 0;
    params.ds2_trigger_sync_valid = 0;
    params.ds2_position_sync_valid = 0;
    params.ds2_timescale_last_valid = 0;
    params.ds2_timescale_change_pending = 0;
}


screen_thread::~screen_thread()
{
    int i;

    for(i=0; i<MAX_CHNS; i++)
    {
        free(params.wavebuf[i]);
    }
}


void screen_thread::set_params(struct device_settings *dev_parms)
{
    deviceparms = dev_parms;
    params.connected = deviceparms->connected;
    params.modelserie = deviceparms->modelserie;
    // At connect, read_settings_thread already fetched :TIM:SCAL?.
    // Use this as the baseline before the first periodic poll.
    if(params.modelserie == 2 && !params.ds2_timescale_last_valid &&
        std::isfinite(deviceparms->timebasescale) &&
        deviceparms->timebasescale > 0.0)
    {
        params.ds2_timescale_last_polled = deviceparms->timebasescale;
        params.ds2_timescale_last_valid = 1;
    }
    params.triggermode = deviceparms->triggermode;
    params.chandisplay[0] = deviceparms->chandisplay[0];
    params.chandisplay[1] = deviceparms->chandisplay[1];
    params.chandisplay[2] = deviceparms->chandisplay[2];
    params.chandisplay[3] = deviceparms->chandisplay[3];
    params.chanscale[0] = deviceparms->chanscale[0];
    params.chanscale[1] = deviceparms->chanscale[1];
    params.chanscale[2] = deviceparms->chanscale[2];
    params.chanscale[3] = deviceparms->chanscale[3];
    params.countersrc = deviceparms->countersrc;
    params.cmd_cue_idx_in = deviceparms->cmd_cue_idx_in;
    params.math_fft_src = deviceparms->math_fft_src;
    params.math_fft = deviceparms->math_fft;
    params.math_fft_unit = deviceparms->math_fft_unit;
    params.fftbuf_in = deviceparms->fftbuf_in;
    params.fftbuf_out = deviceparms->fftbuf_out;
    params.fftbufsz = deviceparms->fftbufsz;
    params.k_cfg = deviceparms->k_cfg;
    params.kiss_fftbuf = deviceparms->kiss_fftbuf;
    params.current_screen_sf = deviceparms->current_screen_sf;
    params.debug_str[0] = 0;
    params.func_wrec_enable = deviceparms->func_wrec_enable;
    params.func_wrec_operate = deviceparms->func_wrec_operate;
    params.func_wplay_operate = deviceparms->func_wplay_operate;
    params.func_wplay_fcur = deviceparms->func_wplay_fcur;
    params.func_wrec_fmax = deviceparms->func_wrec_fmax;
    params.func_wrep_fmax = deviceparms->func_wplay_fmax;
}


void screen_thread::get_params(struct device_settings *dev_parms)
{
    int i;

    dev_parms->connected = params.connected;
    dev_parms->triggerstatus = params.triggerstatus;
    dev_parms->triggersweep = params.triggersweep;
    dev_parms->samplerate = params.samplerate;
    dev_parms->acquirememdepth = params.memdepth;
    dev_parms->counterfreq = params.counterfreq;
    dev_parms->wavebufsz = params.wavebufsz;
    for(i=0; i<MAX_CHNS; i++)
    {
        if(params.chandisplay[i])
        {
            memcpy(dev_parms->wavebuf[i], params.wavebuf[i], params.wavebufsz * sizeof(short));
            dev_parms->xorigin[i] = params.xorigin[i];
        }
    }
    dev_parms->thread_error_stat = params.error_stat;
    dev_parms->thread_error_line = params.error_line;
    dev_parms->cmd_cue_idx_out = params.cmd_cue_idx_out;
    dev_parms->thread_result = params.result;
    // DS2302: Aktive Kanalauswahl nur bei einer Aenderung uebernehmen
    if(params.modelserie == 2 &&
        params.ds2_pending_activechannel >= 0)
    {
        const int ch = params.ds2_pending_activechannel;

        if(ch < MAX_CHNS && params.chandisplay[ch])
        {
            dev_parms->activechannel = ch;
        }

        params.ds2_pending_activechannel = -1;
    }
    dev_parms->thread_job = params.job;

    // Laeuft im GUI-Thread: die beiden zusammengehoerigen Werte atomar
    // aus dem abgeschlossenen Screen-Thread uebernehmen. Kein SCPI-Write.
    if(params.modelserie == 2 && params.ds2_trigger_sync_valid)
    {
        const int chn = params.ds2_trigger_sync_source;
        if(chn >= 0 && chn < dev_parms->channel_cnt)
        {
            dev_parms->triggeredgelevel[chn] = params.ds2_trigger_sync_level;
            dev_parms->triggeredgesource = chn;
        }
    }
    // DS2302: Positionen aus dem Scope uebernehmen.
    if(params.modelserie == 2 && params.ds2_position_sync_valid)
    {
        dev_parms->chanoffset[0] = params.ds2_position_chanoffset[0];
        dev_parms->chanoffset[1] = params.ds2_position_chanoffset[1];
        dev_parms->timebaseoffset = params.ds2_position_timeoffset;

        dev_parms->chandisplay[0] = params.chandisplay[0];
        dev_parms->chandisplay[1] = params.chandisplay[1];
    }
    // DS2302_TIMEBASE_SCOPE_SYNC_V1: accept the newly observed scope timebase
    // only if DSRemote still shows the PREVIOUS scope value. When the operator
    // has already changed the timebase locally, preserve the local request.
    // This is deliberately event-driven rather than copying every poll result.
    if(params.modelserie == 2 && params.ds2_timescale_change_pending)
    {
        const double old_scale = params.ds2_timescale_change_from;
        const double new_scale = params.ds2_timescale_change_to;
        const double tolerance = std::max(1e-15, std::fabs(old_scale) * 1e-7);
        if(std::isfinite(new_scale) && new_scale > 0.0 &&
            std::fabs(dev_parms->timebasescale - old_scale) <= tolerance)
        {
            dev_parms->timebasescale = new_scale;
            if(!dev_parms->timebasedelayenable)
                dev_parms->current_screen_sf = 100.0 / new_scale;
        }
        params.ds2_timescale_change_pending = 0;
    }
    if(dev_parms->thread_job == TMC_THRD_JOB_TRIGEDGELEV)
    {
        dev_parms->thread_value = params.triggeredgelevel;
    }
    if(dev_parms->thread_job == TMC_THRD_JOB_TIMDELAY)
    {
        dev_parms->timebasedelayoffset = params.timebasedelayoffset;
        dev_parms->timebasedelayscale = params.timebasedelayscale;
    }
    if(dev_parms->thread_job == TMC_THRD_JOB_FFTHZDIV)
    {
        dev_parms->math_fft_hscale = params.math_fft_hscale;
        dev_parms->math_fft_hcenter = params.math_fft_hcenter;
    }
    if(dev_parms->func_wrec_enable)
    {
        dev_parms->func_wrec_operate = params.func_wrec_operate;
        dev_parms->func_wplay_operate = params.func_wplay_operate;
        deviceparms->func_wplay_fcur = params.func_wplay_fcur;
        deviceparms->func_wrec_fmax = params.func_wrec_fmax;
        deviceparms->func_wplay_fmax = params.func_wrep_fmax;
    }
    if(params.debug_str[0])
    {
        params.debug_str[1023] = 0;

        printf("params.debug_str: ->%s<-\n", params.debug_str);
    }
}


int screen_thread::get_devicestatus()
{
    int line;

    usleep(TMC_GDS_DELAY);

    if(tmc_write(":TRIG:STAT?") != 11)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(tmc_read() < 1)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(!strcmp(device->buf, "TD"))
    {
        params.triggerstatus = 0;
    }
    else if(!strcmp(device->buf, "WAIT"))
    {
        params.triggerstatus = 1;
    }
    else if(!strcmp(device->buf, "RUN"))
    {
        params.triggerstatus = 2;
    }
    else if(!strcmp(device->buf, "AUTO"))
    {
        params.triggerstatus = 3;
    }
    else if(!strcmp(device->buf, "FIN"))
    {
        params.triggerstatus = 4;
    }
    else if(!strcmp(device->buf, "STOP"))
    {
        params.triggerstatus = 5;
    }
    else
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    usleep(TMC_GDS_DELAY);

    if(tmc_write(":TRIG:SWE?") != 10)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(tmc_read() < 1)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(!strcmp(device->buf, "AUTO"))
    {
        params.triggersweep = 0;
    }
    else if(!strcmp(device->buf, "NORM"))
    {
        params.triggersweep = 1;
    }
    else if(!strcmp(device->buf, "SING"))
    {
        params.triggersweep = 2;
    }
    else
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    usleep(TMC_GDS_DELAY);

    if(tmc_write(":ACQ:SRAT?") != 10)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(tmc_read() < 1)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    params.samplerate = atof(device->buf);

    usleep(TMC_GDS_DELAY);

    if(tmc_write(":ACQ:MDEP?") != 10)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    if(tmc_read() < 1)
    {
        line = __LINE__;
        goto OUT_ERROR;
    }

    params.memdepth = atoi(device->buf);

    // DS2302: etwa jede Sekunde die tatsaechliche EDGE-Triggerquelle und
    // deren Level einlesen, ohne den Trigger am Oszi umzuschalten.
    // Nur bei normalem Status-Polling (nicht waehrend CMD/RAW-Verarbeitung).
    if(params.modelserie == 2 && params.triggermode == 0 &&
        ds2_trigger_sync_timer.isValid() &&
        ds2_trigger_sync_timer.elapsed() >= 1000)
    {
        ds2_trigger_sync_timer.restart();

        QString trig_source, trig_level;
        if(!ds2_scpi_query(device, ":TRIG:EDG:SOUR?", trig_source))
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        int chn = -1;
        if(trig_source == "CHAN1") chn = 0;
        else if(trig_source == "CHAN2") chn = 1;

        // Nur CH1/CH2 werden derzeit synchronisiert, keine EXT-Quelle.
        if(chn >= 0)
        {
            if(!ds2_scpi_query(device, ":TRIG:EDG:LEV?", trig_level))
            {
                line = __LINE__;
                goto OUT_ERROR;
            }

            bool valid_level = false;
            const double level = trig_level.toDouble(&valid_level);
            if(valid_level && std::isfinite(level))
            {
                params.ds2_trigger_sync_source = chn;
                params.ds2_trigger_sync_level = level;
                params.ds2_trigger_sync_valid = 1;
            }
        }
    }

    // DS2302: CH1-/CH2-Position, Horizontalposition UND Zeitbasis
    // aus einem zusammengehoerigen Status-Poll lesen.
    if(params.modelserie == 2 &&
        ds2_position_sync_timer.isValid() &&
        ds2_position_sync_timer.elapsed() >= 1000)
    {
        ds2_position_sync_timer.restart();

        QString ch1_text, ch2_text, time_text, scale_text;
        QString disp1_text, disp2_text;

        if(!ds2_scpi_query(device, ":CHAN1:OFFS?", ch1_text) ||
            !ds2_scpi_query(device, ":CHAN2:OFFS?", ch2_text) ||
            !ds2_scpi_query(device, ":TIM:OFFS?", time_text) ||
            !ds2_scpi_query(device, ":TIM:SCAL?", scale_text) ||
            !ds2_scpi_query(device, ":CHAN1:DISP?", disp1_text) ||
            !ds2_scpi_query(device, ":CHAN2:DISP?", disp2_text))
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;

        const double ch1 = ch1_text.toDouble(&ok1);
        const double ch2 = ch2_text.toDouble(&ok2);
        const double tim = time_text.toDouble(&ok3);
        const double scale = scale_text.toDouble(&ok4);

        if(ok1 && ok2 && ok3 && ok4 &&
            std::isfinite(ch1) &&
            std::isfinite(ch2) &&
            std::isfinite(tim) &&
            std::isfinite(scale) && scale > 0.0)
        {
            params.ds2_position_chanoffset[0] = ch1;
            params.ds2_position_chanoffset[1] = ch2;
            params.ds2_position_timeoffset = tim;

            // Only publish an actual CHANGE measured at the scope.
            // Always keep the previous scope scale for the GUI-side conflict check.
            if(params.ds2_timescale_last_valid)
            {
                const double previous = params.ds2_timescale_last_polled;
                const double tolerance = std::max(1e-15, std::fabs(previous) * 1e-7);
                if(std::fabs(scale - previous) > tolerance)
                {
                    params.ds2_timescale_change_from = previous;
                    params.ds2_timescale_change_to = scale;
                    params.ds2_timescale_change_pending = 1;
                }
            }
            params.ds2_timescale_last_polled = scale;
            params.ds2_timescale_last_valid = 1;
            const QString d1 = disp1_text.trimmed();
            const QString d2 = disp2_text.trimmed();

            if((d1 == "0" || d1 == "1") &&
                (d2 == "0" || d2 == "1"))
            {
                params.chandisplay[0] = (d1 == "1");
                params.chandisplay[1] = (d2 == "1");
            }
            params.ds2_position_sync_valid = 1;
        }
    }

    // DS2302: Diagnose des aktiven Kanals aus dem Setup-Binaerblock
    if(params.modelserie == 2)
    {
        static QElapsedTimer setup_timer;
        static int last_channel_mask = -1;

        const int channel_mask =
            (params.chandisplay[0] ? 1 : 0) |
            (params.chandisplay[1] ? 2 : 0);

        if(!setup_timer.isValid() ||
            setup_timer.elapsed() >= 5000 ||
            channel_mask != last_channel_mask)
        {
            setup_timer.start();
            last_channel_mask = channel_mask;

            if(ds2_scpi_write(":SYST:SET?"))
            {
                const int n = tmc_read();

                if(n > 0 && device && device->buf)
                {
                    const unsigned char *data =
                        reinterpret_cast<const unsigned char *>(device->buf);

                    int len = n;

                    // Manche Empfangspfade liefern den IEEE-488.2-Header mit.
                    if(len >= 11 &&
                        data[0] == '#' && data[1] == '9' &&
                        std::memcmp(data + 2, "000002814", 9) == 0)
                    {
                        data += 11;
                        len -= 11;
                    }

                    // 2814 Nutzbytes, ggf. mit abschliessendem LF.
                    if((len == 2814 ||
                         (len == 2815 && data[2814] == '\n')) &&
                        std::memcmp(data + 4, "DS2A1", 5) == 0)
                    {
                        const int a = data[1011];
                        const int b = data[1889];

                        if((a == 1 && b == 0) ||
                            (a == 2 && b == 1))
                        {
                            const int selected = a - 1;  // CH1 -> 0, CH2 -> 1

                            if(selected != params.ds2_last_scope_activechannel)
                            {
                                params.ds2_last_scope_activechannel = selected;
                                params.ds2_pending_activechannel = selected;
                            }
                            fprintf(stderr,
                                    "DS2302 SETUP: CH%d aktiv, Mask=%d, Bytes=%d\n",
                                    a, channel_mask, n);
                        }
                        else
                        {
                            fprintf(stderr,
                                    "DS2302 SETUP: unerwartete Werte %d / %d\n",
                                    a, b);
                        }
                    }
                    else
                    {
                        fprintf(stderr,
                                "DS2302 SETUP: unbekanntes Datenformat, Bytes=%d\n",
                                n);
                    }
                }
                else
                {
                    fprintf(stderr,
                            "DS2302 SETUP: Leseversuch fehlgeschlagen (%d)\n",
                            n);
                }
            }
        }
    }

    if(params.countersrc)
    {
        usleep(TMC_GDS_DELAY);

        if(tmc_write(":MEAS:COUN:VAL?") != 15)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        params.counterfreq = atof(device->buf);
    }

    if(params.func_wrec_enable)
    {
        usleep(TMC_GDS_DELAY);

        if(tmc_write(":FUNC:WREC:OPER?") != 16)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(params.modelserie == 6)
        {
            if(!strcmp(device->buf, "REC"))
            {
                params.func_wrec_operate = 1;
            }
            else if(!strcmp(device->buf, "STOP"))
            {
                params.func_wrec_operate = 0;
            }
            else
            {
                line = __LINE__;
                goto OUT_ERROR;
            }
        }
        else
        {
            if(!strcmp(device->buf, "RUN"))
            {
                params.func_wrec_operate = 1;
            }
            else if(!strcmp(device->buf, "STOP"))
            {
                params.func_wrec_operate = 0;
            }
            else
            {
                line = __LINE__;
                goto OUT_ERROR;
            }
        }

        usleep(TMC_GDS_DELAY);

        if(tmc_write(":FUNC:WREP:OPER?") != 16)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(!strcmp(device->buf, "PLAY"))
        {
            params.func_wplay_operate = 1;
        }
        else if(!strcmp(device->buf, "STOP"))
        {
            params.func_wplay_operate = 0;
        }
        else if(!strcmp(device->buf, "PAUS"))
        {
            params.func_wplay_operate = 2;
        }
        else
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        usleep(TMC_GDS_DELAY);

        if(tmc_write(":FUNC:WREP:FCUR?") != 16)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        params.func_wplay_fcur = atoi(device->buf);

        usleep(TMC_GDS_DELAY);

        if(tmc_write(":FUNC:WREC:FMAX?") != 16)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        params.func_wrec_fmax = atoi(device->buf);

        usleep(TMC_GDS_DELAY);

        if(tmc_write(":FUNC:WREP:FMAX?") != 16)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        if(tmc_read() < 1)
        {
            line = __LINE__;
            goto OUT_ERROR;
        }

        params.func_wrep_fmax = atoi(device->buf);
    }

    params.debug_str[0] = 0;

    return 0;

OUT_ERROR:

    strlcpy(params.debug_str, device->hdrbuf, 1024);

    params.error_line = line;

    return -1;
}


void screen_thread::run()
{
    int i, j, k, n=0, chns=0, line, cmd_sent=0;

    char str[512];

    double y_incr, binsz;

    params.error_stat = 0;

    params.result = TMC_THRD_RESULT_NONE;

    params.job = TMC_THRD_JOB_NONE;

    params.ds2_trigger_sync_valid = 0;
    params.ds2_position_sync_valid = 0;

    params.wavebufsz = 0;

    if(device == NULL)
    {
        return;
    }

    if(h_busy)
    {
        return;
    }

    if(!params.connected)
    {
        h_busy = 0;

        return;
    }

    h_busy = 1;

    while(params.cmd_cue_idx_out != params.cmd_cue_idx_in)
    {
        usleep(TMC_GDS_DELAY);

        // DS2302_CURSOR_MODE_SYNC_V1: ein interner Auftrag in der bestehenden
        // Befehlswarteschlange. Diesen String NICHT an das Rigol senden.
        const char *queued = deviceparms->cmd_cue[params.cmd_cue_idx_out];
        if(std::strncmp(queued, "@DS2:CURSOR:", 12) == 0)
        {
            const char *requested = queued + 12;
            const bool ok = (params.modelserie == 2) &&
                            ds2_cursor_mode_set(device, requested);
            fprintf(stderr, "DS2302 Cursor-Mode %s: %s\n",
                    requested, ok ? "OK" : "FEHLER");
            fflush(stderr);
            params.cmd_cue_idx_out = (params.cmd_cue_idx_out + 1) % TMC_CMD_CUE_SZ;
            cmd_sent = 1;
            continue;
        }

        tmc_write(deviceparms->cmd_cue[params.cmd_cue_idx_out]);

        if(deviceparms->cmd_cue_resp[params.cmd_cue_idx_out] != NULL)
        {
            usleep(TMC_GDS_DELAY);

            if(tmc_read() < 1)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            strlcpy(deviceparms->cmd_cue_resp[params.cmd_cue_idx_out], device->buf, 128);
        }

        if((!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":TLHA", 5)) ||
            ((!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":CHAN", 5)) &&
             (!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out] + 6, ":SCAL ", 6))))
        {
            usleep(TMC_GDS_DELAY);

            if(tmc_write(":TRIG:EDG:LEV?") != 14)
            {
                printf("Can not write to device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            if(tmc_read() < 1)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            params.triggeredgelevel = atof(device->buf);

            params.job = TMC_THRD_JOB_TRIGEDGELEV;
        }
        else if(!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":TIM:DEL:ENAB 1", 15))
        {
            usleep(TMC_GDS_DELAY);

            if(tmc_write(":TIM:DEL:OFFS?") != 14)
            {
                printf("Can not write to device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            if(tmc_read() < 1)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            params.timebasedelayoffset = atof(device->buf);

            usleep(TMC_GDS_DELAY);

            if(tmc_write(":TIM:DEL:SCAL?") != 14)
            {
                printf("Can not write to device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            if(tmc_read() < 1)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            params.timebasedelayscale = atof(device->buf);

            params.job = TMC_THRD_JOB_TIMDELAY;
        }

        if(params.math_fft)
        {
            if((!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":TIM:SCAL ", 10)) ||
                (!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":MATH:OPER FFT", 14)) ||
                (!strncmp(deviceparms->cmd_cue[params.cmd_cue_idx_out], ":CALC:MODE FFT", 14)))
            {
                usleep(TMC_GDS_DELAY * 10);

                if(params.modelserie != 1)
                {
                    if(tmc_write(":CALC:FFT:HSP?") != 14)
                    {
                        line = __LINE__;
                        goto OUT_ERROR;
                    }
                }
                else
                {
                    if(tmc_write(":MATH:FFT:HSC?") != 14)
                    {
                        printf("Can not write to device.\n");
                        line = __LINE__;
                        goto OUT_ERROR;
                    }
                }

                if(tmc_read() < 1)
                {
                    printf("Can not read from device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }

                params.math_fft_hscale = atof(device->buf);

                usleep(TMC_GDS_DELAY);

                if(params.modelserie != 1)
                {
                    if(tmc_write(":CALC:FFT:HCEN?") != 15)
                    {
                        line = __LINE__;
                        goto OUT_ERROR;
                    }
                }
                else
                {
                    if(tmc_write(":MATH:FFT:HCEN?") != 15)
                    {
                        printf("Can not write to device.\n");
                        line = __LINE__;
                        goto OUT_ERROR;
                    }
                }

                if(tmc_read() < 1)
                {
                    printf("Can not read from device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }

                params.math_fft_hcenter = atof(device->buf);

                params.job = TMC_THRD_JOB_FFTHZDIV;
            }
        }

        params.cmd_cue_idx_out++;

        params.cmd_cue_idx_out %= TMC_CMD_CUE_SZ;

        cmd_sent = 1;
    }

    if(cmd_sent)
    {
        h_busy = 0;

        params.result = TMC_THRD_RESULT_CMD;

        return;
    }

    params.error_stat = get_devicestatus();

    // RUN invalidates a cached STOP snapshot; never show old RAW as live data.
    if(params.modelserie == 2 && !params.error_stat && params.triggerstatus != 5)
        ds2_raw_cancel_if_running();

    if(params.error_stat)
    {
        h_busy = 0;

        return;
    }

    if(!params.connected)
    {
        h_busy = 0;

        return;
    }

    for(i=0; i<MAX_CHNS; i++)
    {
        if(!params.chandisplay[i])  // Download data only when channel is switched on
        {
            continue;
        }

        chns++;
    }

    if(!chns)
    {
        h_busy = 0;

        return;
    }

    // Capture on demand only. The normal NORM channel loop below is unchanged.
    // The request is set by Ctrl+Shift+R in SignalCurve, NEVER by the worker.
    if(params.modelserie == 2)
    {
        unsigned ticket = 0;
        if(ds2_raw_take_request(ticket))
        {
            DS2RawSnapshot snap;
            snap.enabled = true;
            if(params.triggerstatus != 5)
            {
                snap.message = "RAW nur im STOP-Zustand verfuegbar. Umschalten und erneut versuchen.";
            }
            else
            {
                const int depth = params.memdepth;
                if(depth < 2 || depth > DS2_RAW_SAFE_POINT_LIMIT)
                {
                    snap.message = QString("RAW: Speichertiefe %1 ausserhalb Limit 2..%2. Keine Daten gelesen.")
                    .arg(depth).arg(DS2_RAW_SAFE_POINT_LIMIT);
                }
                else
                {
                    bool success = true;
                    int nch = 0;
                    const int requested_channels = int(bool(params.chandisplay[0])) +
                                                   int(bool(params.chandisplay[1]));
                    const int expected_total = depth * requested_channels;
                    ds2_raw_progress(ticket, 0, expected_total);
                    for(int channel = 0; channel < 2; ++channel)
                    {
                        if(!params.chandisplay[channel]) continue;
                        ++nch;
                        QString reason;
                        if(!ds2_capture_channel(device, channel, depth, snap.traces[channel], reason,
                                                 ticket, (nch - 1) * depth, expected_total))
                        {
                            success = false;
                            snap.message = QString("RAW CH%1: %2").arg(channel+1).arg(reason);
                            break;
                        }
                    }
                    if(success)
                    {
                        snap.complete = true;
                        snap.message = QString("RAW STOP: %1 Punkt(e) je Kanal, %2 Kanal/Kanaele")
                                           .arg(depth).arg(nch);
                    }
                }
            }
            ds2_raw_publish(ticket, snap);
        }
    }

    // DS2302 SINGLE live update: poll NORM waveform data while the Rigol
    // reports TD (triggered, acquisition still running), just like RUN.
    // The bounded NORM-LAN recovery in tmc_read() discards malformed
    // replies; its -4 result is handled below by skipping only that poll.
    // No change to the STOP/RAW capture path.

    params.result = TMC_THRD_RESULT_SCRN;

    //struct waveform_preamble wfp;

    //  if(params.triggerstatus != 1)  // Don't download waveform data when triggerstatus is "wait"
    if(1)
    {
        for(i=0; i<MAX_CHNS; i++)
        {
            if(!params.chandisplay[i])  // Download data only when channel is switched on
            {
                continue;
            }

            ///////////////////////////////////////////////////////////

            //     tmc_write(":WAV:PRE?");
            //
            //     n = tmc_read();
            //
            //     if(n < 0)
            //     {
            //       strlcpy(str, "Can not read from device.", 512);
            //       goto OUT_ERROR;
            //     }
            //
            //     printf("waveform preamble: %s\n", device->buf);
            //
            //     if(parse_preamble(device->buf, device->sz, &wfp, i))
            //     {
            //       strlcpy(str, "Preamble parsing error.", 512);
            //       goto OUT_ERROR;
            //     }
            //
            //     printf("waveform preamble:\n"
            //            "format: %i\n"
            //            "type: %i\n"
            //            "points: %i\n"
            //            "count: %i\n"
            //            "xincrement: %e\n"
            //            "xorigin: %e\n"
            //            "xreference: %e\n"
            //            "yincrement: %e\n"
            //            "yorigin: %e\n"
            //            "yreference: %i\n",
            //            wfp.format, wfp.type, wfp.points, wfp.count,
            //            wfp.xincrement[i], wfp.xorigin[i], wfp.xreference[i],
            //            wfp.yincrement[i], wfp.yorigin[i], wfp.yreference[i]);
            //
            //     printf("chanoffset[] is %e\n", params.chanoffset[i]);

            //     rec_len = wfp.xincrement[i] * wfp.points;

            ///////////////////////////////////////////////////////////
            extern int tmc_connection_type;
            if (tmc_connection_type) {
                snprintf(str, 512, ":WAV:SOUR CHAN%i\n:WAV:FORM BYTE\n:WAV:MODE NORM\n:WAV:XOR?", i + 1);
                if(tmc_write(str) != 55)
                {
                    printf("Can not write to device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }
            }
            else {

                snprintf(str, 512, ":WAV:SOUR CHAN%i", i + 1);

                if(tmc_write(str) != 15)
                {
                    printf("Can not write to device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }

                if(tmc_write(":WAV:FORM BYTE") != 14)
                {
                    printf("Can not write to device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }

                if(tmc_write(":WAV:MODE NORM") != 14)
                {
                    printf("Can not write to device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }

                if(tmc_write(":WAV:XOR?") != 9)
                {
                    printf("Can not write to device.\n");
                    line = __LINE__;
                    goto OUT_ERROR;
                }
            }

            if(tmc_read() < 1)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            params.xorigin[i] = atof(device->buf);

            if(tmc_write(":WAV:DATA?") != 10)
            {
                printf("Can not write to device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }
            n = tmc_read();

            if(n == -4 && params.modelserie == 2)
            {
                // The LAN receiver discarded one malformed but bounded NORM
                // reply and drained it to LF. Do not re-read the same response.
                // Skip this frame and allow the next normal screen poll.
                params.wavebufsz = 0;
                params.result = TMC_THRD_RESULT_SCRN;
                h_busy = 0;
                return;
            }
            if(n < 0)
            {
                printf("Can not read from device.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            if(n > WAVFRM_MAX_BUFSZ)
            {
                printf("Datablock too big for buffer.\n");
                line = __LINE__;
                goto OUT_ERROR;
            }

            if(n < 32)
            {
                n = 0;
            }

            for(j=0; j<n; j++)
            {
                params.wavebuf[i][j] = (int)(((unsigned char *)device->buf)[j]) - 127;
            }

            if((n == (params.fftbufsz * 2)) && (params.math_fft == 1) && (i == params.math_fft_src))
            {
                if(params.modelserie == 6)
                {
                    y_incr = params.chanscale[i] / 32.0;
                }
                else
                {
                    y_incr = params.chanscale[i] / 25.0;
                }

                binsz = (double)params.current_screen_sf / (params.fftbufsz * 2.0);

                for(j=0; j<n; j++)
                {
                    params.fftbuf_in[j] = params.wavebuf[i][j] * y_incr;
                }

                kiss_fftr(params.k_cfg, params.fftbuf_in, params.kiss_fftbuf);

                for(k=0; k<params.fftbufsz; k++)
                {
                    params.fftbuf_out[k] = (((params.kiss_fftbuf[k].r * params.kiss_fftbuf[k].r) +
                                             (params.kiss_fftbuf[k].i * params.kiss_fftbuf[k].i)) / params.fftbufsz);

                    params.fftbuf_out[k] /= params.current_screen_sf;

                    if(k==0)  // DC!
                    {
                        params.fftbuf_out[k] /= 2.0;
                    }

                    params.fftbuf_out[k] *= binsz;

                    if(params.math_fft_unit)  // dBm
                    {
                        if(params.fftbuf_out[k] < SPECT_LOG_MINIMUM)
                        {
                            params.fftbuf_out[k] = SPECT_LOG_MINIMUM;
                        }
                        // convert to deciBel's, not to Bel's!
                        params.fftbuf_out[k] = log10(params.fftbuf_out[k]) * 10.0;

                        if(params.fftbuf_out[k] < SPECT_LOG_MINIMUM_LOG)
                        {
                            params.fftbuf_out[k] = SPECT_LOG_MINIMUM_LOG;
                        }
                    }
                    else  // Vrms
                    {
                        params.fftbuf_out[k] = sqrt(params.fftbuf_out[k]);
                    }
                }
            }
        }

        params.wavebufsz = n;
    }
    else  // triggerstatus is "wait"
    {
        params.wavebufsz = 0;
    }

    h_busy = 0;

    return;

OUT_ERROR:

    params.result = TMC_THRD_RESULT_NONE;

    snprintf(str, 512, "An error occurred while reading screen data from device.\n"
                       "File %s line %i", __FILE__, line);

    h_busy = 0;

    params.error_line = line;

    params.error_stat = -1;

    return;
}
















