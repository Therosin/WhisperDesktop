#pragma once
#include "../API/MfStructs.h"

namespace Whisper
{
	struct iAudioCapture;

	// Open the WASAPI loopback capture on the default render device ("System Audio", the same data
	// that goes into the speakers). The returned object implements both iAudioCapture and the
	// optional iWasmCapture interface, and delivers float PCM resampled to SAMPLE_RATE.
	HRESULT __stdcall captureOpenSystemAudio( const sCaptureParams& captureParams, iAudioCapture** pp ) noexcept;
}