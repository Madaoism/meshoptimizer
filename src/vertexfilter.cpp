// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#include "meshoptimizer.h"

#include <math.h>

#if defined(__wasm_simd128__)
#define SIMD_WASM
#endif

#ifdef SIMD_WASM
#include <wasm_simd128.h>
#endif

#ifdef SIMD_WASM
#define wasmx_unpacklo_v16x8(a, b) wasm_v8x16_shuffle(a, b, 0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23)
#define wasmx_unpackhi_v16x8(a, b) wasm_v8x16_shuffle(a, b, 8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31)
#define wasmx_unziplo_v32x4(a, b) wasm_v8x16_shuffle(a, b, 0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27)
#define wasmx_unziphi_v32x4(a, b) wasm_v8x16_shuffle(a, b, 4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29, 30, 31)
#endif

namespace meshopt
{

#if !defined(SIMD_WASM)
template <typename T>
static void decodeFilterOct(T* data, size_t count)
{
	const float max = float((1 << (sizeof(T) * 8 - 1)) - 1);

	for (size_t i = 0; i < count; ++i)
	{
		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		float x = float(data[i * 4 + 0]);
		float y = float(data[i * 4 + 1]);
		float z = float(data[i * 4 + 2]) - fabsf(x) - fabsf(y);

		// fixup octahedral coordinates for z<0
		float t = (z >= 0.f) ? 0.f : z;

		x += (x >= 0.f) ? t : -t;
		y += (y >= 0.f) ? t : -t;

		// compute normal length & scale
		float l = sqrtf(x * x + y * y + z * z);
		float s = max / l;

		// rounded signed float->int
		int xf = int(x * s + (x >= 0.f ? 0.5f : -0.5f));
		int yf = int(y * s + (y >= 0.f ? 0.5f : -0.5f));
		int zf = int(z * s + (z >= 0.f ? 0.5f : -0.5f));

		data[i * 4 + 0] = T(xf);
		data[i * 4 + 1] = T(yf);
		data[i * 4 + 2] = T(zf);
	}
}

static void decodeFilterQuat(short* data, size_t count)
{
	const float scale = 1.f / (2047.f * sqrtf(2.f));

	static const int order[4][4] = {
	    {1, 2, 3, 0},
	    {2, 3, 0, 1},
	    {3, 0, 1, 2},
	    {0, 1, 2, 3},
	};

	for (size_t i = 0; i < count; ++i)
	{
		// convert x/y/z to [-1..1] (scaled...)
		float x = float(data[i * 4 + 0]) * scale;
		float y = float(data[i * 4 + 1]) * scale;
		float z = float(data[i * 4 + 2]) * scale;

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		float ww = 1.f - x * x - y * y - z * z;
		float w = sqrtf(ww >= 0.f ? ww : 0.f);

		// rounded signed float->int
		int xf = int(x * 32767.f + (x >= 0.f ? 0.5f : -0.5f));
		int yf = int(y * 32767.f + (y >= 0.f ? 0.5f : -0.5f));
		int zf = int(z * 32767.f + (z >= 0.f ? 0.5f : -0.5f));
		int wf = int(w * 32767.f + 0.5f);

		int qc = data[i * 4 + 3] & 3;

		// output order is dictated by input index
		data[i * 4 + order[qc][0]] = short(xf);
		data[i * 4 + order[qc][1]] = short(yf);
		data[i * 4 + order[qc][2]] = short(zf);
		data[i * 4 + order[qc][3]] = short(wf);
	}
}
#endif

#ifdef SIMD_WASM
static void decodeFilterOctSimd(signed char* data, size_t count)
{
	const v128_t sign = wasm_f32x4_splat(-0.f);

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t n4 = wasm_v128_load(&data[i * 4]);

		// sign-extends each of x,y in [x y ? ?] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 24), 24);
		v128_t yf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 16), 24);

		// unpack z; note that z is unsigned so we technically don't need to sign extend it
		v128_t zf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 8), 24);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		v128_t x = wasm_f32x4_convert_i32x4(xf);
		v128_t y = wasm_f32x4_convert_i32x4(yf);
		// TODO: when i32x4_abs is available it might be faster, f32x4_abs is 3 instructions in v8
		v128_t z = wasm_f32x4_sub(wasm_f32x4_convert_i32x4(zf), wasm_f32x4_add(wasm_f32x4_abs(x), wasm_f32x4_abs(y)));

		// fixup octahedral coordinates for z<0
		// note: i32x4_min_s with 0 is equvalent to f32x4_min
		v128_t t = wasm_i32x4_min_s(z, wasm_i32x4_splat(0));

		x = wasm_f32x4_add(x, wasm_v128_xor(t, wasm_v128_and(x, sign)));
		y = wasm_f32x4_add(y, wasm_v128_xor(t, wasm_v128_and(y, sign)));

		// compute normal length & scale
		v128_t l = wasm_f32x4_sqrt(wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z))));
		v128_t s = wasm_f32x4_div(wasm_f32x4_splat(127.f), l);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 8 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);

		// combine xr/yr/zr into final value
		v128_t res = wasm_v128_and(n4, wasm_i32x4_splat(0xff000000));
		res = wasm_v128_or(res, wasm_v128_and(xr, wasm_i32x4_splat(0xff)));
		res = wasm_v128_or(res, wasm_i32x4_shl(wasm_v128_and(yr, wasm_i32x4_splat(0xff)), 8));
		res = wasm_v128_or(res, wasm_i32x4_shl(wasm_v128_and(zr, wasm_i32x4_splat(0xff)), 16));

		wasm_v128_store(&data[i * 4], res);
	}
}

