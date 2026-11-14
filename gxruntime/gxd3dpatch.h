#ifndef GXD3DPATCH_H
#define GXD3DPATCH_H

// Lifts the 2048x2048 render target limit of the Direct3D 7 runtime (d3dim700.dll) in memory,
// for the current process only. Call once before DirectDraw is used. Returns true if patched.
bool gxD3DUnlockResolution();

#endif
