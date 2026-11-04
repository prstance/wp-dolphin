// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteReal/IOBridge.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fmt/format.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"

namespace WiimoteReal
{
// Reads exactly `size` bytes. A report arrives in one piece, so this doesn't wait for long.
static bool ReadAll(int socket, u8* buf, size_t size)
{
  while (size != 0)
  {
    const ssize_t got = read(socket, buf, size);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    buf += got;
    size -= size_t(got);
  }
  return true;
}

static bool WriteAll(int socket, const u8* buf, size_t size)
{
  while (size != 0)
  {
    const ssize_t sent = write(socket, buf, size);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    buf += sent;
    size -= size_t(sent);
  }
  return true;
}

// Asks the launcher for the remote in a slot. Returns its socket, or -1.
static int OpenSlot(const std::string& path, int slot)
{
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path))
    return -1;
  std::ranges::copy(path, addr.sun_path);

  const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1)
    return -1;

  const u8 request = u8(slot);
  u8 answer = 0;
  if (connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
      !WriteAll(sock, &request, 1) || !ReadAll(sock, &answer, 1) || answer != 1)
  {
    close(sock);
    return -1;
  }
  return sock;
}

WiimoteScannerBridge::WiimoteScannerBridge(std::string socket_path)
    : m_socket_path(std::move(socket_path))
{
  NOTICE_LOG_FMT(WIIMOTE, "Wii Remotes come from the launcher, through {}.", m_socket_path);
}

auto WiimoteScannerBridge::FindAttachedWiimotes() -> FindResults
{
  FindResults results;
  for (int slot = 0; slot != MAX_WIIMOTES; ++slot)
  {
    const int sock = OpenSlot(m_socket_path, slot);
    if (sock != -1)
      results.wii_remotes.push_back(std::make_unique<WiimoteBridge>(slot, sock));
  }
  return results;
}

WiimoteBridge::WiimoteBridge(int slot, int socket) : m_slot(slot), m_socket(socket)
{
  if (pipe(m_wakeup_pipe.data()) != 0)
    ERROR_LOG_FMT(WIIMOTE, "Bridge pipe failed: {}", Common::LastStrerrorString());
}

WiimoteBridge::~WiimoteBridge()
{
  Shutdown();
  close(m_wakeup_pipe[0]);
  close(m_wakeup_pipe[1]);
}

std::string WiimoteBridge::GetId() const
{
  return fmt::format("launcher-{}", m_slot);
}

// Reports are dropped while the Wii isn't linked, but a press must still wake it up.
WiimoteCommon::ButtonData WiimoteBridge::GetCurrentlyPressedButtons()
{
  WiimoteCommon::ButtonData buttons = {};
  buttons.hex = m_buttons.load();
  return buttons;
}

bool WiimoteBridge::ConnectInternal()
{
  return IsConnected();
}

void WiimoteBridge::DisconnectInternal()
{
  const int sock = m_socket.exchange(-1);
  if (sock != -1)
    close(sock);
}

bool WiimoteBridge::IsConnected() const
{
  return m_socket.load() != -1;
}

void WiimoteBridge::IOWakeup()
{
  const u8 wake = 1;
  if (write(m_wakeup_pipe[1], &wake, 1) != 1)
    ERROR_LOG_FMT(WIIMOTE, "Bridge wakeup failed: {}", Common::LastStrerrorString());
}

// positive = read packet
// negative = didn't read packet
// zero = error
int WiimoteBridge::IORead(u8* buf)
{
  const int sock = m_socket.load();
  std::array<pollfd, 2> pollfds{
      pollfd{.fd = m_wakeup_pipe[0], .events = POLLIN},
      pollfd{.fd = sock, .events = POLLIN},
  };
  while (poll(pollfds.data(), pollfds.size(), -1) < 0)
  {
    if (errno != EINTR)
      return 0;
  }

  if (pollfds[0].revents != 0)
  {
    u8 wake;
    return read(m_wakeup_pipe[0], &wake, 1) == 1 ? -1 : 0;
  }

  u8 size = 0;
  if (!ReadAll(sock, &size, 1) || size == 0 || size > MAX_PAYLOAD || !ReadAll(sock, buf, size))
    return 0;

  // HID header, report id, then the core buttons, which every data report starts with.
  if (size >= 4 && buf[1] >= u8(WiimoteCommon::InputReportID::ReportCore) &&
      buf[1] != u8(WiimoteCommon::InputReportID::ReportExt21))
  {
    m_buttons.store(u16(buf[2] << 8 | buf[3]) & WiimoteCommon::ButtonData::BUTTON_MASK);
  }
  return size;
}

int WiimoteBridge::IOWrite(const u8* buf, size_t len)
{
  std::array<u8, MAX_PAYLOAD + 1> frame;
  if (len > MAX_PAYLOAD)
    return 0;
  frame[0] = u8(len);
  std::copy_n(buf, len, frame.begin() + 1);

  // Closing the socket also ends the read thread, through poll.
  if (!WriteAll(m_socket.load(), frame.data(), len + 1))
  {
    ERROR_LOG_FMT(WIIMOTE, "Wiimote {} write failed: {}", m_index + 1,
                  Common::LastStrerrorString());
    return 0;
  }
  return int(len);
}
}  // namespace WiimoteReal
