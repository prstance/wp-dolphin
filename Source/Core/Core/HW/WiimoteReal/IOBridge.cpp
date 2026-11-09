// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteReal/IOBridge.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"

#ifdef _WIN32
#include "Common/StringUtil.h"
#endif

namespace WiimoteReal
{
using Connection = WiimoteBridge::Connection;
constexpr Connection NO_CONNECTION = WiimoteBridge::NO_CONNECTION;

// An output report's header on Bluetooth, which the launcher expects. Dolphin uses another one on
// Windows (WR_SET_REPORT).
constexpr u8 BRIDGE_OUTPUT_HEADER = 0xA0 | BT_OUTPUT;

#ifdef _WIN32
// How long to wait, at most, for the launcher to open another instance of its pipe, and how many
// times.
constexpr DWORD PIPE_BUSY_TIMEOUT_MS = 500;
constexpr int PIPE_BUSY_ATTEMPTS = 4;

// An event for the overlapped transfers of the calling thread: a remote is read and written from
// two threads at once. Without one, a transfer couldn't be waited for.
static HANDLE ThreadEvent()
{
  struct Event
  {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event()
    {
      if (handle != nullptr)
        CloseHandle(handle);
    }
  };
  thread_local Event event;
  return event.handle;
}

// Waits for a transfer on the pipe that ReadFile or WriteFile started, returning `started`. If set,
// `wakeup` cancels it. Returns the bytes moved, -1 if woken up before any, or 0 if the pipe failed
// or was closed.
static int Finish(HANDLE pipe, OVERLAPPED* overlapped, BOOL started, HANDLE wakeup)
{
  bool wait_failed = false;
  if (!started)
  {
    const DWORD error = GetLastError();
    // ERROR_MORE_DATA: a message longer than the read, which is over all the same.
    if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA)
      return 0;

    if (error == ERROR_IO_PENDING && wakeup != nullptr)
    {
      const std::array<HANDLE, 2> objs{wakeup, overlapped->hEvent};
      constexpr BOOL wait_for_all = FALSE;
      const DWORD wait_result =
          WaitForMultipleObjects(DWORD(objs.size()), objs.data(), wait_for_all, INFINITE);
      if (wait_result != WAIT_OBJECT_0 + 1)
      {
        if (wait_result != WAIT_OBJECT_0)
        {
          ERROR_LOG_FMT(WIIMOTE, "Bridge WaitForMultipleObjects: {}", Common::GetLastErrorString());
          wait_failed = true;
        }
        // The transfer may still end before it's cancelled.
        CancelIoEx(pipe, overlapped);
      }
    }
  }

  // Waits for the end of the transfer: `overlapped` must outlive it.
  DWORD moved = 0;
  const BOOL done = GetOverlappedResult(pipe, overlapped, &moved, TRUE);
  if (wait_failed)
    return 0;
  if (done)
    return int(moved);

  switch (GetLastError())
  {
  case ERROR_MORE_DATA:
    return int(moved);
  case ERROR_OPERATION_ABORTED:
    return moved != 0 ? int(moved) : -1;
  default:
    // ERROR_BROKEN_PIPE once the launcher closes its end.
    return 0;
  }
}

// Reads exactly `size` bytes. A report arrives in one piece, so this doesn't wait for long.
static bool ReadAll(HANDLE pipe, u8* buf, size_t size)
{
  while (size != 0)
  {
    OVERLAPPED overlapped{};
    overlapped.hEvent = ThreadEvent();
    if (overlapped.hEvent == nullptr)
      return false;
    const BOOL started = ReadFile(pipe, buf, DWORD(size), nullptr, &overlapped);
    const int got = Finish(pipe, &overlapped, started, nullptr);
    if (got <= 0)
      return false;
    buf += got;
    size -= size_t(got);
  }
  return true;
}

static bool WriteAll(HANDLE pipe, const u8* buf, size_t size)
{
  while (size != 0)
  {
    OVERLAPPED overlapped{};
    overlapped.hEvent = ThreadEvent();
    if (overlapped.hEvent == nullptr)
      return false;
    const BOOL started = WriteFile(pipe, buf, DWORD(size), nullptr, &overlapped);
    const int sent = Finish(pipe, &overlapped, started, nullptr);
    if (sent <= 0)
      return false;
    buf += sent;
    size -= size_t(sent);
  }
  return true;
}

// Returns the pipe, or nullptr if the launcher isn't there.
static HANDLE Connect(const std::string& path)
{
  const std::wstring name = UTF8ToWString(path);
  for (int attempt = 0; attempt != PIPE_BUSY_ATTEMPTS; ++attempt)
  {
    // SECURITY_IDENTIFICATION: whoever serves the pipe can't act as this user.
    const HANDLE pipe = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
      return pipe;

    // Every instance of the pipe is taken: the launcher is opening another one.
    if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), PIPE_BUSY_TIMEOUT_MS))
      break;
  }
  return nullptr;
}

static void Close(HANDLE pipe)
{
  CloseHandle(pipe);
}

static std::string LastErrorString()
{
  return Common::GetLastErrorString();
}
#else
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

