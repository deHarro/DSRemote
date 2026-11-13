#!/usr/bin/env python3
"""Rigol DS2202: TCP-SCPI (localhost:5555) -> VXI-11 bridge for DSRemote.

Requires: python -m pip install python-vxi11

Protocol towards DSRemote: one newline-terminated SCPI command per line;
query replies are newline-terminated ASCII or #<digits><length><binary><LF>.
"""
import argparse
import datetime
import re
import socket
import sys
import traceback

try:
    import vxi11
except ImportError:
    sys.exit("'python-vxi11' fehlt: python -m pip install python-vxi11")

MAX_COMMAND = 65536
MAX_RESPONSE = 2 * 1024 * 1024  # the corresponding C++ buffer is 2 MiB

# Standard: ruhige Ausgabe. Bei --verbose alle SCPI-Transfers anzeigen.
VERBOSE = False
SEEN_ALIASES = set()


def log(*parts):
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"[{stamp}]", *parts, flush=True)


def normalize_response(raw):
    """Normalize VXI-11 EOM to a *single* framed TCP reply.

    The bytes inside a SCPI definite-length binary data block MUST be preserved,
    even when they contain \n or \x00.  Newline is appended after the block.
    """
    if not isinstance(raw, (bytes, bytearray)):
        raise TypeError("VXI-11 response must be bytes")
    raw = bytes(raw)
    if raw.startswith(b"#") and len(raw) >= 2 and ord("1") <= raw[1] <= ord("9"):
        digits = raw[1] - ord("0")
        header_end = 2 + digits
        if len(raw) < header_end or not raw[2:header_end].isdigit():
            raise ValueError(f"Ungültiger SCPI-Blockheader: {raw[:32]!r}")
        payload_length = int(raw[2:header_end])
        end = header_end + payload_length
        if len(raw) < end:
            raise ValueError(f"Binärblock unvollständig: {len(raw)} < {end}")
        if raw[end:] not in (b"", b"\n", b"\r\n"):
            raise ValueError(f"Unerwartete Bytes hinter Binärblock: {raw[end:end+32]!r}")
        framed = raw[:end] + b"\n"
    else:
        framed = raw.rstrip(b"\r\n") + b"\n"
    if len(framed) > MAX_RESPONSE:
        raise ValueError(f"Antwort ist zu groß ({len(framed)} Bytes > {MAX_RESPONSE})")
    return framed


def command_is_query(cmd):
    # '?' is not necessarily the last character (e.g. :MEAS:ITEM? VPP,CHAN1)
    return b"?" in cmd


def translate_ds2000_command(cmd):
    """Translate DSRemote's two SPI data lines to DS2000's single SDA line.

    The Rigol DS2000 programming guide documents only SDA:SOURce,
    SDA:POLarity and SDA:THReshold for the SPI data channel on BUS1/BUS2.
    Both MISO and MOSI from DSRemote are aliases to this *same* physical
    data channel. Reads reflect real instrument state, not invented values.
    Writes therefore also affect the shared SDA channel, not separate pins.
    Unrecognized commands are passed through unchanged for diagnosis.
    """
    match = re.fullmatch(
        rb"(?P<prefix>:BUS[12]:SPI:)(?P<line>MISO|MOSI):"
        rb"(?P<field>THR(?:ESHOLD)?|SOUR(?:CE)?|POL(?:ARITY)?)"
        rb"(?P<tail>\?|[ \t]+[^\r\n]+)",
        cmd, flags=re.IGNORECASE,
    )
    if not match:
        return cmd

    translated = (match.group("prefix") + b"SDA:" +
                  match.group("field") + match.group("tail"))
    # Jede Art von Übersetzung nur beim ersten Auftreten melden;
    # laufende Statusabfragen sollen das Terminal nicht fluten.
    is_query = match.group("tail").startswith(b"?")
    alias_key = (match.group("prefix").upper(), match.group("line").upper(),
                 match.group("field").upper(), is_query)
    if VERBOSE or alias_key not in SEEN_ALIASES:
        if not VERBOSE:
            SEEN_ALIASES.add(alias_key)
        log("DS2000-SPI-Alias:", repr(cmd), "=>", repr(translated))
        if not is_query:
            log("HINWEIS: MISO/MOSI-Schreibzugriffe betreffen dieselbe SDA-Leitung")
    return translated


