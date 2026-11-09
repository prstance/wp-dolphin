// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <string>

#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
// Wii Remotes held by the Wii Party launcher, which stays connected to them over Bluetooth and
// passes their reports through a local socket (a named pipe on Windows). A remote keeps its link
// and its player slot.
//
// WP_WIIMOTE_BRIDGE names the socket, or the pipe on Windows (\\.\pipe\...). For each connection
// Dolphin sends the slot it wants (one byte) and the launcher answers 1 if that slot has a remote
// for it, else 0. Then reports flow both ways, each preceded by its length (one byte). Reports to
// the launcher start with 0xA2, as on Bluetooth, on every OS.
class WiimoteBridge final : public Wiimote
{
public:
#ifdef _WIN32
  // The pipe's HANDLE.
  using Connection = void*;
  static constexpr Connection NO_CONNECTION = nullptr;
#else
  // The socket.
  using Connection = int;
  static constexpr Connection NO_CONNECTION = -1;
#endif

  WiimoteBridge(int slot, Connection connection);
  ~WiimoteBridge() override;
  std::string GetId() const override;
  std::optional<int> GetPreferredSlot() const override { return m_slot; }
  WiimoteCommon::ButtonData GetCurrentlyPressedButtons() override;

protected:
  bool ConnectInternal() override;
  void DisconnectInternal() override;
  bool IsConnected() const override;
  void IOWakeup() override;
  int IORead(u8* buf) override;
  int IOWrite(const u8* buf, size_t len) override;

private:
  // Waits for the length of the next report, or for IOWakeup. Returns what IORead does.
  int ReadReportSize(Connection connection, u8* size);

  const int m_slot;
  std::atomic<Connection> m_connection;
#ifdef _WIN32
  void* m_wakeup_event = nullptr;
#else
  std::array<int, 2> m_wakeup_pipe{-1, -1};
#endif
  std::atomic<u16> m_buttons = 0;
};

class WiimoteScannerBridge final : public WiimoteScannerBackend
{
public:
  explicit WiimoteScannerBridge(std::string path);

  FindResults FindAttachedWiimotes() override;

  void Update() override {}
  void RequestStopSearching() override {}

private:
  const std::string m_path;
};
}  // namespace WiimoteReal
