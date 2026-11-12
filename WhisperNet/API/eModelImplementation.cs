namespace Whisper
{
	/// <summary>Implementation value for the <see cref="Library.loadModel(string, eGpuModelFlags, string?, eModelImplementation)" /> factory function</summary>
	public enum eModelImplementation: uint
	{
		/// <summary>GPGPU implementation based on Direct3D 11.0 compute shaders</summary>
		GPU = 1,

		/// <summary>A hybrid implementation which uses DirectCompute for encode, and decodes on CPU</summary>
		/// <remarks>
		/// <para>The build of the native DLL included into this nuget package doesn’t implement this version.<br/>
		/// To enable, edit <c>stdafx.h</c> in Whisper project, change the value of <c>BUILD_HYBRID_VERSION</c> macro from zero to one, and build.</para>
		/// <para>This implementation requires a CPU with AVX1, FMA3, F16C and BMI1 instruction set extensions.</para>
		/// </remarks>
		Hybrid = 2,

		/// <summary>Reserved for the removed reference CPU implementation; selecting it returns E_NOTIMPL.</summary>
		Reference = 3,
	}
}