def transfer_command(instr, client, cmd):
    if VERBOSE:
        log("->", repr(cmd.decode("latin1", errors="replace")))
    cmd = translate_ds2000_command(cmd)
    if command_is_query(cmd):
        raw = instr.ask_raw(cmd + b"\n")
        response = normalize_response(raw)
        if VERBOSE:
            if response.startswith(b"#"):
                log("<- Binärantwort:", len(response), "Bytes; header", repr(response[:24]))
            else:
                log("<-", repr(response[:160]))
        client.sendall(response)
    else:
        instr.write_raw(cmd + b"\n")
        if VERBOSE:
            log("   Write OK")
        # DSRemote sends a separate *OPC? to synchronize non-queries.
        # Do NOT send unsolicited acknowledgements here.


def serve(scope_ip, host, port, timeout):
    instr = vxi11.Instrument(scope_ip)
    instr.timeout = timeout
    try:
        # Explicit I/O, so a failure is seen *before* DSRemote connects.
        instr.open()
        ident = instr.ask("*IDN?")
        log("VXI-11 OK:", ident)
    except Exception as exc:
        log("VXI-11-Verbindung oder *IDN? fehlgeschlagen:", repr(exc))
        try:
            instr.close()
        except Exception:
            pass
        return 1

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        log(f"Bridge bereit: {host}:{port} -> {scope_ip} (VXI-11)")
        log("Leise Protokollierung aktiv; für alle SCPI-Befehle: --verbose")
        try:
            while True:
                client, address = server.accept()
                log("DSRemote verbunden:", address)
                with client:
                    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    # No idle timeout. A connected UI may sit idle for minutes.
                    buffered = bytearray()
                    try:
                        while True:
                            chunk = client.recv(16384)
                            if not chunk:
                                log("DSRemote hat die Verbindung geschlossen")
                                break
                            buffered.extend(chunk)
                            if len(buffered) > MAX_COMMAND and b"\n" not in buffered:
                                raise ValueError("Eingabezeile zu lang")
                            while True:
                                eol = buffered.find(b"\n")
                                if eol == -1:
                                    break
                                cmd = bytes(buffered[:eol]).strip(b" \t\r")
                                del buffered[:eol + 1]
                                if not cmd:
                                    continue
                                if len(cmd) > MAX_COMMAND:
                                    raise ValueError("SCPI-Befehl zu lang")
                                transfer_command(instr, client, cmd)
                    except Exception as exc:
                        log("Fehler im DSRemote-Dialog:", repr(exc))
                        traceback.print_exc()
                        # Never invent '0' as a SCPI response: it would desync DSRemote.
                        # Drop/reopen VXI-11 link in case a timed-out transaction left it stale.
                        try:
                            instr.close()
                        except Exception:
                            pass
                        try:
                            instr.open()
                            log("VXI-11-Link wieder geöffnet")
                        except Exception as reopen_err:
                            log("VXI-11-Reconnect fehlgeschlagen:", repr(reopen_err))
                    log("Warte auf neue DSRemote-Verbindung")
        except KeyboardInterrupt:
            log("Bridge beendet")
        finally:
            try:
                instr.close()
            except Exception:
                pass
    return 0


def main():
    global VERBOSE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope-ip", default="192.168.1.206")
    parser.add_argument("--host", default="127.0.0.1", help="nur lokal zugänglich")
    parser.add_argument("--port", type=int, default=5555)
    parser.add_argument("--timeout", type=float, default=10.0, help="VXI-11 timeout in s")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="alle SCPI-Abfragen, Antworten und Schreibbefehle protokollieren")
    args = parser.parse_args()
    VERBOSE = args.verbose
    return serve(args.scope_ip, args.host, args.port, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
