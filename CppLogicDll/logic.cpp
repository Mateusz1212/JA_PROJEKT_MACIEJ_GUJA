
/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – odpowiednik kodu asemblerowego MASM x64 napisany w C++
 * DATA WYKONANIA: luty 2026 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Maciej Guja
 * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

#define NOMINMAX
#undef max
#undef min

#include "logic.h"
#include <sstream>
#include <fstream>
#include <algorithm>

 // ============================================================
 // Inicjalizacja GDI+ — wywolac raz przed uzyciem Gdiplus::Bitmap
 // Zainicjalizowana przy ladowaniu DLL (DllMain), shutdown przy zwalnianiu.
 // ============================================================
static ULONG_PTR g_gdiplusToken = 0;

BOOL WINAPI DllMain(HINSTANCE /*hInst*/, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_gdiplusToken != 0) {
            Gdiplus::GdiplusShutdown(g_gdiplusToken);
            g_gdiplusToken = 0;
        }
    }
    return TRUE;
}

// ============================================================
// Pomocnicze: ladowanie funkcji LZ77 z wybranej DLL
// Ladujemy tylko jedna DLL naraz (useASM decyduje ktora).
// Zwraca false i wypelnia errorOut jesli cos zawiedzie.
// ============================================================
static bool LoadLZ77DLL(bool useASM,
    HMODULE& hMod,
    LZ77CompressFunc& compFn,
    LZ77DecompressFunc& decompFn,
    std::wstring& errorOut)
{
    // Nazwy DLL zgodne z nazwami projektow w rozwiazaniu VS
    const char* dllName = useASM ? "AsmDll.dll" : "CppDll.dll";

    hMod = LoadLibraryA(dllName);
    if (!hMod) {
        DWORD err = GetLastError();
        std::wstringstream ss;
        ss << L"LoadLibraryA(" << dllName << L") failed, kod bledu: " << err;
        errorOut = ss.str();
        return false;
    }

    compFn = reinterpret_cast<LZ77CompressFunc>  (GetProcAddress(hMod, "lz77_rgba_compress"));
    decompFn = reinterpret_cast<LZ77DecompressFunc>(GetProcAddress(hMod, "lz77_rgba_decompress"));

    if (!compFn || !decompFn) {
        std::wstringstream ss;
        ss << L"GetProcAddress nie znalazlo eksportow LZ77 w: " << dllName;
        errorOut = ss.str();
        FreeLibrary(hMod);
        hMod = nullptr;
        return false;
    }
    return true;
}

// ============================================================
// Pomocnicze: wczytywanie obrazka do tablicy pikseli RGBA
// za pomoca GDI+ (obslugiuje PNG, JPG, BMP, TIFF, GIF, ...)
// Zwraca false jesli plik jest niedostepny lub uszkodzony.
// ============================================================
static bool LoadImagePixels(const std::wstring& path,
    std::vector<uint32_t>& pixels,
    uint32_t& width,
    uint32_t& height)
{
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return false;
    }

    width = bmp->GetWidth();
    height = bmp->GetHeight();

    if (width == 0 || height == 0) {
        delete bmp;
        return false;
    }

    pixels.resize(static_cast<size_t>(width) * height);

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData bmpData{};

    if (bmp->LockBits(&rect,
        Gdiplus::ImageLockModeRead,
        PixelFormat32bppARGB,
        &bmpData) != Gdiplus::Ok)
    {
        delete bmp;
        return false;
    }

    // Kopiowanie wiersz po wierszu (stride moze byc wiekszy niz width*4)
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(pixels.data() + y * width,
            reinterpret_cast<const uint8_t*>(bmpData.Scan0) + y * bmpData.Stride,
            static_cast<size_t>(width) * sizeof(uint32_t));
    }

    bmp->UnlockBits(&bmpData);
    delete bmp;
    return true;
}

