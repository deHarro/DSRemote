# DSRemote – Windows / Rigol DS2302: Entwicklungsstand 24.11.2026

Dieser Stand dokumentiert die bisher erreichten Funktionen, den zuletzt am realen Gerät getesteten GUI-Zustand und die bekannten offenen Probleme. Er ist als Sicherungspunkt des Forks gedacht, **nicht** als Behauptung, dass sämtliche Funktionen des ursprünglichen DSRemote bereits vollständig mit dem DS2000 kompatibel sind.

## Ausgangslage und Umgebung

- Ursprungsprojekt: DSRemote (Qt/C++) für Rigol-Oszilloskope; erweitert für Windows und die DS2000-Serie.
- Fork: `deHarro/DSRemote` (`origin`), Upstream: `pvvx/DSRemote` (`upstream`).
- Testgerät: Rigol DS2202 mit Feature-Freischaltung als **DS2302 (300 MHz)**, Firmware `00.03.06`.
- Entwicklung: Windows 11, Qt **5.15.2**, MinGW **8.1 / 64 Bit**.
- Verbindung: LAN / VXI-11 direkt aus DSRemote. Die ehemals verwendete separate Python-Bridge ist **nicht mehr erforderlich**.
- Älterer gesicherter Stand: Commit `09d65ba`, Tag `ds2302-cursor-control-v1` vom 18.11.2026.

## Erreichte Funktionen

### Windows und Gerätesteuerung

- Windows-/MinGW-Portierung einschließlich erforderlicher Winsock-Anbindung.
- Direkte Rigol-LAN-Kommunikation in C++/Qt (`osc_lan`/SCPI); Identifikation über `*IDN?` und viele Geräteeinstellungen funktionieren.
- DS2000-spezifische Abfragen und Einstellungen angepasst, einschließlich Unterschieden zur bisherigen DSRemote-Gerätefamilie; einzelne Funktionen bleiben geräte-/firmwareabhängig.
- Bedienung unter Windows auch als portable Release-Zusammenstellung auf einem weiteren PC getestet – ohne separate Python-Bridge und ohne lokale Qt-Installation.

### Live-Anzeige und Datenübertragung

- Live-Darstellung der NORM-Wellenformdaten des DS2302 über LAN; teilweise aufgebaute Sweeps werden sichtbar aktualisiert.
- **NORM-Recovery V1** gegen gelegentlich fehlerhafte bzw. ungewöhnlich terminierte SCPI-Binärblöcke: Zusatzbytes werden bis zum Blockende aufgefangen, fehlerhafte Einzelblöcke können verworfen werden, ohne die ganze Verbindung sofort zu verlieren.
- RUN, STOP und SINGLE einschließlich der Anzeige während des laufenden SINGLE-Sweeps; bisherige funktionierende CLEAN-Variante bewahrt.
- Frühere experimentelle Polling-/Sweep-Diagnosen sind **nicht** Bestandteil der beabsichtigten produktiven Ablaufsteuerung.

### Darstellung, Messung, Navigation

- Hochauflösende RAW-Erfassungsdaten: Auslese- und Anzeigeweg sowie Zoom-/Navigationsfunktionen implementiert; große Transfers benötigen noch Robustheitsarbeit.
- Lokaler Display-Zoom unabhängig von der physikalischen Rigol-Zeitbasis.
- Zeit- und Spannungscursor mit Messwertanzeige in Normal-/Zoomansichten, soweit Daten und Gerätezustand die jeweilige Funktion unterstützen.
- Cursor-/Triggerbedienung und -anzeige für DS2000 weiterentwickelt.
- **Connect/Disconnect-Menü-Graying**: korrekte Aktivierung und Deaktivierung bei Verbindung, Trennung und erneutem Verbindungsaufbau; am 24.11.2026 bestätigt.
- **Obere horizontale Zeitfensteranzeige V3c**: maßstabsgerechte schematische Wellenlinie, seitliche blaue Balken für nicht sichtbare Bereiche, H-Positionsverschiebung, kleiner orangefarbener Triggermarker; Grenzfälle bei kleinerer und größerer Zeitbasis sowie korrekte Zeichenreihenfolge (Wellenlinie vor Balken). Mit dem Original am Rigol verglichen und in mehreren Schritten korrigiert.

## Seit dem Stand `ds2302-cursor-control-v1` besonders relevant

- Stabilisierung der NORM-Binärblockbehandlung durch Recovery und Beibehaltung des funktionierenden RUN-/SINGLE-Verhaltens.
- Abgrenzung der langsamen Sweep-Aktualisierung mittels unabhängiger Python-NORM/MAX-Messungen (nicht nur DSRemote-GUI).
- Bereinigung bzw. Rücknahme experimenteller Sweep-Polling-Varianten zugunsten der CLEAN-Version.
- Korrektes Connect-/Disconnect-Graying.
- Obere Zeitfenster-/Triggerpositionsanzeige bis V3c.

## Bekannte Einschränkungen / Aufgaben

1. **Langsame Zeitbasen:** Unter anderem bei 200 ms/div erscheint gelegentlich nur jeder zweite vollständige Sweep in der externen Datenanzeige. Im unabhängigen NORM/MAX-Test traten Hinweise auf Aktualisierungsabstände von ca. 2,8 bzw. 5,6 s auf. Ursache im Bereitstellen/Auslesen der SCPI-Wellenformdaten nicht abschließend geklärt. Keine Polling-Experimente als endgültige Lösung übernehmen.
2. **Große RAW-Datensätze:** Insbesondere Auslesen und Fehlersituationen bei hohen Speichertiefen weiter stabilisieren und testen.
3. **Verbindungs-/Gerätestart:** Direkt nach dem Einschalten kann der Rigol SCPI-Abfragen noch nicht zuverlässig beantworten; nach längerer Initialisierung funktionierte die Verbindung wieder. Noch kein gesicherter DSRemote-Softwarefehler.
4. **Aufräumen:** veraltete Konsolentexte (z. B. `TCP bridge connected ...:5555`), Disconnect-/`closeEvent()`-Vereinheitlichung, gezielte Fehlerdiagnose, kleinere Compilerwarnungen.
5. **Spätere Erweiterungen:** zusätzliche RAW-Funktionen, Recorder-Funktionen und weiterführende DS2000-Kompatibilität gesondert prüfen.

## Test- und Versionshinweise

- **Zielstand:** funktionierender lokaler Quellcode mit NORM-Recovery/CLEAN RUN-SINGLE, Connect/Disconnect-Graying und **Zeitfensteranzeige V3c**.
- Zuletzt wurden die Funktion des Menü-Graying und die Zeitfensteranzeige durch Bedienversuche am echten DS2302 bestätigt.
- Diese Datei beschreibt den Entwicklungsstand, ersetzt aber keinen Build- oder Regressionstest. Ein vollständiger automatisierter Testlauf oder ein erneuter externer Windows-Build ist damit nicht behauptet.
- Experimentelle Python-Probes, Logs, Debug-ZIPs und Build-Verzeichnisse gehören nicht automatisch in den produktiven Commit.

## Vorgeschlagener Commit / Tag

**Commit-Titel**

`DS2302: stabilize LAN waveform handling and refine Windows GUI`

**Tag**

`ds2302-lan-gui-2026-11-24`
