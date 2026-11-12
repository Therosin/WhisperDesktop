#include "stdafx.h"
#include "ModelBuffers.h"

__m128i __declspec( noinline ) DirectCompute::TensorPair::getMemoryUse() const
{
	return _mm_add_epi64( w.getMemoryUse(), b.getMemoryUse() );
}

__m128i DirectCompute::LayerEncoder::getMemoryUse() const
{
	__m128i v = attnLn0.getMemoryUse();
	v = _mm_add_epi64( v, attnLn1.getMemoryUse() );
	v = _mm_add_epi64( v, attnQuery.getMemoryUse() );
	v = _mm_add_epi64( v, attnKey.getMemoryUse() );
	v = _mm_add_epi64( v, attnValue.getMemoryUse() );
	v = _mm_add_epi64( v, mlpLn.getMemoryUse() );
	v = _mm_add_epi64( v, mlp0.getMemoryUse() );
	v = _mm_add_epi64( v, mlp1.getMemoryUse() );
	return v;
}

__m128i DirectCompute::EncoderBuffers::getMemoryUse() const
{
	__m128i v = _mm_cvtsi64_si128( vectorMemoryUse( layers ) );
	v = _mm_add_epi64( v, positionalEmbedding.getMemoryUse() );
	v = _mm_add_epi64( v, conv1.getMemoryUse() );
	v = _mm_add_epi64( v, conv2.getMemoryUse() );
	v = _mm_add_epi64( v, lnPost.getMemoryUse() );
	for( const auto& layer : layers )
		v = _mm_add_epi64( v, layer.getMemoryUse() );
	return v;
}

__m128i DirectCompute::LayerDecoder::getMemoryUse() const
{
	__m128i v = attnLn0.getMemoryUse();
	v = _mm_add_epi64( v, attnLn1.getMemoryUse() );
	v = _mm_add_epi64( v, attnQuery.getMemoryUse() );
	v = _mm_add_epi64( v, attnKey.getMemoryUse() );
	v = _mm_add_epi64( v, attnValue.getMemoryUse() );
	v = _mm_add_epi64( v, crossAttnLn0.getMemoryUse() );
	v = _mm_add_epi64( v, crossAttnLn1.getMemoryUse() );
	v = _mm_add_epi64( v, crossAttnQuery.getMemoryUse() );
	v = _mm_add_epi64( v, crossAttnKey.getMemoryUse() );
	v = _mm_add_epi64( v, crossAttnValue.getMemoryUse() );
	v = _mm_add_epi64( v, mlpLn.getMemoryUse() );
	v = _mm_add_epi64( v, mlp0.getMemoryUse() );
	v = _mm_add_epi64( v, mlp1.getMemoryUse() );
	return v;
}

__m128i DirectCompute::DecoderBuffers::getMemoryUse() const
{
	__m128i v = _mm_cvtsi64_si128( vectorMemoryUse( layers ) );
	v = _mm_add_epi64( v, positionalEmbedding.getMemoryUse() );
	v = _mm_add_epi64( v, tokenEmbedding.getMemoryUse() );
	v = _mm_add_epi64( v, ln.getMemoryUse() );
	for( const auto& layer : layers )
		v = _mm_add_epi64( v, layer.getMemoryUse() );
	return v;
}

__m128i DirectCompute::ModelBuffers::getMemoryUse() const
{
	return _mm_add_epi64( enc.getMemoryUse(), dec.getMemoryUse() );
}