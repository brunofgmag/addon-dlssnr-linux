#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace ngx_probe {
inline void Log(const char* msg) { std::printf("  log: %s\n", msg); }
inline void Logf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::printf("  log: ");
  std::vprintf(fmt, args);
  std::printf("\n");
  va_end(args);
}
inline void Warnf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vprintf(fmt, args);
  std::printf("\n");
  va_end(args);
}
}  // namespace ngx_probe

#include "../../src/nr_runner.hpp"

namespace {

int failures = 0;

void Expect(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}

const void* const kMain = reinterpret_cast<const void*>(0x41D3CE70);
const void* const kSecond = reinterpret_cast<const void*>(0x40B63A70);

bool Follows(const void* handle, unsigned out_w) {
  const nr_runner::DlssFeature* f = nr_runner::Follow(handle);
  return f != nullptr && f->handle == handle && f->out_w == out_w;
}

}  // namespace

int main() {
  nr_runner::OnDlssCreate(kMain, 1920, 1080, 3840, 2160, 11);
  Expect("the only feature is followed", Follows(kMain, 3840));

  nr_runner::OnDlssCreate(kSecond, 1280, 720, 2560, 1440, 11);
  Expect("a smaller feature created later does not take over", Follows(kMain, 3840));

  bool second_skipped = true;
  bool main_kept = true;
  for (int frame = 0; frame < 1000; ++frame) {
    main_kept &= Follows(kMain, 3840);
    second_skipped &= nr_runner::Follow(kSecond) == nullptr;
  }
  Expect("both evaluated every frame: the larger one keeps the pass", main_kept);
  Expect("both evaluated every frame: the smaller one is skipped", second_skipped);

  bool took_over = false;
  for (uint64_t i = 0; i <= nr_runner::kStaleEvaluates + 1; ++i)
    took_over = Follows(kSecond, 2560);
  Expect("when the larger one stops evaluating, the smaller one takes over", took_over);

  Expect("the larger one is followed again once it evaluates", Follows(kMain, 3840));

  nr_runner::OnDlssRelease(kMain);
  Expect("after releasing the larger one, the smaller one is followed at once",
         Follows(kSecond, 2560));

  Expect("an unknown handle is not followed",
         nr_runner::Follow(reinterpret_cast<const void*>(0x1234)) == nullptr);

  nr_runner::OnDlssCreate(kSecond, 1920, 1080, 3840, 2160, 11);
  Expect("a recreated handle carries its new geometry", Follows(kSecond, 3840));

  std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
  return failures == 0 ? 0 : 1;
}
