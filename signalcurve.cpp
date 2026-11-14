/*
***************************************************************************
*
* Author: Teunis van Beelen
*
* Copyright (C) 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019 Teunis van Beelen
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



#include "signalcurve.h"
#include "ds2302_raw_shared.h"
#include <QShortcut>
#include <QKeySequence>
#include <QToolTip>
#include <QCursor>
#include <QPointF>
#include <QToolButton>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QStringList>

#include "time.h"

#include <QWheelEvent>
#include <QEvent>
#include <QVariant>
#include <QDockWidget>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QByteArray>
#include <QSizePolicy>
#include <QTimer>
#include <cmath>
#include <algorithm>
#include <functional>
#include <vector>

// DS2000 display-only horizontal zoom, independent of the instrument timebase.
// Event filter avoids changing signalcurve.h or the existing trigger/marker handlers.
namespace {
// Display-only time scale: readable SI units, without changing instrument settings.
static QString ds2_format_time_per_div(double seconds)
{
  double scaled = seconds;
  const char *unit = "s/div";
  if(seconds >= 1e-3 && seconds < 1.0)
  {
    scaled = seconds * 1e3;
    unit = "ms/div";
  }
  else if(seconds >= 1e-6 && seconds < 1e-3)
  {
    scaled = seconds * 1e6;
    unit = "\xC2\xB5s/div"; // UTF-8: micro sign
  }
  else if(seconds >= 1e-9 && seconds < 1e-6)
  {
    scaled = seconds * 1e9;
    unit = "ns/div";
  }
  else if(seconds > 0.0 && seconds < 1e-9)
  {
    scaled = seconds * 1e12;
    unit = "ps/div";
  }
  return QString::number(scaled, 'g', 4) + " " + QString::fromUtf8(unit);
}

// Return the next finer/coarser 1-2-5 time/div value (seconds).
// The instrument's timebase is never changed, only the local display zoom.
static double ds2_adjacent_time_per_div(double current, bool finer)
{
  if(!(current > 0.0) || !std::isfinite(current)) return current;
  const double decade = std::floor(std::log10(current));
  const double factors[] = {1.0, 2.0, 5.0};
  double best = finer ? 0.0 : HUGE_VAL;
  const double tolerance = current * 1e-9;

  for(int i = -2; i <= 2; ++i)
  {
    const double power = std::pow(10.0, decade + i);
    for(int j = 0; j < 3; ++j)
    {
      const double candidate = factors[j] * power;
      if(finer && candidate < current - tolerance && candidate > best)
        best = candidate;
      if(!finer && candidate > current + tolerance && candidate < best)
        best = candidate;
    }
  }
  return (std::isfinite(best) && best > 0.0) ? best : current;
}

// Display-only cursor measurements; seconds are relative to the trigger.
static QString ds2_format_seconds(double seconds)
{
  const double abs_seconds = std::fabs(seconds);
  double scaled = seconds;
  const char *unit = "s";
  if(abs_seconds >= 1e-3 && abs_seconds < 1.0)
  {
    scaled = seconds * 1e3;
    unit = "ms";
  }
  else if(abs_seconds >= 1e-6 && abs_seconds < 1e-3)
  {
    scaled = seconds * 1e6;
    unit = "\xC2\xB5s";
  }
  else if(abs_seconds >= 1e-9 && abs_seconds < 1e-6)
  {
    scaled = seconds * 1e9;
    unit = "ns";
  }
  else if(abs_seconds > 0.0 && abs_seconds < 1e-9)
  {
    scaled = seconds * 1e12;
    unit = "ps";
  }
  return QString::number(scaled, 'g', 5) + " " + QString::fromUtf8(unit);
}

static QString ds2_format_frequency(double seconds)
{
  if(!(seconds > 0.0) || !std::isfinite(seconds))
    return QString::fromUtf8("--");
  const double hz = 1.0 / seconds;
  double scaled = hz;
  const char *unit = "Hz";
  if(hz >= 1e9)      { scaled = hz / 1e9; unit = "GHz"; }
  else if(hz >= 1e6) { scaled = hz / 1e6; unit = "MHz"; }
  else if(hz >= 1e3) { scaled = hz / 1e3; unit = "kHz"; }
  return QString::number(scaled, 'g', 5) + " " + unit;
}

// Voltage cursor labels with convenient SI prefixes.
static QString ds2_format_volts(double voltage)
{
  if(!std::isfinite(voltage)) return QString("--");
  const double av = std::fabs(voltage);
  double scaled = voltage;
  const char *unit = "V";
  if(av >= 1000.0)
  {
    scaled = voltage / 1000.0;
    unit = "kV";
  }
  else if(av > 0.0 && av < 1e-3)
  {
    if(av >= 1e-6) { scaled = voltage * 1e6; unit = "\xC2\xB5V"; }
    else            { scaled = voltage * 1e9; unit = "nV"; }
  }
  else if(av >= 1e-3 && av < 1.0)
  {
    scaled = voltage * 1e3;
    unit = "mV";
  }
  return QString::number(scaled, 'g', 5) + " " + QString::fromUtf8(unit);
}

// Small real Qt controls in the existing LOWER black instrument status strip.
// These do not occupy the crowded top Rigol status row.
class DS2RawControls : public QWidget
{
public:
  QToolButton *toggleButton;
  QToolButton *reloadButton;
  QLabel *statusLabel;
  QProgressBar *progressBar;

  // GUI-only timing estimate. Do NOT change the real RAW block reader.
  QElapsedTimer loadTimer;
  bool loadWasBusy = false;
  unsigned activeGeneration = 0;
  int activeExpectedBytes = 0;
  int learnedBytes = 0;
  qint64 learnedMs = 0;
  int lastEstimatedPercent = 0;

  explicit DS2RawControls(QWidget *parent) : QWidget(parent)
  {
    setObjectName("DS2302RawControls");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(
      "QWidget#DS2302RawControls { background: #202020; }"
      "QToolButton { color: #dfdfdf; background: #333333; border: 1px solid #777; "
      "border-radius: 3px; padding: 1px 3px; font-size: 11px; }"
      "QToolButton:checked { color: #69eea7; border: 1px solid #69eea7; }"
      "QToolButton:disabled { color: #666; background: #252525; border-color: #454545; }"
      "QProgressBar { background: #202020; color: white; border: 1px solid #777; "
      "border-radius: 3px; text-align: center; font-size: 11px; }"
      "QProgressBar::chunk { background: #e3bc57; }"
    );
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(2, 1, 2, 1);
    layout->setSpacing(5);
    toggleButton = new QToolButton(this);
    toggleButton->setText("RAW");
    toggleButton->setCheckable(true);
    toggleButton->setFixedWidth(43);
    toggleButton->setToolTip("Zwischen NORM und gespeicherter RAW-Aufnahme wechseln");
    reloadButton = new QToolButton(this);
    reloadButton->setText("RAW laden");
    reloadButton->setFixedWidth(78);
    reloadButton->setToolTip("Erfassungsspeicher neu laden (nur im STOP-Zustand)");
    statusLabel = new QLabel("NORM", this);
    statusLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    statusLabel->setStyleSheet("color: #bbb; background: transparent; font-size: 11px;");
    statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    progressBar = new QProgressBar(this);
    progressBar->setFixedHeight(19);
    progressBar->setTextVisible(true);
    progressBar->setRange(0, 100);
    progressBar->setFormat("RAW ~%p%"); // elapsed-time estimate, not bytes
    progressBar->setToolTip("Geschaetzter Ladefortschritt aus gemessenen Zeiten. "
                            "100 % erst nach erfolgreichem Empfang.");
    progressBar->hide();
    layout->addWidget(toggleButton);
    layout->addWidget(reloadButton);
    layout->addWidget(statusLabel, 1);
    layout->addWidget(progressBar, 1);
    hide();
  }

  void refresh(bool device_is_ds2000, bool connected, bool stopped,
               unsigned live_channel_mask)
  {
    if(!device_is_ds2000 || !connected || width() < 250) { hide(); return; }
    if(!isVisible()) show();
    const DS2RawSnapshot s = ds2_raw_snapshot();
    QStringList stored_channels;
    QStringList live_channels;
    int points = 0;
    double interval = 0.0;
    for(int ch = 0; ch < 4; ++ch)
    {
      if(live_channel_mask & (1u << ch))
        live_channels << QString("CH%1").arg(ch + 1);
      if(s.traces[ch].valid())
      {
        stored_channels << QString("CH%1").arg(ch + 1);
        points = s.traces[ch].bytes.size();
        interval = s.traces[ch].xincrement;
      }
    }
    const bool has_cache = !stored_channels.isEmpty();
    const QString stored = stored_channels.join("+");
    const QString live = live_channels.isEmpty() ? "--" : live_channels.join("+");

    // Toggling never starts I/O. A missing cache must be loaded explicitly.
    // If a first reload failed, allow toggling the error view back to LIVE.
    toggleButton->setEnabled(!s.busy && (has_cache || s.enabled));
    toggleButton->setChecked(s.enabled);
    reloadButton->setEnabled(stopped && !s.busy);
    reloadButton->setToolTip(stopped ? "RAW-Snapshot neu vom Oszilloskop laden"
                                      : "RAW laden nur bei STOP moeglich");

    if(s.busy)
    {
      if(!loadWasBusy || activeGeneration != s.generation)
      {
        loadTimer.start();
        loadWasBusy = true;
        activeGeneration = s.generation;
        activeExpectedBytes = 0;
        lastEstimatedPercent = 2; // visible movement from the first refresh
        progressBar->setValue(lastEstimatedPercent);
      }
      if(s.progress_total > 0)
        activeExpectedBytes = s.progress_total;

      // Default for the FIRST load: 6 seconds per 280k points.
      // Subsequent successful loads teach us an observed duration/byte.
      // This is only a visual ETA; actual completion is always authoritative.
      const qint64 sizeForEstimate = activeExpectedBytes > 0
          ? activeExpectedBytes : 280000;
      qint64 expectedMs = (learnedMs > 0 && learnedBytes > 0)
          ? (learnedMs * sizeForEstimate / learnedBytes)
          : (6000LL * sizeForEstimate / 280000);
      expectedMs = std::max<qint64>(1500, std::min<qint64>(120000, expectedMs));
      const qint64 elapsed = loadTimer.elapsed();
      // Max 95 % until the worker has published the complete snapshot.
      const int estimatedPercent = std::min(95, int(95LL * elapsed / expectedMs));
      lastEstimatedPercent = std::max(lastEstimatedPercent,
                                      std::max(2, estimatedPercent));
      statusLabel->hide();
      progressBar->show();
      progressBar->setValue(lastEstimatedPercent);
      return;
    }

    if(loadWasBusy)
    {
      const qint64 measuredMs = loadTimer.elapsed();
      if(activeGeneration == s.generation && !s.load_failed && s.complete &&
         activeExpectedBytes > 0 && measuredMs > 0)
      {
        // Exponentially smoothed estimate, corrected for transfer size.
        // Keep learned cost per point independent of CH1/CH2 count.
        if(learnedMs > 0 && learnedBytes > 0)
        {
          qint64 previousScaled = learnedMs * activeExpectedBytes / learnedBytes;
          learnedMs = (previousScaled * 2 + measuredMs) / 3;
        }
        else learnedMs = measuredMs;
        learnedBytes = activeExpectedBytes;
        progressBar->setValue(100);
      }
      loadWasBusy = false;
      lastEstimatedPercent = 0;
    }
    progressBar->hide();
    statusLabel->show();
    QString message;
    QString tooltip;
    QString color = "#babfc5";
    if(!s.enabled)
    {
      message = QString("LIVE %1 / NORM").arg(live);
      tooltip = has_cache ? QString("LIVE-Daten. RAW-Snapshot gespeichert: %1. ").arg(stored)
                          : QString("LIVE-Daten; kein RAW-Snapshot gespeichert. ");
      tooltip += "Mit RAW nur gespeicherte Daten anzeigen. 'RAW laden' liest bei STOP neu.";
    }
    else if(!has_cache)
    {
      message = s.load_failed ? "SNAPSHOT: LADEFEHLER" : "SNAPSHOT: KEINE DATEN";
      color = "#ff8178";
      tooltip = s.message;
    }
    else
    {
      message = QString("SNAPSHOT %1 / %2 P/K / %3")
            .arg(stored).arg(points).arg(ds2_format_seconds(interval));
      color = "#75f3a4";
      if(s.stale)
      {
        message += " (ALT)";
        color = "#f2c66c";
      }
      if(s.load_failed)
      {
        message += " !";
        color = "#ff8178";
      }
      tooltip = QString("Gespeicherte RAW-Aufnahme: %1, %2 Abtastwerte/Kanal, %3 pro Punkt. ")
            .arg(stored).arg(points).arg(ds2_format_seconds(interval));
      tooltip += "Keine Live-Aktualisierung. Neu laden nur mit 'RAW laden' bei STOP.";
      if(s.stale) tooltip += " Die Aufnahme ist ALT (Oszi war seitdem nicht in STOP).";
      if(s.load_failed) tooltip += QString(" Letztes Nachladen fehlgeschlagen: %1").arg(s.message);
    }
    if(statusLabel->text() != message) statusLabel->setText(message);
    statusLabel->setToolTip(tooltip);
    statusLabel->setStyleSheet(QString("color: %1; background: transparent; font-size: 11px;").arg(color));
    toggleButton->setToolTip(s.enabled
        ? QString("SNAPSHOT sichtbar (%1). Klick: NORM/LIVE").arg(stored)
        : (has_cache ? QString("RAW-Snapshot %1 anzeigen (ohne Download)").arg(stored)
                     : "Noch kein RAW-Snapshot. Erst 'RAW laden' im STOP-Modus."));
  }

};

// Visual measurement aid. The measurements remain in native seconds/volts;
// plot coordinates are used only for hit testing and the on-screen marker.
struct DS2CursorTarget
{
  bool valid = false;
  double x = 0.0;
  double y = 0.0;
  double time = 0.0;
  double voltage = 0.0;
};

using DS2CursorTargetQuery =
    std::function<DS2CursorTarget(int, double, double, int, int)>;

// Return the nearest measured crossing of a voltage level in the visible
// time range. Segments on both sides of the cursor are searched separately:
// the first valid crossing in each direction is the nearest on that side.
// For a long RAW trace this normally examines only a small local region.
// The search is independent of trigger EDGE polarity and time zero.
template<typename VoltageAt>
static bool ds2_find_nearest_level_crossing(int count, double firstTime,
    double dt, double viewLeft, double viewRight, double cursorTime,
    double level, const VoltageAt &voltageAt, double &resultTime)
{
  if(count < 2 || !(dt > 0.0) || !std::isfinite(dt) ||
     !std::isfinite(firstTime) || !std::isfinite(level) ||
     !(viewRight > viewLeft)) return false;

  const double lastTime = firstTime + (count - 1) * dt;
  if(lastTime < viewLeft || firstTime > viewRight) return false;

  // Clamp in floating point BEFORE converting to int. This also avoids
  // overflow if the scope offset is far outside the acquired record.
  auto sampleIndex = [&](double t) -> int {
    const double position = (t - firstTime) / dt;
    if(!(position > 0.0)) return 0;
    if(position >= count - 1) return count - 1;
    return int(std::floor(position));
  };
  const int begin = std::max(0, sampleIndex(viewLeft) - 1);
  const int finish = std::min(count - 2, sampleIndex(viewRight) + 1);
  if(begin > finish) return false;
  const int pivot = std::max(begin, std::min(finish, sampleIndex(cursorTime)));

  bool found = false;
  double bestDistance = 0.0;
  auto checkSegment = [&](int segment) -> bool {
    const double v0 = voltageAt(segment);
    const double v1 = voltageAt(segment + 1);
    if(!std::isfinite(v0) || !std::isfinite(v1) || v0 == v1)
      return false; // Flat runs at the threshold are ambiguous.
    if(!((v0 <= level && v1 >= level) ||
         (v0 >= level && v1 <= level))) return false;
    const double fraction = (level - v0) / (v1 - v0);
    if(fraction < 0.0 || fraction > 1.0) return false;
    const double t = firstTime + (segment + fraction) * dt;
    if(t < viewLeft || t > viewRight) return false;
    const double distance = std::fabs(t - cursorTime);
    if(!found || distance < bestDistance)
    {
      resultTime = t;
      bestDistance = distance;
      found = true;
    }
    return true;
  };

  // Stop after the first crossing in either direction; any subsequent one
  // on that same side would be farther from the X cursor.
  for(int segment = pivot; segment >= begin; --segment)
    if(checkSegment(segment)) break;
  for(int segment = pivot + 1; segment <= finish; ++segment)
    if(checkSegment(segment)) break;
  return found;
}

class DS2000ZoomFilter : public QObject
{
public:
  explicit DS2000ZoomFilter(QWidget *owner, DS2CursorTargetQuery target_query)
    : QObject(owner), view(owner), dragging(false), last_x(0),
      targetQuery(target_query) {}

protected:
  bool eventFilter(QObject *watched, QEvent *event) override
  {
    if(watched != view || !view->property("ds2_zoom_enabled").toBool())
      return QObject::eventFilter(watched, event);

    const int border = view->property("ds2_zoom_border").toInt();
    const int plot_width = view->width() - 2 * border;
    const int plot_height = view->height() - 2 * border;
    if(plot_width <= 0 || plot_height <= 0)
      return QObject::eventFilter(watched, event);

    // An eingeblendetes Messwertefeld kann an seiner Titelleiste verschoben
    // oder mit dem kleinen Fensterknopf in ein echtes QDockWidget geloest werden.
    if(event->type() == QEvent::MouseButtonPress)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      const QRect overlay = view->property("ds2_measure_overlay_rect").toRect();
      if(mouse->button() == Qt::LeftButton && overlay.isValid() &&
         overlay.contains(mouse->pos()) &&
         !view->property("ds2_measure_in_dock").toBool())
      {
        const QRect popout = view->property("ds2_measure_popout_rect").toRect();
        if(popout.contains(mouse->pos()))
        {
          openMeasurementDock();
          mouse->accept();
          return true;
        }
        // Title bar: drag panel without interfering with X/Y cursors.
        if(mouse->y() < overlay.top() + 22)
        {
          moving_overlay = true;
          last_overlay_pos = mouse->pos();
          view->setCursor(Qt::SizeAllCursor);
        }
        mouse->accept();
        return true;
      }
    }
    if(event->type() == QEvent::MouseMove && moving_overlay)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      if(!(mouse->buttons() & Qt::LeftButton))
      {
        moving_overlay = false;
        view->unsetCursor();
        return false;
      }
      const QPoint delta = mouse->pos() - last_overlay_pos;
      last_overlay_pos = mouse->pos();
      QPoint p = view->property("ds2_measure_overlay_pos").toPoint() + delta;
      const QRect box = view->property("ds2_measure_overlay_rect").toRect();
      p.setX(std::max(0, std::min(plot_width - box.width(), p.x())));
      p.setY(std::max(0, std::min(plot_height - box.height(), p.y())));
      view->setProperty("ds2_measure_overlay_pos", p);
      view->update();
      mouse->accept();
      return true;
    }
    if(event->type() == QEvent::MouseButtonRelease && moving_overlay)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      if(mouse->button() == Qt::LeftButton)
      {
        moving_overlay = false;
        view->unsetCursor();
        mouse->accept();
        return true;
      }
    }

    // Cursor dragging: keep existing trigger/voltage arrows and pan gestures.
    // Time cursor lines are selected near X, voltage cursor lines near Y.
    const bool time_active = view->property("ds2_cursor_interactive").toBool() &&
                             view->property("ds2_cursor_inited").toBool();
    const bool volt_active = view->property("ds2_y_cursor_interactive").toBool() &&
                             view->property("ds2_y_cursor_inited").toBool();
    if(time_active || volt_active)
    {
      if(event->type() == QEvent::MouseButtonPress)
      {
        QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if(mouse->button() == Qt::LeftButton &&
           !(mouse->modifiers() & Qt::ShiftModifier) &&
           inPlot(mouse->pos(), border, plot_width, plot_height))
        {
          int chosen = 0;
          double best = 13.0;
          const double x = mouse->x() - border;
          const double y = mouse->y() - border;

          // YA/YB handles occupy x=14..44 relative to the plot origin.
          // Give the handles priority when an X cursor crosses a Y cursor.
          // The V2 hit test started at x=45 and therefore excluded them!
          if(volt_active && x >= 12.0 && x <= 48.0)
          {
            const double da = std::fabs(y - cursorPlotY("ds2_y_cursor_a_volt", plot_height));
            const double db = std::fabs(y - cursorPlotY("ds2_y_cursor_b_volt", plot_height));
            if(std::min(da, db) <= 12.0)
            {
              chosen = (da <= db) ? 3 : 4;
            }
          }

          // Otherwise choose the nearest visible cursor line.
          // Original trigger/channel marker controls remain outside plot.
          if(!chosen && time_active && x >= 12 && x < plot_width - 12 &&
             y >= 30 && y < plot_height - 12)
          {
            const double da = std::fabs(x - cursorPlotX("ds2_cursor_a_time", plot_width));
            const double db = std::fabs(x - cursorPlotX("ds2_cursor_b_time", plot_width));
            if(da <= 11.0 && da < best) { best = da; chosen = 1; }
            if(db <= 11.0 && db < best) { best = db; chosen = 2; }
          }
          if(!chosen && volt_active && x >= 10 && x < plot_width - 10 &&
             y >= 0 && y < plot_height)
          {
            const double da = std::fabs(y - cursorPlotY("ds2_y_cursor_a_volt", plot_height));
            const double db = std::fabs(y - cursorPlotY("ds2_y_cursor_b_volt", plot_height));
            if(da <= 12.0 && da < best) { best = da; chosen = 3; }
            if(db <= 12.0 && db < best) { chosen = 4; }
          }
          if(chosen)
          {
            moving_cursor = chosen;
            // Cursor-line hit testing is unchanged. Ctrl is the optional snap
            // modifier; Shift+left remains reserved for panning.
            const DS2CursorTarget target = targetQuery(chosen, x, y,
                                                        plot_width, plot_height);
            showTarget(target, false);
            view->setCursor(chosen <= 2 ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            view->update();
            mouse->accept();
            return true;
          }
        }
      }
      if(event->type() == QEvent::MouseMove && moving_cursor)
      {
        QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if(!(mouse->buttons() & Qt::LeftButton))
        {
          moving_cursor = 0;
          hideTarget();
          view->unsetCursor();
          view->update();
          return false;
        }
        const double mx = std::max(0.0, std::min(double(plot_width),
                                                 double(mouse->x() - border)));
        const double my = std::max(0.0, std::min(double(plot_height),
                                                 double(mouse->y() - border)));
        const DS2CursorTarget target = targetQuery(moving_cursor, mx, my,
                                                     plot_width, plot_height);
        const bool ctrl = (mouse->modifiers() & Qt::ControlModifier) != 0;
        const bool snapped = ctrl && target.valid &&
            (moving_cursor <= 2 ? std::fabs(mx - target.x) <= 10.0
                                : std::fabs(my - target.y) <= 10.0);
        showTarget(target, snapped);
        if(moving_cursor <= 2)
        {
          view->setProperty(moving_cursor == 1 ? "ds2_cursor_a_time" : "ds2_cursor_b_time",
                            snapped ? target.time : plotTimeAtX(mx, plot_width));
        }
        else
        {
          view->setProperty(moving_cursor == 3 ? "ds2_y_cursor_a_volt" : "ds2_y_cursor_b_volt",
                            snapped ? target.voltage : plotVoltageAtY(my, plot_height));
        }
        view->update();
        mouse->accept();
        return true;
      }
      if(event->type() == QEvent::MouseButtonRelease && moving_cursor)
      {
        QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
        if(mouse->button() == Qt::LeftButton)
        {
          moving_cursor = 0;
          hideTarget();
          view->unsetCursor();
          view->update();
          mouse->accept();
          return true;
        }
      }
    }

    if(event->type() == QEvent::Wheel)
    {
      QWheelEvent *wheel = static_cast<QWheelEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
      const QPoint p = wheel->position().toPoint();
#else
      const QPoint p = wheel->pos();
#endif
      if(!inPlot(p, border, plot_width, plot_height))
        return QObject::eventFilter(watched, event);
      // Accumulate sub-notch wheel movements (some Windows mice/trackpads
      // report angle deltas smaller than the conventional 120 per detent).
      wheel_remainder += wheel->angleDelta().y();
      const int notches = wheel_remainder / 120;
      wheel_remainder -= notches * 120;
      if(notches == 0) { wheel->accept(); return true; }

      const double base_time = view->property("ds2_base_time_per_div").toDouble();
      if(!(base_time > 0.0) || !std::isfinite(base_time))
        return QObject::eventFilter(watched, event);

      // The 1400-point NORM trace keeps its familiar 16x limit.
      // A completed RAW snapshot can be inspected down to individual
      // acquisition samples; allow the additional 1-2-5 steps for RAW only.
      bool raw_ready = false;
      const DS2RawSnapshot raw_capture = ds2_raw_snapshot();
      if(raw_capture.enabled && !raw_capture.busy)
      {
        for(int channel = 0; channel < 4; ++channel)
        {
          if(raw_capture.traces[channel].valid())
          {
            raw_ready = true;
            break;
          }
        }
      }
      const double max_zoom = raw_ready ? 2048.0 : 16.0;

      const double old_zoom = factor();
      double new_zoom = old_zoom;
      const bool finer = notches > 0;
      for(int step = 0; step < std::abs(notches); ++step)
      {
        const double now_time = base_time / new_zoom;
        double next_time = ds2_adjacent_time_per_div(now_time, finer);
        // Keep exactly the original 1-2-5 steps; the allowed range
        // depends on whether a captured RAW waveform is available.
        if(!finer && next_time >= base_time) next_time = base_time;
        if(finer && next_time < base_time / max_zoom * (1.0 - 1e-9)) break;
        if(!(next_time > 0.0)) break;
        new_zoom = base_time / next_time;
        if(new_zoom < 1.0) new_zoom = 1.0;
        if(new_zoom > max_zoom) new_zoom = max_zoom;
      }

      const double cursor = (p.x() - border) / static_cast<double>(plot_width);
      const double original_point = center() + (cursor - 0.5) / old_zoom;
      const double new_center = original_point - (cursor - 0.5) / new_zoom;
      setZoom(new_zoom, new_center);
      wheel->accept();
      return true;
    }

    if(event->type() == QEvent::MouseButtonDblClick)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      if(mouse->button() == Qt::LeftButton && factor() > 1.0 &&
         inPlot(mouse->pos(), border, plot_width, plot_height))
      {
        setZoom(1.0, 0.5);
        mouse->accept();
        return true;
      }
    }

    if(event->type() == QEvent::MouseButtonPress)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      // Middle mouse drag is the preferred pan gesture. Shift + left drag
      // remains available for users without a clickable scroll wheel.
      const bool middle_pan = (mouse->button() == Qt::MiddleButton) &&
                              inPlot(mouse->pos(), border, plot_width, plot_height);
      const bool shift_left_pan = (mouse->button() == Qt::LeftButton) &&
                                  (mouse->modifiers() & Qt::ShiftModifier) &&
                                  mouse->x() >= border + 24 &&
                                  mouse->x() < border + plot_width - 24 &&
                                  mouse->y() >= border + 26 &&
                                  mouse->y() < border + plot_height - 24;
      if(factor() > 1.0 && (middle_pan || shift_left_pan))
      {
        dragging = true;
        drag_button = mouse->button();
        last_x = mouse->x();
        view->setCursor(Qt::ClosedHandCursor);
        mouse->accept();
        return true;
      }
    }

    if(event->type() == QEvent::MouseMove && dragging)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      if(!(mouse->buttons() & drag_button))
      {
        dragging = false;
        view->unsetCursor();
        return false;
      }
      const int dx = mouse->x() - last_x;
      last_x = mouse->x();
      setZoom(factor(), center() - dx / (plot_width * factor()));
      mouse->accept();
      return true;
    }

    if(event->type() == QEvent::MouseButtonRelease && dragging)
    {
      QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
      if(mouse->button() == drag_button)
      {
        dragging = false;
        view->unsetCursor();
        mouse->accept();
        return true;
      }
    }

    return QObject::eventFilter(watched, event);
  }

private:
  QWidget *view;
  bool dragging;
  Qt::MouseButton drag_button = Qt::NoButton;
  int last_x;
  int wheel_remainder = 0;
  int moving_cursor = 0;  // 1/2 = time A/B, 3/4 = voltage A/B
  DS2CursorTargetQuery targetQuery;

  void showTarget(const DS2CursorTarget &target, bool snapped)
  {
    view->setProperty("ds2_target_visible", target.valid);
    view->setProperty("ds2_target_x", target.x);
    view->setProperty("ds2_target_y", target.y);
    view->setProperty("ds2_target_snap", snapped);
  }

  void hideTarget()
  {
    view->setProperty("ds2_target_visible", false);
    view->setProperty("ds2_target_snap", false);
  }
  bool moving_overlay = false;
  QPoint last_overlay_pos;
  QPointer<QDockWidget> measure_dock;
  QPointer<QLabel> x_measure_label;
  QPointer<QLabel> y_measure_label;
  QPointer<QTimer> measure_refresh;

  void openMeasurementDock()
  {
    if(!measure_dock)
    {
      QMainWindow *mw = qobject_cast<QMainWindow *>(view->window());
      if(!mw) return;

      QDockWidget *dock = new QDockWidget(QString::fromUtf8("Cursor-Messwerte"), mw);
      dock->setObjectName("DSRemote_DS2000_CursorMesswerte");
      dock->setFeatures(QDockWidget::DockWidgetMovable |
                        QDockWidget::DockWidgetFloatable |
                        QDockWidget::DockWidgetClosable);
      dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea |
                            Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);

      QWidget *content = new QWidget(dock);
      QVBoxLayout *layout = new QVBoxLayout(content);
      // Compact two-row layout. The former expanding QLabel rows made the
      // floating window tall and Qt's docking preview unnecessarily high.
      layout->setContentsMargins(8, 5, 8, 5);
      layout->setSpacing(2);
      QLabel *x = new QLabel(content);
      QLabel *y = new QLabel(content);
      // Values can still be selected and copied with the mouse.
      x->setTextInteractionFlags(Qt::TextSelectableByMouse);
      y->setTextInteractionFlags(Qt::TextSelectableByMouse);
      x->setWordWrap(false);
      y->setWordWrap(false);
      QFont f("Consolas", 11);
      f.setBold(true);
      x->setFont(f);
      y->setFont(f);
      const int line_height = QFontMetrics(f).height() + 4;
      x->setFixedHeight(line_height);
      y->setFixedHeight(line_height);
      layout->addWidget(x);
      layout->addWidget(y);
      content->setMinimumWidth(515);
      content->setFixedHeight(10 + 2 * line_height + 2);
      content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
      dock->setWidget(content);
      mw->addDockWidget(Qt::RightDockWidgetArea, dock);
      // Qt's stock dock title bar already supports double-click to
      // switch between floating and docked mode (like the control panel).
      dock->setFloating(true);
      dock->resize(550, 88);
      measure_dock = dock;
      x_measure_label = x;
      y_measure_label = y;

      connect(dock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if(!visible)
        {
          // A float/re-dock transition can emit a temporary 'hidden'.
          QTimer::singleShot(0, this, [this]() {
            if(measure_dock && !measure_dock->isVisible())
            {
              view->setProperty("ds2_measure_in_dock", false);
              view->update();
            }
          });
        }
      });
      QTimer *timer = new QTimer(this);
      measure_refresh = timer;
      timer->setInterval(100);
      connect(timer, &QTimer::timeout, this, [this]() {
        if(!measure_dock || !measure_dock->isVisible()) return;
        if(x_measure_label)
        {
          QString s = view->property("ds2_measure_x_text").toString();
          if(s.isEmpty()) s = QString::fromUtf8("Zeitcursor: aus");
          if(x_measure_label->text() != s) x_measure_label->setText(s);
        }
        if(y_measure_label)
        {
          QString s = view->property("ds2_measure_y_text").toString();
          if(s.isEmpty()) s = QString::fromUtf8("Spannungscursor: aus");
          if(y_measure_label->text() != s) y_measure_label->setText(s);
        }
      });
      timer->start();
    }

    measure_dock->setFloating(true);
    measure_dock->show();
    measure_dock->raise();
    view->setProperty("ds2_measure_in_dock", true);
    view->update();
  }

  // Inverse of SignalCurve's actual X mapping (including display-only zoom).
  // The time coordinate is relative to the physical trigger (t=0).
  double plotTimeAtX(double x, int w) const
  {
    const double scale = view->property("ds2_base_time_per_div").toDouble();
    const double divisions = view->property("ds2_cursor_divisions").toDouble();
    const double offset = view->property("ds2_cursor_offset").toDouble();
    const double base_x = (x - w / 2.0) / factor() + center() * w;
    return offset + (base_x / w - 0.5) * divisions * scale;
  }
  double cursorPlotX(const char *key, int w) const
  {
    const double scale = view->property("ds2_base_time_per_div").toDouble();
    const double divisions = view->property("ds2_cursor_divisions").toDouble();
    if(!(scale > 0.0) || !(divisions > 0.0)) return -1e9;
    const double offset = view->property("ds2_cursor_offset").toDouble();
    const double t = view->property(key).toDouble();
    const double base_x = w * (0.5 + (t - offset) / (divisions * scale));
    return (base_x - center() * w) * factor() + w / 2.0;
  }

  // Voltage is referred to CHn's zero-voltage position, not to a pixel.
  // These equations are the inverse of SignalCurve's existing channel mapping:
  // y = h/2 - (voltage + chanoffset) * h / (Vdiv * divisions).
  double plotVoltageAtY(double y, int h) const
  {
    const double scale = view->property("ds2_y_cursor_scale").toDouble();
    const double divisions = view->property("ds2_y_cursor_divisions").toDouble();
    const double offset = view->property("ds2_y_cursor_offset").toDouble();
    if(!(scale > 0.0) || !(divisions > 0.0)) return 0.0;
    return (0.5 - y / h) * scale * divisions - offset;
  }
  double cursorPlotY(const char *key, int h) const
  {
    const double scale = view->property("ds2_y_cursor_scale").toDouble();
    const double divisions = view->property("ds2_y_cursor_divisions").toDouble();
    if(!(scale > 0.0) || !(divisions > 0.0)) return -1e9;
    const double offset = view->property("ds2_y_cursor_offset").toDouble();
    const double volts = view->property(key).toDouble();
    return h * (0.5 - (volts + offset) / (scale * divisions));
  }

  static bool inPlot(const QPoint &p, int b, int w, int h)
  {
    return p.x() >= b && p.x() < b+w && p.y() >= b && p.y() < b+h;
  }
  double factor() const
  {
    const double z = view->property("ds2_zoom_factor").toDouble();
    return z >= 1.0 ? z : 1.0;
  }
  double center() const
  {
    return view->property("ds2_zoom_center").toDouble();
  }
  void setZoom(double z, double c)
  {
    if(z <= 1.0) { z = 1.0; c = 0.5; }
    const double half = 0.5 / z;
    if(c < half) c = half;
    if(c > 1.0-half) c = 1.0-half;
    view->setProperty("ds2_zoom_factor", z);
    view->setProperty("ds2_zoom_center", c);
    view->update();
  }
};

// Draw a captured STOP/RAW trace in the existing time and voltage system.
// Overview: min/max envelope for every pixel column (keeps narrow spikes).
// Zoom: genuine sample-to-sample vectors or dots; no invented interpolation.
static void ds2_draw_raw_trace(QPainter *painter, const DS2RawTrace &t,
                               int width, int height, double offset, double secdiv,
                               int hdiv, double chan_vdiv, double chan_offset,
                               int vdiv, double zoom, double zoom_center,
                               bool dots)
{
  if(!t.valid() || width <= 2 || height <= 2 ||
     secdiv <= 0.0 || hdiv <= 0 || chan_vdiv <= 0.0 || vdiv <= 0) return;
  const double span = hdiv * secdiv;
  const double visible_span = span / zoom;
  const double time_left = offset + (zoom_center - 0.5 - 0.5/zoom) * span;
  const double dt = t.xincrement;
  const int count = t.bytes.size();
  auto index_at_time = [&](double seconds) -> double {
    return t.xreference + (seconds - t.xorigin) / dt - (t.first_point - 1);
  };
  const int begin = std::max(0, std::min(count-1, int(std::floor(index_at_time(time_left))) - 2));
  const int end = std::max(0, std::min(count-1,
             int(std::ceil(index_at_time(time_left + visible_span))) + 2));
  if(end < begin || index_at_time(time_left + visible_span) < 0 ||
     index_at_time(time_left) > count-1) return;

  const double fullscale = chan_vdiv * vdiv;
  auto y_at = [&](int i) -> double {
    const double volts = (double(t.bytes[i]) - t.yorigin - t.yreference) * t.yincrement;
    return height * (0.5 - (volts + chan_offset) / fullscale);
  };
  const double pixels_per_sample = width * dt / visible_span;
  if(pixels_per_sample >= 0.75)
  {
    double last_x = 0.0, last_y = 0.0;
    bool have_previous = false;
    for(int i = begin; i <= end; ++i)
    {
      const double sample_t = t.xorigin + ((i + t.first_point-1) - t.xreference) * dt;
      const double x = (sample_t - time_left) * width / visible_span;
      const double y = y_at(i);
      if(dots)
        painter->drawPoint(QPointF(x, y));
      else if(have_previous)
        painter->drawLine(QPointF(last_x, last_y), QPointF(x, y));
      last_x = x; last_y = y;
      have_previous = true;
    }
    return;
  }

  // Aggregate by columns, not by skipping samples: spikes cannot disappear.
  double prev_y = 0.0;
  bool prev_valid = false;
  for(int x = 0; x < width; ++x)
  {
    const double x0 = time_left + (double(x) / width) * visible_span;
    const double x1 = time_left + (double(x+1) / width) * visible_span;
    const int a = std::max(0, int(std::floor(index_at_time(x0))));
    const int b = std::min(count-1, int(std::ceil(index_at_time(x1))));
    if(b < a) { prev_valid = false; continue; }
    quint8 lo = 255, hi = 0;
    for(int i = a; i <= b; ++i)
    {
      lo = std::min(lo, t.bytes[i]);
      hi = std::max(hi, t.bytes[i]);
    }
    // Y coordinates run opposite to ADC voltage.
    const double y_hi = height * (0.5 -
        (((double(hi) - t.yorigin - t.yreference) * t.yincrement) + chan_offset)/fullscale);
    const double y_lo = height * (0.5 -
        (((double(lo) - t.yorigin - t.yreference) * t.yincrement) + chan_offset)/fullscale);
    painter->drawLine(QPointF(x+0.5, y_hi), QPointF(x+0.5, y_lo));
    const double mid = (y_lo + y_hi) * 0.5;
    if(prev_valid)
      painter->drawLine(QPointF(x-0.5, prev_y), QPointF(x+0.5, mid));
    prev_y = mid;
    prev_valid = true;
  }
}

} // namespace




SignalCurve::SignalCurve(QWidget *w_parent) : QWidget(w_parent)
{
  int i;

  mainwindow = (UI_Mainwindow *)w_parent;

  setAttribute(Qt::WA_OpaquePaintEvent);

  SignalColor[0] = Qt::blue;
  SignalColor[1] = Qt::blue;
  SignalColor[2] = Qt::blue;
  SignalColor[3] = Qt::blue;
  tracewidth = 0;
  BackgroundColor = Qt::gray;
  RasterColor = Qt::darkGray;
  TextColor = Qt::black;

  smallfont.setFamily("Arial");
  smallfont.setPixelSize(8);

  trig_line_timer = new QTimer(this);
  trig_line_timer->setSingleShot(true);

  trig_stat_timer = new QTimer(this);

#if QT_VERSION >= 0x050000
  trig_line_timer->setTimerType(Qt::PreciseTimer);
  trig_stat_timer->setTimerType(Qt::PreciseTimer);
#endif

  bufsize = 0;
  bordersize = 60;

  // Keep normal scope controls intact. Only the waveform area uses this filter.
  setProperty("ds2_zoom_enabled", false);
  setProperty("ds2_zoom_border", bordersize);
  setProperty("ds2_zoom_factor", 1.0);
  setProperty("ds2_zoom_center", 0.5);
  setProperty("ds2_cursor_enabled", false);
  setProperty("ds2_cursor_interactive", false);
  setProperty("ds2_cursor_inited", false);
  setProperty("ds2_cursor_a_time", 0.0);
  setProperty("ds2_cursor_b_time", 0.0);
  setProperty("ds2_cursor_offset", 0.0);
  setProperty("ds2_cursor_divisions", 14.0);
  setProperty("ds2_y_cursor_enabled", false);
  setProperty("ds2_y_cursor_interactive", false);
  setProperty("ds2_y_cursor_inited", false);
  setProperty("ds2_y_cursor_a_volt", 0.0);
  setProperty("ds2_y_cursor_b_volt", 0.0);
  setProperty("ds2_y_cursor_offset", 0.0);
  setProperty("ds2_y_cursor_scale", 1.0);
  setProperty("ds2_y_cursor_divisions", 8.0);
  setProperty("ds2_y_cursor_channel", -1);
  setProperty("ds2_measure_in_dock", false);
  setProperty("ds2_measure_overlay_pos", QPoint(8, 7));
  setProperty("ds2_measure_overlay_rect", QRect());
  setProperty("ds2_measure_popout_rect", QRect());
  setProperty("ds2_measure_x_text", QString());
  setProperty("ds2_measure_y_text", QString());
  setProperty("ds2_target_visible", false);
  setProperty("ds2_target_snap", false);

  // Query native scope samples without modifying their buffers. The filter
  // calls this only while the user is actively dragging a measurement cursor.
  // Same time/voltage transforms are used in drawWidget/ds2_draw_raw_trace.
  const DS2CursorTargetQuery cursorTargetQuery =
      [this](int which, double mouseX, double mouseY, int pw, int ph) -> DS2CursorTarget {
    DS2CursorTarget target;
    if(!devparms || devparms->modelserie != 2 || !devparms->connected ||
       pw < 10 || ph < 10 || devparms->math_fft || devparms->timebasedelayenable ||
       !(devparms->timebasescale > 0.0) || devparms->hordivisions <= 0 ||
       devparms->vertdivisions <= 0) return target;

    // X: selected EDGE trigger source. Y: currently selected voltage channel.
    const int ch = which <= 2 ? devparms->triggeredgesource
                              : property("ds2_y_cursor_channel").toInt();
    if(ch < 0 || ch >= devparms->channel_cnt || ch >= MAX_CHNS || ch >= 4 ||
       !(devparms->chanscale[ch] > 0.0)) return target;
    const DS2RawSnapshot snapshot = ds2_raw_snapshot();
    // V1.9d: Precision cursor targets are available only from a cached RAW
    // snapshot. NORM waveforms are too sparse for dependable level crossings.
    // Returning an invalid target also disables Ctrl-snap while dragging.
    if(!snapshot.enabled || snapshot.busy) return target;
    const bool raw_mode = snapshot.enabled;
    if(raw_mode ? !snapshot.traces[ch].valid() : !devparms->chandisplay[ch])
      return target;

    const double zoom = std::max(1.0, property("ds2_zoom_factor").toDouble());
    const double center = property("ds2_zoom_center").toDouble();
    const double span = devparms->hordivisions * devparms->timebasescale;
    const double visibleSpan = span / zoom;
    const double leftTime = devparms->timebaseoffset +
                            (center - 0.5 - 0.5/zoom) * span;
    const double fullScale = devparms->chanscale[ch] * devparms->vertdivisions;
    const double offset = devparms->chanoffset[ch];
    auto toX = [&](double t) -> double { return (t - leftTime) * pw / visibleSpan; };
    auto toY = [&](double v) -> double { return ph * (0.5 - (v + offset) / fullScale); };
    auto toVoltage = [&](double py) -> double {
      return (0.5 - py / ph) * fullScale - offset;
    };

    // X cursor: find the nearest crossing of the *trigger voltage* by
    // the selected trigger-source channel. Search BOTH rising and falling
    // crossings, anywhere in the visible trace. Unlike V1.9b this is NOT
    // restricted to the scope's time-zero reference. Mouse Y is irrelevant:
    // grabbing the A/B handle at the top must show the same target as
    // grabbing the vertical cursor line.
    if(which <= 2)
    {
      const double triggerVoltage = devparms->triggeredgelevel[ch];
      const double targetY = toY(triggerVoltage);
      if(!std::isfinite(targetY) || targetY < 0.0 || targetY >= ph)
        return target;

      double firstSampleTime = 0.0;
      double sampleInterval = 0.0;
      int sampleCount = 0;
      if(raw_mode)
      {
        const DS2RawTrace &trace = snapshot.traces[ch];
        sampleInterval = trace.xincrement;
        firstSampleTime = trace.xorigin +
            (double(trace.first_point - 1) - trace.xreference) * sampleInterval;
        sampleCount = trace.bytes.size();
      }
      else
      {
        sampleInterval = devparms->timebasescale / 100.0;
        firstSampleTime = devparms->xorigin[ch];
        sampleCount = (devparms->wavebuf[ch] != nullptr) ?
                       devparms->wavebufsz : 0;
      }
      if(!(sampleInterval > 0.0) || !std::isfinite(sampleInterval) ||
         !std::isfinite(firstSampleTime) || sampleCount < 2)
        return target;

      // Use the same calibrated voltage conversion as the two trace
      // renderers. Read the real samples, never the min/max pixel envelope.
      auto sampleVoltage = [&](int index) -> double {
        if(raw_mode)
        {
          const DS2RawTrace &trace = snapshot.traces[ch];
          return (double(trace.bytes[index]) - trace.yorigin -
                  trace.yreference) * trace.yincrement;
        }
        return double(devparms->wavebuf[ch][index]) *
               devparms->chanscale[ch] / 25.0;
      };

      double crossingTime = 0.0;
      const double cursorTime = leftTime + (mouseX / pw) * visibleSpan;
      if(ds2_find_nearest_level_crossing(sampleCount, firstSampleTime,
                 sampleInterval, leftTime, leftTime + visibleSpan,
                 cursorTime, triggerVoltage, sampleVoltage, crossingTime))
      {
        target.valid = true;
        target.x = toX(crossingTime);
        target.y = targetY;
        target.time = crossingTime;
        target.voltage = triggerVoltage;
      }
      return target;
    }

    // Y cursor: determine a local, robust plateau voltage near the mouse.
    // NORM carries far fewer samples, so use a somewhat wider pixel region
    // than RAW while still rejecting isolated spikes/steep transitions.
    const double searchRadius = raw_mode ? 18.0 : 32.0;
    const double xMin = mouseX - searchRadius;
    const double xMax = mouseX + searchRadius;
    std::vector<double> nearY;
    auto collect = [&](double time, double volts) {
      const double px = toX(time);
      if(px < xMin || px > xMax) return;
      const double py = toY(volts);
      if(std::fabs(py - mouseY) <= 22.0)
        nearY.push_back(py);
    };

    if(raw_mode)
    {
      const DS2RawTrace &trace = snapshot.traces[ch];
      const int count = trace.bytes.size();
      const double tStart = leftTime + (xMin / pw) * visibleSpan;
      const double tEnd = leftTime + (xMax / pw) * visibleSpan;
      auto index = [&](double t) -> double {
        return trace.xreference + (t - trace.xorigin) / trace.xincrement
               - (trace.first_point - 1);
      };
      const int start = std::max(0, std::min(count, int(std::floor(index(tStart))) - 2));
      const int finish = std::max(-1, std::min(count - 1, int(std::ceil(index(tEnd))) + 2));
      const int stride = std::max(1, (finish - start + 1) / 600);
      for(int sampleIndex = start; sampleIndex <= finish; sampleIndex += stride)
      {
        const double t = trace.xorigin +
             ((sampleIndex + trace.first_point - 1) - trace.xreference) * trace.xincrement;
        const double v = (double(trace.bytes[sampleIndex]) - trace.yorigin -
                          trace.yreference) * trace.yincrement;
        collect(t, v);
      }
    }
    else if(devparms->wavebuf[ch] && devparms->wavebufsz > 1)
    {
      const int count = devparms->wavebufsz;
      const double dt = devparms->timebasescale / 100.0;
      if(!(dt > 0.0)) return target;
      const double tStart = leftTime + (xMin / pw) * visibleSpan;
      const double tEnd = leftTime + (xMax / pw) * visibleSpan;
      const int start = std::max(0, std::min(count, int(std::floor(
          (tStart - devparms->xorigin[ch]) / dt)) - 2));
      const int finish = std::max(-1, std::min(count - 1, int(std::ceil(
          (tEnd - devparms->xorigin[ch]) / dt)) + 2));
      for(int sampleIndex = start; sampleIndex <= finish; ++sampleIndex)
      {
        const double t = devparms->xorigin[ch] + sampleIndex * dt;
        const double v = double(devparms->wavebuf[ch][sampleIndex]) *
                         devparms->chanscale[ch] / 25.0;
        collect(t, v);
      }
    }

    // Local level estimator: median with a trimmed mean near the median.
    // This rejects narrow noise spikes; a steep edge or ambiguous level
    // (large dispersion in this neighbourhood) produces no snap target.
    if(nearY.size() < 2) return target;
    std::sort(nearY.begin(), nearY.end());
    const double median = nearY[nearY.size() / 2];
    double sum = 0.0;
    int accepted = 0;
    for(double py : nearY)
      if(std::fabs(py - median) <= 5.0) { sum += py; ++accepted; }
    if(accepted < 2 || accepted * 2 < int(nearY.size())) return target;
    const double averagedY = sum / accepted;
    if(averagedY < 0 || averagedY >= ph ||
       std::fabs(averagedY - mouseY) > 22.0) return target;
    target.valid = true;
    target.x = mouseX;
    target.y = averagedY;
    target.voltage = toVoltage(averagedY);
    return target;
  };
  installEventFilter(new DS2000ZoomFilter(this, cursorTargetQuery));
  // DS2302 RAW controls: real Qt buttons in the lower black status strip.
  // The shortcut and the toggle button share exactly the same behavior.
  DS2RawControls *raw_controls = new DS2RawControls(this);
  raw_controls->setGeometry(std::max(0, width()-440), height()-29, 438, 24);
  auto raw_toggle_requested = [this]() {
    if(!devparms || devparms->modelserie != 2 || !devparms->connected) return;
    const DS2RawSnapshot before = ds2_raw_snapshot();
    bool cached = false;
    for(int c=0; c<4; ++c) cached |= before.traces[c].valid();
    if(before.busy) return;
    if(!before.enabled && !cached)
    {
      QToolTip::showText(QCursor::pos(),
                         "Kein RAW-Snapshot vorhanden. Bitte bei STOP 'RAW laden' klicken.");
      return;
    }
    const bool leaving_raw = before.enabled;
    ds2_raw_toggle();
    // Never carry an old target square or snap indication across modes.
    setProperty("ds2_target_visible", false);
    setProperty("ds2_target_snap", false);
    if(leaving_raw && property("ds2_zoom_factor").toDouble() > 16.0)
    {
      setProperty("ds2_zoom_factor", 1.0);
      setProperty("ds2_zoom_center", 0.5);
    }
    update();
  };
  QShortcut *raw_shortcut = new QShortcut(QKeySequence("Ctrl+Shift+R"), this);
  raw_shortcut->setContext(Qt::WindowShortcut);
  connect(raw_shortcut, &QShortcut::activated, this, raw_toggle_requested);
  connect(raw_controls->toggleButton, &QToolButton::clicked, this, raw_toggle_requested);
  connect(raw_controls->reloadButton, &QToolButton::clicked, this, [this]() {
    if(!devparms || devparms->modelserie != 2 || !devparms->connected) return;
    // UI check AND worker check; no new acquisition in RUN.
    if(devparms->triggerstatus != 5)
    {
      QToolTip::showText(QCursor::pos(), "RAW laden nur bei STOP.");
      return;
    }
    ds2_raw_reload();
    update();
  });
  QTimer *raw_ui_timer = new QTimer(this);
  raw_ui_timer->setInterval(120);
  connect(raw_ui_timer, &QTimer::timeout, this, [this, raw_controls]() {
    if(devparms)
    {
      unsigned live_mask = 0;
      for(int ch = 0; ch < devparms->channel_cnt && ch < 4; ++ch)
        if(devparms->chandisplay[ch]) live_mask |= (1u << ch);
      raw_controls->refresh(devparms->modelserie == 2,
                            devparms->connected, devparms->triggerstatus == 5,
                            live_mask);
    }
  });
  raw_ui_timer->start();

  v_sense = 1;

  mouse_x = 0;
  mouse_y = 0;
  mouse_old_x = 0;
  mouse_old_y = 0;

  label_active = LABEL_ACTIVE_NONE;

  for(i=0; i<MAX_CHNS; i++)
  {
    chan_arrow_moving[i] = 0;

    chan_arrow_pos[i] = 127;

    chan_tmp_y_pixel_offset[i] = 0;

    chan_tmp_old_y_pixel_offset[i] = 0;
  }

  trig_level_arrow_moving = 0;

  trig_pos_arrow_moving = 0;

  fft_arrow_pos = 0;

  fft_arrow_moving = 0;

  trig_line_visible = 0;

  trig_stat_flash = 0;

  trig_level_arrow_pos = 127;

  trig_pos_arrow_pos = 100;

  use_move_events = 0;

  updates_enabled = true;

  old_w = 10000;

  devparms = NULL;

  device = NULL;

  connect(trig_line_timer, SIGNAL(timeout()), this, SLOT(trig_line_timer_handler()));
  connect(trig_stat_timer, SIGNAL(timeout()), this, SLOT(trig_stat_timer_handler()));
}


void SignalCurve::clear()
{
  bufsize = 0;

  update();
}


void SignalCurve::resizeEvent(QResizeEvent *resize_event)
{
  QWidget::resizeEvent(resize_event);
  QWidget *found_panel = findChild<QWidget *>("DS2302RawControls");
  DS2RawControls *panel = static_cast<DS2RawControls *>(found_panel);
  if(panel)
  {
    const int left_reserved = 265; // original CH1/CH2 label region
    const int usable = std::max(0, width() - left_reserved - 10);
    const int bar_width = std::min(438, usable);
    panel->setGeometry(width() - bar_width - 5,
                       height() - 29, bar_width, 24);
    panel->setVisible(bar_width >= 250 && devparms &&
                      devparms->connected && devparms->modelserie == 2);
  }
}


void SignalCurve::setUpdatesEnabled(bool enabled)
{
  updates_enabled = enabled;
}


void SignalCurve::paintEvent(QPaintEvent *)
{
  if(updates_enabled == true)
  {
    QPainter paint(this);
#if QT_VERSION >= 0x050000
    paint.setRenderHint(QPainter::Qt4CompatiblePainting, true);
#endif

    smallfont.setPixelSize(devparms->font_size);

    paint.setFont(smallfont);

    drawWidget(&paint, width(), height());

    old_w = width();
  }
}


void SignalCurve::drawWidget(QPainter *painter, int curve_w, int curve_h)
{
  int i, chn, small_rulers, curve_w_backup, curve_h_backup, w_trace_offset,
      chns_done;

  char str[1024];

  double h_step=0.0,
         step,
         step2;

//  clk_start = clock();

  if(devparms == NULL)
  {
    return;
  }

  const DS2RawSnapshot ds2_raw_state_local = (devparms->modelserie == 2)
                                             ? ds2_raw_snapshot() : DS2RawSnapshot();
  // Snapshot is an all-or-nothing display source, not a channel-by-channel
  // fallback to live NORM. A missing snapshot trace is NEVER drawn as live.
  const bool ds2_snapshot_mode = (devparms->modelserie == 2 &&
                                  ds2_raw_state_local.enabled);

  curve_w_backup = curve_w;

  curve_h_backup = curve_h;

  small_rulers = 5 * devparms->hordivisions;

  painter->fillRect(0, 0, curve_w, curve_h, BackgroundColor);

  if((curve_w < ((bordersize * 2) + 5)) || (curve_h < ((bordersize * 2) + 5)))
  {
    return;
  }

  painter->fillRect(0, 0, curve_w, 30, QColor(32, 32, 32));

  drawTopLabels(painter);

  // Restore the original bottom strip and the interactive CH1/CH2
  // channel-setting labels. They were accidentally lost in the RAW-UI
  // refactor, even though drawChanLabel() was still intact.
  painter->fillRect(0, curve_h - 30, curve_w, 30, QColor(32, 32, 32));
  for(i = 0; i < devparms->channel_cnt; ++i)
  {
    drawChanLabel(painter, 8 + (i * 130), curve_h - 25, i);
  }
  if(devparms->connected && devparms->show_fps)
  {
    drawfpsLabel(painter, curve_w - 80, curve_h - 11);
  }

  // RAW controls occupy the FREE area to the right of CH1/CH2.
  // The top Rigol status row stays unchanged.
/////////////////////////////////// translate coordinates, draw and fill a rectangle ///////////////////////////////////////////

  painter->translate(bordersize, bordersize);

  curve_w -= (bordersize * 2);

  curve_h -= (bordersize * 2);

  // If a STOP snapshot is invalidated by RUN, do not keep a RAW-only
  // magnification for the live NORM plot.
  if(devparms->modelserie == 2 &&
     property("ds2_zoom_factor").toDouble() > 16.0 &&
     !ds2_raw_snapshot().enabled)
  {
    setProperty("ds2_zoom_factor", 1.0);
    setProperty("ds2_zoom_center", 0.5);
  }

  // Original coordinates are retained for all device models and at 1x zoom.
  const double ds2_zoom = (devparms->modelserie == 2 && !devparms->math_fft)
                       ? property("ds2_zoom_factor").toDouble() : 1.0;
  setProperty("ds2_zoom_enabled", devparms->modelserie == 2 && !devparms->math_fft);
  // Waveform X-positions are based on timebasescale in the existing code.
  // Use the same basis for the zoom steps and the effective time/div label.
  setProperty("ds2_base_time_per_div", devparms->timebasescale);
  setProperty("ds2_cursor_offset", devparms->timebaseoffset);
  setProperty("ds2_cursor_divisions", devparms->hordivisions);
  const double ds2_center = property("ds2_zoom_center").toDouble() * curve_w;
  const bool ds2_cursor_visible = (devparms->modelserie == 2 &&
      !devparms->math_fft && !devparms->timebasedelayenable &&
      property("ds2_cursor_enabled").toBool() &&
      devparms->timebasescale > 0.0 && devparms->hordivisions > 0);
  setProperty("ds2_cursor_interactive", ds2_cursor_visible);

  // Y cursors belong to a physical channel.  CH1 and CH2 can have very
  // different V/div or offsets; simply reinterpreting CH1's voltages as CH2
  // values made both Y lines disappear off screen when CH1 was switched off.
  // Keep independent absolute voltage levels for each channel and restore
  // them whenever that channel is selected again.
  auto ds2_channel_visible = [&](int c) -> bool {
    if(c < 0 || c >= devparms->channel_cnt || c >= MAX_CHNS) return false;
    if(ds2_snapshot_mode)
      return c < 4 && ds2_raw_state_local.traces[c].valid();
    return devparms->chandisplay[c];
  };
  int ds2_y_chn = devparms->activechannel;
  if(!ds2_channel_visible(ds2_y_chn))
  {
    ds2_y_chn = -1;
    for(int c=0; c<devparms->channel_cnt && c<MAX_CHNS; ++c)
      if(ds2_channel_visible(c)) { ds2_y_chn = c; break; }
  }
  const int ds2_previous_y_chn = property("ds2_y_cursor_channel").toInt();
  if(ds2_y_chn != ds2_previous_y_chn)
  {
    if(ds2_previous_y_chn >= 0 && ds2_previous_y_chn < MAX_CHNS &&
       property("ds2_y_cursor_inited").toBool())
    {
      const QByteArray prefix = QByteArray("ds2_y_ch") + QByteArray::number(ds2_previous_y_chn);
      setProperty((prefix + "_a").constData(), property("ds2_y_cursor_a_volt"));
      setProperty((prefix + "_b").constData(), property("ds2_y_cursor_b_volt"));
      setProperty((prefix + "_valid").constData(), true);
    }

    if(ds2_y_chn >= 0 && devparms->vertdivisions > 0 &&
       devparms->chanscale[ds2_y_chn] > 0.0)
    {
      const QByteArray prefix = QByteArray("ds2_y_ch") + QByteArray::number(ds2_y_chn);
      if(property((prefix + "_valid").constData()).toBool())
      {
        setProperty("ds2_y_cursor_a_volt", property((prefix + "_a").constData()));
        setProperty("ds2_y_cursor_b_volt", property((prefix + "_b").constData()));
      }
      else
      {
        // First selection of this channel: place both handles on screen.
        const double range = devparms->chanscale[ds2_y_chn] * devparms->vertdivisions;
        const double offset = devparms->chanoffset[ds2_y_chn];
        setProperty("ds2_y_cursor_a_volt", 0.15 * range - offset);
        setProperty("ds2_y_cursor_b_volt", -0.15 * range - offset);
      }
      setProperty("ds2_y_cursor_inited", true);
    }
    else
    {
      setProperty("ds2_y_cursor_inited", false);
    }
    setProperty("ds2_y_cursor_channel", ds2_y_chn);
  }

  const bool ds2_y_cursor_visible = devparms->modelserie == 2 &&
      !devparms->math_fft && !devparms->timebasedelayenable &&
      property("ds2_y_cursor_enabled").toBool() &&
      ds2_y_chn >= 0 && devparms->vertdivisions > 0 &&
      devparms->chanscale[ds2_y_chn] > 0.0;
  if(ds2_y_chn >= 0)
  {
    setProperty("ds2_y_cursor_scale", devparms->chanscale[ds2_y_chn]);
    setProperty("ds2_y_cursor_offset", devparms->chanoffset[ds2_y_chn]);
    setProperty("ds2_y_cursor_divisions", devparms->vertdivisions);
  }
  setProperty("ds2_y_cursor_interactive", ds2_y_cursor_visible);

  if(devparms->math_fft && devparms->math_fft_split)
  {
    curve_h /= 3;
  }

/////////////////////////////////// draw the rasters ///////////////////////////////////////////

  painter->setPen(RasterColor);

  painter->drawRect (0, 0, curve_w - 1, curve_h - 1);

  // Keep the physical graticule fixed (14 visible divisions).
  // The effective time per visible division is scope_timebase / display_zoom.

  if((devparms->math_fft == 0) || (devparms->math_fft_split == 0))
  {
    if(devparms->displaygrid)
    {
      painter->setPen(QPen(QBrush(RasterColor, Qt::SolidPattern), tracewidth, Qt::DotLine, Qt::SquareCap, Qt::BevelJoin));

      if(devparms->displaygrid == 2)
      {
        step = (double)curve_w / (double)devparms->hordivisions;

        for(i=1; i<devparms->hordivisions; i++)
        {
          painter->drawLine(step * i, curve_h - 1, step * i, 0);
        }

        step = curve_h / (double)devparms->vertdivisions;

        for(i=1; i<devparms->vertdivisions; i++)
        {
          painter->drawLine(0, step * i, curve_w - 1, step * i);
        }
      }
      else
      {
        painter->drawLine(curve_w / 2, curve_h - 1, curve_w / 2, 0);

        painter->drawLine(0, curve_h / 2, curve_w - 1, curve_h / 2);
      }
    }

    painter->setPen(RasterColor);

    step = (double)curve_w / (double)small_rulers;

    for(i=1; i<small_rulers; i++)
    {
      step2 = step * i;

      if(devparms->displaygrid)
      {
        painter->drawLine(step2, curve_h / 2 + 2, step2, curve_h / 2 - 2);
      }

      if(i % 5)
      {
        painter->drawLine(step2, curve_h - 1, step2, curve_h - 5);

        painter->drawLine(step2, 0, step2, 4);
      }
      else
      {
        painter->drawLine(step2, curve_h - 1, step2, curve_h - 9);

        painter->drawLine(step2, 0, step2, 8);
      }
    }

    step = curve_h / (5.0 * devparms->vertdivisions);

    for(i=1; i<(5 * devparms->vertdivisions); i++)
    {
      step2 = step * i;

      if(devparms->displaygrid)
      {
        painter->drawLine(curve_w / 2 + 2, step2, curve_w / 2  - 2, step2);
      }

      if(i % 5)
      {
        painter->drawLine(curve_w - 1, step2, curve_w - 5, step2);

        painter->drawLine(0, step2, 4, step2);
      }
      else
      {
        painter->drawLine(curve_w - 1, step2, curve_w - 9, step2);

        painter->drawLine(0, step2, 8, step2);
      }
    }
  }  // if((devparms->math_fft == 0) || (devparms->math_fft_split == 0))
  else
  {
    painter->drawLine(curve_w / 2, curve_h - 1, curve_w / 2, 0);

    painter->drawLine(0, curve_h / 2, curve_w - 1, curve_h / 2);
  }

/////////////////////////////////// draw the arrows ///////////////////////////////////////////

  if(devparms->modelserie == 6)
  {
    v_sense = -((double)curve_h / 256.0);
  }
  else if(devparms->modelserie == 4)
    {
      v_sense = -((double)curve_h / (32.0 * devparms->vertdivisions));
    }
    else
    {
      v_sense = -((double)curve_h / (25.0 * devparms->vertdivisions));
    }

  drawTrigCenterArrow(painter, curve_w / 2, 0);

  for(chn=0; chn<devparms->channel_cnt; chn++)
  {
    if(!ds2_channel_visible(chn))
    {
      continue;
    }

    if(chan_arrow_moving[chn])
    {
      drawArrow(painter, 0, chan_arrow_pos[chn], 0, SignalColor[chn], '1' + chn);
    }
    else
    {
      chan_arrow_pos[chn] =  (curve_h / 2) - (devparms->chanoffset[chn] / ((devparms->chanscale[chn] * devparms->vertdivisions) / curve_h));

      if(chan_arrow_pos[chn] < 0)
      {
        chan_arrow_pos[chn] = -1;

        drawArrow(painter, -6, chan_arrow_pos[chn], 3, SignalColor[chn], '1' + chn);
      }
      else if(chan_arrow_pos[chn] > curve_h)
        {
          chan_arrow_pos[chn] = curve_h + 1;

          drawArrow(painter, -6, chan_arrow_pos[chn], 1, SignalColor[chn], '1' + chn);
        }
        else
        {
          drawArrow(painter, 0, chan_arrow_pos[chn], 0, SignalColor[chn], '1' + chn);
        }
    }
  }

/////////////////////////////////// FFT: draw the curve ///////////////////////////////////////////

  if((devparms->math_fft == 1) && (devparms->math_fft_split == 0))
  {
    drawFFT(painter, curve_h_backup, curve_w_backup);
  }

/////////////////////////////////// draw the curve ///////////////////////////////////////////

  if(bufsize > 32 || ds2_snapshot_mode)
  {
    painter->setClipping(true);
    painter->setClipRegion(QRegion(0, 0, curve_w, curve_h), Qt::ReplaceClip);

    // DS2302 / DS2000: subpixel-accurate waveform rendering (test).
    // Other model series retain their original drawing algorithm.
    if(devparms->modelserie == 2)
    {
      painter->setRenderHint(QPainter::Antialiasing, true);
    }

    h_step = (double)curve_w / (devparms->hordivisions * 100);

    for(chn=0, chns_done=0; chn<=devparms->channel_cnt; chn++)
    {
      if(chns_done)  break;

      if(chn == devparms->activechannel)  continue;

      if(chn == devparms->channel_cnt)
      {
        chn = devparms->activechannel;

        chns_done = 1;
      }

      if(!ds2_channel_visible(chn))
      {
        continue;
      }

      const double w_trace_offset_f = (curve_w / 2.0) - (((devparms->timebaseoffset - devparms->xorigin[chn]) / devparms->timebasescale) * ((double)curve_w / (double)(devparms->hordivisions)));
      w_trace_offset = w_trace_offset_f;  // Preserves the original integer-based drawing for other models.

      painter->setPen(QPen(QBrush(SignalColor[chn], Qt::SolidPattern), tracewidth, Qt::SolidLine, Qt::SquareCap, Qt::BevelJoin));

      // SNAPSHOT mode draws exclusively those channels saved together in
      // this RAW capture. Never mix a frozen CH1 with live NORM CH2.
      if(ds2_snapshot_mode)
      {
        if(chn >= 0 && chn < 4 && ds2_raw_state_local.traces[chn].valid())
          ds2_draw_raw_trace(painter, ds2_raw_state_local.traces[chn],
                             curve_w, curve_h, devparms->timebaseoffset,
                             devparms->timebasescale, devparms->hordivisions,
                             devparms->chanscale[chn], devparms->chanoffset[chn],
                             devparms->vertdivisions, ds2_zoom,
                             property("ds2_zoom_center").toDouble(),
                             devparms->displaytype != 0);
        continue;
      }

      if(devparms->modelserie == 2)
      {
        // Subpixel rendering and display-only zoom; unchanged raw samples.
        // Both VECT and DOTS work at any window width.
        for(i=0; i<bufsize; i++)
        {
          const double base_x = i * h_step + w_trace_offset_f;
          const double x = (base_x - ds2_center) * ds2_zoom + curve_w / 2.0;
          const double y = (devparms->wavebuf[chn][i] * v_sense)
                         + (curve_h / 2.0) - chan_tmp_y_pixel_offset[chn];
          if(devparms->displaytype)
          {
            painter->drawPoint(QPointF(x, y));
          }
          else if(i + 1 < bufsize)
          {
            const double next_base_x = (i + 1) * h_step + w_trace_offset_f;
            const double next_x = (next_base_x - ds2_center) * ds2_zoom + curve_w / 2.0;
            const double next_y = (devparms->wavebuf[chn][i + 1] * v_sense)
                                + (curve_h / 2.0) - chan_tmp_y_pixel_offset[chn];
            painter->drawLine(QPointF(x, y), QPointF(next_x, next_y));
          }
        }
      }
      else
      {
      for(i=0; i<bufsize; i++)
      {
        if(bufsize < (curve_w / 2))
        {
          painter->drawLine(i * h_step + w_trace_offset,
                            (devparms->wavebuf[chn][i] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn],
                            (i + 1) * h_step + w_trace_offset,
                            (devparms->wavebuf[chn][i] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn]);
          if(i)
          {
            painter->drawLine(i * h_step + w_trace_offset,
                              (devparms->wavebuf[chn][i - 1] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn],
                              i * h_step + w_trace_offset,
                              (devparms->wavebuf[chn][i] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn]);
          }
        }
        else
        {
          if(i < (bufsize - 1))
          {
            if(devparms->displaytype)
            {
              painter->drawPoint(i * h_step + w_trace_offset,
                                 (devparms->wavebuf[chn][i] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn]);
            }
            else
            {
              painter->drawLine(i * h_step + w_trace_offset,
                                (devparms->wavebuf[chn][i] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn],
                                (i + 1) * h_step + w_trace_offset,
                                (devparms->wavebuf[chn][i + 1] * v_sense) + (curve_h / 2) - chan_tmp_y_pixel_offset[chn]);
            }
          }
        }
      }
      }
    }

    painter->setClipping(false);
    if(devparms->modelserie == 2)
    {
      painter->setRenderHint(QPainter::Antialiasing, false);
      if(ds2_zoom > 1.0)
      {
        // Distinguish display zoom from the instrument's actual timebase.
        const double scope_sec_div = devparms->timebasescale;
        painter->fillRect(curve_w - 208, 8, 200, 23, QColor(0, 0, 0, 210));
        painter->setPen(Qt::white);
        painter->drawText(curve_w - 202, 25,
                 QString("Ansicht: %1")
                 .arg(ds2_format_time_per_div(scope_sec_div / ds2_zoom)));
      }
    }
  }

/////////////////////////////////// draw the decoder ///////////////////////////////////////////

  if(devparms->math_decode_display)  draw_decoder(painter, curve_w, curve_h);

/////////////////////////////////// draw the trigger arrows ///////////////////////////////////////////

  if(trig_level_arrow_moving)
  {
    drawArrow(painter, curve_w, trig_level_arrow_pos, 2, QColor(255, 128, 0), 'T');

    painter->setPen(QPen(QBrush(QColor(255, 128, 0), Qt::SolidPattern), tracewidth, Qt::DashDotLine, Qt::SquareCap, Qt::BevelJoin));

    painter->drawLine(1, trig_level_arrow_pos, curve_w - 2, trig_level_arrow_pos);
  }
  else
  {
    if(devparms->triggeredgesource < 4)
    {
      trig_level_arrow_pos = (curve_h / 2) - ((devparms->triggeredgelevel[devparms->triggeredgesource] + devparms->chanoffset[devparms->triggeredgesource]) / ((devparms->chanscale[devparms->triggeredgesource] * devparms->vertdivisions) / curve_h));

      if(trig_level_arrow_pos < 0)
      {
        trig_level_arrow_pos = -1;

        drawArrow(painter, curve_w + 6, trig_level_arrow_pos, 3, QColor(255, 128, 0), 'T');
      }
      else if(trig_level_arrow_pos > curve_h)
        {
          trig_level_arrow_pos = curve_h + 1;

          drawArrow(painter, curve_w + 6, trig_level_arrow_pos, 1, QColor(255, 128, 0), 'T');
        }
        else
        {
          drawArrow(painter, curve_w, trig_level_arrow_pos, 2, QColor(255, 128, 0), 'T');
        }
    }

    if(trig_line_visible)
    {
      painter->setPen(QPen(QBrush(QColor(255, 128, 0), Qt::SolidPattern), tracewidth, Qt::DashDotLine, Qt::SquareCap, Qt::BevelJoin));

      painter->drawLine(1, trig_level_arrow_pos, curve_w - 2, trig_level_arrow_pos);
    }
  }

  if(trig_pos_arrow_moving)
  {
    drawArrow(painter, trig_pos_arrow_pos, 27, 1, QColor(255, 128, 0), 'T');
  }
  else
  {
    if(devparms->timebasedelayenable)
    {
      trig_pos_arrow_pos = (curve_w / 2) - ((devparms->timebasedelayoffset / ((double)devparms->timebasedelayscale * (double)devparms->hordivisions)) * curve_w);
    }
    else
    {
      trig_pos_arrow_pos = (curve_w / 2) - ((devparms->timebaseoffset / ((double)devparms->timebasescale * (double)devparms->hordivisions)) * curve_w);
    }

    if(trig_pos_arrow_pos < 0)
    {
      trig_pos_arrow_pos = -1;

      drawArrow(painter, trig_pos_arrow_pos, 18, 2, QColor(255, 128, 0), 'T');
    }
    else if(trig_pos_arrow_pos > curve_w)
      {
        trig_pos_arrow_pos = curve_w + 1;

        drawArrow(painter, trig_pos_arrow_pos, 18, 0, QColor(255, 128, 0), 'T');
      }
      else
      {
        drawArrow(painter, trig_pos_arrow_pos, 27, 1, QColor(255, 128, 0), 'T');
      }
  }

  // Both measurements feed the shared frame or its detachable dock.
  // The event filter updates dock labels on a timer, never during painting.
  setProperty("ds2_measure_x_text", QString());
  setProperty("ds2_measure_y_text", QString());
  setProperty("ds2_measure_x_inline", QString());
  setProperty("ds2_measure_y_inline", QString());
  setProperty("ds2_measure_overlay_rect", QRect());
  setProperty("ds2_measure_popout_rect", QRect());

  // DS2000: interactive time cursors, drawn above waveform/grid.
  // Store time relative to trigger, never X pixel: zoom and pan do not
  // distort the measured time difference.
  if(ds2_cursor_visible)
  {
    const double sec_div = devparms->timebasescale;
    const double divisions = devparms->hordivisions;
    const double offset = devparms->timebaseoffset;
    const double center_px = property("ds2_zoom_center").toDouble() * curve_w;
    if(!property("ds2_cursor_inited").toBool())
    {
      // Initialize on first activation in the CURRENT visible window.
      const double x_a = (0.35 * curve_w - curve_w / 2.0) / ds2_zoom + center_px;
      const double x_b = (0.65 * curve_w - curve_w / 2.0) / ds2_zoom + center_px;
      setProperty("ds2_cursor_a_time", offset + (x_a / curve_w - 0.5) * divisions * sec_div);
      setProperty("ds2_cursor_b_time", offset + (x_b / curve_w - 0.5) * divisions * sec_div);
      setProperty("ds2_cursor_inited", true);
    }

    const double ta = property("ds2_cursor_a_time").toDouble();
    const double tb = property("ds2_cursor_b_time").toDouble();
    const double xa_base = curve_w * (0.5 + (ta - offset) / (divisions * sec_div));
    const double xb_base = curve_w * (0.5 + (tb - offset) / (divisions * sec_div));
    const double xa = (xa_base - center_px) * ds2_zoom + curve_w / 2.0;
    const double xb = (xb_base - center_px) * ds2_zoom + curve_w / 2.0;
    const QColor color_a(105, 245, 155);
    const QColor color_b(255, 185, 70);

    painter->save();
    painter->setClipRect(QRect(0, 0, curve_w, curve_h));
    painter->setRenderHint(QPainter::Antialiasing, true);
    if(xa >= 0 && xa < curve_w)
    {
      painter->setPen(QPen(color_a, 1.3, Qt::DashLine));
      painter->drawLine(QPointF(xa, 0), QPointF(xa, curve_h - 1));
      painter->setPen(QPen(Qt::black));
      painter->fillRect(QRectF(xa - 11, 35, 22, 20), color_a);
      painter->drawText(QRectF(xa - 11, 35, 22, 20), Qt::AlignCenter, "A");
    }
    if(xb >= 0 && xb < curve_w)
    {
      painter->setPen(QPen(color_b, 1.3, Qt::DashLine));
      painter->drawLine(QPointF(xb, 0), QPointF(xb, curve_h - 1));
      painter->setPen(QPen(Qt::black));
      painter->fillRect(QRectF(xb - 11, 35, 22, 20), color_b);
      painter->drawText(QRectF(xb - 11, 35, 22, 20), Qt::AlignCenter, "B");
    }
    painter->restore();

    const QString values = QString::fromUtf8("A: %1   B: %2   \xCE\x94t: %3   1/|\xCE\x94t|: %4")
            .arg(ds2_format_seconds(ta),
                 ds2_format_seconds(tb),
                 ds2_format_seconds(tb - ta),
                 ds2_format_frequency(std::fabs(tb - ta)));
    const QString compact = QString::fromUtf8("\xCE\x94t: %1   1/|\xCE\x94t|: %2")
            .arg(ds2_format_seconds(tb - ta),
                 ds2_format_frequency(std::fabs(tb - ta)));
    setProperty("ds2_measure_x_text", values);
    const QFontMetrics ds2_fm(painter->font());
    setProperty("ds2_measure_x_inline",
                ds2_fm.horizontalAdvance(values) + 38 <= curve_w ? values : compact);
  }

  // DS2000 Y cursors: independently draggable absolute voltage levels.
  if(ds2_y_cursor_visible)
  {
    const double scale = devparms->chanscale[ds2_y_chn];
    const double offset = devparms->chanoffset[ds2_y_chn];
    const double divisions = devparms->vertdivisions;
    if(!property("ds2_y_cursor_inited").toBool())
    {
      // On first use, place A/B inside the visible raster.
      setProperty("ds2_y_cursor_a_volt", (0.5 - 0.35) * scale * divisions - offset);
      setProperty("ds2_y_cursor_b_volt", (0.5 - 0.65) * scale * divisions - offset);
      setProperty("ds2_y_cursor_inited", true);
    }
    const double va = property("ds2_y_cursor_a_volt").toDouble();
    const double vb = property("ds2_y_cursor_b_volt").toDouble();
    const double ya = curve_h * (0.5 - (va + offset) / (scale * divisions));
    const double yb = curve_h * (0.5 - (vb + offset) / (scale * divisions));
    const QColor color_a(90, 205, 255);  // blue-cyan to distinguish from time A
    const QColor color_b(255, 110, 205); // magenta to distinguish from time B

    painter->save();
    painter->setClipRect(QRect(0, 0, curve_w, curve_h));
    painter->setRenderHint(QPainter::Antialiasing, true);
    if(ya >= 0.0 && ya < curve_h)
    {
      painter->setPen(QPen(color_a, 1.3, Qt::DashLine));
      painter->drawLine(QPointF(0, ya), QPointF(curve_w - 1, ya));
      painter->fillRect(QRectF(14, ya - 10, 30, 20), color_a);
      painter->setPen(Qt::black);
      painter->drawText(QRectF(14, ya - 10, 30, 20), Qt::AlignCenter, "YA");
    }
    if(yb >= 0.0 && yb < curve_h)
    {
      painter->setPen(QPen(color_b, 1.3, Qt::DashLine));
      painter->drawLine(QPointF(0, yb), QPointF(curve_w - 1, yb));
      painter->fillRect(QRectF(14, yb - 10, 30, 20), color_b);
      painter->setPen(Qt::black);
      painter->drawText(QRectF(14, yb - 10, 30, 20), Qt::AlignCenter, "YB");
    }
    painter->restore();

    const QString values = QString("CH%1:  ").arg(ds2_y_chn + 1) +
      QString::fromUtf8("YA: %1   YB: %2   \xCE\x94V: %3")
      .arg(ds2_format_volts(va), ds2_format_volts(vb), ds2_format_volts(vb - va));
    const QString compact = QString("CH%1:  ").arg(ds2_y_chn + 1) +
      QString::fromUtf8("\xCE\x94V: %1").arg(ds2_format_volts(vb - va));
    setProperty("ds2_measure_y_text", values);
    const QFontMetrics ds2_fm(painter->font());
    setProperty("ds2_measure_y_inline",
                ds2_fm.horizontalAdvance(values) + 38 <= curve_w ? values : compact);
  }

  // V1.9d: target square and Ctrl-snap feedback ONLY for the RAW snapshot.
  // The small square is drawn while dragging; the ring confirms Ctrl-snap.
  if(ds2_snapshot_mode && !ds2_raw_state_local.busy &&
     (ds2_cursor_visible || ds2_y_cursor_visible) &&
     property("ds2_target_visible").toBool())
  {
    const double tx = property("ds2_target_x").toDouble();
    const double ty = property("ds2_target_y").toDouble();
    if(tx >= 0 && tx < curve_w && ty >= 0 && ty < curve_h)
    {
      painter->save();
      painter->setClipRect(QRect(0, 0, curve_w, curve_h));
      painter->setRenderHint(QPainter::Antialiasing, true);
      painter->setPen(QPen(Qt::black, 3.0));
      painter->setBrush(Qt::NoBrush);
      painter->drawRect(QRectF(tx - 4, ty - 4, 8, 8));
      painter->setPen(QPen(Qt::white, 1.5));
      painter->drawRect(QRectF(tx - 4, ty - 4, 8, 8));
      if(property("ds2_target_snap").toBool())
      {
        painter->setPen(QPen(QColor(80, 240, 150), 1.5));
        painter->drawEllipse(QPointF(tx, ty), 11.0, 11.0);
      }
      painter->restore();
    }
  }

  // DS2000: one framed, movable on-screen value panel.
  // Klick auf [>>] docks into a native QDockWidget (with float/re-dock).
  if(devparms->modelserie == 2 && !property("ds2_measure_in_dock").toBool() &&
     (ds2_cursor_visible || ds2_y_cursor_visible))
  {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    const QString x_text = property("ds2_measure_x_inline").toString();
    const QString y_text = property("ds2_measure_y_inline").toString();
    const QFontMetrics fm(painter->font());
    const int row_height = fm.height() + 7;
    const int rows = int(!x_text.isEmpty()) + int(!y_text.isEmpty());
    const int box_h = 25 + rows * row_height + 5;
    const int text_w = std::max(fm.horizontalAdvance(x_text), fm.horizontalAdvance(y_text));
    const int box_w = std::min(curve_w - 16, std::max(200, text_w + 24));
    QPoint pos = property("ds2_measure_overlay_pos").toPoint();
    pos.setX(std::max(0, std::min(curve_w - box_w, pos.x())));
    pos.setY(std::max(0, std::min(curve_h - box_h, pos.y())));
    setProperty("ds2_measure_overlay_pos", pos);
    const QRect box(pos, QSize(box_w, box_h));
    const QRect popout(box.right() - 27, box.top() + 3, 23, 19);

    painter->fillRect(box, QColor(10, 17, 23, 232));
    painter->setPen(QPen(QColor(115, 166, 196), 1));
    painter->drawRect(box.adjusted(0, 0, -1, -1));
    painter->fillRect(QRect(box.x() + 1, box.y() + 1, box.width() - 2, 23),
                      QColor(35, 53, 66, 238));
    painter->setPen(QColor(220, 231, 240));
    painter->drawText(QRect(box.x() + 8, box.y() + 3, box.width() - 50, 19),
                      Qt::AlignVCenter | Qt::AlignLeft,
                      QString::fromUtf8("Messwerte  (ziehen)"));
    painter->setPen(QPen(QColor(145, 191, 218), 1));
    painter->drawRect(popout);
    painter->setPen(Qt::white);
    painter->drawText(popout, Qt::AlignCenter, QString::fromUtf8("↗"));
    int row_y = box.y() + 26;
    if(!x_text.isEmpty())
    {
      painter->setPen(QColor(175, 255, 203));
      painter->drawText(QRect(box.x() + 10, row_y, box.width() - 18, row_height),
                        Qt::AlignVCenter | Qt::AlignLeft, x_text);
      row_y += row_height;
    }
    if(!y_text.isEmpty())
    {
      painter->setPen(QColor(137, 218, 255));
      painter->drawText(QRect(box.x() + 10, row_y, box.width() - 18, row_height),
                        Qt::AlignVCenter | Qt::AlignLeft, y_text);
    }
    setProperty("ds2_measure_overlay_rect", box.translated(bordersize, bordersize));
    setProperty("ds2_measure_popout_rect", popout.translated(bordersize, bordersize));
    painter->restore();
  }

  if(devparms->countersrc)
  {
    paintCounterLabel(painter, curve_w - 180, 6);
  }

  if(devparms->func_wrec_enable)
  {
    paintPlaybackLabel(painter, curve_w - 180, 40);
  }

  if((mainwindow->adjDialFunc == ADJ_DIAL_FUNC_HOLDOFF) || (mainwindow->navDialFunc == NAV_DIAL_FUNC_HOLDOFF))
  {
    convert_to_metric_suffix(str, devparms->triggerholdoff, 2, 1024);

    strlcat(str, "S", 1024);

    paintLabel(painter, curve_w - 110, 5, 100, 20, str, Qt::white);
  }
  else if(mainwindow->adjDialFunc == ADJ_DIAL_FUNC_ACQ_AVG)
    {
      snprintf(str, 1024, "%i", devparms->acquireaverages);

      paintLabel(painter, curve_w - 110, 5, 100, 20, str, Qt::white);
    }

  if(label_active == LABEL_ACTIVE_TRIG &&
     devparms->triggeredgesource >= 0 &&
     devparms->triggeredgesource < MAX_CHNS &&
     devparms->triggeredgesource < devparms->channel_cnt)
  {
    convert_to_metric_suffix(str, devparms->triggeredgelevel[devparms->triggeredgesource], 2, 1024);

    strlcat(str, devparms->chanunitstr[devparms->chanunit[devparms->triggeredgesource]], 1024);

    paintLabel(painter, curve_w - 120, curve_h - 50, 100, 20, str, QColor(255, 128, 0));
  }
  else if(label_active == LABEL_ACTIVE_CHAN1)
    {
      convert_to_metric_suffix(str, devparms->chanoffset[0], 2, 1024);

      strlcat(str, devparms->chanunitstr[devparms->chanunit[0]], 1024);

      paintLabel(painter, 20, curve_h - 50, 100, 20, str, SignalColor[0]);
    }
    else if(label_active == LABEL_ACTIVE_CHAN2)
      {
        convert_to_metric_suffix(str, devparms->chanoffset[1], 2, 1024);

        strlcat(str, devparms->chanunitstr[devparms->chanunit[1]], 1024);

        paintLabel(painter, 20, curve_h - 50, 100, 20, str, SignalColor[1]);
      }
      else if(label_active == LABEL_ACTIVE_CHAN3)
        {
          convert_to_metric_suffix(str, devparms->chanoffset[2], 2, 1024);

          strlcat(str, devparms->chanunitstr[devparms->chanunit[2]], 1024);

          paintLabel(painter, 20, curve_h - 50, 100, 20, str, SignalColor[2]);
        }
        else if(label_active == LABEL_ACTIVE_CHAN4)
          {
            convert_to_metric_suffix(str, devparms->chanoffset[3], 2, 1024);

            strlcat(str, devparms->chanunitstr[devparms->chanunit[3]], 1024);

            paintLabel(painter, 20, curve_h - 50, 100, 20, str, SignalColor[3]);
          }

  if((devparms->math_fft == 1) && (devparms->math_fft_split == 1))
  {
    drawFFT(painter, curve_h_backup, curve_w_backup);

    if(label_active == LABEL_ACTIVE_FFT)
    {
      if(devparms->math_fft_unit == 0)
      {
        strlcpy(str, "POS: ", 1024);

        convert_to_metric_suffix(str + strlen(str), devparms->fft_voffset, 1, 1024);

        strlcat(str, "V", 1024);
      }
      else
      {
        snprintf(str, 1024, "POS: %.1fdB", devparms->fft_voffset);
      }

      paintLabel(painter, 20, curve_h * 1.85 - 50.0, 100, 20, str, QColor(128, 64, 255));
    }
  }

//   clk_end = clock();
//
//   cpu_time_used += ((double) (clk_end - clk_start)) / CLOCKS_PER_SEC;
//
//   scr_update_cntr++;
//
//   if(!(scr_update_cntr % 50))
//   {
//     printf("CPU time used: %f\n", cpu_time_used / 50);
//
//     cpu_time_used = 0;
//   }
}


void SignalCurve::drawFFT(QPainter *painter, int curve_h_b, int curve_w_b)
{
  int i, small_rulers, curve_w, curve_h;

  char str[1024];

  double h_step=0.0,
         step,
         step2,
         fft_h_offset=0.0;

  small_rulers = 5 * devparms->hordivisions;

/////////////////////////////////// FFT: translate coordinates, draw and fill a rectangle ///////////////////////////////////////////

  if(devparms->math_fft_split == 0)
  {
    curve_w = curve_w_b - (bordersize * 2);

    curve_h = curve_h_b - (bordersize * 2);
  }
  else
  {
    curve_h = curve_h_b - (bordersize * 2);

    curve_h /= 3;

    painter->resetTransform();

    painter->translate(bordersize, bordersize + curve_h + 15);

    curve_w = curve_w_b - (bordersize * 2);

    curve_h = curve_h_b - (bordersize * 2);

    curve_h *= 0.64;

/////////////////////////////////// FFT: draw the rasters ///////////////////////////////////////////

    painter->setPen(RasterColor);

    painter->drawRect (0, 0, curve_w - 1, curve_h - 1);

    if(devparms->displaygrid)
    {
      painter->setPen(QPen(QBrush(RasterColor, Qt::SolidPattern), tracewidth, Qt::DotLine, Qt::SquareCap, Qt::BevelJoin));

      if(devparms->displaygrid == 2)
      {
        step = (double)curve_w / (double)devparms->hordivisions;

        for(i=1; i<devparms->hordivisions; i++)
        {
          painter->drawLine(step * i, curve_h - 1, step * i, 0);
        }

        step = curve_h / 8.0;

        for(i=1; i<8; i++)
        {
          painter->drawLine(0, step * i, curve_w - 1, step * i);
        }
      }
      else
      {
        painter->drawLine(curve_w / 2, curve_h - 1, curve_w / 2, 0);

        painter->drawLine(0, curve_h / 2, curve_w - 1, curve_h / 2);
      }
    }

    painter->setPen(RasterColor);

    step = (double)curve_w / (double)small_rulers;

    for(i=1; i<small_rulers; i++)
    {
      step2 = step * i;

      if(devparms->displaygrid)
      {
        painter->drawLine(step2, curve_h / 2 + 2, step2, curve_h / 2 - 2);
      }

      if(i % 5)
      {
        painter->drawLine(step2, curve_h - 1, step2, curve_h - 5);

        painter->drawLine(step2, 0, step2, 4);
      }
      else
      {
        painter->drawLine(step2, curve_h - 1, step2, curve_h - 9);

        painter->drawLine(step2, 0, step2, 8);
      }
    }

    step = curve_h / 40.0;

    for(i=1; i<40; i++)
    {
      step2 = step * i;

      if(devparms->displaygrid)
      {
        painter->drawLine(curve_w / 2 + 2, step2, curve_w / 2  - 2, step2);
      }

      if(i % 5)
      {
        painter->drawLine(curve_w - 1, step2, curve_w - 5, step2);

        painter->drawLine(0, step2, 4, step2);
      }
      else
      {
        painter->drawLine(curve_w - 1, step2, curve_w - 9, step2);

        painter->drawLine(0, step2, 8, step2);
      }
    }

/////////////////////////////////// FFT: draw the arrow ///////////////////////////////////////////

    fft_arrow_pos = (curve_h / 2.0) - (((double)curve_h / (8.0 * devparms->fft_vscale)) * devparms->fft_voffset);

    drawArrow(painter, 0, fft_arrow_pos, 0, QColor(128, 64, 255), 'M');
  }

/////////////////////////////////// FFT: draw the curve ///////////////////////////////////////////

  if(bufsize < 32)
  {
    return;
  }

  if((devparms->fftbufsz > 32) && devparms->chandisplay[devparms->math_fft_src])
  {
    painter->setClipping(true);
    painter->setClipRegion(QRegion(0, 0, curve_w, curve_h), Qt::ReplaceClip);

    h_step = (double)curve_w / (double)devparms->fftbufsz;

    if(devparms->timebasedelayenable)
    {
      h_step *= (100.0 / devparms->timebasedelayscale) / devparms->math_fft_hscale;
    }
    else
    {
      h_step *= (100.0 / devparms->timebasescale) / devparms->math_fft_hscale;
    }

    if(devparms->modelserie != 1)
    {
      h_step /= 28.0;
    }
    else
    {
      h_step /= 24.0;
    }

    fft_v_sense = (double)curve_h / (-8.0 * devparms->fft_vscale);

    fft_v_offset = (curve_h / 2.0) + (fft_v_sense * devparms->fft_voffset);

    fft_h_offset = (curve_w / 2) - ((devparms->math_fft_hcenter / devparms->math_fft_hscale) * curve_w / devparms->hordivisions);

//     fft_smpls_onscreen = (double)devparms->fftbufsz * ((devparms->math_fft_hscale * devparms->hordivisions) / (double)devparms->current_screen_sf);

    painter->setPen(QPen(QBrush(QColor(128, 64, 255), Qt::SolidPattern), tracewidth, Qt::SolidLine, Qt::SquareCap, Qt::BevelJoin));

    for(i=0; i<(devparms->fftbufsz - 1); i++)
    {
//       if(fft_smpls_onscreen < (curve_w / 2))
//       {
//         painter->drawLine(i * h_step + fft_h_offset, (devparms->fftbuf_out[i] * fft_v_sense) + fft_v_offset, (i + 1) * h_step + fft_h_offset, (devparms->fftbuf_out[i] * fft_v_sense) + fft_v_offset);
//         if(i)
//         {
//           painter->drawLine(i * h_step + fft_h_offset, (devparms->fftbuf_out[i - 1] * fft_v_sense) + fft_v_offset, i * h_step + fft_h_offset, (devparms->fftbuf_out[i] * fft_v_sense) + fft_v_offset);
//         }
//       }
//       else
//       {
//         if(i < (devparms->fftbufsz - 1))
//         {
          painter->drawLine(i * h_step + fft_h_offset, (devparms->fftbuf_out[i] * fft_v_sense) + fft_v_offset, (i + 1) * h_step + fft_h_offset, (devparms->fftbuf_out[i + 1] * fft_v_sense) + fft_v_offset);
//          }
//       }
    }

    snprintf(str, 1024, "FFT:  CH%i  ", devparms->math_fft_src + 1);

    convert_to_metric_suffix(str + strlen(str), devparms->fft_vscale, 2, 1024 - strlen(str));

    if(devparms->math_fft_unit == 0)
    {
      strlcat(str, "V/Div   Center ", 1024);
    }
    else
    {
      strlcat(str, "dBV/Div   Center ", 1024);
    }

    convert_to_metric_suffix(str + strlen(str), devparms->math_fft_hcenter, 1, 1024 - strlen(str));

    strlcat(str, "Hz   ", 1024);

    convert_to_metric_suffix(str + strlen(str), devparms->math_fft_hscale, 2, 1024 - strlen(str));

    strlcat(str, "Hz/Div   ", 1024);

    if(devparms->timebasedelayenable)
    {
      convert_to_metric_suffix(str + strlen(str), 100.0 / devparms->timebasedelayscale, 0, 1024 - strlen(str));
    }
    else
    {
      convert_to_metric_suffix(str + strlen(str), 100.0 / devparms->timebasescale, 0, 1024 - strlen(str));
    }

    strlcat(str, "Sa/s", 1024);

    painter->drawText(15, 30, str);

    painter->setClipping(false);
  }
  else
  {
    painter->setPen(QPen(QBrush(QColor(128, 64, 255), Qt::SolidPattern), tracewidth, Qt::SolidLine, Qt::SquareCap, Qt::BevelJoin));

    snprintf(str, 1024, "FFT: CH%i Data Invalid!", devparms->math_fft_src + 1);

    painter->drawText(15, 30, str);
  }
}


void SignalCurve::drawCurve(struct device_settings *devp, struct tmcdev *dev)
{
  devparms = devp;
  setProperty("ds2_zoom_enabled", devparms && devparms->modelserie == 2);

  device = dev;

  bufsize = devparms->wavebufsz;

  update();
}


void SignalCurve::setDeviceParameters(struct device_settings *devp)
{
  devparms = devp;
  setProperty("ds2_zoom_enabled", devparms && devparms->modelserie == 2);
}


void SignalCurve::drawTopLabels(QPainter *painter)
{
  int i, x1, tmp;

  char str[512];

  double dtmp1, dtmp2;

  QPainterPath path;

  if(devparms == NULL)
  {
    return;
  }

  path.addRoundedRect(5, 5, 70, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::white);

  painter->drawText(5, 5, 70, 20, Qt::AlignCenter, devparms->modelname);

//////////////// triggerstatus ///////////////////////////////

  path = QPainterPath();

  path.addRoundedRect(80, 5, 35, 20, 3, 3);

  if((devparms->triggerstatus == 1) || (devparms->triggerstatus == 3))
  {
    if(!trig_stat_flash)
    {
      trig_stat_flash = 1;

      trig_stat_timer->start(1000);
    }
  }
  else
  {
    if(trig_stat_flash)
    {
      trig_stat_flash = 0;

      trig_stat_timer->stop();
    }
  }

  if(trig_stat_flash == 2)
  {
    painter->fillPath(path, Qt::green);

    painter->setPen(Qt::black);
  }
  else
  {
    painter->fillPath(path, Qt::black);

    painter->setPen(Qt::green);
  }

  switch(devparms->triggerstatus)
  {
    case 0 : painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "T'D");
             break;
    case 1 : painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "WAIT");
             break;
    case 2 : painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "RUN");
             break;
    case 3 : painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "AUTO");
             break;
    case 4 : painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "FIN");
             break;
    case 5 : painter->setPen(Qt::red);
             painter->drawText(80, 5, 35, 20, Qt::AlignCenter, "STOP");
             break;
  }

//////////////// horizontal ///////////////////////////////

  path = QPainterPath();

  path.addRoundedRect(140, 5, 70, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::white);

  painter->drawText(125, 20, "H");

  if(devparms->timebasedelayenable)
  {
    convert_to_metric_suffix(str, devparms->timebasedelayscale, 1, 512);
  }
  else
  {
    convert_to_metric_suffix(str, devparms->timebasescale, 1, 512);
  }

  remove_trailing_zeros(str);

  strlcat(str, "s", 512);

  painter->drawText(140, 5, 70, 20, Qt::AlignCenter, str);

//////////////// samplerate ///////////////////////////////

  painter->setPen(Qt::gray);

  convert_to_metric_suffix(str, devparms->samplerate, 0, 512);

  strlcat(str, "Sa/s", 512);

  painter->drawText(200, -1, 85, 20, Qt::AlignCenter, str);

  if(devparms->acquirememdepth)
  {
    convert_to_metric_suffix(str, devparms->acquirememdepth, 1, 512);

    remove_trailing_zeros(str);

    strlcat(str, "pts", 512);

    painter->drawText(200, 14, 85, 20, Qt::AlignCenter, str);
  }
  else
  {
    painter->drawText(200, 14, 85, 20, Qt::AlignCenter, "AUTO");
  }

//////////////// memory position ///////////////////////////////

  path = QPainterPath();

  path.addRoundedRect(285, 5, 240, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::gray);

  if(devparms->timebasedelayenable)
  {
    dtmp1 = devparms->timebasedelayscale / devparms->timebasescale;

    dtmp2 = (devparms->timebaseoffset - devparms->timebasedelayoffset) / ((devparms->hordivisions / 2) * devparms->timebasescale);

    tmp = (116 - (dtmp1 * 116)) - (dtmp2 * 116);

    if(tmp > 0)
    {
      painter->fillRect(288, 16, tmp, 8, QColor(64, 160, 255));
    }

    x1 = (116 - (dtmp1 * 116)) + (dtmp2 * 116);

    if(x1 > 0)
    {
      painter->fillRect(288 + 233 - x1, 16, x1, 8, QColor(64, 160, 255));
    }
  }
  else if(devparms->acquirememdepth > 1000)
  {
    dtmp1 = (devparms->hordivisions * devparms->timebasescale) / (devparms->acquirememdepth / devparms->samplerate);

    dtmp2 = devparms->timebaseoffset / dtmp1;

    tmp = (116 - (dtmp1 * 116)) - (dtmp2 * 116);

    if(tmp > 0)
    {
      painter->fillRect(288, 16, tmp, 8, QColor(64, 160, 255));
    }

    x1 = (116 - (dtmp1 * 116)) + (dtmp2 * 116);

    if(x1 > 0)
    {
      painter->fillRect(288 + 233 - x1, 16, x1, 8, QColor(64, 160, 255));
    }
  }

  painter->drawRect(288, 16, 233, 8);

  painter->setPen(Qt::white);

  painter->drawLine(289, 20, 291, 22);

  for(i=0; i<19; i++)
  {
    painter->drawLine((i * 12) + 291, 22, (i * 12) + 293, 22);

    painter->drawLine((i * 12) + 297, 18, (i * 12) + 299, 18);

    painter->drawLine((i * 12) + 294, 21, (i * 12) + 296, 19);

    painter->drawLine((i * 12) + 300, 19, (i * 12) + 302, 21);
  }

  painter->drawLine(519, 22, 520, 22);

//////////////// delay ///////////////////////////////

  path = QPainterPath();

  path.addRoundedRect(570, 5, 85, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(QColor(255, 128, 0));

  painter->drawText(555, 20, "D");

  if(devparms->timebasedelayenable)
  {
    convert_to_metric_suffix(str, devparms->timebasedelayoffset, 4, 512);
  }
  else
  {
    convert_to_metric_suffix(str, devparms->timebaseoffset, 4, 512);
  }

  strlcat(str, "s", 512);

  painter->drawText(570, 5, 85, 20, Qt::AlignCenter, str);

//////////////// trigger ///////////////////////////////

  path = QPainterPath();

  path.addRoundedRect(685, 5, 125, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::gray);

  painter->drawText(670, 20, "T");

  if(devparms->triggeredgesource >= 0 &&
     devparms->triggeredgesource < MAX_CHNS &&
     devparms->triggeredgesource < devparms->channel_cnt)
  {
    convert_to_metric_suffix(str, devparms->triggeredgelevel[devparms->triggeredgesource], 2, 512);
    strlcat(str, devparms->chanunitstr[devparms->chanunit[devparms->triggeredgesource]], 512);
  }
  else
  {
    // EXT/EXT5/AC are trigger sources, not indices into chanunit[].
    strlcpy(str, devparms->triggeredgesource == 6 ? "AC" : "EXT", 512);
  }

  if(devparms->triggeredgesource >= 0 && devparms->triggeredgesource < 4)
  {
    painter->setPen(SignalColor[devparms->triggeredgesource]);
  }
  else
  {
    switch(devparms->triggeredgesource)
    {
      case 4:
      case 5: painter->setPen(Qt::green);
              break;
      case 6: painter->setPen(QColor(255, 64, 0));
              break;
    }
  }

  if(devparms->triggeredgesource != 6)
  {
    painter->drawText(735, 5, 85, 20, Qt::AlignCenter, str);
  }

  path = QPainterPath();

  path.addRoundedRect(725, 7, 15, 15, 3, 3);

  if(devparms->triggeredgesource < 4)
  {
    painter->fillPath(path, SignalColor[devparms->triggeredgesource]);

    snprintf(str, 512, "%i", devparms->triggeredgesource + 1);
  }
  else
  {
    switch(devparms->triggeredgesource)
    {
      case 4:
      case 5: painter->fillPath(path, Qt::green);
              strlcpy(str, "E", 512);
              break;
      case 6: painter->fillPath(path, QColor(255, 64, 0));
              strlcpy(str, "AC", 512);
              break;
    }
  }

  if(devparms->triggeredgeslope == 0)
  {
    painter->drawLine(705, 8, 710, 8);
    painter->drawLine(705, 8, 705, 20);
    painter->drawLine(700, 20, 705, 20);
    painter->drawLine(701, 15, 705, 11);
    painter->drawLine(709, 15, 705, 11);
  }

  if(devparms->triggeredgeslope == 1)
  {
    painter->drawLine(700, 8, 705, 8);
    painter->drawLine(705, 8, 705, 20);
    painter->drawLine(705, 20, 710, 20);
    painter->drawLine(701, 12, 705, 16);
    painter->drawLine(709, 12, 705, 16);
  }

  if(devparms->triggeredgeslope == 2)
  {
    painter->drawLine(702, 8, 702, 18);
    painter->drawLine(700, 10, 702, 8);
    painter->drawLine(704, 10, 702, 8);
    painter->drawLine(708, 8, 708, 18);
    painter->drawLine(706, 16, 708, 18);
    painter->drawLine(710, 16, 708, 18);
  }

  painter->setPen(Qt::black);

  painter->drawText(725, 8, 15, 15, Qt::AlignCenter, str);
}


void SignalCurve::drawfpsLabel(QPainter *painter, int xpos, int ypos)
{
  char str[512];

  static struct timespec tp1, tp2;

  painter->setPen(Qt::red);

  clock_gettime(CLOCK_REALTIME, &tp1);

  if(tp1.tv_nsec >= tp2.tv_nsec)
  {
    snprintf(str, 512, "%04.1f fps", 1.0 / ((tp1.tv_sec - tp2.tv_sec) + ((tp1.tv_nsec - tp2.tv_nsec) / 1e9)));
  }
  else
  {
    snprintf(str, 512, "%04.1f fps", 1.0 / ((tp1.tv_sec - tp2.tv_sec - 1) + ((tp1.tv_nsec - tp2.tv_nsec + 1000000000) / 1e9)));
  }

  painter->drawText(xpos, ypos, str);

  tp2 = tp1;
}


void SignalCurve::drawChanLabel(QPainter *painter, int xpos, int ypos, int chn)
{
  QPainterPath path;

  char str1[4],
       str2[512];

  if(devparms == NULL)
  {
    return;
  }

  str1[0] = '1' + chn;
  str1[1] = 0;

  convert_to_metric_suffix(str2, devparms->chanscale[chn], 2, 512);

  strlcat(str2, devparms->chanunitstr[devparms->chanunit[chn]], 512);

  if(devparms->chanbwlimit[chn])
  {
    strlcat(str2, " B", 512);
  }

  if(devparms->chandisplay[chn])
  {
    if(chn == devparms->activechannel)
    {
      path.addRoundedRect(xpos, ypos, 20, 20, 3, 3);

      painter->fillPath(path, SignalColor[chn]);

      painter->setPen(Qt::black);

      painter->drawText(xpos + 6, ypos + 15, str1);

      if(devparms->chaninvert[chn])
      {
        painter->drawLine(xpos + 6, ypos + 3, xpos + 14, ypos + 3);
      }

      path = QPainterPath();

      path.addRoundedRect(xpos + 25, ypos, 90, 20, 3, 3);

      painter->fillPath(path, Qt::black);

      painter->setPen(SignalColor[chn]);

      painter->drawRoundedRect(xpos + 25, ypos, 90, 20, 3, 3);

      painter->drawText(xpos + 35, ypos + 1, 90, 20, Qt::AlignCenter, str2);

      if(devparms->chancoupling[chn] == 0)
      {
        painter->drawLine(xpos + 33, ypos + 6, xpos + 33, ypos + 10);

        painter->drawLine(xpos + 28, ypos + 10, xpos + 38, ypos + 10);

        painter->drawLine(xpos + 30, ypos + 12, xpos + 36, ypos + 12);

        painter->drawLine(xpos + 32, ypos + 14, xpos + 34, ypos + 14);
      }
      else if(devparms->chancoupling[chn] == 1)
        {
          painter->drawLine(xpos + 28, ypos + 8, xpos + 38, ypos + 8);

          painter->drawLine(xpos + 28, ypos + 12, xpos + 30, ypos + 12);

          painter->drawLine(xpos + 32, ypos + 12, xpos + 34, ypos + 12);

          painter->drawLine(xpos + 36, ypos + 12, xpos + 38, ypos + 12);
        }
        else if(devparms->chancoupling[chn] == 2)
          {
            painter->drawArc(xpos + 30, ypos + 8, 5, 5, 10 * 16, 160 * 16);

            painter->drawArc(xpos + 35, ypos + 8, 5, 5, -10 * 16, -160 * 16);
          }
    }
    else
    {
      path.addRoundedRect(xpos, ypos, 20, 20, 3, 3);

      path.addRoundedRect(xpos + 25, ypos, 85, 20, 3, 3);

      painter->fillPath(path, Qt::black);

      painter->setPen(SignalColor[chn]);

      painter->drawText(xpos + 6, ypos + 15, str1);

      if(devparms->chaninvert[chn])
      {
        painter->drawLine(xpos + 6, ypos + 3, xpos + 14, ypos + 3);
      }

      painter->drawText(xpos + 35, ypos + 1, 90, 20, Qt::AlignCenter, str2);

      if(devparms->chancoupling[chn] == 0)
      {
        painter->drawLine(xpos + 33, ypos + 6, xpos + 33, ypos + 10);

        painter->drawLine(xpos + 28, ypos + 10, xpos + 38, ypos + 10);

        painter->drawLine(xpos + 30, ypos + 12, xpos + 36, ypos + 12);

        painter->drawLine(xpos + 32, ypos + 14, xpos + 34, ypos + 14);
      }
      else if(devparms->chancoupling[chn] == 1)
        {
          painter->drawLine(xpos + 28, ypos + 8, xpos + 38, ypos + 8);

          painter->drawLine(xpos + 28, ypos + 12, xpos + 30, ypos + 12);

          painter->drawLine(xpos + 32, ypos + 12, xpos + 34, ypos + 12);

          painter->drawLine(xpos + 36, ypos + 12, xpos + 38, ypos + 12);
        }
        else if(devparms->chancoupling[chn] == 2)
          {
            painter->drawArc(xpos + 30, ypos + 8, 5, 5, 10 * 16, 160 * 16);

            painter->drawArc(xpos + 35, ypos + 8, 5, 5, -10 * 16, -160 * 16);
          }
    }
  }
  else
  {
    path.addRoundedRect(xpos, ypos, 20, 20, 3, 3);

    path.addRoundedRect(xpos + 25, ypos, 85, 20, 3, 3);

    painter->fillPath(path, Qt::black);

    painter->setPen(QColor(48, 48, 48));

    painter->drawText(xpos + 6, ypos + 15, str1);

    painter->drawText(xpos + 30, ypos + 1, 85, 20, Qt::AlignCenter, str2);

    if(devparms->chanbwlimit[chn])
    {
      painter->drawText(xpos + 90, ypos + 1, 20, 20, Qt::AlignCenter, "B");
    }

    if(devparms->chancoupling[chn] == 0)
    {
      painter->drawLine(xpos + 33, ypos + 6, xpos + 33, ypos + 10);

      painter->drawLine(xpos + 28, ypos + 10, xpos + 38, ypos + 10);

      painter->drawLine(xpos + 30, ypos + 12, xpos + 36, ypos + 12);

      painter->drawLine(xpos + 32, ypos + 14, xpos + 34, ypos + 14);
    }
    else if(devparms->chancoupling[chn] == 1)
      {
        painter->drawLine(xpos + 28, ypos + 8, xpos + 38, ypos + 8);

        painter->drawLine(xpos + 28, ypos + 12, xpos + 30, ypos + 12);

        painter->drawLine(xpos + 32, ypos + 12, xpos + 34, ypos + 12);

        painter->drawLine(xpos + 36, ypos + 12, xpos + 38, ypos + 12);
      }
      else if(devparms->chancoupling[chn] == 2)
        {
          painter->drawArc(xpos + 30, ypos + 8, 5, 5, 10 * 16, 160 * 16);

          painter->drawArc(xpos + 35, ypos + 8, 5, 5, -10 * 16, -160 * 16);
        }
  }
}


void SignalCurve::drawArrow(QPainter *painter, int xpos, int ypos, int rot, QColor color, char ch)
{
  QPainterPath path;

  char str[4];

  str[0] = ch;
  str[1] = 0;

  if(rot == 0)
  {
    path.moveTo(xpos - 20, ypos + 6);
    path.lineTo(xpos - 7,  ypos + 6);
    path.lineTo(xpos,     ypos);
    path.lineTo(xpos - 7,  ypos - 6);
    path.lineTo(xpos - 20, ypos - 6);
    path.lineTo(xpos - 20, ypos + 6);
    painter->fillPath(path, color);

    painter->setPen(Qt::black);

    painter->drawText(xpos - 17, ypos + 4, str);
  }
  else if(rot == 1)
    {
      path.moveTo(xpos + 6, ypos - 20);
      path.lineTo(xpos + 6,  ypos - 7);
      path.lineTo(xpos,     ypos);
      path.lineTo(xpos - 6,  ypos - 7);
      path.lineTo(xpos - 6, ypos - 20);
      path.lineTo(xpos + 6, ypos - 20);
      painter->fillPath(path, color);

      painter->setPen(Qt::black);

      painter->drawText(xpos - 3, ypos - 7, str);
    }
    else if(rot == 2)
      {
        path.moveTo(xpos + 20, ypos + 6);
        path.lineTo(xpos + 7,  ypos + 6);
        path.lineTo(xpos,     ypos);
        path.lineTo(xpos + 7,  ypos - 6);
        path.lineTo(xpos + 20, ypos - 6);
        path.lineTo(xpos + 20, ypos + 6);
        painter->fillPath(path, color);

        painter->setPen(Qt::black);

        painter->drawText(xpos + 9, ypos + 4, str);
      }
      else if(rot == 3)
        {
          path.moveTo(xpos + 6, ypos + 20);
          path.lineTo(xpos + 6,  ypos + 7);
          path.lineTo(xpos,     ypos);
          path.lineTo(xpos - 6,  ypos + 7);
          path.lineTo(xpos - 6, ypos + 20);
          path.lineTo(xpos + 6, ypos + 20);
          painter->fillPath(path, color);

          painter->setPen(Qt::black);

          painter->drawText(xpos - 3, ypos + 16, str);
        }
}


void SignalCurve::drawSmallTriggerArrow(QPainter *painter, int xpos, int ypos, int rot, QColor color)
{
  QPainterPath path;

  if(rot == 0)
  {
    path.moveTo(xpos - 13, ypos - 5);
    path.lineTo(xpos - 5,  ypos - 5);
    path.lineTo(xpos,     ypos);
    path.lineTo(xpos - 5,  ypos + 5);
    path.lineTo(xpos - 13, ypos + 5);
    path.lineTo(xpos - 13, ypos - 5);

    painter->fillPath(path, color);

    painter->setPen(Qt::black);

    painter->drawLine(xpos - 10, ypos - 4, xpos - 6, ypos - 4);

    painter->drawLine(xpos - 8, ypos - 4, xpos - 8, ypos + 4);
  }
  else if(rot == 1)
    {
      path.moveTo(xpos + 5, ypos - 10);
      path.lineTo(xpos + 5,  ypos - 5);
      path.lineTo(xpos,     ypos);
      path.lineTo(xpos - 4,  ypos - 5);
      path.lineTo(xpos - 4, ypos - 10);
      path.lineTo(xpos + 5, ypos - 10);

      painter->fillPath(path, color);

      painter->setPen(Qt::black);

      painter->drawLine(xpos - 2, ypos - 8, xpos + 2, ypos - 8);

      painter->drawLine(xpos, ypos - 8, xpos, ypos - 3);
    }
    else if(rot == 2)
      {
        path.moveTo(xpos + 12, ypos - 5);
        path.lineTo(xpos + 5,  ypos - 5);
        path.lineTo(xpos,     ypos);
        path.lineTo(xpos + 5,  ypos + 5);
        path.lineTo(xpos + 12, ypos + 5);
        path.lineTo(xpos + 12, ypos - 5);

        painter->fillPath(path, color);

        painter->setPen(Qt::black);

        painter->drawLine(xpos + 9, ypos - 4, xpos + 5, ypos - 4);

        painter->drawLine(xpos + 7, ypos - 4, xpos + 7, ypos + 4);
      }
}


void SignalCurve::drawTrigCenterArrow(QPainter *painter, int xpos, int ypos)
{
  QPainterPath path;

  path.moveTo(xpos + 7, ypos);
  path.lineTo(xpos - 6, ypos);
  path.lineTo(xpos, ypos + 7);
  path.lineTo(xpos + 7, ypos);
  painter->fillPath(path, QColor(255, 128, 0));
}


void SignalCurve::setSignalColor1(QColor newColor)
{
  SignalColor[0] = newColor;
  update();
}


void SignalCurve::setSignalColor2(QColor newColor)
{
  SignalColor[1] = newColor;
  update();
}


void SignalCurve::setSignalColor3(QColor newColor)
{
  SignalColor[2] = newColor;
  update();
}


void SignalCurve::setSignalColor4(QColor newColor)
{
  SignalColor[3] = newColor;
  update();
}


void SignalCurve::setTraceWidth(int tr_width)
{
  tracewidth = tr_width;
  if(tracewidth < 0)  tracewidth = 0;
  update();
}


void SignalCurve::setBackgroundColor(QColor newColor)
{
  BackgroundColor = newColor;
  update();
}


void SignalCurve::setRasterColor(QColor newColor)
{
  RasterColor = newColor;
  update();
}


void SignalCurve::setBorderSize(int newsize)
{
  bordersize = newsize;
  if(bordersize < 0)  bordersize = 0;
  setProperty("ds2_zoom_border", bordersize);
  update();
}


void SignalCurve::mousePressEvent(QMouseEvent *press_event)
{
  int chn,
      m_x,
      m_y;

  setFocus(Qt::MouseFocusReason);

  w = width() - (2 * bordersize);
  h = height() - (2 * bordersize);

  m_x = press_event->x() - bordersize;
  m_y = press_event->y() - bordersize;

  if(devparms == NULL)
  {
    return;
  }

  if(!devparms->connected)
  {
    return;
  }

  if(devparms->math_fft && devparms->math_fft_split)
  {
    m_y -= ((h / 3) + 15);

    if((m_x > -26) && (m_x < 0) && (m_y > (fft_arrow_pos - 7)) && (m_y < (fft_arrow_pos + 7)))
    {
      fft_arrow_moving = 1;
      use_move_events = 1;
      setMouseTracking(true);
      mouse_old_x = m_x;
      mouse_old_y = m_y;
      mainwindow->scrn_timer->stop();
    }

    return;
  }

//  printf("m_x: %i   m_y: %i   trig_pos_arrow_pos: %i\n",m_x, m_y, trig_pos_arrow_pos);

  if(press_event->button() == Qt::LeftButton)
  {
    if(m_y > (h + 12))
    {
//      printf("m_x is: %i   m_y is: %i\n", m_x, m_y);

      m_x += bordersize;

      if((m_x > 8) && (m_x < 118))
      {
        emit chan1Clicked();
      }
      else if((m_x > 133) && (m_x < 243))
        {
          emit chan2Clicked();
        }
        else if((m_x > 258) && (m_x < 368))
          {
            if(devparms->channel_cnt > 2)
            {
              emit chan3Clicked();
            }
          }
          else if((m_x > 383) && (m_x < 493))
            {
              if(devparms->channel_cnt > 3)
              {
                emit chan4Clicked();
              }
            }

      return;
    }

    if(((m_x > (trig_pos_arrow_pos - 8)) && (m_x < (trig_pos_arrow_pos + 8)) && (m_y > 5) && (m_y < 24)) ||
       ((trig_pos_arrow_pos > w) && (m_x > (trig_pos_arrow_pos - 24)) && (m_x <= trig_pos_arrow_pos) && (m_y > 9) && (m_y < 26)) ||
       ((trig_pos_arrow_pos < 0) && (m_x < 24) && (m_x >= 0) && (m_y > 9) && (m_y < 26)))
    {
      trig_pos_arrow_moving = 1;
      use_move_events = 1;
      setMouseTracking(true);
      mouse_old_x = m_x;
      mouse_old_y = m_y;
    }
    else if(((m_x > w) && (m_x < (w + 26)) && (m_y > (trig_level_arrow_pos - 7)) && (m_y < (trig_level_arrow_pos + 7))) ||
            ((trig_level_arrow_pos < 0) && (m_x >= w) && (m_x < (w + 18)) && (m_y >= 0) && (m_y < 22)) ||
            ((trig_level_arrow_pos > h) && (m_x >= w) && (m_x < (w + 18)) && (m_y <= h) && (m_y > (h - 22))))
      {
        trig_level_arrow_moving = 1;
        use_move_events = 1;
        trig_line_visible = 1;
        setMouseTracking(true);
        mouse_old_x = m_x;
        mouse_old_y = m_y;
      }
      else
      {
        for(chn=0; chn<devparms->channel_cnt; chn++)
        {
          if(!devparms->chandisplay[chn])
          {
            continue;
          }

          if((m_x > -26 && (m_x < 0) && (m_y > (chan_arrow_pos[chn] - 7)) && (m_y < (chan_arrow_pos[chn] + 7))) ||
             ((chan_arrow_pos[chn] < 0) && (m_x > -18) && (m_x <= 0) && (m_y >= 0) && (m_y < 22)) ||
             ((chan_arrow_pos[chn] > h) && (m_x > -18) && (m_x <= 0) && (m_y <= h) && (m_y > (h - 22))))
          {
            chan_arrow_moving[chn] = 1;
            devparms->activechannel = chn;
            use_move_events = 1;
            setMouseTracking(true);
            mouse_old_x = m_x;
            mouse_old_y = m_y;
            chan_tmp_old_y_pixel_offset[chn] = m_y;

            break;
          }
        }
      }
  }

  if(use_move_events)
  {
    mainwindow->scrn_timer->stop();
  }
}


void SignalCurve::mouseReleaseEvent(QMouseEvent *release_event)
{
  int chn, tmp;

  char str[512];

  double lefttime, righttime, delayrange;

  w = width() - (2 * bordersize);
  h = height() - (2 * bordersize);

  mouse_x = release_event->x() - bordersize;
  mouse_y = release_event->y() - bordersize;

  if(devparms == NULL)
  {
    return;
  }

  if(!devparms->connected)
  {
    return;
  }

  if(devparms->math_fft && devparms->math_fft_split)
  {
    use_move_events = 0;
    setMouseTracking(false);

    if(fft_arrow_moving)
    {
      fft_arrow_moving = 0;

      if(devparms->screenupdates_on == 1)
      {
        mainwindow->scrn_timer->start(devparms->screentimerival);
      }

      if(devparms->fft_vscale > 9.0)
      {
        devparms->fft_voffset = nearbyint(devparms->fft_voffset);
      }
      else
      {
        devparms->fft_voffset = nearbyint(devparms->fft_voffset * 10.0) / 10.0;
      }

      if(devparms->modelserie == 1)
      {
        snprintf(str, 512, ":MATH:OFFS %e", devparms->fft_voffset);

        mainwindow->set_cue_cmd(str);
      }

      if(devparms->math_fft_unit == 0)
      {
        strlcpy(str, "FFT position: ", 512);

        convert_to_metric_suffix(str + strlen(str), devparms->fft_voffset, 1, 512 - strlen(str));

        strlcat(str, "V/Div", 512);
      }
      else
      {
        snprintf(str, 512, "FFT position: %+.0fdB", devparms->fft_voffset);
      }

      mainwindow->statusLabel->setText(str);

      update();
    }

    return;
  }

  if(trig_pos_arrow_moving)
  {
    trig_pos_arrow_pos = mouse_x;

    if(trig_pos_arrow_pos < 0)
    {
      trig_pos_arrow_pos = 0;
    }

    if(trig_pos_arrow_pos > w)
    {
      trig_pos_arrow_pos = w;
    }

//    printf("w is %i   trig_pos_arrow_pos is %i\n", w, trig_pos_arrow_pos);

    if(devparms->timebasedelayenable)
    {
      devparms->timebasedelayoffset = ((devparms->timebasedelayscale * (double)devparms->hordivisions) / w) * ((w / 2) - trig_pos_arrow_pos);

      tmp = devparms->timebasedelayoffset / (devparms->timebasedelayscale / 50);

      devparms->timebasedelayoffset = (devparms->timebasedelayscale / 50) * tmp;

      lefttime = ((devparms->hordivisions / 2) * devparms->timebasescale) - devparms->timebaseoffset;

      righttime = ((devparms->hordivisions / 2) * devparms->timebasescale) + devparms->timebaseoffset;

      delayrange = (devparms->hordivisions / 2) * devparms->timebasedelayscale;

      if(devparms->timebasedelayoffset < -(lefttime - delayrange))
      {
        devparms->timebasedelayoffset = -(lefttime - delayrange);
      }

      if(devparms->timebasedelayoffset > (righttime - delayrange))
      {
        devparms->timebasedelayoffset = (righttime - delayrange);
      }

      strlcpy(str, "Delayed timebase position: ", 512);

      convert_to_metric_suffix(str + strlen(str), devparms->timebasedelayoffset, 2, 512 - strlen(str));

      strlcat(str, "s", 512);

      mainwindow->statusLabel->setText(str);

      snprintf(str, 512, ":TIM:DEL:OFFS %e", devparms->timebasedelayoffset);

      mainwindow->set_cue_cmd(str);
    }
    else
    {
      devparms->timebaseoffset = ((devparms->timebasescale * (double)devparms->hordivisions) / w) * ((w / 2) - trig_pos_arrow_pos);

      tmp = devparms->timebaseoffset / (devparms->timebasescale / 50);

      devparms->timebaseoffset = (devparms->timebasescale / 50) * tmp;

      strlcpy(str, "Horizontal position: ", 512);

      convert_to_metric_suffix(str + strlen(str), devparms->timebaseoffset, 2, 512 - strlen(str));

      strlcat(str, "s", 512);

      mainwindow->statusLabel->setText(str);

      snprintf(str, 512, ":TIM:OFFS %e", devparms->timebaseoffset);

      mainwindow->set_cue_cmd(str);
    }
  }
  else if(trig_level_arrow_moving)
    {
      if(devparms->triggeredgesource < 0 ||
         devparms->triggeredgesource >= MAX_CHNS ||
         devparms->triggeredgesource >= devparms->channel_cnt)
      {
        return;
      }

      trig_level_arrow_pos = mouse_y;

      if(trig_level_arrow_pos < 0)
      {
        trig_level_arrow_pos = 0;
      }

      if(trig_level_arrow_pos > h)
      {
        trig_level_arrow_pos = h;
      }

  //       printf("chanoffset[chn] is: %e   chanscale[chn] is %e   trig_level_arrow_pos is: %i   v_sense is: %e\n",
  //              devparms->chanoffset[chn], devparms->chanscale[chn], trig_level_arrow_pos, v_sense);

      devparms->triggeredgelevel[devparms->triggeredgesource] = (((h / 2) - trig_level_arrow_pos) * ((devparms->chanscale[devparms->triggeredgesource] * devparms->vertdivisions) / h))
                                                                - devparms->chanoffset[devparms->triggeredgesource];

      tmp = devparms->triggeredgelevel[devparms->triggeredgesource] / (devparms->chanscale[devparms->triggeredgesource] / 50);

      devparms->triggeredgelevel[devparms->triggeredgesource] = (devparms->chanscale[devparms->triggeredgesource] / 50) * tmp;

      snprintf(str, 512, "Trigger level: ");

      convert_to_metric_suffix(str + strlen(str), devparms->triggeredgelevel[devparms->triggeredgesource], 2, 512 - strlen(str));

      strlcat(str, devparms->chanunitstr[devparms->chanunit[devparms->triggeredgesource]], 512);

      mainwindow->statusLabel->setText(str);

      snprintf(str, 512, ":TRIG:EDG:LEV %e", devparms->triggeredgelevel[devparms->triggeredgesource]);

      mainwindow->set_cue_cmd(str);

      trig_line_timer->start(1300);
    }
    else
    {
      for(chn=0; chn<devparms->channel_cnt; chn++)
      {
        if(!devparms->chandisplay[chn])
        {
          continue;
        }

        if(chan_arrow_moving[chn])
        {
          chan_arrow_pos[chn] = mouse_y;

          if(chan_arrow_pos[chn] < 0)
          {
            chan_arrow_pos[chn] = 0;
          }

          if(chan_arrow_pos[chn] > h)
          {
            chan_arrow_pos[chn] = h;
          }

    //       printf("chanoffset[chn] is: %e   chanscale[chn] is %e   chan_arrow_pos[chn] is: %i   v_sense is: %e\n",
    //              devparms->chanoffset[chn], devparms->chanscale[chn], chan_arrow_pos[chn], v_sense);

          devparms->chanoffset[chn] = ((h / 2) - chan_arrow_pos[chn]) * ((devparms->chanscale[chn] * devparms->vertdivisions) / h);

          tmp = devparms->chanoffset[chn] / (devparms->chanscale[chn] / 50);

          devparms->chanoffset[chn] = (devparms->chanscale[chn] / 50) * tmp;

          snprintf(str, 512, "Channel %i offset: ", chn + 1);

          convert_to_metric_suffix(str + strlen(str), devparms->chanoffset[chn], 3, 512 - strlen(str));

          strlcat(str, devparms->chanunitstr[devparms->chanunit[chn]], 512);

          mainwindow->statusLabel->setText(str);

          snprintf(str, 512, ":CHAN%i:OFFS %e", chn + 1, devparms->chanoffset[chn]);

          mainwindow->set_cue_cmd(str);

          devparms->activechannel = chn;

          chan_tmp_y_pixel_offset[chn] = 0;

          chan_tmp_old_y_pixel_offset[chn] = 0;

          break;
        }
      }
    }

  for(chn=0; chn<MAX_CHNS; chn++)
  {
    chan_arrow_moving[chn] = 0;
  }
  trig_level_arrow_moving = 0;
  trig_pos_arrow_moving = 0;
  use_move_events = 0;
  setMouseTracking(false);

  if(devparms->screenupdates_on == 1)
  {
    mainwindow->scrn_timer->start(devparms->screentimerival);
  }

  update();
}


void SignalCurve::mouseMoveEvent(QMouseEvent *move_event)
{
  int chn, h_fft, a_pos;

  double dtmp;

  if(!use_move_events)
  {
    return;
  }

  mouse_x = move_event->x() - bordersize;
  mouse_y = move_event->y() - bordersize;

  if(devparms == NULL)
  {
    return;
  }

  if(!devparms->connected)
  {
    return;
  }

  if(devparms->math_fft && devparms->math_fft_split)
  {
    mouse_y -= ((h / 3) + 15);

    h_fft = h * 0.64;

    if(fft_arrow_moving)
    {
      a_pos = mouse_y;

      if(a_pos < 0)
      {
        a_pos = 0;
      }

      if(a_pos > (h * 0.64))
      {
        a_pos = (h * 0.64);
      }

      devparms->fft_voffset = ((h_fft / 2) - a_pos) * (devparms->fft_vscale * 8.0 / h_fft);

      label_active = LABEL_ACTIVE_FFT;

      mainwindow->label_timer->start(LABEL_TIMER_IVAL);
    }

    update();

    return;
  }

  if(trig_pos_arrow_moving)
  {
    trig_pos_arrow_pos = mouse_x;

    if(trig_pos_arrow_pos < 0)
    {
      trig_pos_arrow_pos = 0;
    }

    if(trig_pos_arrow_pos > w)
    {
      trig_pos_arrow_pos = w;
    }
  }
  else if(trig_level_arrow_moving &&
          devparms->triggeredgesource >= 0 &&
          devparms->triggeredgesource < MAX_CHNS &&
          devparms->triggeredgesource < devparms->channel_cnt)
    {
      trig_level_arrow_pos = mouse_y;

      if(trig_level_arrow_pos < 0)
      {
        trig_level_arrow_pos = 0;
      }

      if(trig_level_arrow_pos > h)
      {
        trig_level_arrow_pos = h;
      }

      devparms->triggeredgelevel[devparms->triggeredgesource] = (((h / 2) - trig_level_arrow_pos) * ((devparms->chanscale[devparms->triggeredgesource] * devparms->vertdivisions) / h))
                                                                - devparms->chanoffset[devparms->triggeredgesource];

      dtmp = devparms->triggeredgelevel[devparms->triggeredgesource] / (devparms->chanscale[devparms->triggeredgesource] / 50);

      devparms->triggeredgelevel[devparms->triggeredgesource] = (devparms->chanscale[devparms->triggeredgesource] / 50) * dtmp;

      label_active = LABEL_ACTIVE_TRIG;

      mainwindow->label_timer->start(LABEL_TIMER_IVAL);
    }
    else
    {
      for(chn=0; chn<devparms->channel_cnt; chn++)
      {
        if(!devparms->chandisplay[chn])
        {
          continue;
        }

        if(chan_arrow_moving[chn])
        {
          chan_arrow_pos[chn] = mouse_y;

          if(chan_arrow_pos[chn] < 0)
          {
            chan_arrow_pos[chn] = 0;
          }

          if(chan_arrow_pos[chn] > h)
          {
            chan_arrow_pos[chn] = h;
          }

          devparms->chanoffset[chn] = ((h / 2) - chan_arrow_pos[chn]) * ((devparms->chanscale[chn] * devparms->vertdivisions) / h);

//          chan_tmp_y_pixel_offset[chn] = (h / 2) - chan_arrow_pos[chn];

          chan_tmp_y_pixel_offset[chn] = chan_tmp_old_y_pixel_offset[chn] - chan_arrow_pos[chn];

          dtmp = devparms->chanoffset[chn] / (devparms->chanscale[chn] / 50);

          devparms->chanoffset[chn] = (devparms->chanscale[chn] / 50) * dtmp;

          label_active = chn + 1;

          mainwindow->label_timer->start(LABEL_TIMER_IVAL);

          break;
        }
      }
    }

  update();
}


void SignalCurve::trig_line_timer_handler()
{
  trig_line_visible = 0;
}


void SignalCurve::setTrigLineVisible(void)
{
  trig_line_visible = 1;

  trig_line_timer->start(1300);
}


void SignalCurve::trig_stat_timer_handler()
{
  if(!trig_stat_flash)
  {
    trig_stat_timer->stop();

    return;
  }

  if(trig_stat_flash == 1)
  {
    trig_stat_flash = 2;
  }
  else
  {
    trig_stat_flash = 1;
  }
}


void SignalCurve::paintLabel(QPainter *painter, int xpos, int ypos, int xw, int yh, const char *str, QColor color)
{
  QPainterPath path;

  path.addRoundedRect(xpos, ypos, xw, yh, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(color);

  painter->drawRoundedRect(xpos, ypos, xw, yh, 3, 3);

  painter->drawText(xpos, ypos, xw, yh, Qt::AlignCenter, str);
}


void SignalCurve::paintCounterLabel(QPainter *painter, int xpos, int ypos)
{
  int i;

  char str[512];

  QPainterPath path;

  path.addRoundedRect(xpos, ypos, 175, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::darkGray);

  painter->drawRoundedRect(xpos, ypos, 175, 20, 3, 3);

  path = QPainterPath();

  path.addRoundedRect(xpos + 4, ypos + 3, 14, 14, 3, 3);

  painter->fillPath(path, SignalColor[devparms->countersrc - 1]);

  painter->setPen(Qt::black);

  painter->drawLine(xpos + 7, ypos + 6, xpos + 15, ypos + 6);

  painter->drawLine(xpos + 11, ypos + 6, xpos + 11, ypos + 14);

  painter->setPen(Qt::white);

  if((devparms->counterfreq < 15) || (devparms->counterfreq > 1.1e9))
  {
    strlcpy(str, "< 15 Hz", 512);
  }
  else
  {
    convert_to_metric_suffix(str, devparms->counterfreq, 5, 512);

    strlcat(str, "Hz", 512);
  }

  for(i=0; i<3; i++)
  {
    painter->drawLine(xpos + 22 + (i * 14), ypos + 14, xpos + 29 + (i * 14), ypos + 14);
    painter->drawLine(xpos + 29 + (i * 14), ypos + 14, xpos + 29 + (i * 14), ypos + 7);
    painter->drawLine(xpos + 29 + (i * 14), ypos + 7, xpos + 36 + (i * 14), ypos + 7);
    painter->drawLine(xpos + 36 + (i * 14), ypos + 7, xpos + 36 + (i * 14), ypos + 14);
  }
  painter->drawLine(xpos + 22 + (i * 14), ypos + 14, xpos + 29 + (i * 14), ypos + 14);

  painter->drawText(xpos + 75, ypos, 100, 20, Qt::AlignCenter, str);
}


void SignalCurve::paintPlaybackLabel(QPainter *painter, int xpos, int ypos)
{
  char str[512];

  QPainterPath path;

  path.addRoundedRect(xpos, ypos, 175, 20, 3, 3);

  painter->fillPath(path, Qt::black);

  painter->setPen(Qt::darkGray);

  painter->drawRoundedRect(xpos, ypos, 175, 20, 3, 3);

  if(devparms->func_wrec_operate || !devparms->func_has_record)
  {
    painter->fillRect(xpos + 5, ypos + 5, 10, 10, Qt::red);

    painter->setPen(Qt::red);

    snprintf(str, 512, "%i/%i", 0, devparms->func_wrec_fend);

    painter->drawText(xpos + 30, ypos, 120, 20, Qt::AlignCenter, str);
  }
  else
  {
    painter->fillRect(xpos + 5, ypos + 5, 10, 10, Qt::green);

    painter->setPen(Qt::green);

    snprintf(str, 512, "%i/%i", devparms->func_wplay_fcur, devparms->func_wrec_fend);

    painter->drawText(xpos + 30, ypos, 120, 20, Qt::AlignCenter, str);
  }
}


bool SignalCurve::hasMoveEvent(void)
{
  if(use_move_events)
  {
    return true;
  }

  return false;
}


void SignalCurve::draw_decoder(QPainter *painter, int dw, int dh)
{
  int i, j, k,
      cell_width,
      base_line,
      line_h_uart_tx=0,
      line_h_uart_rx=0,
      line_h_spi_mosi=0,
      line_h_spi_miso=0,
      spi_chars=1,
      pixel_per_bit=1;

  double pix_per_smpl;

  char str[512];


  if(devparms->modelserie != 1)
  {
    base_line = (dh / 2) - (((double)dh / 400.0) * devparms->math_decode_pos);
  }
  else
  {
    base_line = ((double)dh / 400.0) * devparms->math_decode_pos;
  }

  pix_per_smpl = (double)dw / (devparms->hordivisions * 100);

  switch(devparms->math_decode_format)
  {
    case 0:  cell_width = 40;  // hex
             break;
    case 1:  cell_width = 30;  // ASCII
             break;
    case 2:  cell_width = 30;  // decimal;
             break;
    case 3:  cell_width = 70;  // binary
             break;
    default: cell_width = 70;  // line
             break;
  }

  if(devparms->math_decode_mode == DECODE_MODE_UART)
  {
    if(devparms->timebasedelayenable)
    {
      pixel_per_bit = ((double)dw / 12.0 / devparms->timebasedelayscale) / (double)devparms->math_decode_uart_baud;
    }
    else
    {
      pixel_per_bit = ((double)dw / 12.0 / devparms->timebasescale) / (double)devparms->math_decode_uart_baud;
    }

    cell_width = pixel_per_bit * devparms->math_decode_uart_width;

    painter->setPen(Qt::green);

    if(devparms->math_decode_uart_tx && devparms->math_decode_uart_rx)
    {
      line_h_uart_tx = base_line - 5;

      line_h_uart_rx = base_line + 45;

      painter->drawLine(0, line_h_uart_tx, dw, line_h_uart_tx);

      painter->drawLine(0, line_h_uart_rx, dw, line_h_uart_rx);
    }
    else if(devparms->math_decode_uart_tx)
      {
        line_h_uart_tx = base_line;

        painter->drawLine(0, line_h_uart_tx, dw, line_h_uart_tx);
      }
      else if(devparms->math_decode_uart_rx)
        {
          line_h_uart_rx = base_line;

          painter->drawLine(0, line_h_uart_rx, dw, line_h_uart_rx);
        }

    if(devparms->math_decode_uart_tx)
    {
      for(i=0; i<devparms->math_decode_uart_tx_nval; i++)
      {
        painter->fillRect(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 26, Qt::black);

        painter->drawRect(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 26);
      }
    }

    if(devparms->math_decode_uart_rx)
    {
      for(i=0; i<devparms->math_decode_uart_rx_nval; i++)
      {
        painter->fillRect(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 26, Qt::black);

        painter->drawRect(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 26);
      }
    }

    painter->setPen(Qt::white);

    if(devparms->math_decode_uart_tx)
    {
      switch(devparms->math_decode_format)
      {
        case 0: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[HEX]");
                break;
        case 1: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[ASC]");
                break;
        case 2: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[DEC]");
                break;
        case 3: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[BIN]");
                break;
        case 4: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[LINE]");
                break;
        default: painter->drawText(5, line_h_uart_tx - 35, 65, 30, Qt::AlignCenter, "Tx[\?\?\?]");
                break;
      }

      for(i=0; i<devparms->math_decode_uart_tx_nval; i++)
      {
        if(devparms->math_decode_format == 0)  // hex
        {
          snprintf(str, 512, "%02X", devparms->math_decode_uart_tx_val[i]);

          painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 30, Qt::AlignCenter, str);
        }
        else if(devparms->math_decode_format == 1)  // ASCII
          {
            ascii_decode_control_char(devparms->math_decode_uart_tx_val[i], str, 512);

            painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 30, Qt::AlignCenter, str);
          }
          else if(devparms->math_decode_format == 2)  // decimal
            {
              snprintf(str, 512, "%u", (unsigned int)devparms->math_decode_uart_tx_val[i]);

              painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 30, Qt::AlignCenter, str);
            }
            else if(devparms->math_decode_format == 3)  // binary
              {
                for(j=0; j<devparms->math_decode_uart_width; j++)
                {
                  str[devparms->math_decode_uart_width - 1 - j] = ((devparms->math_decode_uart_tx_val[i] >> j) & 1) + '0';
                }

                str[j] = 0;

                painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 30, Qt::AlignCenter, str);
              }
              else if(devparms->math_decode_format == 4)  // line
                {
                  for(j=0; j<devparms->math_decode_uart_width; j++)
                  {
                    str[j] = ((devparms->math_decode_uart_tx_val[i] >> j) & 1) + '0';
                  }

                  str[j] = 0;

                  painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl, line_h_uart_tx - 13, cell_width, 30, Qt::AlignCenter, str);
                }

          if(devparms->math_decode_uart_tx_err[i])
          {
            painter->setPen(Qt::red);

            painter->drawText(devparms->math_decode_uart_tx_val_pos[i] * pix_per_smpl + cell_width, line_h_uart_tx - 13, 25, 25, Qt::AlignCenter, "?");

            painter->setPen(Qt::white);
          }
      }
    }

    if(devparms->math_decode_uart_rx)
    {
      switch(devparms->math_decode_format)
      {
        case 0: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[HEX]");
                break;
        case 1: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[ASC]");
                break;
        case 2: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[DEC]");
                break;
        case 3: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[BIN]");
                break;
        case 4: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[LINE]");
                break;
        default: painter->drawText(5, line_h_uart_rx - 35, 65, 30, Qt::AlignCenter, "Rx[\?\?\?]");
                break;
      }

      for(i=0; i<devparms->math_decode_uart_rx_nval; i++)
      {
        if(devparms->math_decode_format == 0)  // hex
        {
          snprintf(str, 512, "%02X", devparms->math_decode_uart_rx_val[i]);

          painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 30, Qt::AlignCenter, str);
        }
        else if(devparms->math_decode_format == 1)  // ASCII
          {
            ascii_decode_control_char(devparms->math_decode_uart_rx_val[i], str, 512);

            painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 30, Qt::AlignCenter, str);
          }
          else if(devparms->math_decode_format == 2)  // decimal
            {
              snprintf(str, 512, "%u", (unsigned int)devparms->math_decode_uart_rx_val[i]);

              painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 30, Qt::AlignCenter, str);
            }
            else if(devparms->math_decode_format == 3)  // binary
              {
                for(j=0; j<devparms->math_decode_uart_width; j++)
                {
                  str[devparms->math_decode_uart_width - 1 - j] = ((devparms->math_decode_uart_rx_val[i] >> j) & 1) + '0';
                }

                str[j] = 0;

                painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 30, Qt::AlignCenter, str);
              }
              else if(devparms->math_decode_format == 4)  // line
                {
                  for(j=0; j<devparms->math_decode_uart_width; j++)
                  {
                    str[j] = ((devparms->math_decode_uart_rx_val[i] >> j) & 1) + '0';
                  }

                  str[j] = 0;

                  painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl, line_h_uart_rx - 13, cell_width, 30, Qt::AlignCenter, str);
                }

          if(devparms->math_decode_uart_rx_err[i])
          {
            painter->setPen(Qt::red);

            painter->drawText(devparms->math_decode_uart_rx_val_pos[i] * pix_per_smpl + cell_width, line_h_uart_rx - 13, 25, 25, Qt::AlignCenter, "?");

            painter->setPen(Qt::white);
          }
      }
    }
  }

  if(devparms->math_decode_mode == DECODE_MODE_SPI)
  {
    painter->setPen(Qt::green);

    if(devparms->math_decode_spi_width > 24)
    {
      spi_chars = 4;
    }
    else if(devparms->math_decode_spi_width > 16)
      {
        spi_chars = 3;
      }
      else if(devparms->math_decode_spi_width > 8)
        {
          spi_chars = 2;
        }
        else
        {
          spi_chars = 1;
        }

    cell_width *= spi_chars;

    if(devparms->math_decode_spi_mosi && devparms->math_decode_spi_miso)
    {
      line_h_spi_mosi = base_line - 5;

      line_h_spi_miso = base_line + 45;

      painter->drawLine(0, line_h_spi_mosi, dw, line_h_spi_mosi);

      painter->drawLine(0, line_h_spi_miso, dw, line_h_spi_miso);
    }
    else if(devparms->math_decode_spi_mosi)
      {
        line_h_spi_mosi = base_line;

        painter->drawLine(0, line_h_spi_mosi, dw, line_h_spi_mosi);
      }
      else if(devparms->math_decode_spi_miso)
        {
          line_h_spi_miso = base_line;

          painter->drawLine(0, line_h_spi_miso, dw, line_h_spi_miso);
        }

    if(devparms->math_decode_spi_mosi)
    {
      for(i=0; i<devparms->math_decode_spi_mosi_nval; i++)
      {
        cell_width = (devparms->math_decode_spi_mosi_val_pos_end[i] - devparms->math_decode_spi_mosi_val_pos[i]) *
                      pix_per_smpl;

        painter->fillRect(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 26, Qt::black);

        painter->drawRect(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 26);
      }
    }

    if(devparms->math_decode_spi_miso)
    {
      for(i=0; i<devparms->math_decode_spi_miso_nval; i++)
      {
        cell_width = (devparms->math_decode_spi_miso_val_pos_end[i] - devparms->math_decode_spi_miso_val_pos[i]) *
                      pix_per_smpl;

        painter->fillRect(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 26, Qt::black);

        painter->drawRect(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 26);
      }
    }

    painter->setPen(Qt::white);

    if(devparms->math_decode_spi_mosi)
    {
      switch(devparms->math_decode_format)
      {
        case 0: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[HEX]");
                break;
        case 1: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[ASC]");
                break;
        case 2: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[DEC]");
                break;
        case 3: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[BIN]");
                break;
        case 4: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[LINE]");
                break;
        default: painter->drawText(5, line_h_spi_mosi - 35, 80, 30, Qt::AlignCenter, "Mosi[\?\?\?]");
                break;
      }

      for(i=0; i<devparms->math_decode_spi_mosi_nval; i++)
      {
        if(devparms->math_decode_format == 0)  // hex
        {
          switch(spi_chars)
          {
            case 1: snprintf(str, 512, "%02X", devparms->math_decode_spi_mosi_val[i]);
                    break;
            case 2: snprintf(str, 512, "%04X", devparms->math_decode_spi_mosi_val[i]);
                    break;
            case 3: snprintf(str, 512, "%06X", devparms->math_decode_spi_mosi_val[i]);
                    break;
            case 4: snprintf(str, 512, "%08X", devparms->math_decode_spi_mosi_val[i]);
                    break;
          }

          painter->drawText(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 30, Qt::AlignCenter, str);
        }
        else if(devparms->math_decode_format == 1)  // ASCII
          {
            for(k=0, j=0; k<spi_chars; k++)
            {
              j += ascii_decode_control_char(devparms->math_decode_spi_mosi_val[i] >> (k * 8), str + j, 512 - j);
            }

            painter->drawText(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 30, Qt::AlignCenter, str);
          }
          else if(devparms->math_decode_format == 2)  // decimal
            {
              snprintf(str, 512, "%u", devparms->math_decode_spi_mosi_val[i]);

              painter->drawText(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 30, Qt::AlignCenter, str);
            }
            else if(devparms->math_decode_format == 3)  // binary
              {
                for(j=0; j<devparms->math_decode_spi_width; j++)
                {
                  str[devparms->math_decode_spi_width - 1 - j] = ((devparms->math_decode_spi_mosi_val[i] >> j) & 1) + '0';
                }

                str[j] = 0;

                painter->drawText(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 30, Qt::AlignCenter, str);
              }
              else if(devparms->math_decode_format == 4)  // line
                {
                  for(j=0; j<devparms->math_decode_spi_width; j++)
                  {
                    str[j] = ((devparms->math_decode_spi_mosi_val[i] >> j) & 1) + '0';
                  }

                  str[devparms->math_decode_spi_width] = 0;

                  painter->drawText(devparms->math_decode_spi_mosi_val_pos[i] * pix_per_smpl, line_h_spi_mosi - 13, cell_width, 30, Qt::AlignCenter, str);
                }
      }
    }

    if(devparms->math_decode_spi_miso)
    {
      switch(devparms->math_decode_format)
      {
        case 0: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[HEX]");
                break;
        case 1: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[HEX]");
                break;
        case 2: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[DEC]");
                break;
        case 3: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[BIN]");
                break;
        case 4: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[LINE]");
                break;
        default: painter->drawText(5, line_h_spi_miso - 35, 80, 30, Qt::AlignCenter, "Miso[\?\?\?]");
                break;
      }

      for(i=0; i<devparms->math_decode_spi_miso_nval; i++)
      {
        if(devparms->math_decode_format == 0)  // hex
        {
          switch(spi_chars)
          {
            case 1: snprintf(str, 512, "%02X", devparms->math_decode_spi_miso_val[i]);
                    break;
            case 2: snprintf(str, 512, "%04X", devparms->math_decode_spi_miso_val[i]);
                    break;
            case 3: snprintf(str, 512, "%06X", devparms->math_decode_spi_miso_val[i]);
                    break;
            case 4: snprintf(str, 512, "%08X", devparms->math_decode_spi_miso_val[i]);
                    break;
          }

          painter->drawText(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 30, Qt::AlignCenter, str);
        }
        else if(devparms->math_decode_format == 1)  // ASCII
          {
            for(k=0, j=0; k<spi_chars; k++)
            {
              j += ascii_decode_control_char(devparms->math_decode_spi_miso_val[i] >> (k * 8), str + j, 512 - j);
            }

            painter->drawText(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 30, Qt::AlignCenter, str);
          }
          else if(devparms->math_decode_format == 2)  // decimal
            {
              snprintf(str, 512, "%u", devparms->math_decode_spi_miso_val[i]);

              painter->drawText(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 30, Qt::AlignCenter, str);
            }
            else if(devparms->math_decode_format == 3)  // binary
              {
                for(j=0; j<devparms->math_decode_spi_width; j++)
                {
                  str[devparms->math_decode_spi_width - 1 - j] = ((devparms->math_decode_spi_miso_val[i] >> j) & 1) + '0';
                }

                str[j] = 0;

                painter->drawText(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 30, Qt::AlignCenter, str);
              }
              else if(devparms->math_decode_format == 4)  // line
                {
                  for(j=0; j<devparms->math_decode_spi_width; j++)
                  {
                    str[j] = ((devparms->math_decode_spi_miso_val[i] >> j) & 1) + '0';
                  }

                  str[devparms->math_decode_spi_width] = 0;

                  painter->drawText(devparms->math_decode_spi_miso_val_pos[i] * pix_per_smpl, line_h_spi_miso - 13, cell_width, 30, Qt::AlignCenter, str);
                }
      }
    }
  }
}


int SignalCurve::ascii_decode_control_char(char ch, char *str, int sz)
{
  if((ch > 32) && (ch < 127))
  {
    str[0] = ch;

    str[1] = 0;

    return 1;
  }

  switch(ch)
  {
    case  0: strlcpy(str, "NULL", sz);
             break;
    case  1: strlcpy(str, "SOH", sz);
             break;
    case  2: strlcpy(str, "STX", sz);
             break;
    case  3: strlcpy(str, "ETX", sz);
             break;
    case  4: strlcpy(str, "EOT", sz);
             break;
    case  5: strlcpy(str, "ENQ", sz);
             break;
    case  6: strlcpy(str, "ACK", sz);
             break;
    case  7: strlcpy(str, "BEL", sz);
             break;
    case  8: strlcpy(str, "BS", sz);
             break;
    case  9: strlcpy(str, "HT", sz);
             break;
    case 10: strlcpy(str, "LF", sz);
             break;
    case 11: strlcpy(str, "VT", sz);
             break;
    case 12: strlcpy(str, "FF", sz);
             break;
    case 13: strlcpy(str, "CR", sz);
             break;
    case 14: strlcpy(str, "SO", sz);
             break;
    case 15: strlcpy(str, "SI", sz);
             break;
    case 16: strlcpy(str, "DLE", sz);
             break;
    case 17: strlcpy(str, "DC1", sz);
             break;
    case 18: strlcpy(str, "DC2", sz);
             break;
    case 19: strlcpy(str, "DC3", sz);
             break;
    case 20: strlcpy(str, "DC4", sz);
             break;
    case 21: strlcpy(str, "NAK", sz);
             break;
    case 22: strlcpy(str, "SYN", sz);
             break;
    case 23: strlcpy(str, "ETB", sz);
             break;
    case 24: strlcpy(str, "CAN", sz);
             break;
    case 25: strlcpy(str, "EM", sz);
             break;
    case 26: strlcpy(str, "SUB", sz);
             break;
    case 27: strlcpy(str, "ESC", sz);
             break;
    case 28: strlcpy(str, "FS", sz);
             break;
    case 29: strlcpy(str, "GS", sz);
             break;
    case 30: strlcpy(str, "RS", sz);
             break;
    case 31: strlcpy(str, "US", sz);
             break;
    case 32: strlcpy(str, "SP", sz);
             break;
    case 127: strlcpy(str, "DEL", sz);
             break;
    default: strlcpy(str, ".", sz);
             break;
  }

  return strlen(str);
}












