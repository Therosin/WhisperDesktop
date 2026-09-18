#include "stdafx.h"
#include "createDevice.h"
#include "listGPUs.h"
#include "RenderDoc/renderDoc.h"
#pragma comment(lib, "D3D11.lib")
#include <atlstr.h>

namespace
{
	// Whisper inference is a throughput workload and does not need low latency;
	// the desktop/compositor does. A negative GPU thread priority makes WDDM schedule
	// other processes' graphics work (browser presentation, video) ahead of our
	// long D3D11 compute dispatches, trading a little transcription speed for
	// smooth desktop. Range is [-7..7], see IDXGIDevice::SetGPUThreadPriority.
	// Prefer mild values, profiling shows -1..-3 to be a good compromise on AMD iGPU.
	inline constexpr INT kGpuThreadPriority = -2;

	void __stdcall setGpuThreadPriority( ID3D11Device* dev )
	{
		CComPtr<IDXGIDevice> dxgiDevice;
		if( FAILED( dev->QueryInterface( &dxgiDevice ) ) )
			return;
		HRESULT hr = dxgiDevice->SetGPUThreadPriority( kGpuThreadPriority );
		if( SUCCEEDED( hr ) && willLogMessage( Whisper::eLogLevel::Debug ) )
			logDebug16( L"D3D11: set GPU thread priority to %2d", kGpuThreadPriority );
	}
}

HRESULT DirectCompute::createDevice( const std::wstring& adapterName, ID3D11Device** dev, ID3D11DeviceContext** context )
{
	CComPtr<IDXGIAdapter1> adapter = selectAdapter( adapterName );
	const D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

	const std::array<D3D_FEATURE_LEVEL, 4> levels = { D3D_FEATURE_LEVEL_12_1 , D3D_FEATURE_LEVEL_12_0 , D3D_FEATURE_LEVEL_11_1 , D3D_FEATURE_LEVEL_11_0 };
	UINT flags = D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT | D3D11_CREATE_DEVICE_SINGLETHREADED;
	bool renderDoc = initializeRenderDoc();
#ifdef _DEBUG
	if( !renderDoc )
	{
		// Last time I checked, RenderDoc crashed with debug version of D3D11 runtime
		// Only setting this flag unless renderdoc.dll is loaded to the current process
		flags |= D3D11_CREATE_DEVICE_DEBUG;
	}
#endif
	constexpr UINT levelsCount = (UINT)levels.size();
	HRESULT hr = D3D11CreateDevice( adapter, driverType, nullptr, flags, levels.data(), levelsCount, D3D11_SDK_VERSION, dev, nullptr, context );
	if( SUCCEEDED( hr ) )
	{
		setGpuThreadPriority( *dev );
		return S_OK;
	}
	// D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT: This value is not supported until Direct3D 11.1
	// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_create_device_flag
	flags = _andn_u32( D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT, flags );

	hr = D3D11CreateDevice( adapter, driverType, nullptr, flags, levels.data(), levelsCount, D3D11_SDK_VERSION, dev, nullptr, context );
	if( SUCCEEDED( hr ) )
	{
		setGpuThreadPriority( *dev );
		return S_OK;
	}
	return hr;
}

HRESULT DirectCompute::cloneDevice( ID3D11Device* source, ID3D11Device** dev, ID3D11DeviceContext** context )
{
	CComPtr<IDXGIDevice> dxgiDev;
	CHECK( source->QueryInterface( &dxgiDev ) );

	CComPtr<IDXGIAdapter> adapter;
	CHECK( dxgiDev->GetAdapter( &adapter ) );

	const uint32_t flags = source->GetCreationFlags();
	const D3D_FEATURE_LEVEL level = source->GetFeatureLevel();
	return D3D11CreateDevice( adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &level, 1,
		D3D11_SDK_VERSION, dev, nullptr, context );
}

namespace
{
	using Whisper::eGpuModelFlags;
	inline constexpr uint32_t operator|( eGpuModelFlags a, eGpuModelFlags b )
	{
		return (uint32_t)a | (uint32_t)b;
	}
	inline bool operator&( uint32_t flags, eGpuModelFlags bit )
	{
		return 0 != ( flags & (uint32_t)bit );
	}
	inline bool merge3( uint32_t flags, eGpuModelFlags enabled, eGpuModelFlags disabled, bool def )
	{
		if( flags & enabled )
			return true;
		if( flags & disabled )
			return false;
		return def;
	}
}

