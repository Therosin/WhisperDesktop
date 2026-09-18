#include "stdafx.h"
#include "ContextImpl.h"
#include "../API/iMediaFoundation.cl.h"
#include "../MF/AudioBuffer.h"
#include "../MF/mfUtils.h"
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <deque>
#include <memory>
#include <thread>
#include "voiceActivityDetection.h"

namespace
{
	using namespace Whisper;

	class TranscribeBuffer : public ComLight::ObjectRoot<iAudioBuffer>
	{
		// ==== iAudioBuffer ====
		uint32_t COMLIGHTCALL countSamples() const override final
		{
			return (uint32_t)pcm.mono.size();
		}
		const float* COMLIGHTCALL getPcmMono() const override final
		{
			if( !pcm.mono.empty() )
				return pcm.mono.data();
			return nullptr;
		}
		const float* COMLIGHTCALL getPcmStereo() const override final
		{
			if( !pcm.stereo.empty() )
				return pcm.stereo.data();
			return nullptr;
		}
		HRESULT COMLIGHTCALL getTime( int64_t& rdi ) const override final
		{
			rdi = MFllMulDiv( currentOffset, 10'000'000, SAMPLE_RATE, 0 );
			return S_OK;
		}
	public:
		AudioBuffer pcm;
		int64_t currentOffset = 0;
	};

	class TranscribeBufferObj : public ComLight::Object<TranscribeBuffer>
	{
		uint32_t Release() override final
		{
			return RefCounter::implRelease();
		}
	};

	// Same data as in the Whisper.sCaptureParams public structure, the durations are scaled from FP32 seconds into uint32_t samples at 16 kHz
	struct CaptureParams
	{
		uint32_t minDuration, maxDuration, dropStartSilence, pauseDuration;
		uint32_t flags;

		CaptureParams( const sCaptureParams& cp )
		{
			// Convert these floats from seconds to samples
			__m128 floats = _mm_loadu_ps( &cp.minDuration );
			floats = _mm_mul_ps( floats, _mm_set1_ps( (float)SAMPLE_RATE ) );
			floats = _mm_round_ps( floats, _MM_FROUND_NINT );
			__m128i ints = _mm_cvtps_epi32( floats );
			store16( &minDuration, ints );

			flags = cp.flags;
		}
	};

	class Capture
	{
		CComPtr<IMFSourceReader> reader;
		// Non-null when the source is the WASAPI loopback capture, pulls PCM directly
		ComLight::CComPtr<iWasmCapture> wasm;
		const CaptureParams captureParams;
		const sCaptureCallbacks callbacks;
		// Count of channels delivered from the source reader
		uint8_t readerChannels = 0;
		volatile char stateFlags = 0;

		mutable CComAutoCriticalSection critSec;
		AudioBuffer pcm;
		AudioBuffer::pfnAppendSamples pfnAppendSamples = nullptr;
		int64_t pcmStartTime = 0;
		int64_t nextSampleTime = 0;
		VAD vad;
		sFullParams fullParams;
		ProfileCollection& profiler;
		iContext* const whisperContext;

		// Queue of PCM chunks waiting for the transcription thread. Capture never drops data:
		// while the model is busy, audio keeps being spooled here as fixed-size chunks.
		std::deque<std::unique_ptr<TranscribeBufferObj>> queue;
		HANDLE queueEvent = nullptr;
		std::thread worker;
		std::atomic_bool stopWorker { false };
		volatile HRESULT lastError = S_OK;

		// Detach the current PCM buffer and hand it over to the transcription thread.
		HRESULT submitChunk();

		// The transcription thread: pops chunks from the queue, runs the model on them in order.
		void workerMain();

		// Set the state bit, and if needed notify user with the callback.
		HRESULT setStateFlag( eCaptureStatus newBit ) noexcept
		{
			const uint8_t bit = (uint8_t)newBit;
			const uint8_t oldVal = (uint8_t)InterlockedOr8( &stateFlags, (char)bit );
			if( nullptr == callbacks.captureStatus )
				return S_OK;	// no callbacks
			if( 0 != ( oldVal & bit ) )
				return S_OK;	// The bit was already set
			return callbacks.captureStatus( callbacks.pv, (eCaptureStatus)( oldVal | bit ) );
		}

