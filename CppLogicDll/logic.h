#pragma once

// Wymagane przed windows.h, aby uniknąć kolizji min/max z STL
#define NOMINMAX

#include <windows.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <string>
#include <sstream>
#include <vector>
#include <deque>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ============================================================
// Typy wskaznikow na funkcje LZ77 ladowane dynamicznie z DLL
// (Dll_CPP.dll lub Dll_ASM.dll)
// ============================================================
using LZ77CompressFunc = void(*)(const uint32_t*, size_t,
    uint8_t*, size_t,
    void*, size_t,
    size_t*);

using LZ77DecompressFunc = void(*)(const uint8_t*, size_t,
    uint32_t*, size_t,
    size_t*);

// ============================================================
// Minimalny rozmiar bufora roboczego wymaganego przez
// lz77_rgba_compress (powielenie stalej z lz77.h, aby nie
// zalezec od naglowka DLL w projekcie Logic)
//   head[65536] = 256 KB  +  prev[4096] = 16 KB
// ============================================================
static const size_t LOGIC_LZ77_WORK_BYTES = (65536u + 4096u) * sizeof(uint32_t);

// ============================================================
// Naglowek wlasnego formatu plikow .lz77 zapisywanych przez
// workerow kompresji. Format:
//   [uint32 magic]          0x4C5A3737 ("LZ77")
//   [uint32 width]          szerokosc obrazu w pikselach
//   [uint32 height]         wysokosc obrazu w pikselach
//   [uint64 compressedBytes] liczba bajtow danych po naglowku
//   [compressedBytes bajtow danych tokenow LZ77]
// ============================================================
#pragma pack(push, 1)
struct Lz77FileHeader {
    uint32_t magic;             // 0x4C5A3737
    uint32_t width;
    uint32_t height;
    uint64_t compressedBytes;
};
#pragma pack(pop)

static const uint32_t LZ77_FILE_MAGIC = 0x4C5A3737u;

// ============================================================
// Typy callbackow dla C# (P/Invoke __stdcall)
// ============================================================
using ProgressCallback = void(__stdcall*)(int percent);
using LogCallback = void(__stdcall*)(const wchar_t* message);

// ============================================================
// Eksporty DLL (Logic.dll) wywolywane z C# przez P/Invoke.
//
// StartCompression:
//   sourceFolder  - folder z plikami obrazkow (PNG/JPG/BMP/...)
//   outputFolder  - folder docelowy dla plikow .lz77
//   useASM        - true = Dll_ASM.dll, false = Dll_CPP.dll
//   numThreads    - liczba workerow z suwaka GUI (min. 1)
//   progressCb    - callback wywoływany po zakonczeniu każdego pliku
//   logCb         - callback z komunikatami tekstowymi
//
// StartDecompression:
//   sourceFolder  - folder z plikami .lz77 (wcześniej wypakowany ZIP)
//   outputFolder  - folder docelowy dla zdekompresowanych obrazow .bmp
//   pozostale parametry jak wyzej
// ============================================================
extern "C" {
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