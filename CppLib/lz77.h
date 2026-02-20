
/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – porównanie algorytmu C++ i Asemblera MASM x64
 * DATA WYKONANIA: luty 2026 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Maciej Guja
 * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
	#endif

	__declspec(dllexport)
	void lz77_rgba_compress(const uint32_t* src_px, size_t src_count, uint8_t* dst, size_t dst_cap_bytes, void* work, size_t work_cap_bytes, size_t* out_len_bytes);
	void lz77_rgba_decompress(const uint8_t* src, size_t src_len_bytes,uint32_t* dst_px, size_t dst_cap_px,size_t* out_len_px);

	#ifdef __cplusplus
}
#endif