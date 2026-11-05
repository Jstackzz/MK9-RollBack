#pragma once
// What the graphics card was doing when it was lost: DRED on for Radeons (MK9_DRED=1 turns it on, MK9_NO_DRED=1 off), each
// adapter's driver version in the log, and the data a lost device leaves in crash.txt: whether the GPU hung or faulted,
// the operations around where it stood, a page fault's address and what was or lately had been there.

namespace mk9::gpudiag {

// before the runtime makes its device, with d3d12_adapter settled
void BeforeDevice();

// the D3D12 device (ID3D12Device*), once the runtime has made it
void SetDevice(void* d3d12_device);

// every adapter with its driver version, to the log
void LogAdapters();

// from the crash writer: `line` gets each line of the report, nothing when the device is fine
using Line = void (*)(void* context, const char* text);
void Report(Line line, void* context);

// tests (MK9_CRASH_TEST=<frame>:gpu): the device removed, as a lost card leaves it
void RemoveDeviceForTest();

}  // namespace mk9::gpudiag