static void decodeFilterOctSimd(short* data, size_t count)
{
	const v128_t sign = wasm_f32x4_splat(-0.f);
	volatile v128_t zmask = wasm_i32x4_splat(0x7fff); // TODO: volatile works around LLVM shuffle "optimizations"

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t n4_0 = wasm_v128_load(&data[(i + 0) * 4]);
		v128_t n4_1 = wasm_v128_load(&data[(i + 2) * 4]);

		// gather both x/y 16-bit pairs in each 32-bit lane
		v128_t n4 = wasmx_unziplo_v32x4(n4_0, n4_1);

		// sign-extends each of x,y in [x y] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(n4, 16), 16);
		v128_t yf = wasm_i32x4_shr(n4, 16);

		// unpack z; note that z is unsigned so we don't need to sign extend it
		v128_t z4 = wasmx_unziphi_v32x4(n4_0, n4_1);
		v128_t zf = wasm_v128_and(z4, zmask);

		// convert x and y to floats and reconstruct z; this assumes zf encodes 1.f at the same bit count
		v128_t x = wasm_f32x4_convert_i32x4(xf);
		v128_t y = wasm_f32x4_convert_i32x4(yf);
		// TODO: when i32x4_abs is available it might be faster, f32x4_abs is 3 instructions in v8
		v128_t z = wasm_f32x4_sub(wasm_f32x4_convert_i32x4(zf), wasm_f32x4_add(wasm_f32x4_abs(x), wasm_f32x4_abs(y)));

		// fixup octahedral coordinates for z<0
		// note: i32x4_min_s with 0 is equvalent to f32x4_min
		v128_t t = wasm_i32x4_min_s(z, wasm_i32x4_splat(0));

		x = wasm_f32x4_add(x, wasm_v128_xor(t, wasm_v128_and(x, sign)));
		y = wasm_f32x4_add(y, wasm_v128_xor(t, wasm_v128_and(y, sign)));

		// compute normal length & scale
		v128_t l = wasm_f32x4_sqrt(wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z))));
		v128_t s = wasm_f32x4_div(wasm_f32x4_splat(32767.f), l);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);

		// mix x/z and y/0 to make 16-bit unpack easier
		v128_t xzr = wasm_v128_or(wasm_v128_and(xr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(zr, 16));
		v128_t y0r = wasm_v128_and(yr, wasm_i32x4_splat(0xffff));

		// pack x/y/z using 16-bit unpacks; note that this has 0 where we should have .w
		v128_t res_0 = wasmx_unpacklo_v16x8(xzr, y0r);
		v128_t res_1 = wasmx_unpackhi_v16x8(xzr, y0r);

		// patch in .w
		// TODO: this can use pblendw-like shuffles and we can remove y0r - once LLVM fixes shuffle merging
		res_0 = wasm_v128_or(res_0, wasm_v128_and(n4_0, wasm_i64x2_splat(0xffff000000000000)));
		res_1 = wasm_v128_or(res_1, wasm_v128_and(n4_1, wasm_i64x2_splat(0xffff000000000000)));

		wasm_v128_store(&data[(i + 0) * 4], res_0);
		wasm_v128_store(&data[(i + 2) * 4], res_1);
	}
}

