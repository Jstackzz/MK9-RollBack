// Crash reports, see crash.h.
//
// The crashed thread hands its crash to a thread made for it at start (which has a stack of its
// own: a stack overflow leaves the crashed thread almost none) and waits; that one writes crash.txt
// as it goes (a report that fails half way keeps its first half), then crash.dmp, and the process
// ends with the crash's code. Reading memory for the report checks first that it is there.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include "mk9/crash.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/net/netplay.h"

#ifndef MK9_VERSION
#define MK9_VERSION "0.0.0"
#endif

namespace mk9::crash {
namespace {

constexpr DWORD kAbortCode = 3;  // what abort() ends a process with

// what a crash hands the thread that writes its report
struct Request {
  EXCEPTION_POINTERS* ep = nullptr;
  const char* kind = "";
  DWORD tid = 0;
  DWORD exit_code = 0;
  const PPCContext* guest = nullptr;    // the crashed thread's, when it is a guest thread
  ULONG_PTR stack_lo = 0, stack_hi = 0;  // its host stack
  EXCEPTION_RECORD record{};             // a crash without an exception (abort...): made up here
  CONTEXT context{};
  EXCEPTION_POINTERS pointers{};
};
Request g_req;
HANDLE g_request = nullptr, g_done = nullptr, g_writer = nullptr;
DWORD g_writer_tid = 0;
void* g_veh = nullptr;
std::atomic<DWORD> g_owner{0};  // the thread whose crash is being reported
std::atomic<bool> g_installed{false};

wchar_t g_dir[MAX_PATH];  // where reports go
time_t g_started = 0;
uint64_t g_test_frame = 0;
char g_test_kind[16] = "";
volatile int* volatile g_nowhere = nullptr;  // MK9_CRASH_TEST writes through it
char g_notes[int(Note::Count)][192];

// the recompiled functions by host address (PPCFuncMappings is by guest address)
struct HostFunc {
  uintptr_t host;
  uint32_t guest;
};
std::vector<HostFunc> g_funcs;
uintptr_t g_exe_lo = 0, g_exe_hi = 0;

// the report, written as it is made
class Out {
 public:
  explicit Out(HANDLE f) : f_(f) {}
  void operator()(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    n = std::min(n, int(sizeof b) - 1);
    DWORD written = 0;
    WriteFile(f_, b, DWORD(n), &written, nullptr);
  }

 private:
  HANDLE f_;
};

bool Readable(const void* p, size_t n) {
  const char* a = static_cast<const char*>(p);
  while (n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
      return false;
    const size_t room = size_t(static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize - a);
    if (room >= n) return true;
    a += room;
    n -= room;
  }
  return true;
}

// guest memory the report reads: mapped and readable
bool GuestReadable(uint32_t a, size_t n) {
  auto* ks = REX_KERNEL_STATE();
  const uint8_t* p = ks ? ks->memory()->TranslateVirtual<const uint8_t*>(a) : nullptr;
  return p && Readable(p, n);
}

const char* CodeName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (a file the game was reading went away)";
    case EXCEPTION_BREAKPOINT: return "breakpoint (a failed assertion)";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating-point operation";
    case EXCEPTION_FLT_OVERFLOW: return "floating-point overflow";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
    case 0xC0000017: return "out of memory";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun (fail fast)";
    case 0xE06D7363: return "C++ exception";
  }
  return "";
}

// where a host address is: "mk9.exe+0x1234"
void ModuleOf(uintptr_t a, char* out, size_t n) {
  HMODULE m = nullptr;
  wchar_t path[MAX_PATH];
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(a), &m) &&
      m && GetModuleFileNameW(m, path, MAX_PATH)) {
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; c++)
      if (*c == L'\\' || *c == L'/') name = c + 1;
    char narrow[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, nullptr, nullptr)) narrow[0] = 0;
    std::snprintf(out, n, "%s+0x%llx", narrow, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
  } else {
    std::snprintf(out, n, "0x%016llx", static_cast<unsigned long long>(a));
  }
}

