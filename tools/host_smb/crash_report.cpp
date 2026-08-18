// Печать стека вызовов при аварийном завершении симулятора.
//
// Зачем: на ESP падение видно только как reset_reason=4 без единой подсказки о
// месте. На хосте адрес можно было доставать из журнала Windows и сопоставлять
// с .map — но это один адрес за прогон и без цепочки вызовов. Здесь ставится
// перехватчик необработанных исключений, который печатает весь стек с именами
// функций и номерами строк.
//
// Отдельный отладчик не нужен: dbghelp.dll входит в состав Windows, а символы
// берутся из PDB, который и так собирается ключом /Zi.

#include <windows.h>

#include <dbghelp.h>
#include <stdio.h>

#pragma comment(lib, "dbghelp.lib")

namespace {

const char* exceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION (обращение по недопустимому адресу)";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    default:
      return "неизвестное исключение";
  }
}

LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
  const HANDLE process = GetCurrentProcess();
  const HANDLE thread = GetCurrentThread();

  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, NULL, TRUE);

  printf("\n=========== АВАРИЙНОЕ ЗАВЕРШЕНИЕ ===========\n");
  printf("Исключение: 0x%08lX — %s\n",
         info->ExceptionRecord->ExceptionCode,
         exceptionName(info->ExceptionRecord->ExceptionCode));
  printf("Адрес: %p\n", info->ExceptionRecord->ExceptionAddress);
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      info->ExceptionRecord->NumberParameters >= 2) {
    printf("Операция: %s по адресу 0x%llX\n",
           info->ExceptionRecord->ExceptionInformation[0] == 0 ? "чтение"
           : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "запись"
                                                                 : "исполнение",
           (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
  }
  printf("\nСтек вызовов:\n");

  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  unsigned char symbolStorage[sizeof(SYMBOL_INFO) + 512] = {};
  SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolStorage;
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;

  for (int depth = 0; depth < 40; ++depth) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                     &context, NULL, SymFunctionTableAccess64,
                     SymGetModuleBase64, NULL)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }

    DWORD64 displacement = 0;
    const char* name = "<неизвестно>";
    if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
      name = symbol->Name;
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement,
                             &line)) {
      printf("  %2d. %s\n      %s:%lu\n", depth, name, line.FileName,
             line.LineNumber);
    } else {
      printf("  %2d. %s + 0x%llX\n", depth, name,
             (unsigned long long)displacement);
    }
  }
  printf("============================================\n");
  fflush(stdout);

  SymCleanup(process);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

namespace zifi {
namespace host {

void installCrashReporter() {
  SetUnhandledExceptionFilter(crashHandler);
}

}  // namespace host
}  // namespace zifi
