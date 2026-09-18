#pragma once
#include <stdint.h>
#include <stddef.h>
#include "MfStructs.h"
struct IMFSourceReader;

namespace Whisper
{
	__interface __declspec( novtable, uuid( "013583aa-c9eb-42bc-83db-633c2c317051" ) ) iAudioBuffer : public IUnknown
	{
		uint32_t __stdcall countSamples() const;
		const float* __stdcall getPcmMono() const;
		const float* __stdcall getPcmStereo() const;
		HRESULT __stdcall getTime( int64_t& rdi ) const;
	};

	__interface __declspec( novtable, uuid( "35b988da-04a6-476a-a193-d8891d5dc390" ) ) iAudioReader : public IUnknown
	{
		HRESULT __stdcall getDuration( int64_t& rdi ) const;
		HRESULT __stdcall getReader( IMFSourceReader** pp ) const;
		HRESULT __stdcall requestedStereo() const;
	};

	__interface __declspec( novtable, uuid( "747752c2-d9fd-40df-8847-583c781bf013" ) ) iAudioCapture : public IUnknown
	{
		HRESULT __stdcall getReader( IMFSourceReader** pp ) const;
		const sCaptureParams& __stdcall getParams() const;
	};

	// Optional interface implemented by the WASAPI loopback capture object, see captureOpenSystemAudio().
	// The object returns PCM resampled to SAMPLE_RATE float, already downmixed to 1 or 2 channels,
	// so it can be fed into the regular audio capture pipeline without Media Foundation.
	__interface __declspec( novtable, uuid( "efb26d62-9f6f-4a71-9b1e-7d54d5f3642b" ) ) iWasmCapture : public IUnknown
	{
		// Number of channels in the PCM data returned from getSample(), always 1 or 2.
		uint32_t __stdcall channels() const;
		// Pull the next chunk of interleaved float PCM at SAMPLE_RATE. The returned buffer is owned by the object,
		// and remains valid until the next call. The method blocks until a non-empty chunk is available.
		HRESULT __stdcall getSample( const float** pp, size_t& countFloats );
	};

	__interface __declspec( novtable, uuid( "fb9763a5-d77d-4b6e-aff8-f494813cebd8" ) ) iMediaFoundation : public IUnknown
	{
		HRESULT __stdcall loadAudioFile( LPCTSTR path, bool stereo, iAudioBuffer** pp ) const;
		HRESULT __stdcall openAudioFile( LPCTSTR path, bool stereo, iAudioReader** pp );
		HRESULT __stdcall loadAudioFileData( const void* data, uint64_t size, bool stereo, iAudioReader** pp );

		HRESULT __stdcall listCaptureDevices( pfnFoundCaptureDevices pfn, void* pv );
		HRESULT __stdcall openCaptureDevice( LPCTSTR endpoint, const sCaptureParams& captureParams, iAudioCapture** pp );
	};

	HRESULT __stdcall initMediaFoundation( iMediaFoundation** pp );
}