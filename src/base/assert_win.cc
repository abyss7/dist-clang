#include <base/assert.h>

#include <WinBase.h>

namespace dist_clang {
namespace base {

void GetStackTrace(ui8 depth, Vector<String>& strings) {
  auto process = GetCurrentProcess();
  SymInitialize(process, nullptr, true);

  auto symbol = reinterpret_cast<SYMBOL_INFO*>(
      calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1));
  symbol->MaxNameLen = 255;
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

  void* buffer[depth];
  auto size = CaptureStackBackTrace(1, depth, buffer, nullptr);
  strings.resize(size);
  for (ui8 i = 0; i < size; ++i) {
    SymFromAddr(process, reinterpret_cast<DWORD64>(stack[i]), 0, symbol);
    strings[i] = String(symbol->Name, symbol->NameLen);
  }
  free(symbol);
}

}  // namespace base
}  // namespace dist_clang
