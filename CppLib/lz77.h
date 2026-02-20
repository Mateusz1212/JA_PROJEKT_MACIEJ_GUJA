/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków –
 *                 odpowiednik kodu asemblerowego MASM x64 napisany w C++
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
        void lz77_rgba_compress(
            const uint32_t* src_px,      // wejœcie: tablica pikseli RGBA
            size_t          src_count,   // liczba pikseli
            uint8_t* dst,         // wyjœcie: bufor na tokeny
            size_t          dst_cap,     // pojemnoœæ bufora wyjœciowego w bajtach
            void* work,        // bufor roboczy (min. WORK_NEED_BYTES)
            size_t          work_cap,    // pojemnoœæ bufora roboczego w bajtach
            size_t* out_len      // [out] liczba zapisanych bajtów (0 = b³¹d)
        );

    __declspec(dllexport)
        void lz77_rgba_decompress(
            const uint8_t* src,         // wejœcie: skompresowany strumieñ
            size_t          src_len,     // d³ugoœæ strumienia w bajtach
            uint32_t* dst_px,      // wyjœcie: tablica pikseli RGBA
            size_t          dst_cap,     // pojemnoœæ bufora wyjœciowego w pikselach
            size_t* out_len      // [out] liczba zdekompresowanych pikseli (0 = b³¹d)
        );

    // Minimalny rozmiar bufora roboczego wymaganego przez lz77_rgba_compress
    // head[65536] = 256 KB  +  prev[4096] = 16 KB
    static const size_t LZ77_WORK_NEED_BYTES = (65536 + 4096) * sizeof(uint32_t);

#ifdef __cplusplus
}
#endif