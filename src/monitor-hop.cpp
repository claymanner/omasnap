/** @fileoverview Moves a capture overlay to the monitor the pointer enters. */
#include "monitor-hop.hpp"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr int kPollIntervalMs = 66;
constexpr char kRequest[] = "j/cursorpos";
}

MonitorHop::MonitorHop(CaptureEditor &editor, QScreen *current,
                       std::function<void(const QString &)> hop)
    : editor_(editor), current_(current), hop_(std::move(hop)) {
  const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  const QString instance = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
  if (runtime.isEmpty() || instance.isEmpty())
    return;
  socketPath_ = QStringLiteral("%1/hypr/%2/.socket.sock").arg(runtime, instance);
  timer_ = new QTimer(this);
  timer_->setInterval(kPollIntervalMs);
  connect(timer_, &QTimer::timeout, this, &MonitorHop::poll);
  timer_->start();
}

MonitorHop::~MonitorHop() { closeSocket(); }

void MonitorHop::closeSocket() {
  delete notifier_;
  notifier_ = nullptr;
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
}

void MonitorHop::poll() {
  if (hopped_ || !editor_.choosingCapture()) {
    // Once the capture is chosen there is nothing left to follow.
    timer_->stop();
    closeSocket();
    return;
  }
  // A request still in flight answers before the next one goes out.
  if (fd_ >= 0 || !editor_.canMoveToMonitor())
    return;
  sockaddr_un address{};
  const QByteArray path = socketPath_.toLocal8Bit();
  if (path.size() >= static_cast<qsizetype>(sizeof(address.sun_path)))
    return;
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.constData(), path.size());
  fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0)
    return;
  // Hyprland's socket is local: connect and the short write complete at
  // once, and the reply is read asynchronously.
  if (::connect(fd_, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0 ||
      ::write(fd_, kRequest, sizeof(kRequest) - 1) !=
          static_cast<ssize_t>(sizeof(kRequest) - 1)) {
    closeSocket();
    return;
  }
  reply_.clear();
  notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this, &MonitorHop::readReply);
}

void MonitorHop::readReply() {
  char buffer[256];
  const ssize_t count = ::read(fd_, buffer, sizeof(buffer));
  if (count > 0) {
    reply_.append(buffer, count);
    return;
  }
  // Hyprland answers one request per connection and then closes it.
  closeSocket();
  const QJsonObject position = QJsonDocument::fromJson(reply_).object();
  if (hopped_ || !position.contains(QStringLiteral("x")) ||
      !editor_.canMoveToMonitor())
    return;
  const QPoint cursor(position.value(QStringLiteral("x")).toInt(),
                      position.value(QStringLiteral("y")).toInt());
  if (current_ && current_->geometry().contains(cursor))
    return;
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen == current_ || !screen->geometry().contains(cursor))
      continue;
    hopped_ = true;
    timer_->stop();
    hop_(screen->name());
    return;
  }
}

QStringList monitorHopArguments(QStringList arguments,
                                CaptureEditor::CaptureMode mode,
                                const QString &monitor) {
  static const QStringList modeFlags = {
      QStringLiteral("--capture-fullscreen"), QStringLiteral("--capture-window"),
      QStringLiteral("--capture-windows"), QStringLiteral("--capture-region")};
  static const QStringList modeWords = {
      QStringLiteral("smart"), QStringLiteral("region"),
      QStringLiteral("windows"), QStringLiteral("window"),
      QStringLiteral("fullscreen")};
  QStringList kept;
  for (int i = 0; i < arguments.size(); ++i) {
    const QString &argument = arguments.at(i);
    if (argument == QStringLiteral("--capture-monitor")) {
      ++i;
      continue;
    }
    if (argument.startsWith(QStringLiteral("--capture-monitor=")) ||
        modeFlags.contains(argument) || modeWords.contains(argument))
      continue;
    kept << argument;
  }
  switch (mode) {
  case CaptureEditor::CaptureMode::Region:
    kept << QStringLiteral("region");
    break;
  case CaptureEditor::CaptureMode::Window:
    kept << QStringLiteral("windows");
    break;
  case CaptureEditor::CaptureMode::Fullscreen:
    kept << QStringLiteral("fullscreen");
    break;
  default:
    kept << QStringLiteral("smart");
    break;
  }
  kept << QStringLiteral("--capture-monitor") << monitor;
  return kept;
}