// ============================================================
// Pomocnicze: wyszukiwanie CLSID enkodera GDI+ po MIME type
// Potrzebne do zapisu BMP przez Gdiplus::Bitmap::Save().
// ============================================================
static bool GetGdiplusEncoderClsid(const wchar_t* mimeType, CLSID& clsid)
{
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;

    std::vector<uint8_t> buf(size);
    Gdiplus::GetImageEncoders(num, size,
        reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data()));

    for (UINT i = 0; i < num; ++i) {
        const auto& codec = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data())[i];
        if (wcscmp(codec.MimeType, mimeType) == 0) {
            clsid = codec.Clsid;
            return true;
        }
    }
    return false;
}

// ============================================================
// Pomocnicze: zapisywanie tablicy pikseli RGBA jako plik BMP
// za pomoca GDI+.
// ============================================================
static bool SavePixelsAsBMP(const std::wstring& path,
    const std::vector<uint32_t>& pixels,
    uint32_t width,
    uint32_t height)
{
    Gdiplus::Bitmap bmp(static_cast<INT>(width),
        static_cast<INT>(height),
        PixelFormat32bppARGB);

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData bmpData{};

    if (bmp.LockBits(&rect,
        Gdiplus::ImageLockModeWrite,
        PixelFormat32bppARGB,
        &bmpData) != Gdiplus::Ok)
        return false;

    for (uint32_t y = 0; y < height; ++y) {
        memcpy(reinterpret_cast<uint8_t*>(bmpData.Scan0) + y * bmpData.Stride,
            pixels.data() + y * width,
            static_cast<size_t>(width) * sizeof(uint32_t));
    }

    bmp.UnlockBits(&bmpData);

    CLSID bmpClsid{};
    if (!GetGdiplusEncoderClsid(L"image/bmp", bmpClsid))
        return false;

    return bmp.Save(path.c_str(), &bmpClsid) == Gdiplus::Ok;
}

// ============================================================
// Pomocnicze: zapis skompresowanego pliku .lz77
// Naglowek Lz77FileHeader + dane tokenow.
// ============================================================
static bool WriteCompressedFile(const std::wstring& path,
    uint32_t width,
    uint32_t height,
    const uint8_t* data,
    size_t dataSize)
{
    HANDLE hFile = CreateFileW(path.c_str(),
        GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    Lz77FileHeader hdr{};
    hdr.magic = LZ77_FILE_MAGIC;
    hdr.width = width;
    hdr.height = height;
    hdr.compressedBytes = static_cast<uint64_t>(dataSize);

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, &hdr, sizeof(hdr), &written, nullptr);
    ok = ok && WriteFile(hFile, data, static_cast<DWORD>(dataSize), &written, nullptr);

    CloseHandle(hFile);
    return ok && (written == static_cast<DWORD>(dataSize));
}