HRESULT DirectCompute::validateFlags( uint32_t flags )
{
	constexpr uint32_t waveBoth = eGpuModelFlags::Wave32 | eGpuModelFlags::Wave64;
	if( ( flags & waveBoth ) == waveBoth )
	{
		logError( u8"eGpuModelFlags.%s and eGpuModelFlags.%s are mutually exclusive", "Wave32", "Wave64" );
		return E_INVALIDARG;
	}

	constexpr uint32_t reshapedBoth = eGpuModelFlags::NoReshapedMatMul | eGpuModelFlags::UseReshapedMatMul;
	if( ( flags & reshapedBoth ) == reshapedBoth )
	{
		logError( u8"eGpuModelFlags.%s and eGpuModelFlags.%s are mutually exclusive", "NoReshapedMatMul", "UseReshapedMatMul" );
		return E_INVALIDARG;
	}

	constexpr uint32_t fp16Both = eGpuModelFlags::NoFp16Compute | eGpuModelFlags::UseFp16Compute;
	if( ( flags & fp16Both ) == fp16Both )
	{
		logError( u8"eGpuModelFlags.%s and eGpuModelFlags.%s are mutually exclusive", "NoFp16Compute", "UseFp16Compute" );
		return E_INVALIDARG;
	}
	return S_OK;
}

HRESULT DirectCompute::queryDeviceInfo( sGpuInfo& rdi, ID3D11Device* dev, uint32_t flags )
{
	if( nullptr == dev )
		return OLE_E_BLANK;

	CComPtr<IDXGIDevice> dd;
	CHECK( dev->QueryInterface( &dd ) );

	CComPtr<IDXGIAdapter> adapter;
	CHECK( dd->GetAdapter( &adapter ) );

	DXGI_ADAPTER_DESC desc;
	adapter->GetDesc( &desc );

	const size_t descLen = wcsnlen_s( desc.Description, 128 );
	const wchar_t* rsi = &desc.Description[ 0 ];
	rdi.description.assign( rsi, rsi + descLen );
	rdi.vendor = (eGpuVendor)desc.VendorId;
	rdi.device = (uint16_t)desc.DeviceId;
	rdi.revision = (uint16_t)desc.Revision;
	rdi.subsystem = desc.SubSysId;
	rdi.vramDedicated = desc.DedicatedVideoMemory;
	rdi.ramDedicated = desc.DedicatedSystemMemory;
	rdi.ramShared = desc.SharedSystemMemory;

	// Set up these flags
	uint8_t ef = 0;
	const bool amd = ( rdi.vendor == eGpuVendor::AMD );
	if( merge3( flags, eGpuModelFlags::Wave64, eGpuModelFlags::Wave32, amd ) )
		ef |= (uint8_t)eGpuEffectiveFlags::Wave64;
	if( merge3( flags, eGpuModelFlags::UseReshapedMatMul, eGpuModelFlags::NoReshapedMatMul, amd ) )
		ef |= (uint8_t)eGpuEffectiveFlags::ReshapedMatMul;
	if( 0 != ( flags & eGpuModelFlags::Cloneable ) )
		ef |= (uint8_t)eGpuEffectiveFlags::Cloneable;

	// FP16 math for matmul shaders. Both AMD and nVidia have had 2x-throughput FP16 units
	// for years (Ryzen iGPU / Vega / RDNA and Pascal / Turing / Ada). Even when the driver
	// doesn't map min16float to real FP16 ops, it degrades to FP32 with no loss of precision.
	const bool fp16Default = amd || ( rdi.vendor == eGpuVendor::NVidia );
	if( merge3( flags, eGpuModelFlags::UseFp16Compute, eGpuModelFlags::NoFp16Compute, fp16Default ) )
		ef |= (uint8_t)eGpuEffectiveFlags::Fp16Compute;
	rdi.flags = (eGpuEffectiveFlags)ef;

	if( willLogMessage( Whisper::eLogLevel::Debug ) )
	{
		const int fl = dev->GetFeatureLevel();
		const int flMajor = ( fl >> 12 ) & 0xF;
		const int flMinor = ( fl >> 8 ) & 0xF;

		CStringA flagsString;
		flagsString.Format( "%s | %s", rdi.wave64() ? "Wave64" : "Wave32",
			rdi.useReshapedMatMul() ? "UseReshapedMatMul" : "NoReshapedMatMul" );
		if( rdi.cloneableModel() )
			flagsString += " | Cloneable";
		if( rdi.fp16Compute() )
			flagsString += " | Fp16Compute";

		logDebug16( L"Using GPU \"%s\", feature level %i.%i, effective flags %S",
			rdi.description.c_str(), flMajor, flMinor,
			flagsString.operator const char* ( ) );
	}
	return S_OK;
}