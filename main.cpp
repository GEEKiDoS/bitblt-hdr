#include <string>

#include <MinHook.h>

#include "resource.h"

#include "bitblt_hdr.hpp"
#include "utils/trampoline.hpp"

namespace
{
  trampoline<decltype(BitBlt)> bitblt_original;
  std::unique_ptr<bitblt_hdr> bitblt;

  HINSTANCE self_instance;

  BOOL WINAPI
  bitblt_hook(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
  {
    printf("bitblt called\n");

    if (!bitblt)
    {
      bitblt = std::make_unique<bitblt_hdr>(bitblt_original);
#ifndef _DEBUG
      bitblt->create_shader_from_resource(self_instance, TONEMAPPER_SHADER);
#endif
    }

#ifdef _DEBUG
    bitblt->create_shader_from_source_file(L"tonemapper.hlsl");
#endif

    return bitblt->bitblt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
  }

  void free_desktop_dup()
  {
    if (bitblt)
    {
      bitblt.reset();
    }
  }

  trampoline<void WINAPI(UINT)> exit_process;
  void exit_process_hook(UINT code)
  {
    free_desktop_dup();
    exit_process(code);
  }

#ifdef _DEBUG
  void create_console()
  {
    AllocConsole();

    FILE* f;
    auto _ = freopen_s(&f, "CONOUT$", "w+t", stdout);
    _ = freopen_s(&f, "CONOUT$", "w", stderr);
    _ = freopen_s(&f, "CONIN$", "r", stdin);
  }
#endif

  class hook_autoinit
  {
   public:
    hook_autoinit()
    {
#ifdef _DEBUG
      create_console();
#endif
      LoadLibraryA("gdi32.dll");
      MH_Initialize();
      MH_CreateHookApi(L"gdi32.dll", "BitBlt", bitblt_hook, &bitblt_original);
      MH_CreateHookApi(L"kernel32.dll", "ExitProcess", exit_process_hook, &exit_process);
      MH_EnableHook(MH_ALL_HOOKS);
    }
  } hook;
}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD fdwReason, LPVOID)
{
  if (fdwReason == DLL_PROCESS_ATTACH)
  {
    self_instance = instance;
  }

  return TRUE;
}
