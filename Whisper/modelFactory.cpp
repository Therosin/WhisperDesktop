#include "stdafx.h"
#include "modelFactory.h"
#include "API/iContext.cl.h"

HRESULT COMLIGHTCALL Whisper::loadModel( const wchar_t* path, const sModelSetup& setup, const sLoadModelCallbacks* callbacks, iModel** pp )
{
	switch( setup.impl )
	{
	case eModelImplementation::GPU:
	case eModelImplementation::Hybrid:
		return loadGpuModel( path, setup, callbacks, pp );
	case eModelImplementation::Reference:
		logError( u8"The reference CPU model is no longer implemented." );
		return E_NOTIMPL;
	}

	logError( u8"Unknown model implementation 0x%X", (int)setup.impl );
	return E_INVALIDARG;
}