static void decodeFilterQuatSimd(short* data, size_t count)
{
	const float scale = 1.f / (2047.f * sqrtf(2.f));

	for (size_t i = 0; i < count; i += 4)
	{
		v128_t q4_0 = wasm_v128_load(&data[(i + 0) * 4]);
		v128_t q4_1 = wasm_v128_load(&data[(i + 2) * 4]);

		// gather both x/y 16-bit pairs in each 32-bit lane
		v128_t q4_xy = wasmx_unziplo_v32x4(q4_0, q4_1);
		v128_t q4_zc = wasmx_unziphi_v32x4(q4_0, q4_1);

		// sign-extends each of x,y in [x y] with arithmetic shifts
		v128_t xf = wasm_i32x4_shr(wasm_i32x4_shl(q4_xy, 16), 16);
		v128_t yf = wasm_i32x4_shr(q4_xy, 16);
		v128_t zf = wasm_i32x4_shr(wasm_i32x4_shl(q4_zc, 16), 16);

		// convert x/y/z to [-1..1] (scaled...)
		v128_t x = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(xf), wasm_f32x4_splat(scale));
		v128_t y = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(yf), wasm_f32x4_splat(scale));
		v128_t z = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(zf), wasm_f32x4_splat(scale));

		// reconstruct w as a square root; we clamp to 0.f to avoid NaN due to precision errors
		// note: i32x4_max_s with 0 is equivalent to f32x4_max
		v128_t ww = wasm_f32x4_sub(wasm_f32x4_splat(1.f), wasm_f32x4_add(wasm_f32x4_mul(x, x), wasm_f32x4_add(wasm_f32x4_mul(y, y), wasm_f32x4_mul(z, z))));
		v128_t w = wasm_f32x4_sqrt(wasm_i32x4_max_s(ww, wasm_i32x4_splat(0)));

		v128_t s = wasm_f32x4_splat(32767.f);

		// fast rounded signed float->int: addition triggers renormalization after which mantissa stores the integer value
		// note: the result is offset by 0x4B40_0000, but we only need the low 16 bits so we can omit the subtraction
		const v128_t fsnap = wasm_f32x4_splat(3 << 22);

		v128_t xr = wasm_f32x4_add(wasm_f32x4_mul(x, s), fsnap);
		v128_t yr = wasm_f32x4_add(wasm_f32x4_mul(y, s), fsnap);
		v128_t zr = wasm_f32x4_add(wasm_f32x4_mul(z, s), fsnap);
		v128_t wr = wasm_f32x4_add(wasm_f32x4_mul(w, s), fsnap);

		// mix x/z and w/y to make 16-bit unpack easier
		v128_t xzr = wasm_v128_or(wasm_v128_and(xr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(zr, 16));
		v128_t wyr = wasm_v128_or(wasm_v128_and(wr, wasm_i32x4_splat(0xffff)), wasm_i32x4_shl(yr, 16));

		// pack x/y/z/w using 16-bit unpacks; we pack wxyz by default (for qc=0)
		v128_t res_0 = wasmx_unpacklo_v16x8(wyr, xzr);
		v128_t res_1 = wasmx_unpackhi_v16x8(wyr, xzr);

		// compute component index shifted left by 4 (and moved into i32x4 slot)
		v128_t cm = wasm_i32x4_shl(wasm_i32x4_shr(q4_zc, 16), 4);

		// rotate and store
		uint64_t* out = (uint64_t*)&data[i * 4];

		out[0] = __builtin_rotateleft64(wasm_i64x2_extract_lane(res_0, 0), wasm_i32x4_extract_lane(cm, 0));
		out[1] = __builtin_rotateleft64(wasm_i64x2_extract_lane(res_0, 1), wasm_i32x4_extract_lane(cm, 1));
		out[2] = __builtin_rotateleft64(wasm_i64x2_extract_lane(res_1, 0), wasm_i32x4_extract_lane(cm, 2));
		out[3] = __builtin_rotateleft64(wasm_i64x2_extract_lane(res_1, 1), wasm_i32x4_extract_lane(cm, 3));
	}
}
#endif

} // namespace meshopt

void meshopt_decodeFilterOct(void* buffer, size_t vertex_count, size_t vertex_size)
{
	using namespace meshopt;

	assert(vertex_count % 4 == 0);
	assert(vertex_size == 4 || vertex_size == 8);

#if defined(SIMD_WASM)
	if (vertex_size == 4)
		decodeFilterOctSimd(static_cast<signed char*>(buffer), vertex_count);
	else
		decodeFilterOctSimd(static_cast<short*>(buffer), vertex_count);
#else
	if (vertex_size == 4)
		decodeFilterOct(static_cast<signed char*>(buffer), vertex_count);
	else
		decodeFilterOct(static_cast<short*>(buffer), vertex_count);
#endif
}

void meshopt_decodeFilterQuat(void* buffer, size_t vertex_count, size_t vertex_size)
{
	using namespace meshopt;

	assert(vertex_count % 4 == 0);
	assert(vertex_size == 8);
	(void)vertex_size;

#if defined(SIMD_WASM)
	decodeFilterQuatSimd(static_cast<short*>(buffer), vertex_count);
#else
	decodeFilterQuat(static_cast<short*>(buffer), vertex_count);
#endif
}

#undef SIMD_WASM
