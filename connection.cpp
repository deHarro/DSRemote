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


/*
 * DSRemote connection.cpp with bridge-aware LAN handling.
 * USB path and public function signatures kept intact.
 */
#include "connection.h"
#include "osc_lan.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <string>
#include <QDebug>
#include <QElapsedTimer>

#define MAX_CMD_LEN  (255)
#define MAX_RESP_LEN (1024 * 1024 * 2)

int tmc_connection_type = 0;
struct tmcdev *tmc_device = nullptr;
extern osclan *ocs;


// The DS2000 family has a single SPI data line (SDA), unlike some other Rigol
// series.  Enable this compatibility alias only after the scope's *IDN? reply.
// Reset the detection for every connection so other models stay untouched.
static bool ds2000_spi_compat = false;
static bool expecting_lan_idn = false;
// DS2302_NORM_RECOVERY_V1_MERGED: Only discard malformed NORM waveform replies.
// Keep RAW and unrelated binary replies on the original strict error path.
static bool ds2_lan_norm_mode = false;
static bool ds2_lan_norm_data_pending = false;

static bool is_ds2000_idn(const char *response)
{
    const char vendor[] = "RIGOL TECHNOLOGIES,";
    if(strncmp(response, vendor, sizeof(vendor) - 1) != 0) return false;
    const char *model = response + sizeof(vendor) - 1;
    if(strlen(model) < 6) return false;
    // DS2xxx, DS2xxxA and DS2xxxA-S are handled as DS2000 by DSRemote.
    return model[0] == 'D' && model[1] == 'S' && model[2] == '2' &&
           std::isdigit(static_cast<unsigned char>(model[3])) &&
           std::isdigit(static_cast<unsigned char>(model[4])) &&
           std::isdigit(static_cast<unsigned char>(model[5]));
}

static std::string ds2000_spi_alias(const char *command)
{
    std::string result(command);
    if(!ds2000_spi_compat) return result;

    // Match the Python bridge's DS2000 alias, but only for SPI data-line
    // THR / SOURCE / POLARITY commands on BUS1 or BUS2.
    std::string upper(result);
    for(char &ch : upper)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    if(upper.compare(0, 10, ":BUS1:SPI:") != 0 &&
        upper.compare(0, 10, ":BUS2:SPI:") != 0)
        return result;

    const size_t line_pos = 10;
    if(upper.compare(line_pos, 5, "MISO:") != 0 &&
        upper.compare(line_pos, 5, "MOSI:") != 0)
        return result;

    const size_t field_pos = line_pos + 5;
    const size_t field_end = upper.find_first_of("? \t", field_pos);
    if(field_end == std::string::npos) return result;
    const std::string field = upper.substr(field_pos, field_end - field_pos);
    if(field != "THR" && field != "THRESHOLD" &&
        field != "SOUR" && field != "SOURCE" &&
        field != "POL" && field != "POLARITY")
        return result;

    // As in the Python bridge, only query ('?') or write (' ' / '\t') forms.
    if(upper[field_end] == '?' && field_end + 1 != upper.size()) return result;
    if(upper[field_end] != '?' && upper.find_first_not_of(" \t", field_end) == std::string::npos)
        return result;

    result.replace(line_pos, 5, "SDA:");
    return result;
}

struct tmcdev *tmc_open_usb(const char *device)
{
    if(tmc_device) tmc_close();
    ds2000_spi_compat = false;
    expecting_lan_idn = false;
    ds2_lan_norm_mode = false;
    ds2_lan_norm_data_pending = false;
    tmc_connection_type = 0;
    tmc_device = tmcdev_open(device);
    return tmc_device;
}

struct tmcdev *tmc_open_lan(const char *address)
{
    if(tmc_device || ocs) tmc_close();
    ds2000_spi_compat = false;
    expecting_lan_idn = false;
    ds2_lan_norm_mode = false;
    ds2_lan_norm_data_pending = false;
    tmc_connection_type = 1;
    ocs = new osclan();

    tmc_device = (struct tmcdev *)calloc(1, sizeof(struct tmcdev));
    if(!tmc_device) { delete ocs; ocs = nullptr; return nullptr; }
    tmc_device->hdrbuf = (char *)calloc(1, MAX_RESP_LEN + 1024);
    if(!tmc_device->hdrbuf) {
        free(tmc_device);
        tmc_device = nullptr;
        delete ocs;
        ocs = nullptr;
        return nullptr;
    }
    tmc_device->buf = tmc_device->hdrbuf;
    tmc_device->fd = ocs->open(address); // 0 = success, -1 = failure
    if(tmc_device->fd < 0) {
        tmc_close();
        return nullptr;
    }
    return tmc_device;
}

