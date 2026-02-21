
/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – odpowiednik kodu asemblerowego MASM x64 napisany w C++
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

    /*
     * lz77_rgba_compress
     *
     * Kompresuje tablice pikseli RGBA do strumienia tokenow LZ77.
     *
     * Parametry:
     *   src_px    – wejscie: tablica pikseli RGBA (uint32_t kazdy)
     *   src_count – liczba pikseli wejsciowych
     *   dst       – wyjscie: bufor na skompresowane tokeny
     *   dst_cap   – pojemnosc bufora wyjsciowego w bajtach
     *   work      – bufor roboczy (min. LZ77_WORK_NEED_BYTES)
     *   work_cap  – pojemnosc bufora roboczego w bajtach
     *   out_len   – [out] liczba zapisanych bajtow (0 = blad lub brak wejscia)
     */
    __declspec(dllexport)
        void lz77_rgba_compress(
            const uint32_t* src_px,
            size_t          src_count,
            uint8_t* dst,
            size_t          dst_cap,
            void* work,
            size_t          work_cap,
            size_t* out_len
        );

    /*
     * lz77_rgba_decompress
     *
     * Dekompresuje strumien tokenow LZ77 z powrotem do tablicy pikseli RGBA.
     *
     * Parametry:
     *   src     – wejscie: skompresowany strumien tokenow
     *   src_len – dlugosc strumienia w bajtach
     *   dst_px  – wyjscie: tablica pikseli RGBA
     *   dst_cap – pojemnosc bufora wyjsciowego w pikselach
     *   out_len – [out] liczba zdekompresowanych pikseli (0 = blad)
     */
    __declspec(dllexport)
        void lz77_rgba_decompress(
            const uint8_t* src,
            size_t          src_len,
            uint32_t* dst_px,
            size_t          dst_cap,
            size_t* out_len
        );

    /*
     * LZ77_WORK_NEED_BYTES
     *
     * Minimalny rozmiar bufora roboczego wymaganego przez lz77_rgba_compress:
     *   head[65536 wpisow] = 256 KB
     *   prev[ 4096 wpisow] =  16 KB
     *   Razem             = 272 KB
     */
    static const size_t LZ77_WORK_NEED_BYTES = (65536u + 4096u) * sizeof(uint32_t);

#ifdef __cplusplus
}
#endif