// FP16 math variant of mulMatTiled (the main matrix*matrix product).
// Feeds the uncompressed FP16 weights into the FP16 FMA units, 2x speed on modern GPUs.
// The body lives in mulMatTiled.hlsl, guarded by F16_MATH.
#define F16_MATH 1
#include "mulMatTiled.hlsl"