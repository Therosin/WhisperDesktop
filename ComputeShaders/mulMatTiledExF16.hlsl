// FP16 math variant of mulMatTiledEx, used on AMD GPUs with reshaped panels.
// The body lives in mulMatTiledEx.hlsl, guarded by F16_MATH.
#define F16_MATH 1
#include "mulMatTiledEx.hlsl"