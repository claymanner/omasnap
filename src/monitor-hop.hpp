/** @fileoverview Moves a capture overlay to the monitor the pointer enters. */
#pragma once

#include "editor.hpp"

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QScreen;
class QTimer;

/**
 * The capture overlay is one layer surface on one monitor, captured before it
 * mapped, and Hyprland gives no other surface the pointer while it holds the
 * keyboard. So while the overlay is still choosing what to capture, ask
 * Hyprland where the cursor is (one IPC round trip, asynchronously) a few
 * times a second. When it has moved to another monitor, `hop` runs with that
 * monitor's name and the caller reopens the capture there.
 */
class MonitorHop final : public QObject {
public:
  MonitorHop(CaptureEditor &editor, QScreen *current,
             std::function<void(const QString &)> hop);
  ~MonitorHop() override;

private:
  void poll();
  void readReply();
  void closeSocket();

  CaptureEditor &editor_;
  QScreen *current_;
  std::function<void(const QString &)> hop_;
  QString socketPath_;
  QTimer *timer_ = nullptr;
  int fd_ = -1;
  QSocketNotifier *notifier_ = nullptr;
  QByteArray reply_;
  bool hopped_ = false;
};

/** Command line for the capture reopened on `monitor`: the original
 *  arguments with the capture mode replaced by the overlay's current one. */
[[nodiscard]] QStringList monitorHopArguments(QStringList arguments,
                                              CaptureEditor::CaptureMode mode,
                                              const QString &monitor);
