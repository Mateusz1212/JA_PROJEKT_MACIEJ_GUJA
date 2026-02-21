
/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – odpowiednik kodu asemblerowego MASM x64 napisany w C++
 * DATA WYKONANIA: luty 2026 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Maciej Guja
 * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

#pragma once

 // WAŻNE: NOMINMAX zapobiega konfliktom między makrami min/max z <windows.h>
 // a szablonami std::min / std::max z nagłówków STL.
 // Musi być zdefiniowane PRZED dołączeniem <windows.h>.
#define NOMINMAX

#include <windows.h>
#include <gdiplus.h>
// WAŻNE: automatyczne linkowanie biblioteki GDI+ (Windows Imaging).
// GDI+ jest używane do odczytu/zapisu obrazów w wielu formatach (PNG, JPG, BMP...).
#pragma comment(lib, "gdiplus.lib")

#include <string>
#include <sstream>
#include <vector>
#include <deque>    // kolejka zadań dla workerów (FIFO)
#include <set>      // zbiór dozwolonych rozszerzeń plików
#include <mutex>    // synchronizacja dostępu do zasobów współdzielonych
#include <thread>   // wielowątkowość
#include <atomic>   // bezpieczne operacje na licznikach z wielu wątków
#include <chrono>   // precyzyjny pomiar czasu (steady_clock)
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ============================================================
// WAŻNE: Typy wskaźników na funkcje LZ77 ładowane dynamicznie z DLL.
//
// Mechanizm działania:
//   - Zamiast linkować statycznie, DLL jest ładowana w czasie działania
//     programu przez LoadLibraryA(), a wskaźniki na konkretne funkcje
//     pobierane przez GetProcAddress().
//   - Dzięki temu ten sam kod C# (Logic.dll) może wybierać w locie,
//     czy chce użyć implementacji w C++ (CppDll.dll) czy w ASM (AsmDll.dll).
//
// LZ77CompressFunc — sygnatura funkcji kompresji:
//   [in]  src         — tablica pikseli RGBA (uint32_t, jeden piksel = 4 bajty)
//   [in]  srcCount    — liczba pikseli (width * height)
//   [out] dst         — bufor wyjściowy na tokeny LZ77
//   [in]  dstCap      — rozmiar bufora wyjściowego w bajtach
//   [in]  work        — bufor roboczy (tablice head[] + prev[])
//   [in]  workBytes   — rozmiar bufora roboczego w bajtach
//   [out] outLen      — liczba faktycznie zapisanych bajtów w dst
// ============================================================
using LZ77CompressFunc = void(*)(const uint32_t*, size_t,
    uint8_t*, size_t,
    void*, size_t,
    size_t*);

// ============================================================
// LZ77DecompressFunc — sygnatura funkcji dekompresji:
//   [in]  src         — bufor z tokenami LZ77 (wyprodukowany przez compFn)
//   [in]  srcBytes    — liczba bajtów w buforze src
//   [out] dst         — bufor wyjściowy na piksele RGBA (uint32_t)
//   [in]  dstCount    — oczekiwana liczba pikseli (width * height)
//   [out] outLen      — liczba faktycznie odtworzonych pikseli
// ============================================================
using LZ77DecompressFunc = void(*)(const uint8_t*, size_t,
    uint32_t*, size_t,
    size_t*);

// ============================================================
// WAŻNE: Minimalny rozmiar bufora roboczego wymaganego przez
// lz77_rgba_compress.
//
// Bufor roboczy składa się z dwóch tablic używanych przez algorytm LZ77
// do wyszukiwania dopasowań w oknie historii (sliding window):
//   head[65536] — tablica głów list powiązanych; indeksowana wartością piksela
//                 (uint32_t mod 65536); przechowuje indeks ostatniego wystąpienia
//                 każdego 16-bitowego "hasha" piksela. Rozmiar: 256 KB.
//   prev[4096]  — tablica poprzednich pozycji; implementuje listy powiązane
//                 dla każdego łańcucha dopasowań wewnątrz okna przesuwnego
//                 o rozmiarze 4096 pikseli. Rozmiar: 16 KB.
//
// Suma: (65536 + 4096) * 4 bajty = 272 KB.
//
// Wartość powielona tutaj celowo, aby Logic.dll NIE zależała od nagłówka DLL.
// ============================================================
static const size_t LOGIC_LZ77_WORK_BYTES = (65536u + 4096u) * sizeof(uint32_t);

