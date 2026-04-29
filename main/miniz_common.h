#pragma once

#include "miniz_export.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef uint8_t mz_uint8;
typedef int16_t mz_int16;
typedef uint16_t mz_uint16;
typedef int32_t mz_int32;
typedef uint32_t mz_uint32;
typedef uint64_t mz_uint64;

typedef size_t mz_uint;
typedef uintptr_t mz_uintptr;

#define MZ_FREE(p) free(p)
#define MZ_MALLOC(p) malloc(p)
#define MZ_REALLOC(p, s) realloc(p, s)

/* ESP32-C3 is a 32-bit RISC-V, no 64-bit registers */
#define MINIZ_X86_OR_X64_CPU 0
#define MINIZ_HAS_64BIT_REGISTERS 0
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0

#define MZ_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MZ_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MZ_ASSERT(x) assert(x)

/* Macro helpers for do { } while(0) loops */
#define MZ_MACRO_END while (0)

#define MZ_CLEAR_ARR(obj) memset((obj), 0, sizeof(obj))

/* Unaligned memory read helpers */
#if MINIZ_USE_UNALIGNED_LOADS_AND_STORES
#define MZ_READ_LE16(p) *((const mz_uint16 *)(p))
#define MZ_READ_LE32(p) *((const mz_uint32 *)(p))
#else
#define MZ_READ_LE16(p) ((mz_uint16)(((const mz_uint8 *)(p))[0]) | ((mz_uint16)(((const mz_uint8 *)(p))[1]) << 8U))
#define MZ_READ_LE32(p) ((mz_uint32)(((const mz_uint8 *)(p))[0]) | ((mz_uint32)(((const mz_uint8 *)(p))[1]) << 8U) | ((mz_uint32)(((const mz_uint8 *)(p))[2]) << 16U) | ((mz_uint32)(((const mz_uint8 *)(p))[3]) << 24U))
#endif
