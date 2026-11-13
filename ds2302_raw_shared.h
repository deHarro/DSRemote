#ifndef DS2302_RAW_SHARED_H
#define DS2302_RAW_SHARED_H

// DS2302 STOP/RAW experiment. No changes to the historical DSRemote headers.
// All communication stays in screen_thread; SignalCurve only displays snapshots.
#include <QVector>
#include <QString>
#include <QtGlobal>

struct DS2RawTrace
{
  QVector<quint8> bytes;
  double xincrement = 0.0;
  double xorigin = 0.0;
  double xreference = 0.0;
  double yincrement = 0.0;
  double yorigin = 0.0;
  double yreference = 0.0;
  int first_point = 1;
  bool valid() const { return bytes.size() > 1 && xincrement > 0.0 && yincrement > 0.0; }
};

struct DS2RawSnapshot
{
  bool enabled = false;
  bool busy = false;
  bool stale = false;       // acquisition changed since cached STOP capture
  bool complete = false;    // cache contains a fully received STOP capture
  bool load_failed = false; // last explicit reload failed; previous cache remains
  int progress_done = 0;   // bytes received for all requested channels
  int progress_total = 0;  // expected bytes
  unsigned generation = 0;
  QString message;
  DS2RawTrace traces[4];
};

// Thread-safe functions implemented in screen_thread.cpp (no extra .pro entry).
DS2RawSnapshot ds2_raw_snapshot();
// Show/hide cached RAW. Only the first activation without a cache requests I/O.
// The UI must allow that initial request only when the device is in STOP.
void ds2_raw_toggle();
// Explicitly refresh the RAW cache. The UI and worker both check STOP.
bool ds2_raw_reload();
void ds2_raw_progress(unsigned generation, int received, int expected);
bool ds2_raw_take_request(unsigned &generation);
void ds2_raw_publish(unsigned generation, const DS2RawSnapshot &snapshot);
void ds2_raw_cancel_if_running(); // marks cached data stale; never deletes it

#endif