// the start of the host function a host address is in, from the unwind tables (0 for a function
// without any: a leaf); a function split in parts is followed back to its first part
uintptr_t FunctionStart(uintptr_t pc) {
  DWORD64 image = 0;
  PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &image, nullptr);
  for (int i = 0; f && i < 16; i++) {
    const auto* info = reinterpret_cast<const uint8_t*>(image + f->UnwindData);
    if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) break;
    const unsigned codes = (info[2] + 1u) & ~1u;
    f = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info + 4 + 2 * codes));
  }
  return f ? uintptr_t(image + f->BeginAddress) : 0;
}

// the guest function behind a host address in mk9.exe: " sub_826D91C0+0x24" when the host function
// the address is in is a recompiled one, " near sub_826D91C0+0x24?" when the address is in a host
// function without unwind data (most likely the recompiled one before it), nothing for host code
void GuestOf(uintptr_t pc, char* out, size_t n) {
  out[0] = 0;
  if (g_funcs.empty() || pc < g_exe_lo || pc >= g_exe_hi) return;
  const uintptr_t start = FunctionStart(pc);
  const uintptr_t key = start ? start : pc;
  auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), key,
                             [](uintptr_t v, const HostFunc& f) { return v < f.host; });
  if (it == g_funcs.begin()) return;
  --it;
  const auto off = static_cast<unsigned long long>(pc - it->host);
  if (start && it->host == start)
    std::snprintf(out, n, " sub_%08X+0x%llx", it->guest, off);
  else if (!start && off < 0x10000)
    std::snprintf(out, n, " near sub_%08X+0x%llx?", it->guest, off);
}

void Where(uintptr_t pc, char* out, size_t n) {
  char module[MAX_PATH + 32], guest[64];
  ModuleOf(pc, module, sizeof module);
  GuestOf(pc, guest, sizeof guest);
  std::snprintf(out, n, "%s%s", module, guest);
}

void ThreadName(DWORD tid, char* out, size_t n) {
  out[0] = 0;
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get = reinterpret_cast<GetDescription>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  if (!get) return;
  HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!t) return;
  PWSTR name = nullptr;
  if (SUCCEEDED(get(t, &name)) && name) {
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, out, int(n), nullptr, nullptr)) out[0] = 0;
    LocalFree(name);
  }
  CloseHandle(t);
}

void HostStack(Out& out, CONTEXT ctx, ULONG_PTR lo, ULONG_PTR hi) {
  out("host call stack:\n");
  for (int i = 0; i < 48 && ctx.Rip; i++) {
    char where[MAX_PATH + 96];
    Where(uintptr_t(i ? ctx.Rip - 1 : ctx.Rip), where, sizeof where);  // a return address: its call
    out("  %2d %s\n", i, where);
    if (ctx.Rsp < lo || ctx.Rsp + 8 > hi || !Readable(reinterpret_cast<void*>(ctx.Rsp), 64)) break;
    DWORD64 image = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
    if (!f) {  // a leaf: its return address is on top of the stack
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    } else {
      void* handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, f, &ctx, &handler_data, &frame, nullptr);
    }
  }
}

void GuestPart(Out& out, const PPCContext* g) {
  if (!g || !Readable(g, sizeof *g)) {
    out("guest: the crashed thread is not a guest thread\n");
    return;
  }
  const PPCRegister* r[32] = {&g->r0,  &g->r1,  &g->r2,  &g->r3,  &g->r4,  &g->r5,  &g->r6,  &g->r7,
                              &g->r8,  &g->r9,  &g->r10, &g->r11, &g->r12, &g->r13, &g->r14, &g->r15,
                              &g->r16, &g->r17, &g->r18, &g->r19, &g->r20, &g->r21, &g->r22, &g->r23,
                              &g->r24, &g->r25, &g->r26, &g->r27, &g->r28, &g->r29, &g->r30, &g->r31};
  out("guest registers: lr %08X ctr %08X\n", uint32_t(g->lr), g->ctr.u32);
  for (int i = 0; i < 32; i += 8)
    out("  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, r[i]->u32, r[i + 1]->u32, r[i + 2]->u32,
        r[i + 3]->u32, r[i + 4]->u32, r[i + 5]->u32, r[i + 6]->u32, r[i + 7]->u32);
  // the guest stack's bounds from the thread's PCR (r13), all of it readable, before walking it
  const uint32_t pcr = g->r13.u32;
  if (!pcr || !GuestReadable(pcr + 0x70, 8)) return;
  const uint32_t hi = game::ReadU32(pcr + 0x70), lo = game::ReadU32(pcr + 0x74);
  if (hi <= lo || hi - lo > (64u << 20) || !GuestReadable(lo, hi - lo)) return;
  uint32_t returns[24];
  const int n = game::StackReturns(*g, returns, 24);
  out("guest call stack (return addresses, innermost first):");
  for (int i = 0; i < n; i++) out("%s %08X", i % 8 ? "" : "\n ", returns[i]);
  out("\n");
}

