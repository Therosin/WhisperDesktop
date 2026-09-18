// FP16 math variant of mulMatByRowTiled (the #2 most expensive shader, matrix*row product).
// The body lives in mulMatByRowTiled.hlsl, guarded by F16_MATH.
#define F16_MATH 1
#include "mulMatByRowTiled.hlsl"