/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/ui/surface_win.h>

#include <dxgi1_6.h>

namespace rex {
namespace ui {

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
bool Win32HwndSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  RECT client_rect;
  if (!GetClientRect(hwnd(), &client_rect)) {
    return false;
  }
  // GetClientRect returns a rectangle with 0 origin.
  width_out = uint32_t(client_rect.right);
  height_out = uint32_t(client_rect.bottom);
  return true;
}

HdrDisplay Win32HwndSurface::GetHdrDisplay() const {
  HdrDisplay result;
  const HMONITOR monitor = MonitorFromWindow(hwnd(), MONITOR_DEFAULTTONEAREST);
  // Loaded on demand so the runtime keeps no static DXGI dependency.
  static const auto create_factory = [] {
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    return dxgi ? reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
                      GetProcAddress(dxgi, "CreateDXGIFactory1"))
                : nullptr;
  }();
  IDXGIFactory1* factory = nullptr;
  if (!monitor || !create_factory ||
      FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
    return result;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      IDXGIOutput6* output6 = nullptr;
      DXGI_OUTPUT_DESC1 desc{};
      if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
          SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
        result.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        result.peak_nits = desc.MaxLuminance;
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, name, sizeof(name), nullptr, nullptr);
        result.name = name;
      }
      if (output6) output6->Release();
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
  return result;
}
#endif

}  // namespace ui
}  // namespace rex