void GamePart(Out& out) {
  const uint64_t frames = frame::Count();
  out("frames run: %llu\n", static_cast<unsigned long long>(frames));
  for (int i = 0; i < int(Note::Count); i++) {
    char note[sizeof g_notes[0]];
    std::memcpy(note, g_notes[i], sizeof note);
    note[sizeof note - 1] = 0;
    static const char* const names[] = {"launcher link status", "match"};
    if (note[0]) out("%s: %s\n", names[i], note);
  }
  const net::Status st = net::GetStatus();
  if (st != net::Status::Off) {
    const net::Stats s = net::GetStats();
    out("netplay: %s, frame %u, ping %d ms, %u rollbacks (longest %u frames), %u waits\n", net::StatusName(st),
        net::Frame(), s.ping_ms, s.rollbacks, s.max_rollback, s.waits);
  }
  if (frames > 0)  // the image is loaded and runs: its globals are there
    out("game: phase %d, screen 0x%X, round %d, timer %d, rounds won %d-%d\n", game::Phase(), game::UiScreen(),
        game::Round(), game::RoundTimer(), game::RoundsWon(0), game::RoundsWon(1));
}

void Modules(Out& out) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;
  out("modules:\n");
  MODULEENTRY32W me{};
  me.dwSize = sizeof me;
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    char name[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof name, nullptr, nullptr)) continue;
    out("  %016llx %8lx %s\n", reinterpret_cast<unsigned long long>(me.modBaseAddr), me.modBaseSize, name);
  }
  CloseHandle(snap);
}

HANDLE CreateIn(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  if (std::swprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, name) < 0) return INVALID_HANDLE_VALUE;
  return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// never inlined into its callers: its buffers would make the crashed thread's frame too large for what
// a stack overflow leaves it
[[gnu::noinline]] void WriteReport(Request& q) {
  CreateDirectoryW(g_dir, nullptr);
  HANDLE f = CreateIn(L"crash.txt");
  if (f != INVALID_HANDLE_VALUE) {
    Out out(f);
    const time_t now = std::time(nullptr);
    char when[64] = "";
    tm local{};
    if (localtime_s(&local, &now) == 0) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
    char thread[128];
    ThreadName(q.tid, thread, sizeof thread);
    out("MK9 Online game crash\n");
    out("game %s, crashed %s, %lld s after it started\n", MK9_VERSION, when, static_cast<long long>(now - g_started));
    out("what: %s\n", q.kind);
    const EXCEPTION_RECORD* rec = q.ep ? q.ep->ExceptionRecord : nullptr;
    if (rec) {
      char where[MAX_PATH + 96];
      Where(reinterpret_cast<uintptr_t>(rec->ExceptionAddress), where, sizeof where);
      out("exception %08lX %s at %s\n", rec->ExceptionCode, CodeName(rec->ExceptionCode), where);
      if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
          rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        out("  %s address 0x%llx\n", op == 0 ? "reading" : op == 1 ? "writing" : "executing",
            static_cast<unsigned long long>(rec->ExceptionInformation[1]));
      }
    }
    out("thread %lu%s%s%s\n", q.tid, thread[0] ? " \"" : "", thread, thread[0] ? "\"" : "");
    if (q.ep && q.ep->ContextRecord) {
      const CONTEXT& c = *q.ep->ContextRecord;
      out("host registers:\n");
      out("  rip %016llx rsp %016llx rbp %016llx\n", c.Rip, c.Rsp, c.Rbp);
      out("  rax %016llx rbx %016llx rcx %016llx rdx %016llx\n", c.Rax, c.Rbx, c.Rcx, c.Rdx);
      out("  rsi %016llx rdi %016llx r8  %016llx r9  %016llx\n", c.Rsi, c.Rdi, c.R8, c.R9);
      out("  r10 %016llx r11 %016llx r12 %016llx r13 %016llx\n", c.R10, c.R11, c.R12, c.R13);
      out("  r14 %016llx r15 %016llx\n", c.R14, c.R15);
      HostStack(out, c, q.stack_lo, q.stack_hi);
    }
    GuestPart(out, q.guest);
    GamePart(out);
    Modules(out);
    FlushFileBuffers(f);
    CloseHandle(f);
  }
  // the dump: the threads' stacks and what they point at, for a debugger
  HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
  using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
  const auto write =
      dbghelp ? reinterpret_cast<WriteDump>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
              : nullptr;
  HANDLE d = write ? CreateIn(L"crash.dmp") : INVALID_HANDLE_VALUE;
  if (d != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{q.tid, q.ep, FALSE};
    write(GetCurrentProcess(), GetCurrentProcessId(), d,
          MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                        MiniDumpWithUnloadedModules),
          q.ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(d);
  }
}

