#include "stdafx.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "wasapiCapture.h"
#include "../API/iMediaFoundation.cl.h"
#include "../ComLightLib/comLightServer.h"
#include "../Whisper/audioConstants.h"
#pragma comment(lib, "ole32.lib")

namespace
{
	using namespace Whisper;

	class WasapiCapture : public ComLight::ObjectRoot<iAudioCapture>, public iWasmCapture
	{
		BEGIN_COM_MAP()
			COM_INTERFACE_ENTRY( iWasmCapture )
		END_COM_MAP()

		sCaptureParams captureParams;
		bool wantStereo = false;
		bool comInitialized = false;
		bool started = false;

		uint32_t mixRate = 0;
		uint32_t mixChannels = 0;
		uint32_t targetChannels = 0;

		std::vector<float> result;

		CComPtr<IAudioClient> audioClient;
		CComPtr<IAudioCaptureClient> captureClient;

		// ==== iAudioCapture ====
		HRESULT COMLIGHTCALL getReader( IMFSourceReader** pp ) const noexcept override final
		{
			if( nullptr == pp )
				return E_POINTER;
			*pp = nullptr;
			return E_NOTIMPL;
		}
		const sCaptureParams& COMLIGHTCALL getParams() const noexcept override final
		{
			return captureParams;
		}

		// ==== iWasmCapture ====
		// The audio engine converts the device mix (typically 48 kHz / 2ch) to SAMPLE_RATE,
		// targetChannels float PCM via AUTOCONVERTPCM + SRC_DEFAULT_QUALITY, so the buffer
		// is already interleaved 1 or 2 channels at 16 kHz.
		uint32_t COMLIGHTCALL channels() const noexcept override final
		{
			return targetChannels;
		}
		HRESULT COMLIGHTCALL getSample( const float** pp, size_t& countFloats ) noexcept override final;

	public:
		~WasapiCapture()
		{
			if( started && audioClient )
				audioClient->Stop();
			if( comInitialized )
				CoUninitialize();
		}
		HRESULT open( const sCaptureParams& cp );
	};

	HRESULT COMLIGHTCALL WasapiCapture::getSample( const float** pp, size_t& countFloats ) noexcept
	{
		if( nullptr == pp )
			return E_POINTER;
		*pp = nullptr;
		countFloats = 0;
		if( nullptr == captureClient )
			return OLE_E_BLANK;

		for( ;; )
		{
			UINT32 packetSize = 0;
			HRESULT hr = captureClient->GetNextPacketSize( &packetSize );
			if( FAILED( hr ) )
			{
				logErrorHr( hr, u8"IAudioCaptureClient.GetNextPacketSize" );
				return hr;
			}
			if( 0 == packetSize )
			{
				// No new capture data yet; wait a little before polling again
				Sleep( 10 );
				continue;
			}

			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD dwFlags = 0;
			UINT64 deviceFrames = 0, qpcFrames = 0;
			hr = captureClient->GetBuffer( &data, &frames, &dwFlags, &deviceFrames, &qpcFrames );
			if( FAILED( hr ) )
			{
				logErrorHr( hr, u8"IAudioCaptureClient.GetBuffer" );
				return hr;
			}

			const size_t count = (size_t)frames * targetChannels;
			const bool silent = 0 != ( dwFlags & AUDCLNT_BUFFERFLAGS_SILENT );
			if( silent || nullptr == data )
			{
				result.assign( count, 0.0f );
			}
			else
			{
				result.assign( (const float*)data, (const float*)data + count );
			}

			hr = captureClient->ReleaseBuffer( frames );
			if( FAILED( hr ) )
			{
				logErrorHr( hr, u8"IAudioCaptureClient.ReleaseBuffer" );
				return hr;
			}

			countFloats = result.size();
			*pp = result.data();
			return S_OK;
		}
	}

	HRESULT WasapiCapture::open( const sCaptureParams& cp )
	{
		captureParams = cp;
		wantStereo = 0 != ( cp.flags & (uint32_t)eCaptureFlags::Stereo );

		const HRESULT coHr = CoInitializeEx( nullptr, COINIT_MULTITHREADED );
		if( FAILED( coHr ) && RPC_E_CHANGED_MODE != coHr )
			return coHr;
		comInitialized = ( SUCCEEDED( coHr ) && RPC_E_CHANGED_MODE != coHr );

		CComPtr<IMMDeviceEnumerator> enumerator;
		CHECK( CoCreateInstance( __uuidof( MMDeviceEnumerator ), nullptr, CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), (void**)&enumerator ) );

		CComPtr<IMMDevice> device;
		HRESULT hr = enumerator->GetDefaultAudioEndpoint( eRender, eMultimedia, &device );
		if( FAILED( hr ) )
			CHECK( enumerator->GetDefaultAudioEndpoint( eRender, eConsole, &device ) );

		CHECK( device->Activate( __uuidof( IAudioClient ), CLSCTX_ALL, nullptr, (void**)&audioClient ) );

		// Read the mix format just to report it and decide whether stereo data is available
		WAVEFORMATEX* pFormat = nullptr;
		CHECK( audioClient->GetMixFormat( &pFormat ) );
		mixRate = pFormat->nSamplesPerSec;
		mixChannels = pFormat->nChannels;
		CoTaskMemFree( pFormat );

		targetChannels = ( wantStereo && mixChannels >= 2 ) ? 2 : 1;

		WAVEFORMATEX desired = {};
		desired.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		desired.nChannels = (WORD)targetChannels;
		desired.nSamplesPerSec = SAMPLE_RATE;
		desired.nBlockAlign = (WORD)( 4 * targetChannels );
		desired.nAvgBytesPerSec = SAMPLE_RATE * desired.nBlockAlign;
		desired.wBitsPerSample = 32;

		// Let the audio engine do the sample-rate conversion and channel downmix
		const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
			AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
			AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
		CHECK( audioClient->Initialize( AUDCLNT_SHAREMODE_SHARED, flags, 10'000'000 / 10, 0, &desired, nullptr ) );

		CHECK( audioClient->GetService( __uuidof( IAudioCaptureClient ), (void**)&captureClient ) );
		CHECK( audioClient->Start() );
		started = true;

		logDebug( u8"WASAPI loopback capture open: %u Hz, %u channels -> %u Hz, %u channels", mixRate, mixChannels, (uint32_t)SAMPLE_RATE, targetChannels );
		return S_OK;
	}
}

HRESULT __stdcall Whisper::captureOpenSystemAudio( const sCaptureParams& captureParams, iAudioCapture** pp ) noexcept
{
	if( nullptr == pp )
		return E_POINTER;

	ComLight::CComPtr<ComLight::Object<WasapiCapture>> res;
	CHECK( ComLight::Object<WasapiCapture>::create( res ) );
	CHECK( res->open( captureParams ) );

	res.detach( pp );
	return S_OK;
}