void tmc_close(void)
{
    if(tmc_connection_type == 0) {
        if(tmc_device) tmcdev_close(tmc_device);
    } else {
        if(ocs) { ocs->close(); delete ocs; ocs = nullptr; }
        if(tmc_device) {
            free(tmc_device->hdrbuf);
            free(tmc_device);
        }
    }
    tmc_device = nullptr;
    ds2000_spi_compat = false;
    expecting_lan_idn = false;
    ds2_lan_norm_mode = false;
    ds2_lan_norm_data_pending = false;
}

int tmc_write(const char *cmd)
{
    if(!cmd || !tmc_device || tmc_device->fd < 0) return -1;
    const int len = static_cast<int>(strlen(cmd));
    if(len < 2 || len > MAX_CMD_LEN) return -1;
    if(tmc_connection_type == 0) return tmcdev_write(tmc_device, cmd);
    if(!ocs) return -1;

    if(strcmp(cmd, "*IDN?") == 0)
        expecting_lan_idn = true;

    const std::string translated = ds2000_spi_alias(cmd);
    const int wire_len = static_cast<int>(translated.size());
    if(wire_len > MAX_CMD_LEN) return -1;
    char wire[MAX_CMD_LEN + 2];
    memcpy(wire, translated.data(), wire_len);
    wire[wire_len] = '\n';
    wire[wire_len + 1] = 0;
    if(translated != cmd)
        qDebug("DS2000 SPI alias: %s => %s", cmd, translated.c_str());
    qDebug("LAN -> %s", translated.c_str());
    if(ocs->write(wire, wire_len + 1) != wire_len + 1) return -1;

    // A query can contain arguments after the '?' (e.g. :MEAS:ITEM? VPP,CHAN1).
    const bool query = strchr(cmd, '?') != nullptr;
    const bool reset = strncmp(cmd, "*RST", 4) == 0;
    if(!query && !reset) {
        // One *OPC? is enough: this SCPI query itself waits for completion.
        if(ocs->write("*OPC?\n", 6) != 6) return -1;
        char reply[128];
        int n = ocs->readLine(reply, sizeof(reply) - 1);
        if(n < 0) { qDebug("*OPC? timed out or bridge disconnected"); return -1; }
        reply[n] = 0;
        while(n > 0 && (reply[n-1] == '\n' || reply[n-1] == '\r')) reply[--n] = 0;
        if(strcmp(reply, "1") != 0) {
            qDebug("Unexpected *OPC? reply: %s", reply);
            return -1;
        }
    }
    // Track only successfully sent SCPI commands. The NORM data response
    // may be recovered, but never a RAW response or another binary reply.
    if(std::strstr(cmd, ":WAV:MODE RAW")) ds2_lan_norm_mode = false;
    if(std::strstr(cmd, ":WAV:MODE NORM")) ds2_lan_norm_mode = true;
    ds2_lan_norm_data_pending = ds2_lan_norm_mode &&
                                (std::strcmp(cmd, ":WAV:DATA?") == 0);
    return len;
}

void tmc_flush(void)
{
    if(tmc_connection_type == 1 && tmc_device && tmc_device->fd >= 0 && ocs)
        ocs->flush();
}

