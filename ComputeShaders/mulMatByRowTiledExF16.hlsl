// FP16 math variant of mulMatByRowTiledEx, used on AMD GPUs with reshaped panels.
// The body lives in mulMatByRowTiledEx.hlsl, guarded by F16_MATH.
#define F16_MATH 1
#include "mulMatByRowTiledEx.hlsl"