// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef _WIN32
#include <array>
#include <atomic>
#include <string>

#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
// Wii Remotes held by the Wii Party launcher, which stays connected to them over Bluetooth and
// passes their reports through a local socket. A remote keeps its link and its player slot.
//
// WP_WIIMOTE_BRIDGE names the socket. For each connection Dolphin sends the slot it wants (one
// byte) and the launcher answers 1 if that slot has a remote for it, else 0. Then reports flow both
// ways, each preceded by its length (one byte).
class WiimoteBridge final : public Wiimote
{
public:
  WiimoteBridge(int slot, int socket);
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
  const int m_slot;
  std::atomic<int> m_socket;
  std::array<int, 2> m_wakeup_pipe{-1, -1};
  std::atomic<u16> m_buttons = 0;
};

class WiimoteScannerBridge final : public WiimoteScannerBackend
{
public:
  explicit WiimoteScannerBridge(std::string socket_path);

  FindResults FindAttachedWiimotes() override;

  void Update() override {}
  void RequestStopSearching() override {}

private:
  const std::string m_socket_path;
};
}  // namespace WiimoteReal
#endif
