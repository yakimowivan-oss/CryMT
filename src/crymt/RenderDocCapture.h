// RenderDoc frame captures from inside CryMT (CRYMT_RENDERDOC=1: <CryMT>\renderdoc\renderdoc.dll, or =<folder or dll>).
#pragma once

namespace rdoc
{
bool Requested();      // CRYMT_RENDERDOC is set (not "0"): pass names are recorded for the capture
bool Load();           // loads renderdoc.dll; call before the D3D12 device and the DXGI factory exist
bool Loaded();         // RenderDoc is in the process and answered its API
void Trigger();        // capture the next presented frame into <CryMT>\logs\renderdoc
void Poll(bool focus); // PgUp (main thread, once a frame)
} // namespace rdoc
