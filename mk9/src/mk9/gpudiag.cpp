// What the graphics card was doing when it was lost. See gpudiag.h.
//
// DRED (D3D12's Device Removed Extended Data) keeps breadcrumbs around each GPU operation and asks the driver for page
// fault addresses. Its settings are the process's and hold for every device made after them, so it goes on before the
// SDK makes its device. The SDK turned it on only with its d3d12_debug option, so that seven reports of
// DXGI_ERROR_DEVICE_HUNG from one player's Radeon RX 6600 (0.5.2 to 0.5.5) said nothing of what the card was doing. On a
// Radeon iGPU it costs nothing measurable (the same menus, just under the cap: 54.6 frames/s with it, 54.5 and 54.6
// without). Only Radeons have it: the one kind that has been lost so far, and the one whose faults end the game at once.

#include "mk9/gpudiag.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>

using Microsoft::WRL::ComPtr;

namespace mk9::gpudiag {
namespace {

constexpr UINT kVendorAmd = 0x1002;

// the card the SDK draws on, with its driver
char g_card[192] = "?";
bool g_dred_on = false;
std::atomic<ID3D12Device*> g_device{nullptr};

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
    case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME: return "DecodeFrame";
    case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES: return "ProcessFrames";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT: return "AtomicCopyBufferUINT";
    case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64: return "AtomicCopyBufferUINT64";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION: return "ResolveSubresourceRegion";
    case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
    case D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION: return "SetProtectedResourceSession";
    case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return "InitializeMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "ExecuteMetaCommand";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
    case static_cast<D3D12_AUTO_BREADCRUMB_OP>(45): return "Barrier";
    case static_cast<D3D12_AUTO_BREADCRUMB_OP>(46): return "BeginCommandList";
    default: return nullptr;
  }
}

const char* AllocationName(D3D12_DRED_ALLOCATION_TYPE type) {
  switch (type) {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "command queue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline state";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "command signature";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
    default: return "other";
  }
}

const char* ReasonName(HRESULT reason) {
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL";
    default: return "?";
  }
}

const char* StateName(D3D12_DRED_DEVICE_STATE state) {
  switch (state) {
    case D3D12_DRED_DEVICE_STATE_HUNG: return "hung (a command ran too long)";
    case D3D12_DRED_DEVICE_STATE_FAULT: return "fault (the GPU stopped on an error, no long wait)";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "page fault (the GPU touched memory that is not there)";
    default: return "unknown";
  }
}

class Printer {
 public:
  Printer(Line line, void* context) : line_(line), context_(context) {}
  [[gnu::format(printf, 2, 3)]] void operator()(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    line_(context_, text);
  }

 private:
  Line line_;
  void* context_;
};

void Name(char* out, size_t n, const char* narrow, const wchar_t* wide) {
  out[0] = 0;
  if (narrow && narrow[0]) {
    snprintf(out, n, "%s", narrow);
  } else if (wide && wide[0]) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, int(n), nullptr, nullptr);
    out[n - 1] = 0;
  }
}

void Allocations(Printer& print, const char* what, const D3D12_DRED_ALLOCATION_NODE1* node) {
  for (int i = 0; node && i < 12; node = node->pNext, i++) {
    char name[128];
    Name(name, sizeof name, node->ObjectNameA, node->ObjectNameW);
    print("  %s there: %s%s%s\n", what, AllocationName(node->AllocationType), name[0] ? " " : "", name);
  }
}

// one command list's operations around where the GPU stood
void Operations(Printer& print, const D3D12_AUTO_BREADCRUMB_NODE1& node) {
  const UINT done = *node.pLastBreadcrumbValue, count = node.BreadcrumbCount;
  const UINT first = done > 6 ? done - 6 : 0, last = std::min(done + 4, count);
  for (UINT i = first; i < last; i++) {
    const D3D12_AUTO_BREADCRUMB_OP op = node.pCommandHistory[i];
    const char* name = OpName(op);
    char number[16];
    if (!name) {
      snprintf(number, sizeof number, "op %d", int(op));
      name = number;
    }
    print("    %s %u %s\n", i == done ? "->" : "  ", i, name);
  }
}

// The lists are kept oldest first. Several wait behind the one the GPU stood in (up to three frames' worth), each named
// with how far it got; the ones it had started show their operations, and when none had, the first and last of those
// waiting do. A Radeon's count lags: a fault tried on one left the list that faulted at operation 0, or counted as
// finished, so the last list finished shows its end too.
void Breadcrumbs(Printer& print, const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1& crumbs) {
  const D3D12_AUTO_BREADCRUMB_NODE1* waiting[24];
  const D3D12_AUTO_BREADCRUMB_NODE1* last_finished = nullptr;
  int count = 0, finished = 0, started = 0;
  for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
    if (!node->pLastBreadcrumbValue || !node->pCommandHistory) continue;
    if (*node->pLastBreadcrumbValue >= node->BreadcrumbCount) {
      finished++;
      last_finished = node;
      continue;
    }
    if (*node->pLastBreadcrumbValue) started++;
    if (count < int(sizeof waiting / sizeof waiting[0])) waiting[count++] = node;
  }
  print("  command lists kept: %d finished, %d not (%d of them started)\n", finished, count, started);
  if (last_finished) {
    print("  the last finished: %u operations\n", last_finished->BreadcrumbCount);
    Operations(print, *last_finished);
  }
  for (int i = 0; i < count; i++) {
    const D3D12_AUTO_BREADCRUMB_NODE1& node = *waiting[i];
    const UINT done = *node.pLastBreadcrumbValue;
    char list[128], queue[128];
    Name(list, sizeof list, node.pCommandListDebugNameA, node.pCommandListDebugNameW);
    Name(queue, sizeof queue, node.pCommandQueueDebugNameA, node.pCommandQueueDebugNameW);
    print("  list %d%s%s%s%s: operation %u of %u\n", i, list[0] ? " " : "", list, queue[0] ? ", queue " : "", queue, done,
          node.BreadcrumbCount);
    if (done || (!started && (i == 0 || i == count - 1))) Operations(print, node);
  }
}