// ============================================================
// Pomocnicze: odczyt pliku .lz77
// Weryfikuje magic, odczytuje wymiary i dane.
// ============================================================
static bool ReadCompressedFile(const std::wstring& path,
    uint32_t& width,
    uint32_t& height,
    std::vector<uint8_t>& data)
{
    HANDLE hFile = CreateFileW(path.c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    Lz77FileHeader hdr{};
    DWORD read = 0;
    ReadFile(hFile, &hdr, sizeof(hdr), &read, nullptr);

    if (read != sizeof(hdr) || hdr.magic != LZ77_FILE_MAGIC) {
        CloseHandle(hFile);
        return false;
    }

    width = hdr.width;
    height = hdr.height;

    if (hdr.compressedBytes == 0 || hdr.compressedBytes > 512u * 1024u * 1024u) {
        // Zabezpieczenie przed uszkodzonymi/gigantycznymi plikami (> 512 MB)
        CloseHandle(hFile);
        return false;
    }

    data.resize(static_cast<size_t>(hdr.compressedBytes));
    ReadFile(hFile, data.data(), static_cast<DWORD>(hdr.compressedBytes), &read, nullptr);

    CloseHandle(hFile);
    return (read == static_cast<DWORD>(hdr.compressedBytes));
}

// ============================================================
// Pomocnicze: rozszerzenia obrazkow obslugiwane przez GDI+
// ============================================================
static const std::set<std::wstring> IMAGE_EXTENSIONS = {
    L".png", L".jpg", L".jpeg", L".bmp",
    L".tiff", L".tif", L".gif"
};

// ============================================================
// StartCompression
//
// Algorytm:
//   1. Laduje Dll_CPP.dll lub Dll_ASM.dll (tylko jedna naraz).
//   2. Zbiera wszystkie pliki obrazkow z sourceFolder do kolejki.
//   3. Uruchamia numThreads workerow.
//   4. Kazdy worker w petli:
//        a) pobiera nastepny plik z kolejki (mutex),
//        b) wczytuje piksele RGBA przez GDI+,
//        c) wywoluje lz77_rgba_compress z zaladowanej DLL,
//        d) zapisuje wynik jako plik .lz77 w outputFolder,
//        e) inkrementuje licznik i raportuje postep.
//   5. Po zakonczeniu wszystkich watkow: zwalnia DLL, koniec.
// ============================================================
void __stdcall StartCompression(
    const wchar_t* sourceFolder,
    const wchar_t* outputFolder,
    bool             useASM,
    int              numThreads,
    ProgressCallback progressCb,
    LogCallback      logCb,
    int64_t* outElapsedMs)
{
    // --- Ladujemy JEDNA wybrana DLL (nie obie naraz)
    HMODULE          hMod = nullptr;
    LZ77CompressFunc compFn = nullptr;
    LZ77DecompressFunc decompFn = nullptr;  // ladujemy tez decomp (wymagane przez LoadLZ77DLL)
    std::wstring dllError;

    if (!LoadLZ77DLL(useASM, hMod, compFn, decompFn, dllError)) {
        if (logCb) logCb((L"Blad ladowania DLL: " + dllError).c_str());
        return;
    }
    if (logCb) logCb(useASM ? L"Zaladowano DLL: Dll_ASM.dll"
        : L"Zaladowano DLL: Dll_CPP.dll");

    // --- Zbierz pliki obrazkow z sourceFolder do kolejki
    std::deque<std::wstring> queue;
    try {
        for (auto& entry : fs::directory_iterator(sourceFolder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (IMAGE_EXTENSIONS.count(ext)) {
                queue.push_back(entry.path().wstring());
            }
        }
    }
    catch (const std::exception& ex) {
        std::string msg(ex.what());
        std::wstring wmsg(msg.begin(), msg.end());
        if (logCb) logCb((L"Blad enumeracji folderu: " + wmsg).c_str());
        FreeLibrary(hMod);
        return;
    }

    int totalFiles = static_cast<int>(queue.size());
    if (totalFiles == 0) {
        if (logCb) logCb(L"Brak plikow obrazkow w folderze zrodlowym.");
        FreeLibrary(hMod);
        return;
    }

    // Upewnij sie, ze folder wyjsciowy istnieje
    CreateDirectoryW(outputFolder, nullptr);

    // --- Przygotowanie zmiennych wspoldzielonych miedzy workerami
    std::mutex       queue_mtx;   // chroni kolejke zadan
    std::mutex       log_mtx;     // chroni wywolania logCb
    std::atomic<int> processed(0);

    // Suma czasow wywolan compFn ze wszystkich watkow [nanosekundy].
    // Kazdy worker dodaje tu czas TYLKO wywolania compFn (nie I/O).
    std::atomic<long long> algoNs{ 0 };

    // --- Worker lambda — kazdy worker przetwarza jeden plik na raz
    auto worker = [&](int workerId) {
        while (true) {
            // Pobierz nastepne zadanie z kolejki
            std::wstring filePath;
            {
                std::lock_guard<std::mutex> lk(queue_mtx);
                if (queue.empty()) break;   // brak zadan — koniec petli workera
                filePath = std::move(queue.front());
                queue.pop_front();
            }

            std::wstring fileName = fs::path(filePath).filename().wstring();

            // Wczytaj piksele RGBA za pomoca GDI+
            std::vector<uint32_t> pixels;
            uint32_t w = 0, h = 0;
            if (!LoadImagePixels(filePath, pixels, w, h)) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Nie mozna wczytac: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            size_t pixelCount = static_cast<size_t>(w) * h;

            // Bufor wyjsciowy: pesymistyczny worst-case = kazdy piksel jako literal token (12 B)
            size_t dstCap = pixelCount * 12u + 64u;
            std::vector<uint8_t> dst(dstCap);

            // Bufor roboczy LZ77 (head[] + prev[]) — LOGIC_LZ77_WORK_BYTES
            std::vector<uint8_t> work(LOGIC_LZ77_WORK_BYTES);

            // Wywolaj funkcje kompresji z zaladowanej DLL — TYLKO ten odcinek jest mierzony
            size_t outLen = 0;
            try {
                auto t0 = std::chrono::steady_clock::now();
                compFn(pixels.data(), pixelCount,
                    dst.data(), dstCap,
                    work.data(), work.size(),
                    &outLen);
                auto t1 = std::chrono::steady_clock::now();
                algoNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            }
            catch (...) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Wyjatek podczas kompresji: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            if (outLen == 0) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Kompresja zwrocila 0 bajtow: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            // Zapisz plik .lz77 (naglowek + dane) do outputFolder
            std::wstring stem = fs::path(filePath).stem().wstring();
            std::wstring outFile = std::wstring(outputFolder) + L"\\" + stem + L".lz77";

            if (!WriteCompressedFile(outFile, w, h, dst.data(), outLen)) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Blad zapisu: " + stem + L".lz77").c_str());
            }
            else {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Skompresowano: " + fileName).c_str());
            }

            // Aktualizacja postępu (thread-safe przez atomic)
            processed++;
            if (progressCb) progressCb((processed.load() * 100) / totalFiles);
        }
        };

    // --- Uruchom numThreads workerow (min. 1 zgodnie z suwakiiem GUI)
    int actualThreads = std::max(1, numThreads);
    std::vector<std::thread> workers;
    workers.reserve(actualThreads);
    for (int i = 0; i < actualThreads; ++i) {
        workers.emplace_back(worker, i);
    }

    // --- Czekaj na zakonczenie wszystkich watkow
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    // Przelicz zsumowany czas wywolan compFn na milisekundy i zwroc do C#
    int64_t elapsedMs = static_cast<int64_t>(algoNs.load() / 1'000'000LL);
    if (outElapsedMs) *outElapsedMs = elapsedMs;

    if (progressCb) progressCb(100);

    std::wstringstream rpt;
    rpt << L"--- Kompresja zakonczona ---\n"
        << L"Plikow: " << totalFiles << L"  |  "
        << L"Watkow: " << actualThreads << L"  |  "
        << L"Czas algorytmu LZ77: " << elapsedMs << L" ms";
    if (logCb) logCb(rpt.str().c_str());

    FreeLibrary(hMod);
}

// ============================================================
// StartDecompression
//
// Algorytm:
//   1. Laduje Dll_CPP.dll lub Dll_ASM.dll (tylko jedna naraz).
//   2. Zbiera wszystkie pliki .lz77 z sourceFolder do kolejki.
//   3. Uruchamia numThreads workerow.
//   4. Kazdy worker w petli:
//        a) pobiera nastepny plik .lz77 z kolejki (mutex),
//        b) odczytuje naglowek (wymiary) i dane skompresowane,
//        c) wywoluje lz77_rgba_decompress z zaladowanej DLL,
//        d) zapisuje zdekompresowany obraz jako .bmp w outputFolder,
//        e) inkrementuje licznik i raportuje postep.
//   5. Po zakonczeniu wszystkich watkow: zwalnia DLL, koniec.
// ============================================================
void __stdcall StartDecompression(
    const wchar_t* sourceFolder,
    const wchar_t* outputFolder,
    bool             useASM,
    int              numThreads,
    ProgressCallback progressCb,
    LogCallback      logCb,
    int64_t* outElapsedMs)
{
    // --- Ladujemy JEDNA wybrana DLL (nie obie naraz)
    HMODULE            hMod = nullptr;
    LZ77CompressFunc   compFn = nullptr;  // wymagane przez LoadLZ77DLL
    LZ77DecompressFunc decompFn = nullptr;
    std::wstring dllError;

    if (!LoadLZ77DLL(useASM, hMod, compFn, decompFn, dllError)) {
        if (logCb) logCb((L"Blad ladowania DLL: " + dllError).c_str());
        return;
    }
    if (logCb) logCb(useASM ? L"Zaladowano DLL: Dll_ASM.dll"
        : L"Zaladowano DLL: Dll_CPP.dll");

    // --- Zbierz pliki .lz77 z sourceFolder do kolejki
    std::deque<std::wstring> queue;
    try {
        for (auto& entry : fs::directory_iterator(sourceFolder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".lz77") {
                queue.push_back(entry.path().wstring());
            }
        }
    }
    catch (const std::exception& ex) {
        std::string msg(ex.what());
        std::wstring wmsg(msg.begin(), msg.end());
        if (logCb) logCb((L"Blad enumeracji folderu: " + wmsg).c_str());
        FreeLibrary(hMod);
        return;
    }

    int totalFiles = static_cast<int>(queue.size());
    if (totalFiles == 0) {
        if (logCb) logCb(L"Brak plikow .lz77 w folderze zrodlowym.");
        FreeLibrary(hMod);
        return;
    }

    // Upewnij sie, ze folder wyjsciowy istnieje
    CreateDirectoryW(outputFolder, nullptr);

    // --- Przygotowanie zmiennych wspoldzielonych
    std::mutex       queue_mtx;
    std::mutex       log_mtx;
    std::atomic<int> processed(0);

    // Suma czasow wywolan decompFn ze wszystkich watkow [nanosekundy].
    // Kazdy worker dodaje tu czas TYLKO wywolania decompFn (nie I/O).
    std::atomic<long long> algoNs{ 0 };

    auto worker = [&](int workerId) {
        while (true) {
            std::wstring filePath;
            {
                std::lock_guard<std::mutex> lk(queue_mtx);
                if (queue.empty()) break;   // brak zadan — koniec petli workera
                filePath = std::move(queue.front());
                queue.pop_front();
            }

            std::wstring fileName = fs::path(filePath).filename().wstring();

            // Odczytaj naglowek + dane skompresowane z pliku .lz77
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> compData;
            if (!ReadCompressedFile(filePath, w, h, compData)) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Nie mozna wczytac lub uszkodzony: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            size_t pixelCount = static_cast<size_t>(w) * h;
            std::vector<uint32_t> pixels(pixelCount, 0);

            // Wywolaj funkcje dekompresji z zaladowanej DLL — TYLKO ten odcinek jest mierzony
            size_t outLen = 0;
            try {
                auto t0 = std::chrono::steady_clock::now();
                decompFn(compData.data(), compData.size(),
                    pixels.data(), pixelCount,
                    &outLen);
                auto t1 = std::chrono::steady_clock::now();
                algoNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            }
            catch (...) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Wyjatek podczas dekompresji: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            if (outLen != pixelCount) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Niezgodna liczba pikseli po dekompresji: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            // Zapisz zdekompresowany obraz jako .bmp w outputFolder
            std::wstring stem = fs::path(filePath).stem().wstring();
            std::wstring outFile = std::wstring(outputFolder) + L"\\" + stem + L".bmp";

            if (!SavePixelsAsBMP(outFile, pixels, w, h)) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Blad zapisu BMP: " + stem).c_str());
            }
            else {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Zdekompresowano: " + stem + L".bmp").c_str());
            }

            processed++;
            if (progressCb) progressCb((processed.load() * 100) / totalFiles);
        }
        };

    // --- Uruchom numThreads workerow
    int actualThreads = std::max(1, numThreads);
    std::vector<std::thread> workers;
    workers.reserve(actualThreads);
    for (int i = 0; i < actualThreads; ++i) {
        workers.emplace_back(worker, i);
    }

    // --- Czekaj na zakonczenie wszystkich watkow
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    // Przelicz zsumowany czas wywolan decompFn na milisekundy i zwroc do C#
    int64_t elapsedMs = static_cast<int64_t>(algoNs.load() / 1'000'000LL);
    if (outElapsedMs) *outElapsedMs = elapsedMs;

    if (progressCb) progressCb(100);

    std::wstringstream rpt;
    rpt << L"--- Dekompresja zakonczona ---\n"
        << L"Plikow: " << totalFiles << L"  |  "
        << L"Watkow: " << actualThreads << L"  |  "
        << L"Czas algorytmu LZ77: " << elapsedMs << L" ms";
    if (logCb) logCb(rpt.str().c_str());

    FreeLibrary(hMod);
}