DWORD WINAPI Writer(void*) {
  g_writer_tid = GetCurrentThreadId();
  WaitForSingleObject(g_request, INFINITE);
  WriteReport(g_req);
  SetEvent(g_done);
  return 0;
}

// the crashed thread: hands its crash over and waits for the report, then the process ends (with as little
// stack as it can: after a stack overflow there is little left)
[[noreturn]] [[gnu::noinline]] void Crashed(EXCEPTION_POINTERS* ep, const char* kind, DWORD exit_code) {
  const DWORD me = GetCurrentThreadId();
  Request& q = g_req;
  // the report itself crashed: what it wrote so far stays
  if (me == g_writer_tid) TerminateProcess(GetCurrentProcess(), q.exit_code);
  DWORD none = 0;
  if (!g_owner.compare_exchange_strong(none, me)) {
    // another thread's crash is being written, and that thread ends the process with its code (or this
    // thread crashed writing its own report)
    if (none != me && g_done) {
      WaitForSingleObject(g_done, 60000);
      Sleep(2000);
    }
    TerminateProcess(GetCurrentProcess(), none == me ? q.exit_code : exit_code);
  }
  q.kind = kind;
  q.tid = me;
  q.exit_code = exit_code;
  if (!ep) {  // abort() and the like: where it was called from stands in for the exception
    RtlCaptureContext(&q.context);
    q.record.ExceptionCode = 0xE0000000u | exit_code;
    q.record.ExceptionAddress = reinterpret_cast<void*>(q.context.Rip);
    q.pointers = {&q.record, &q.context};
    ep = &q.pointers;
  }
  q.ep = ep;
  const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  q.stack_lo = reinterpret_cast<ULONG_PTR>(tib->StackLimit);
  q.stack_hi = reinterpret_cast<ULONG_PTR>(tib->StackBase);
  const auto* ts = rex::runtime::ThreadState::Get();
  q.guest = ts ? ts->context() : nullptr;
  if (g_writer) {
    SetEvent(g_request);
    WaitForSingleObject(g_done, 60000);
  } else {
    WriteReport(q);
  }
  TerminateProcess(GetCurrentProcess(), exit_code);
  for (;;) Sleep(INFINITE);
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
  Crashed(ep, "an exception nothing handled", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 1);
}

// A stack overflow leaves its thread a few kilobytes of stack, too few to get past the other handlers
// (the SDK's copies every register first) to the unhandled exception filter: the thread dies at once of a
// second one. First in line, this hands it over with next to no stack.
LONG CALLBACK OnStackOverflow(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
  Crashed(ep, "a stack overflow", EXCEPTION_STACK_OVERFLOW);
}