// Returns the socket, or -1 if the launcher isn't there.
static int Connect(const std::string& path)
{
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path))
    return -1;
  std::ranges::copy(path, addr.sun_path);

  const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1)
    return -1;

  if (connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
  {
    close(sock);
    return -1;
  }
  return sock;
}

static void Close(int socket)
{
  close(socket);
}

static std::string LastErrorString()
{
  return Common::LastStrerrorString();
}
#endif

// Asks the launcher for the remote in a slot. Returns its connection, or NO_CONNECTION.
static Connection OpenSlot(const std::string& path, int slot)
{
  const Connection connection = Connect(path);
  if (connection == NO_CONNECTION)
    return NO_CONNECTION;

  const u8 request = u8(slot);
  u8 answer = 0;
  if (!WriteAll(connection, &request, 1) || !ReadAll(connection, &answer, 1) || answer != 1)
  {
    Close(connection);
    return NO_CONNECTION;
  }
  return connection;
}

WiimoteScannerBridge::WiimoteScannerBridge(std::string path) : m_path(std::move(path))
{
  NOTICE_LOG_FMT(WIIMOTE, "Wii Remotes come from the launcher, through {}.", m_path);
}

auto WiimoteScannerBridge::FindAttachedWiimotes() -> FindResults
{
  FindResults results;
  for (int slot = 0; slot != MAX_WIIMOTES; ++slot)
  {
    const Connection connection = OpenSlot(m_path, slot);
    if (connection != NO_CONNECTION)
      results.wii_remotes.push_back(std::make_unique<WiimoteBridge>(slot, connection));
  }
  return results;
}

#ifdef _WIN32
WiimoteBridge::WiimoteBridge(int slot, Connection connection)
    : m_slot(slot), m_connection(connection)
{
  constexpr BOOL manual_reset = FALSE;
  constexpr BOOL initial_state = FALSE;
  m_wakeup_event = CreateEventW(nullptr, manual_reset, initial_state, nullptr);
  if (m_wakeup_event == nullptr)
    ERROR_LOG_FMT(WIIMOTE, "Bridge event failed: {}", Common::GetLastErrorString());
}

WiimoteBridge::~WiimoteBridge()
{
  Shutdown();
  if (m_wakeup_event != nullptr)
    CloseHandle(m_wakeup_event);
}

void WiimoteBridge::IOWakeup()
{
  if (!SetEvent(m_wakeup_event))
    ERROR_LOG_FMT(WIIMOTE, "Bridge wakeup failed: {}", Common::GetLastErrorString());
}

int WiimoteBridge::ReadReportSize(Connection connection, u8* size)
{
  OVERLAPPED overlapped{};
  overlapped.hEvent = ThreadEvent();
  if (overlapped.hEvent == nullptr)
    return 0;
  const BOOL started = ReadFile(connection, size, 1, nullptr, &overlapped);
  return Finish(connection, &overlapped, started, m_wakeup_event);
}
#else
WiimoteBridge::WiimoteBridge(int slot, Connection connection)
    : m_slot(slot), m_connection(connection)
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

void WiimoteBridge::IOWakeup()
{
  const u8 wake = 1;
  if (write(m_wakeup_pipe[1], &wake, 1) != 1)
    ERROR_LOG_FMT(WIIMOTE, "Bridge wakeup failed: {}", Common::LastStrerrorString());
}

int WiimoteBridge::ReadReportSize(Connection connection, u8* size)
{
  std::array<pollfd, 2> pollfds{
      pollfd{.fd = m_wakeup_pipe[0], .events = POLLIN},
      pollfd{.fd = connection, .events = POLLIN},
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

  return ReadAll(connection, size, 1) ? 1 : 0;
}
#endif

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
  const Connection connection = m_connection.exchange(NO_CONNECTION);
  if (connection != NO_CONNECTION)
    Close(connection);
}

bool WiimoteBridge::IsConnected() const
{
  return m_connection.load() != NO_CONNECTION;
}

// positive = read packet
// negative = didn't read packet
// zero = error
int WiimoteBridge::IORead(u8* buf)
{
  const Connection connection = m_connection.load();
  u8 size = 0;
  const int result = ReadReportSize(connection, &size);
  if (result <= 0)
    return result;

  if (size == 0 || size > MAX_PAYLOAD || !ReadAll(connection, buf, size))
    return 0;

  // HID header, report id, then the core buttons, which every data report starts with. They are
  // little-endian, as ButtonData::hex.
  if (size >= 4 && buf[1] >= u8(WiimoteCommon::InputReportID::ReportCore) &&
      buf[1] != u8(WiimoteCommon::InputReportID::ReportExt21))
  {
    m_buttons.store(u16(buf[2] | buf[3] << 8) & WiimoteCommon::ButtonData::BUTTON_MASK);
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
  if (len != 0 && frame[1] == (WR_SET_REPORT | BT_OUTPUT))
    frame[1] = BRIDGE_OUTPUT_HEADER;

  // The launcher closing the connection also ends the read thread, whose read fails.
  if (!WriteAll(m_connection.load(), frame.data(), len + 1))
  {
    ERROR_LOG_FMT(WIIMOTE, "Wiimote {} write failed: {}", m_index + 1, LastErrorString());
    return 0;
  }
  return int(len);
}
}  // namespace WiimoteReal