		// Clear the state bit, and if needed notify user with the callback
		HRESULT clearStateFlag( eCaptureStatus clearBit ) noexcept
		{
			const uint8_t bit = (uint8_t)clearBit;
			const uint8_t mask = ~bit;
			const uint8_t oldVal = (uint8_t)InterlockedAnd8( &stateFlags, (char)mask );
			if( nullptr == callbacks.captureStatus )
				return S_OK;	// no callbacks
			if( 0 == ( oldVal & bit ) )
				return S_OK;	// The bit wasn't there
			return callbacks.captureStatus( callbacks.pv, (eCaptureStatus)( oldVal & mask ) );
		}

		bool hasStateFlag( eCaptureStatus testBit ) const
		{
			const uint8_t bit = (uint8_t)testBit;
			return 0 != ( (uint8_t)stateFlags & bit );
		}

		HRESULT readSample();

		// Run voice detection on the data in pcm.mono vector.
		// When not detected, return 0. When detected, return last frame index where it is detected.
		size_t detectVoice();

	public:
		Capture( const sCaptureCallbacks& cb, const iAudioCapture* ac, const sFullParams& sfp, iContext* wc, ProfileCollection& pc ) :
			callbacks( cb ),
			captureParams( ac->getParams() ),
			fullParams( sfp ), whisperContext( wc ), profiler( pc )
		{
		}

		~Capture()
		{
			// Stop the transcription thread. It keeps processing any remaining queued chunks,
			// so nothing captured is ever lost.
			stopWorker = true;
			if( nullptr != queueEvent )
				SetEvent( queueEvent );
			if( worker.joinable() )
				worker.join();
			if( nullptr != queueEvent )
			{
				CloseHandle( queueEvent );
				queueEvent = nullptr;
			}
		}

		HRESULT startup( const iAudioCapture* ac );

		HRESULT checkCancel() noexcept
		{
			if( nullptr == callbacks.shouldCancel )
				return S_OK;
			return callbacks.shouldCancel( callbacks.pv );
		}

		HRESULT run();
	};

	HRESULT Capture::startup( const iAudioCapture* ac )
	{
		// One transcription thread consumes the queued PCM chunks in order
		queueEvent = CreateEvent( nullptr, FALSE, FALSE, nullptr );
		if( nullptr == queueEvent )
			return HRESULT_FROM_WIN32( GetLastError() );

		stopWorker = false;
		try
		{
			worker = std::thread( &Capture::workerMain, this );
		}
		catch( const std::system_error& ex )
		{
			CloseHandle( queueEvent );
			queueEvent = nullptr;
			return HRESULT_FROM_WIN32( ex.code().value() );
		}

		// The WASAPI system-audio source implements an optional extra interface; use it when present.
		if( SUCCEEDED( const_cast<iAudioCapture*>( ac )->QueryInterface( iWasmCapture::iid(), (void**)&wasm ) ) )
		{
			// The WASAPI source already delivers resampled 16 kHz float PCM, 1 or 2 channels
			const uint32_t channels = wasm->channels();
			const bool sourceMono = channels < 2;
			const bool wantStereo = 0 != ( captureParams.flags & (uint32_t)eCaptureFlags::Stereo );
			pfnAppendSamples = AudioBuffer::appendSamplesFunc( sourceMono, wantStereo );
			readerChannels = ( !sourceMono && wantStereo ) ? 2 : 1;

			CHECK( setStateFlag( eCaptureStatus::Listening ) );
			return S_OK;
		}

		// Initialize the MF source reader
		CHECK( ac->getReader( &reader ) );

		// Set up media type, and figure out sample handler
		CHECK( reader->SetStreamSelection( MF_SOURCE_READER_ALL_STREAMS, FALSE ) );
		CHECK( reader->SetStreamSelection( MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE ) );

		CComPtr<IMFMediaType> mtNative;
		CHECK( reader->GetNativeMediaType( MF_SOURCE_READER_FIRST_AUDIO_STREAM, MF_SOURCE_READER_CURRENT_TYPE_INDEX, &mtNative ) );
		UINT32 numChannels;
		CHECK( mtNative->GetUINT32( MF_MT_AUDIO_NUM_CHANNELS, &numChannels ) );

		const bool sourceMono = numChannels < 2;
		const bool wantStereo = 0 != ( captureParams.flags & (uint32_t)eCaptureFlags::Stereo );
		pfnAppendSamples = AudioBuffer::appendSamplesFunc( sourceMono, wantStereo );

		CComPtr<IMFMediaType> mt;
		this->readerChannels = ( !sourceMono && wantStereo ) ? 2 : 1;
		CHECK( createMediaType( !sourceMono, &mt ) );
		CHECK( reader->SetCurrentMediaType( MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mt ) );

		CHECK( setStateFlag( eCaptureStatus::Listening ) );
		return S_OK;
	}