// ============================================================
// WAŻNE: Nagłówek własnego formatu binarnego pliku .lz77.
//
// Każdy skompresowany plik ma następującą strukturę:
//   [uint32  magic]           — identyfikator formatu; wartość 0x4C5A3737 = "LZ77"
//                               w ASCII; służy do weryfikacji integralności pliku
//   [uint32  width]           — szerokość oryginalnego obrazu w pikselach
//   [uint32  height]          — wysokość oryginalnego obrazu w pikselach
//   [uint64  compressedBytes] — liczba bajtów danych tokenów LZ77 po nagłówku
//   [compressedBytes bajtów]  — dane tokenów LZ77 (ciągłe)
//
// UWAGA: #pragma pack(push, 1) wyłącza wyrównanie (padding) pól struktury,
// gwarantując, że sizeof(Lz77FileHeader) == 4+4+4+8 = 20 bajtów,
// niezależnie od platformy i ustawień kompilatora. Jest to konieczne,
// bo nagłówek jest zapisywany i odczytywany jako surowy blok bajtów (ReadFile/WriteFile).
// ============================================================
#pragma pack(push, 1)
struct Lz77FileHeader {
    uint32_t magic;             // 0x4C5A3737 — "LZ77" w ASCII; znacznik początku pliku
    uint32_t width;             // szerokość obrazu w pikselach
    uint32_t height;            // wysokość obrazu w pikselach
    uint64_t compressedBytes;   // rozmiar danych tokenów LZ77 następujących po nagłówku
};
#pragma pack(pop)

// Stała magiczna — "LZ77" zakodowane jako 4 bajty little-endian.
// Używana zarówno przy zapisie (WriteCompressedFile) jak i walidacji odczytu (ReadCompressedFile).
static const uint32_t LZ77_FILE_MAGIC = 0x4C5A3737u;

// ============================================================
// WAŻNE: Typy callbacków dla warstwy C# (P/Invoke).
//
// Konwencja wywołania __stdcall jest WYMAGANA przez P/Invoke w .NET —
// domyślna konwencja C++ (__cdecl) jest niezgodna z P/Invoke i spowoduje
// błąd wywołania lub uszkodzenie stosu.
//
// ProgressCallback — wywoływany po zakończeniu każdego pliku.
//   [in] percent — postęp w procentach (0..100)
//
// LogCallback — wywoływany z komunikatami tekstowymi (stan, błędy, podsumowanie).
//   [in] message — wchar_t* (UTF-16), zgodny z System.String w C#
// ============================================================
using ProgressCallback = void(__stdcall*)(int percent);
using LogCallback = void(__stdcall*)(const wchar_t* message);

// ============================================================
// WAŻNE: Eksporty DLL wywołane z C# przez P/Invoke.
//
// extern "C" — wyłącza name mangling C++; nazwy funkcji w tablicy eksportów DLL
//              muszą być dokładnie "StartCompression" i "StartDecompression",
//              aby P/Invoke mógł je znaleźć przez DllImport.
// __declspec(dllexport) — oznacza funkcje jako widoczne eksporty DLL.
// __stdcall — wymagana konwencja wywołań dla P/Invoke (patrz wyżej).
//
// ============================================================
extern "C" {
    // ----------------------------------------------------------
    // StartCompression — kompresuje wszystkie obrazy z sourceFolder
    // do plików .lz77 w outputFolder.
    //
    // Parametry:
    //   sourceFolder  — folder z plikami obrazów (PNG/JPG/BMP/TIFF/GIF)
    //   outputFolder  — folder docelowy dla plików .lz77
    //   useASM        — true = użyj AsmDll.dll, false = użyj CppDll.dll
    //   numThreads    — liczba wątków roboczych (wartość z suwaka GUI; min. 1)
    //   progressCb    — callback wywoływany po zakończeniu każdego pliku
    //                   (argument: procent ukończenia 0..100)
    //   logCb         — callback z komunikatami tekstowymi (logi postępu i błędów)
    //   outElapsedMs  — [out] TYLKO czas wywołań lz77_rgba_compress w ms
    //                   (bez czasu I/O; używany do porównania ASM vs C++)
    // ----------------------------------------------------------
    __declspec(dllexport)
        void __stdcall StartCompression(
            const wchar_t* sourceFolder,
            const wchar_t* outputFolder,
            bool             useASM,
            int              numThreads,
            ProgressCallback progressCb,
            LogCallback      logCb,
            int64_t* outElapsedMs   // [out] czas samego algorytmu LZ77 w ms
        );

    // ----------------------------------------------------------
    // StartDecompression — dekompresuje wszystkie pliki .lz77 z sourceFolder
    // do plików .bmp w outputFolder.
    //
    // Parametry:
    //   sourceFolder  — folder z plikami .lz77
    //   outputFolder  — folder docelowy dla zdekompresowanych obrazów .bmp
    //   pozostałe     — jak w StartCompression
    //   outElapsedMs  — [out] TYLKO czas wywołań lz77_rgba_decompress w ms
    // ----------------------------------------------------------
    __declspec(dllexport)
        void __stdcall StartDecompression(
            const wchar_t* sourceFolder,
            const wchar_t* outputFolder,
            bool             useASM,
            int              numThreads,
            ProgressCallback progressCb,
            LogCallback      logCb,
            int64_t* outElapsedMs   // [out] czas samego algorytmu LZ77 w ms
        );
}