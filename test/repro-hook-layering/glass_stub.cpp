#include <windows.h>

#include <detours.h>

namespace {

using Evaluate = int(__cdecl*)(void*, void*, void*, void*);
using Mark = void (*)(const char*);

Evaluate real_evaluate = nullptr;

void MarkInHarness(const char* who) {
  const auto mark = reinterpret_cast<Mark>(GetProcAddress(GetModuleHandleA(nullptr), "HarnessMark"));
  if (mark != nullptr) mark(who);
}

int __cdecl GlassEvaluate(void* list, void* handle, void* params, void* callback) {
  MarkInHarness("glass-cockpit");
  return real_evaluate(list, handle, params, callback);
}

}  // namespace

extern "C" __declspec(dllexport) bool GlassStubHook() {
  const HMODULE ngx = GetModuleHandleA("_nvngx.dll");
  if (ngx == nullptr) return false;
  real_evaluate = reinterpret_cast<Evaluate>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
  if (real_evaluate == nullptr) return false;
  if (DetourTransactionBegin() != NO_ERROR) return false;
  DetourUpdateThread(GetCurrentThread());
  if (DetourAttach(reinterpret_cast<PVOID*>(&real_evaluate), reinterpret_cast<PVOID>(&GlassEvaluate)) !=
      NO_ERROR) {
    DetourTransactionAbort();
    return false;
  }
  return DetourTransactionCommit() == NO_ERROR;
}
