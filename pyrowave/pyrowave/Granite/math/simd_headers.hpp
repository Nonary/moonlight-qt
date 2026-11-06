/* Copyright (c) 2017-2026 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))

#include <intrin.h>
#if defined(_INCLUDED_PMM) && !defined(__SSE3__)
#define __SSE3__ 1
#endif
#if !defined(__SSE__)
#define __SSE__ 1
#endif
#if defined(_INCLUDED_IMM) && !defined(__AVX__)
#define __AVX__ 1
#endif

#elif defined(__AVX__)
#include <immintrin.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#elif defined(__SSE3__)
#include <pmmintrin.h>
#elif defined(__SSE__)
#include <xmmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
// MSVC targeting ARM64 defines _M_ARM64 but neither __ARM_NEON nor __aarch64__,
// which the NEON paths in simd.hpp and muglm.cpp test for. Its arm64_neon.h
// provides the standard intrinsic names, so take the NEON paths there too.
// (clang-cl defines both macros itself and is handled above.)
#include <arm64_neon.h>
#ifndef __ARM_NEON
#define __ARM_NEON 1
#endif
#ifndef __aarch64__
#define __aarch64__ 1
#endif
#endif
