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
    tmc_connection_type = 0;
    tmc_device = tmcdev_open(device);
    return tmc_device;
}

struct tmcdev *tmc_open_lan(const char *address)
{
    if(tmc_device || ocs) tmc_close();
    ds2000_spi_compat = false;
    expecting_lan_idn = false;
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
    return len;
}

void tmc_flush(void)
{
    if(tmc_connection_type == 1 && tmc_device && tmc_device->fd >= 0 && ocs)
        ocs->flush();
}

int tmc_read(void)
{
    if(!tmc_device || tmc_device->fd < 0) return -1;
    if(tmc_connection_type == 0) return tmcdev_read(tmc_device);
    if(!ocs) return -1;

    char *dst = tmc_device->hdrbuf;
    tmc_device->buf = dst;
    tmc_device->sz = 0;
    dst[0] = 0;

    if(ocs->readExact(dst, 1) != 1) return -2;
    if(dst[0] != '#') {
        const int rest = ocs->readLine(dst + 1, MAX_RESP_LEN - 1);
        if(rest <= 0) return -2;
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
    if(ocs->readExact(&digits_char, 1) != 1) return -2;
    int ndigits = digits_char - '0';
    if(ndigits < 1 || ndigits > 9) return -3; // #0 unsupported
    char length_ascii[10] = {};
    if(ocs->readExact(length_ascii, ndigits) != ndigits) return -2;
    for(int i = 0; i < ndigits; ++i) {
        if(length_ascii[i] < '0' || length_ascii[i] > '9') return -3;
    }
    const long long count = atoll(length_ascii);
    if(count < 0 || count > MAX_RESP_LEN) {
        qDebug("LAN: SCPI block length invalid: %lld", count);
        return -3;
    }
    const int size = static_cast<int>(count);
    const int header_size = 2 + ndigits;
    dst[0] = '#';
    dst[1] = digits_char;
    memcpy(dst + 2, length_ascii, ndigits);
    if(ocs->readExact(dst + header_size, size) != size) return -2;

    char end;
    if(ocs->readExact(&end, 1) != 1) return -2;
    if(end == '\r') {
        if(ocs->readExact(&end, 1) != 1) return -2;
    }
    if(end != '\n') {
        qDebug("LAN: missing newline after binary block");
        return -3;
    }
    tmc_device->buf = dst + header_size;
    tmc_device->sz = size;
    qDebug("LAN <- binary payload %d bytes", size);
    return size;
}