// "AMD Radeon RX 6600 (vendor 0x1002, device 0x73FF), driver 32.0.21030.2001"
void Describe(IDXGIAdapter1* adapter, const DXGI_ADAPTER_DESC1& desc, char* out, size_t n) {
  char name[128] = "";
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
  name[sizeof name - 1] = 0;
  LARGE_INTEGER version{};
  if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
    snprintf(out, n, "%s (vendor 0x%04X, device 0x%04X), driver %u.%u.%u.%u", name, desc.VendorId, desc.DeviceId,
             unsigned(HIWORD(version.HighPart)), unsigned(LOWORD(version.HighPart)), unsigned(HIWORD(version.LowPart)),
             unsigned(LOWORD(version.LowPart)));
  } else {
    snprintf(out, n, "%s (vendor 0x%04X, device 0x%04X)", name, desc.VendorId, desc.DeviceId);
  }
}

// The card the SDK will draw on: d3d12_adapter's number, else the first in Windows' list that is not software
void FindCard(UINT* vendor) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  const int32_t want = REXCVAR_GET(d3d12_adapter);
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++, adapter.Reset()) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    if (want >= 0 ? int32_t(i) != want : (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
    Describe(adapter.Get(), desc, g_card, sizeof g_card);
    *vendor = desc.VendorId;
    return;
  }
}

void EnableDred() {
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  if (!d3d12) return;
  const auto get =
      reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12GetDebugInterface")));
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
  if (!get || FAILED(get(IID_PPV_ARGS(&dred)))) return;
  dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  g_dred_on = true;
}

}  // namespace

void BeforeDevice() {
  UINT vendor = 0;
  FindCard(&vendor);
  // MK9_NO_DRED=1 and MK9_DRED=1, comparing what it costs
  if (!std::getenv("MK9_NO_DRED") && (vendor == kVendorAmd || std::getenv("MK9_DRED"))) EnableDred();
}

void SetDevice(void* d3d12_device) {
  g_device = static_cast<ID3D12Device*>(d3d12_device);
  REXLOG_INFO("mk9 gpu: drawing on {}; the card's own crash data (DRED) is {}", g_card, g_dred_on ? "on" : "off");
}

void LogAdapters() {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++, adapter.Reset()) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    char text[sizeof g_card];
    Describe(adapter.Get(), desc, text, sizeof text);
    REXLOG_INFO("mk9 gpu: adapter {}: {}, {} MB of its own", i, text, desc.DedicatedVideoMemory >> 20);
  }
}

void RemoveDeviceForTest() {
  ComPtr<ID3D12Device5> device5;
  if (ID3D12Device* device = g_device.load(); device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))))
    device5->RemoveDevice();
}

void Report(Line line, void* context) {
  ID3D12Device* device = g_device.load();
  if (!device) return;
  const HRESULT reason = device->GetDeviceRemovedReason();
  if (SUCCEEDED(reason)) return;
  Printer print(line, context);
  print("graphics card lost: 0x%08lX %s\n", static_cast<unsigned long>(reason), ReasonName(reason));
  print("  the card: %s\n", g_card);
  if (!g_dred_on) {
    print("  DRED was off\n");
    return;
  }
  ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2)))) {
    print("  the GPU's state: %s\n", StateName(dred2->GetDeviceState()));
    dred1 = dred2;
  } else if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred1)))) {
    print("  no DRED data from this Windows\n");
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
  const HRESULT got = dred1->GetAutoBreadcrumbsOutput1(&crumbs);
  if (SUCCEEDED(got)) Breadcrumbs(print, crumbs);
  else print("  no breadcrumbs: 0x%08lX\n", static_cast<unsigned long>(got));
  if (dred2) {
    D3D12_DRED_PAGE_FAULT_OUTPUT2 fault{};
    if (SUCCEEDED(dred2->GetPageFaultAllocationOutput2(&fault)) && fault.PageFaultVA) {
      print("  page fault at GPU address 0x%016llX, flags 0x%X\n", static_cast<unsigned long long>(fault.PageFaultVA),
            static_cast<unsigned>(fault.PageFaultFlags));
      Allocations(print, "still", fault.pHeadExistingAllocationNode);
      Allocations(print, "freed lately", fault.pHeadRecentFreedAllocationNode);
    }
  } else {
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
      print("  page fault at GPU address 0x%016llX\n", static_cast<unsigned long long>(fault.PageFaultVA));
      Allocations(print, "still", fault.pHeadExistingAllocationNode);
      Allocations(print, "freed lately", fault.pHeadRecentFreedAllocationNode);
    }
  }
}

}  // namespace mk9::gpudiag
