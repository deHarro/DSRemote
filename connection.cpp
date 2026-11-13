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
#include <QDebug>

#define MAX_CMD_LEN  (255)
#define MAX_RESP_LEN (1024 * 1024 * 2)

int tmc_connection_type = 0;
struct tmcdev *tmc_device = nullptr;
extern osclan *ocs;

struct tmcdev *tmc_open_usb(const char *device)
{
    if(tmc_device) tmc_close();
    tmc_connection_type = 0;
    tmc_device = tmcdev_open(device);
    return tmc_device;
}

struct tmcdev *tmc_open_lan(const char *address)
{
    if(tmc_device || ocs) tmc_close();
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
}

int tmc_write(const char *cmd)
{
    if(!cmd || !tmc_device || tmc_device->fd < 0) return -1;
    const int len = static_cast<int>(strlen(cmd));
    if(len < 2 || len > MAX_CMD_LEN) return -1;
    if(tmc_connection_type == 0) return tmcdev_write(tmc_device, cmd);
    if(!ocs) return -1;

    char wire[MAX_CMD_LEN + 2];
    memcpy(wire, cmd, len);
    wire[len] = '\n';
    wire[len + 1] = 0;
    qDebug("LAN -> %s", cmd);
    if(ocs->write(wire, len + 1) != len + 1) return -1;

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