	// This method is called in a loop until user stops the audio capture.
	// It never blocks on the model: chunks are detached and handed to the transcription
	// thread, which may lag behind but is guaranteed to consume every captured sample.
	HRESULT Capture::run()
	{
		const HRESULT e = lastError;
		if( FAILED( e ) )
		{
			lastError = S_OK;
			return e;
		}

		const size_t oldSamples = pcm.mono.size();
		CHECK( readSample() );
		const size_t newSamples = pcm.mono.size();

		const size_t lastVoiceFrame = detectVoice();
		if( lastVoiceFrame == 0 )
		{
			// No voice is detected in the entire buffered audio
			clearStateFlag( eCaptureStatus::Voice );
			if( newSamples < captureParams.dropStartSilence )
				return S_OK;

			pcm.clear();
			vad.clear();
			pcmStartTime = nextSampleTime;
			return S_OK;
		}

		const bool newFrameVoice = lastVoiceFrame + captureParams.pauseDuration >= oldSamples;

		if( newFrameVoice )
		{
			// A voice is detected in the buffer, and it was fairly recently
			setStateFlag( eCaptureStatus::Voice );
			if( newSamples < captureParams.maxDuration )
				return S_OK;	// While voice is continuously detected, allow the buffer to grow up to `maxDuration` time
		}
		else
		{
			// A voice is detected in the buffer, but it was a while ago
			clearStateFlag( eCaptureStatus::Voice );
			if( newSamples < captureParams.minDuration && lastVoiceFrame < captureParams.minDuration )
				return S_OK;	// Not enough audio yet; keep buffering up to the `minDuration` floor
		}

		// Enough captured PCM data to run the ASR model
		return submitChunk();
	}