int tmc_read(void)
{
    if(!tmc_device || tmc_device->fd < 0) {
        qWarning("DS2302 LAN-DIAG: tmc_read: no valid device");
        return -1;
    }
    if(tmc_connection_type == 0) return tmcdev_read(tmc_device);
    if(!ocs) {
        qWarning("DS2302 LAN-DIAG: tmc_read: LAN client unavailable");
        return -1;
    }

    const bool ds2_norm_data = ds2_lan_norm_data_pending;
    ds2_lan_norm_data_pending = false;

    char *dst = tmc_device->hdrbuf;
    tmc_device->buf = dst;
    tmc_device->sz = 0;
    dst[0] = 0;

    // DS2302 LAN diagnostics: report WHICH SCPI receive stage fails.
    // This only logs failures; read behavior and timeouts remain unchanged.
    QElapsedTimer receive_timer;
    receive_timer.start();
    const auto read_checked = [&](char *where, int bytes, const char *stage) -> int {
        const int got = ocs->readExact(where, bytes);
        if(got != bytes) {
            qWarning("DS2302 LAN-DIAG: tmc_read failed at %s; requested=%d, return=%d, elapsed=%lld ms",
                     stage, bytes, got, static_cast<long long>(receive_timer.elapsed()));
        }
        return got;
    };

    if(read_checked(dst, 1, "first byte") != 1) return -2;
    if(dst[0] != '#') {
        const int rest = ocs->readLine(dst + 1, MAX_RESP_LEN - 1);
        if(rest <= 0) {
            qWarning("DS2302 LAN-DIAG: tmc_read failed at ASCII line; return=%d, elapsed=%lld ms",
                     rest, static_cast<long long>(receive_timer.elapsed()));
            return -2;
        }
        int size = rest + 1;
        while(size > 0 && (dst[size-1] == '\n' || dst[size-1] == '\r')) --size;
        dst[size] = 0;
        tmc_device->sz = size;
        if(expecting_lan_idn) {
            ds2000_spi_compat = is_ds2000_idn(dst);
            expecting_lan_idn = false;
            qDebug("LAN model: DS2000 SPI compatibility %s",
                   ds2000_spi_compat ? "enabled" : "disabled");
        }
        qDebug("LAN <- ASCII (%d bytes): %.120s", size, dst);
        return size;
    }

    // SCPI definite-length block: #N<exactly N decimal length digits><payload>LF
    char digits_char;
    if(read_checked(&digits_char, 1, "binary header digit count") != 1) return -2;
    int ndigits = digits_char - '0';
    if(ndigits < 1 || ndigits > 9) {
        qWarning("DS2302 LAN-DIAG: invalid binary header: digit count=%d", ndigits);
        return -3; // #0 unsupported
    }
    char length_ascii[10] = {};
    if(read_checked(length_ascii, ndigits, "binary header length") != ndigits) return -2;
    for(int i = 0; i < ndigits; ++i) {
        if(length_ascii[i] < '0' || length_ascii[i] > '9') {
            qWarning("DS2302 LAN-DIAG: invalid binary block length digit at %d", i);
            return -3;
        }
    }
    const long long count = atoll(length_ascii);
    if(count < 0 || count > MAX_RESP_LEN) {
        qWarning("DS2302 LAN-DIAG: SCPI block length invalid: %lld", count);
        return -3;
    }
    const int size = static_cast<int>(count);
    const int header_size = 2 + ndigits;
    dst[0] = '#';
    dst[1] = digits_char;
    memcpy(dst + 2, length_ascii, ndigits);
    if(read_checked(dst + header_size, size, "binary payload") != size) return -2;

    char end;
    if(read_checked(&end, 1, "binary block terminator") != 1) return -2;
    if(end == '\r') {
        if(read_checked(&end, 1, "binary block terminator") != 1) return -2;
    }


    if(end != '\n') {
        // The live Rigol NORM reply can contain a small surplus beyond its
        // declared SCPI length. Only for a recognized DS2000 NORM :WAV:DATA?
        // try to drain a bounded surplus to the response delimiter.
        if(ds2_norm_data && ds2000_spi_compat && size > 0 && size <= 1400) {
            constexpr int max_surplus = 64;
            int surplus = 1; // 'end' was already consumed beyond the declared size.
            char tail = end;
            while(tail != '\n' && surplus < max_surplus) {
                if(read_checked(&tail, 1, "NORM trailing surplus") != 1) {
                    qWarning("DS2302 LAN-RECOVERY: incomplete NORM surplus after %d bytes; cannot resynchronize", surplus);
                    return -3;
                }
                ++surplus;
            }
            if(tail == '\n') {
                // No other SCPI query is pending in the normal sequential path.
                // Clear any buffered trailing delimiter before the next poll.
                ocs->flush();
                qWarning("DS2302 LAN-RECOVERY: NORM declared=%d, surplus=%d byte(s) incl LF; discarded frame, stream drained, %lld ms",
                         size, surplus, static_cast<long long>(receive_timer.elapsed()));
                tmc_device->sz = 0;
                return -4; // Caller skips one frame, then retries on next poll.
            }
            qWarning("DS2302 LAN-RECOVERY: no delimiter within %d surplus bytes; connection unsafe", max_surplus);
            return -3;
        }
        // Keep diagnostics for errors that cannot safely be recovered.
        qWarning("DS2302 LAN-DIAG: header=#%c%s, payload=%d, next=0x%02X, elapsed=%lld ms",
                 digits_char, length_ascii, size,
                 static_cast<unsigned int>(static_cast<unsigned char>(end)),
                 static_cast<long long>(receive_timer.elapsed()));
        if(size >= 8) {
            const unsigned char *tail =
                reinterpret_cast<const unsigned char *>(dst + header_size + size - 8);
            qWarning("DS2302 LAN-DIAG: payload tail = %02X %02X %02X %02X %02X %02X %02X %02X",
                     tail[0], tail[1], tail[2], tail[3],
                     tail[4], tail[5], tail[6], tail[7]);
        }
        return -3;
    }
    tmc_device->buf = dst + header_size;
    tmc_device->sz = size;
    qDebug("LAN <- binary payload %d bytes", size);
    return size;
}