void OnAbort(int) { Crashed(nullptr, "abort()", kAbortCode); }
void OnTerminate() { Crashed(nullptr, "std::terminate (a C++ exception nothing caught)", kAbortCode); }
void OnPurecall() { Crashed(nullptr, "a pure virtual function call", kAbortCode); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
  Crashed(nullptr, "an invalid parameter to a C runtime function", kAbortCode);
}

[[maybe_unused]] const bool g_at_start = (Install(), true);

// Guest threads (the SDK gives each a 16 MB host stack, committed all at once) keep 64 KB of it back for
// a stack overflow's report: an overflow left them no stack at all to hand it over with. Run on every
// thread as it starts (a TLS callback), they are the ones with stacks that large.
void NTAPI OnThreadStart(PVOID, DWORD reason, PVOID) {
  if (reason != DLL_THREAD_ATTACH) return;
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  if (hi - lo < (4u << 20)) return;
  ULONG keep = 64 * 1024;
  SetThreadStackGuarantee(&keep);
}

}  // namespace

void Install() {
  if (!g_installed.exchange(true)) {
    g_started = std::time(nullptr);
    // the launcher's session folder, else crashes\ beside the exe
    const DWORD n = GetEnvironmentVariableW(L"MK9_SESSION_DIR", g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
      wchar_t exe[MAX_PATH];
      DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
      while (len && exe[len - 1] != L'\\' && exe[len - 1] != L'/') len--;
      exe[len] = 0;
      std::swprintf(g_dir, MAX_PATH, L"%lscrashes", exe);
    }
    if (const char* v = std::getenv("MK9_CRASH_TEST")) {
      char* rest = nullptr;
      g_test_frame = std::strtoull(v, &rest, 10);
      if (rest && *rest == ':') std::snprintf(g_test_kind, sizeof g_test_kind, "%s", rest + 1);
    }
    HMODULE exe = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
    g_exe_lo = reinterpret_cast<uintptr_t>(exe);
    g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; m++)
      g_funcs.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});
    std::sort(g_funcs.begin(), g_funcs.end(), [](const HostFunc& a, const HostFunc& b) { return a.host < b.host; });
    g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_request && g_done) g_writer = CreateThread(nullptr, 256 * 1024, Writer, nullptr, 0, nullptr);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no dialog, no fail fast past the handler
  }
  SetUnhandledExceptionFilter(OnUnhandled);
  if (g_veh) RemoveVectoredExceptionHandler(g_veh);
  g_veh = AddVectoredExceptionHandler(1, OnStackOverflow);  // ahead of the SDK's, installed meanwhile
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
  _set_purecall_handler(OnPurecall);
  _set_invalid_parameter_handler(OnInvalidParameter);
}

void SetNote(Note which, const char* text) {
  char* to = g_notes[int(which)];
  const size_t n = std::min(std::strlen(text), sizeof g_notes[0] - 1);
  std::memcpy(to, text, n);
  to[n] = 0;
}

namespace {
// tests: a stack overflow (each call keeps a page of its own)
[[gnu::noinline]] int Deeper(volatile int depth) {
  volatile char page[4096];
  page[0] = char(depth);
  return depth > 1000000 ? 0 : Deeper(depth + 1) + page[0];
}
}  // namespace

void OnFrame(uint64_t frame) {
  if (!g_test_frame || frame != g_test_frame) return;
  REXLOG_ERROR("mk9: crashing on purpose at frame {} (MK9_CRASH_TEST, {})", frame,
               g_test_kind[0] ? g_test_kind : "access violation");
  if (std::strcmp(g_test_kind, "abort") == 0) std::abort();
  if (std::strcmp(g_test_kind, "overflow") == 0) Deeper(0);
  if (std::strcmp(g_test_kind, "throw") == 0) throw std::runtime_error("MK9_CRASH_TEST");
  *g_nowhere = 1;
}

}  // namespace mk9::crash

#pragma section(".CRT$XLM", long, read)
extern "C" __declspec(allocate(".CRT$XLM")) const PIMAGE_TLS_CALLBACK mk9_crash_thread_start =
    mk9::crash::OnThreadStart;
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:mk9_crash_thread_start")