	HRESULT Capture::readSample()
	{
		if( wasm )
		{
			// WASAPI loopback: pull the next resampled 16 kHz float chunk
			const float* pData = nullptr;
			size_t countFloats = 0;
			CHECK( wasm->getSample( &pData, countFloats ) );
			if( countFloats > 0 )
			{
				const size_t prevSize = pcm.mono.size();
				( pcm.*pfnAppendSamples )( pData, countFloats );
				const size_t newSize = pcm.mono.size();
				this->nextSampleTime += ( newSize - prevSize );
			}
			return S_OK;
		}

		while( true )
		{
			DWORD dwFlags = 0;
			CComPtr<IMFSample> sample;

			// Read the next sample
			HRESULT hr = reader->ReadSample( (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &dwFlags, nullptr, &sample );
			if( FAILED( hr ) )
			{
				logErrorHr( hr, u8"IMFSourceReader.ReadSample" );
				return hr;
			}

			if( dwFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED )
			{
				logError( u8"Media type changes ain’t supported by the library." );
				return E_UNEXPECTED;
			}

			if( dwFlags & MF_SOURCE_READERF_ENDOFSTREAM )
				return E_EOF;

			if( !sample )
				continue;

			// Get a pointer to the audio data in the sample.
			CComPtr<IMFMediaBuffer> buffer;
			hr = sample->ConvertToContiguousBuffer( &buffer );
			if( FAILED( hr ) )
				return hr;

			const float* pAudioData = nullptr;
			DWORD cbBuffer;
			hr = buffer->Lock( (BYTE**)&pAudioData, nullptr, &cbBuffer );
			if( FAILED( hr ) )
				return hr;

			try
			{
				assert( 0 == ( cbBuffer % sizeof( float ) ) );
				const size_t countFloats = cbBuffer / sizeof( float );
				const size_t prevSize = pcm.mono.size();
				( pcm.*pfnAppendSamples )( pAudioData, countFloats );
				const size_t newSize = pcm.mono.size();
				this->nextSampleTime += ( newSize - prevSize );
			}
			catch( const std::bad_alloc& )
			{
				buffer->Unlock();
				return E_OUTOFMEMORY;
			}

			// Unlock the buffer
			hr = buffer->Unlock();
			if( FAILED( hr ) )
				return hr;

			return S_OK;
		}
	}

	HRESULT Capture::submitChunk()
	{
		if( pcm.mono.empty() )
			return S_OK;

		std::unique_ptr<TranscribeBufferObj> chunk( new TranscribeBufferObj() );
		chunk->currentOffset = pcmStartTime;
		pcm.swap( chunk->pcm );
		pcmStartTime = nextSampleTime;
		pcm.clear();
		vad.clear();

		CHECK( setStateFlag( eCaptureStatus::Transcribing ) );

		{
			using lock = CComCritSecLock<CComAutoCriticalSection>;
			lock critSec0{ critSec };
			queue.push_back( std::move( chunk ) );
		}
		SetEvent( queueEvent );
		return S_OK;
	}

	void Capture::workerMain()
	{
		for( ;; )
		{
			std::unique_ptr<TranscribeBufferObj> chunk;
			{
				using lock = CComCritSecLock<CComAutoCriticalSection>;
				lock critSec0{ critSec };
				if( !queue.empty() )
				{
					chunk = std::move( queue.front() );
					queue.pop_front();
				}
			}

			if( chunk )
			{
				HRESULT status = E_UNEXPECTED;
				try
				{
					status = whisperContext->runFull( fullParams, chunk.get() );
				}
				catch( HRESULT hr )
				{
					status = hr;
				}
				catch( const std::bad_alloc& )
				{
					status = E_OUTOFMEMORY;
				}
				catch( const std::exception& )
				{
					status = E_FAIL;
				}

				if( FAILED( status ) )
				{
					logErrorHr( status, u8"Failed to transcribe a captured chunk" );
					lastError = status;
				}

				// When the queue is drained, the busy indicator can go away.
				// New chunks submitted in the meantime keep the flag set.
				bool drained = false;
				{
					using lock = CComCritSecLock<CComAutoCriticalSection>;
					lock critSec0{ critSec };
					drained = queue.empty();
				}
				if( drained )
					clearStateFlag( eCaptureStatus::Transcribing );
				continue;
			}

			if( stopWorker )
				break;
			WaitForSingleObject( queueEvent, INFINITE );
		}
	}

	size_t Capture::detectVoice()
	{
		auto pf = profiler.cpuBlock( eCpuBlock::VAD );
		return vad.detect( pcm.mono.data(), pcm.mono.size() );
	}
}

HRESULT COMLIGHTCALL ContextImpl::runCapture( const sFullParams& params, const sCaptureCallbacks& callbacks, const iAudioCapture* reader )
{
	if( nullptr == reader )
		return E_POINTER;

	// Validate a few things
	{
		const auto& cp = reader->getParams();
		if( cp.minDuration < 0.125f || cp.minDuration > 30.0f )
		{
			logError( u8"%s parameter %g is out of range", "minDuration", cp.minDuration );
			return E_INVALIDARG;
		}
		if( cp.maxDuration < 0.125f || cp.maxDuration > 30.0f )
		{
			logError( u8"%s parameter %g is out of range", "maxDuration", cp.maxDuration );
			return E_INVALIDARG;
		}
	}

	auto profCompleteCpu = profiler.cpuBlock( eCpuBlock::RunComplete );
	Capture capture{ callbacks, reader, params, this, profiler };
	CHECK( capture.startup( reader ) );

	while( true )
	{
		HRESULT hr = capture.checkCancel();
		CHECK( hr );
		if( hr != S_OK )
			return S_OK;
		CHECK( capture.run() );
	}
}