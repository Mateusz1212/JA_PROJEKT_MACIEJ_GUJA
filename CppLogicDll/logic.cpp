/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – odpowiednik kodu asemblerowego MASM x64 napisany w C++
 * DATA WYKONANIA: luty 2026 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Maciej Guja
 * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

 // WAŻNE: NOMINMAX i undef min/max — podwójne zabezpieczenie przed konfliktami
 // makr WinAPI (min/max) z szablonami std::min/std::max z STL.
 // #define NOMINMAX zapobiega definiowaniu makr przez <windows.h>,
 // natomiast #undef usuwa je, gdyby zostały zdefiniowane przez inne nagłówki
 // włączone wcześniej (np. z precompiled headers).

#define NOMINMAX
#undef max
#undef min

#include "logic.h"
#include <sstream>
#include <fstream>
#include <algorithm>

static ULONG_PTR g_gdiplusToken = 0;

// ============================================================
// WAŻNE: DllMain — punkt wejścia DLL, wywoływany przez system Windows.
//
// reason == DLL_PROCESS_ATTACH: DLL jest ładowana do procesu.
//   Inicjalizujemy GDI+. Musi się to odbyć przed jakimkolwiek
//   użyciem Gdiplus::Bitmap lub innych klas GDI+.
//
// reason == DLL_PROCESS_DETACH: DLL jest wyładowywana z procesu.
//   Zwalniamy GDI+. Sprawdzamy czy token != 0, bo Shutdown()
//   na niezainicjowanym tokenie spowodowałby crash.
//
// Zwraca TRUE — wymagane przez WinAPI; FALSE zatrzymałoby ładowanie DLL.
// ============================================================
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
// WAŻNE: LoadLZ77DLL — dynamiczne ładowanie implementacji algorytmu LZ77.
//
// Cel: pozwolić GUI przełączać w locie między implementacją C++ a ASM,
// bez rekompilacji Logic.dll. Obie DLL eksportują identyczne sygnatury
// funkcji ("lz77_rgba_compress", "lz77_rgba_decompress"), więc reszta
// kodu jest całkowicie nieświadoma różnicy.
//
// Parametry wyjściowe:
//   hMod     — uchwyt załadowanej DLL; caller musi wywołać FreeLibrary(hMod)
//              po zakończeniu pracy, aby zwolnić zasoby.
//   compFn   — wskaźnik na funkcję kompresji
//   decompFn — wskaźnik na funkcję dekompresji
//   errorOut — komunikat błędu (tylko gdy funkcja zwraca false)
//
// WAŻNE: Ładujemy TYLKO JEDNĄ DLL naraz (albo ASM albo CPP).
// Ładowanie obu jednocześnie jest zbędne i mogłoby powodować konflikty
// globalnych tablic head[]/prev[] gdyby DLL-e nie izolowały swojego stanu.
// ============================================================
static bool LoadLZ77DLL(bool useASM,
    HMODULE& hMod,
    LZ77CompressFunc& compFn,
    LZ77DecompressFunc& decompFn,
    std::wstring& errorOut)
{
    // Nazwy DLL zgodne z nazwami projektow w rozwiazaniu VS
    const char* dllName = useASM ? "AsmDll.dll" : "CppDll.dll";

    // WAŻNE: LoadLibraryA ładuje DLL z tego samego katalogu co Logic.dll
    // (domyślne zachowanie systemu Windows dla ścieżek względnych).
    // Jeśli DLL nie zostanie znaleziona, GetLastError() zwróci kod błędu
    // (np. 126 = ERROR_MOD_NOT_FOUND).
    hMod = LoadLibraryA(dllName);
    if (!hMod) {
        DWORD err = GetLastError();
        std::wstringstream ss;
        ss << L"LoadLibraryA(" << dllName << L") failed, kod bledu: " << err;
        errorOut = ss.str();
        return false;
    }

    // WAŻNE: GetProcAddress — pobieramy wskaźniki funkcji po nazwie eksportu.
    // Rzutowanie reinterpret_cast jest konieczne, bo GetProcAddress zwraca
    // generyczny FARPROC (void*). Sygnatury muszą dokładnie odpowiadać tym
    // z nagłówka DLL — niezgodność typów prowadzi do UB lub naruszenia stosu.
    compFn = reinterpret_cast<LZ77CompressFunc>  (GetProcAddress(hMod, "lz77_rgba_compress"));
    decompFn = reinterpret_cast<LZ77DecompressFunc>(GetProcAddress(hMod, "lz77_rgba_decompress"));

    // WAŻNE: Walidacja obu wskaźników przed zwrotem.
    // Brak eksportu oznacza niezgodną wersję DLL lub błąd budowania projektu.
    // W takim przypadku zwalniamy DLL (FreeLibrary), by nie było wycieku zasobów.
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
// WAŻNE: LoadImagePixels — wczytywanie obrazu do liniowej tablicy pikseli RGBA.
//
// Używa GDI+ (Gdiplus::Bitmap) — obsługuje PNG, JPG, BMP, TIFF, GIF i inne.
// Wszystkie formaty są sprowadzane do jednolitego formatu PixelFormat32bppARGB:
//   każdy piksel = 4 bajty [Alpha, Red, Green, Blue].
//
// WAŻNE — obsługa Stride:
//   Stride (bmpData.Stride) to liczba bajtów w jednym wierszu danych GDI+.
//   Może być WIĘKSZY niż width * 4, ponieważ GDI+ wyrównuje wiersze
//   do wielokrotności 4 bajtów. Dlatego kopiujemy wiersz po wierszu
//   (pętla po y), a nie całość jednym memcpy — kopiowałoby padding!
// ============================================================
static bool LoadImagePixels(const std::wstring& path,
    std::vector<uint32_t>& pixels,
    uint32_t& width,
    uint32_t& height)
{
    // Tworzymy obiekt Bitmap z pliku; GetLastStatus() sprawdza czy GDI+ załadował poprawnie.
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(path.c_str());
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return false;
    }

    width = bmp->GetWidth();
    height = bmp->GetHeight();

    // Zabezpieczenie przed zerowymi wymiarami (uszkodzony lub pusty obraz).
    if (width == 0 || height == 0) {
        delete bmp;
        return false;
    }

    // Alokacja bufora na width * height pikseli (każdy uint32_t = ARGB).
    pixels.resize(static_cast<size_t>(width) * height);

    Gdiplus::Rect rect(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData bmpData{};

    // WAŻNE: LockBits blokuje piksele w pamięci i daje bezpośredni dostęp do danych.
    // ImageLockModeRead — tylko odczyt (wystarczający tutaj; Write byłby nadmiarowy).
    // PixelFormat32bppARGB — wymagamy konkretnego formatu; GDI+ skonwertuje automatycznie.
    if (bmp->LockBits(&rect,
        Gdiplus::ImageLockModeRead,
        PixelFormat32bppARGB,
        &bmpData) != Gdiplus::Ok)
    {
        delete bmp;
        return false;
    }

    // WAŻNE: Kopiowanie wiersz po wierszu z uwzględnieniem Stride.
    // bmpData.Scan0 wskazuje na pierwszy bajt danych GDI+ (wiersz 0).
    // Każdy wiersz jest oddalony od poprzedniego o bmpData.Stride bajtów,
    // ale my chcemy wiersz ciągły o rozmiarze width*4 bajtów.
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(pixels.data() + y * width,
            reinterpret_cast<const uint8_t*>(bmpData.Scan0) + y * bmpData.Stride,
            static_cast<size_t>(width) * sizeof(uint32_t));
    }

    // WAŻNE: UnlockBits musi być wywołane zawsze po LockBits — nawet w ścieżce błędu.
    // Brak UnlockBits powoduje zablokowanie bitmapy i wyciek zasobów GDI+.
    bmp->UnlockBits(&bmpData);
    delete bmp;
    return true;
}

// ============================================================
// WAŻNE: GetGdiplusEncoderClsid — wyszukiwanie CLSID kodera GDI+ po MIME type.
//
// GDI+ identyfikuje kodeki (PNG, BMP, JPEG...) przez CLSID, a nie przez
// nazwy plików. Aby zapisać BMP, musimy znaleźć CLSID kodera "image/bmp".
//
// Mechanizm:
//   1. GetImageEncodersSize() — pyta GDI+ o liczbę i łączny rozmiar (w bajtach)
//      tablicy dostępnych kodeków.
//   2. GetImageEncoders() — wypełnia tablicę ImageCodecInfo wszystkimi dostępnymi
//      kodekami (BMP, JPEG, PNG, GIF, TIFF).
//   3. Iterujemy i porównujemy MimeType z szukanym — jeśli pasuje, kopiujemy CLSID.
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
// WAŻNE: SavePixelsAsBMP — zapis liniowej tablicy pikseli RGBA do pliku BMP.
//
// Analogicznie do LoadImagePixels, ale w kierunku odwrotnym.
// Tworzymy nową bitmapę GDI+ w pamięci, kopiujemy piksele wiersz po wierszu
// (znów z uwzględnieniem Stride), następnie zapisujemy przez kodek BMP.
//
// Używamy formatu BMP (image/bmp), bo:
//   - jest bezstratny (brak kompresji stratnej jak JPEG),
//   - dekoder BMP jest zawsze dostępny w GDI+,
//   - wynik dekompresji musi być identyczny piksel-po-pikselu z oryginałem.
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

    // ImageLockModeWrite — dostęp do zapisu; GDI+ blokuje bufor do czasu UnlockBits.
    if (bmp.LockBits(&rect,
        Gdiplus::ImageLockModeWrite,
        PixelFormat32bppARGB,
        &bmpData) != Gdiplus::Ok)
        return false;

    // Kopiujemy wiersz po wierszu z uwzględnieniem Stride (patrz LoadImagePixels).
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(reinterpret_cast<uint8_t*>(bmpData.Scan0) + y * bmpData.Stride,
            pixels.data() + y * width,
            static_cast<size_t>(width) * sizeof(uint32_t));
    }

    bmp.UnlockBits(&bmpData);

    // Szukamy CLSID dla kodera BMP i zapisujemy plik.
    CLSID bmpClsid{};
    if (!GetGdiplusEncoderClsid(L"image/bmp", bmpClsid))
        return false;

    return bmp.Save(path.c_str(), &bmpClsid) == Gdiplus::Ok;
}

// ============================================================
// WAŻNE: WriteCompressedFile — zapis pliku w formacie .lz77.
//
// Format (zgodny z Lz77FileHeader):
//   [20 bajtów nagłówka] [dataSize bajtów danych tokenów LZ77]
//
// Używa WinAPI (CreateFileW / WriteFile) zamiast std::ofstream,
// bo daje bezpośrednią kontrolę nad trybem dostępu (GENERIC_WRITE)
// i trybem tworzenia pliku (CREATE_ALWAYS — nadpisuje jeśli istnieje).
//
// WAŻNE: Weryfikacja końcowa:
//   'ok' sprawdza czy oba WriteFile zakończyły się sukcesem.
//   'written == dataSize' sprawdza czy faktyczna liczba zapisanych bajtów
//   zgadza się z oczekiwaną (może się różnić np. przy błędzie dysku).
// ============================================================
static bool WriteCompressedFile(const std::wstring& path,
    uint32_t width,
    uint32_t height,
    const uint8_t* data,
    size_t dataSize)
{
    HANDLE hFile = CreateFileW(path.c_str(),
        GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS,          // nadpisz istniejący plik
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    // Budujemy strukturę nagłówka z magicznym znacznikiem i wymiarami.
    Lz77FileHeader hdr{};
    hdr.magic = LZ77_FILE_MAGIC;
    hdr.width = width;
    hdr.height = height;
    hdr.compressedBytes = static_cast<uint64_t>(dataSize);

    DWORD written = 0;
    // Dwa wywołania WriteFile: najpierw nagłówek, potem dane.
    // Operator && zapewnia short-circuit: jeśli zapis nagłówka się nie powiedzie,
    // zapis danych nie jest nawet próbowany.
    BOOL ok = WriteFile(hFile, &hdr, sizeof(hdr), &written, nullptr);
    ok = ok && WriteFile(hFile, data, static_cast<DWORD>(dataSize), &written, nullptr);

    CloseHandle(hFile);
    return ok && (written == static_cast<DWORD>(dataSize));
}

// ============================================================
// WAŻNE: ReadCompressedFile — odczyt i walidacja pliku .lz77.
//
// Kroki:
//   1. Odczytuje nagłówek (20 bajtów).
//   2. Sprawdza magic — ochrona przed przypadkowym przetworzeniem błędnego pliku.
//   3. Sprawdza rozmiar danych — ochrona przed uszkodzonymi plikami, które podają
//      fałszywy compressedBytes (np. gigantyczną wartość), co mogłoby wyczerpać RAM.
//      Limit 512 MB to górna rozsądna granica dla obrazu.
//   4. Alokuje bufor i odczytuje dane tokenów.
//   5. Weryfikuje, że odczytano dokładnie tyle bajtów, ile deklaruje nagłówek.
// ============================================================
static bool ReadCompressedFile(const std::wstring& path,
    uint32_t& width,
    uint32_t& height,
    std::vector<uint8_t>& data)
{
    // FILE_SHARE_READ pozwala innym procesom jednocześnie czytać plik (nieblokujące).
    HANDLE hFile = CreateFileW(path.c_str(),
        GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    Lz77FileHeader hdr{};
    DWORD read = 0;
    ReadFile(hFile, &hdr, sizeof(hdr), &read, nullptr);

    // WAŻNE: Podwójna walidacja nagłówka — sprawdzamy zarówno liczbę odczytanych
    // bajtów (czy plik nie jest krótszy od nagłówka) jak i magic number.
    if (read != sizeof(hdr) || hdr.magic != LZ77_FILE_MAGIC) {
        CloseHandle(hFile);
        return false;
    }

    width = hdr.width;
    height = hdr.height;

    // WAŻNE: Zabezpieczenie przed przepełnieniem pamięci.
    // Zerowe compressedBytes oznacza pusty plik; > 512 MB to prawdopodobnie
    // uszkodzone pole nagłówka. Bez tego limitu wektor mógłby spróbować zarezerwować
    // terabajty pamięci i zakończyć się std::bad_alloc lub naruszeniem ochrony pamięci.
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
// Zbiór rozszerzeń obrazkow obsługiwanych przez GDI+.
// Używany w StartCompression do filtrowania plików podczas iteracji katalogu.
// std::set<> zapewnia O(log n) wyszukiwanie — wydajniejsze niż liniowe
// porównywanie dla potencjalnie dużej listy rozszerzeń.
// ============================================================
static const std::set<std::wstring> IMAGE_EXTENSIONS = {
    L".png", L".jpg", L".jpeg", L".bmp",
    L".tiff", L".tif", L".gif"
};

// ============================================================
// WAŻNE: StartCompression — główna funkcja kompresji, eksportowana do C#.
//
// Architektura pomiaru czasu — trzy oddzielne fazy:
//
//   FAZA 1 — PRE-LOAD (przed stoperem):
//     Wszystkie operacje I/O i alokacje pamięci wykonywane są w wątku głównym
//     zanim stoper zostanie uruchomiony. Dla każdego pliku obrazu:
//       - wczytanie pikseli RGBA przez GDI+ (LoadImagePixels),
//       - pre-alokacja bufora wyjściowego dst (worst-case LZ77),
//       - pre-alokacja bufora roboczego work (head[] + prev[]).
//     Dzięki temu żadne I/O ani malloc nie wchodzi do sekcji mierzonej.
//
//   FAZA 2 — MIERZONA (tstart … tend):
//     Obejmuje dokładnie i wyłącznie:
//       - tworzenie wątków roboczych (emplace_back),
//       - wywołania compFn() we wszystkich wątkach,
//       - oczekiwanie na zakończenie wątków (join()).
//     Wątki NIE wykonują żadnego I/O — operują wyłącznie na pre-alokowanych
//     buforach w pamięci RAM.
//
//   FAZA 3 — POST (po stoperze):
//     Sekwencyjny zapis wyników na dysk, wywołania logCb i progressCb.
//
// Gwarancja poprawności pomiaru:
//   - Każde wywołanie compFn() jest objęte przedziałem [tstart, tend]. ✓
//   - Żaden I/O (odczyt obrazu, zapis .lz77) nie wchodzi do [tstart, tend]. ✓
//   - tstart jest pobierany tuż przed pierwszym emplace_back(). ✓
//   - tend jest pobierany tuż po ostatnim join(). ✓
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
    LZ77DecompressFunc decompFn = nullptr;  // wymagane przez LoadLZ77DLL, nieużywane tutaj
    std::wstring dllError;

    if (!LoadLZ77DLL(useASM, hMod, compFn, decompFn, dllError)) {
        if (logCb) logCb((L"Blad ladowania DLL: " + dllError).c_str());
        return;
    }
    if (logCb) logCb(useASM ? L"Zaladowano DLL: AsmDll.dll"
        : L"Zaladowano DLL: CppDll.dll");

    // ============================================================
    // Struktura zadania kompresji — przechowuje wszystko, czego
    // potrzebuje wątek roboczy (pre-wczytane dane + pre-alokowane bufory).
    // Wypełniana w całości w FAZIE 1 (przed stoperem).
    // ============================================================
    struct CompressTask {
        std::wstring          filePath;   // oryginalna ścieżka (do logowania i zapisu)
        std::vector<uint32_t> pixels;     // pre-wczytane piksele RGBA
        uint32_t              w = 0;      // szerokość obrazu
        uint32_t              h = 0;      // wysokość obrazu
        std::vector<uint8_t>  dst;        // pre-alokowany bufor wyjściowy (tokeny LZ77)
        std::vector<uint8_t>  work;       // pre-alokowany bufor roboczy (head[] + prev[])
        size_t                outLen = 0; // [out] liczba zapisanych bajtów po compFn
        bool                  loadOk = false;    // czy wczytanie obrazu się powiodło
        bool                  exception = false; // czy compFn rzuciła wyjątek
    };

    // ============================================================
    // FAZA 1: PRE-LOAD — wczytanie obrazów i alokacja buforów.
    //
    // Wykonywana sekwencyjnie w wątku głównym, PRZED uruchomieniem stopera.
    // Obejmuje wszystkie operacje I/O i malloc dla wszystkich plików.
    // ============================================================
    std::vector<CompressTask> tasks;

    try {
        for (auto& entry : fs::directory_iterator(sourceFolder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (!IMAGE_EXTENSIONS.count(ext)) continue;

            CompressTask task;
            task.filePath = entry.path().wstring();

            // Wczytaj piksele RGBA przez GDI+ (I/O — przed stoperem).
            task.loadOk = LoadImagePixels(task.filePath, task.pixels, task.w, task.h);

            if (task.loadOk) {
                size_t pixelCount = static_cast<size_t>(task.w) * task.h;

                // Pre-alokuj bufor wyjściowy: pesymistyczny worst-case LZ77.
                // Token literalny = ~12 B/piksel + 64 B margines na nagłówek strumienia.
                task.dst.resize(pixelCount * 12u + 64u);

                // Pre-alokuj bufor roboczy: head[65536] + prev[4096] = 272 KB.
                // Każde zadanie ma własny bufor — brak współdzielenia między wątkami.
                task.work.resize(LOGIC_LZ77_WORK_BYTES);
            }

            tasks.push_back(std::move(task));
        }
    }
    catch (const std::exception& ex) {
        std::string msg(ex.what());
        std::wstring wmsg(msg.begin(), msg.end());
        if (logCb) logCb((L"Blad enumeracji folderu: " + wmsg).c_str());
        FreeLibrary(hMod);
        return;
    }

    int totalFiles = static_cast<int>(tasks.size());
    if (totalFiles == 0) {
        if (logCb) logCb(L"Brak plikow obrazkow w folderze zrodlowym.");
        FreeLibrary(hMod);
        return;
    }

    CreateDirectoryW(outputFolder, nullptr);

    // Atomowy indeks zadania — wątki pobierają kolejne zadania przez fetch_add,
    // bez potrzeby muteksu (brak modyfikacji wektora tasks w wątkach).
    std::atomic<int> taskIndex{ 0 };

    // ============================================================
    // FAZA 2: MIERZONA — tworzenie wątków, compFn, join.
    //
    // WAŻNE: tstart pobierany tuż przed pierwszym emplace_back().
    // WAŻNE: tend pobierany tuż po ostatnim join().
    // Wątki robocze wykonują WYŁĄCZNIE wywołania compFn() na danych
    // z pre-alokowanych buforów — zero I/O, zero logowania, zero malloc.
    // ============================================================
    int actualThreads = std::max(1, numThreads);
    std::vector<std::thread> workers;
    workers.reserve(actualThreads);

    // Worker operuje wyłącznie na pre-alokowanych buforach — żadnego I/O.
    auto worker = [&]() {
        while (true) {
            // fetch_add — atomowe pobranie indeksu bez muteksu.
            int idx = taskIndex.fetch_add(1, std::memory_order_relaxed);
            if (idx >= totalFiles) break;

            CompressTask& task = tasks[static_cast<size_t>(idx)];
            if (!task.loadOk) continue;  // plik nie załadowany — pomiń (wylogowane w FAZIE 3)

            size_t pixelCount = static_cast<size_t>(task.w) * task.h;

            try {
                compFn(task.pixels.data(), pixelCount,
                    task.dst.data(), task.dst.size(),
                    task.work.data(), task.work.size(),
                    &task.outLen);
            }
            catch (...) {
                task.exception = true;
            }
        }
        };

    // --- tstart: tuż przed pierwszym emplace_back()
    auto tstart = std::chrono::steady_clock::now();

    for (int i = 0; i < actualThreads; ++i)
        workers.emplace_back(worker);

    // WAŻNE: join() musi być przed tend — czekamy na zakończenie WSZYSTKICH wątków.
    for (auto& t : workers)
        if (t.joinable()) t.join();

    // --- tend: tuż po ostatnim join()
    auto tend = std::chrono::steady_clock::now();

    // ============================================================
    // FAZA 3: POST — zapis wyników, logowanie, progress.
    //
    // Wykonywana sekwencyjnie po zatrzymaniu stopera.
    // Zawiera wszystkie operacje I/O (zapis .lz77) i wywołania callbacków.
    // ============================================================
    int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        tend - tstart).count();
    if (outElapsedMs) *outElapsedMs = elapsedMs;

    int processed = 0;
    for (auto& task : tasks) {
        std::wstring fileName = fs::path(task.filePath).filename().wstring();
        std::wstring stem = fs::path(task.filePath).stem().wstring();

        if (!task.loadOk) {
            if (logCb) logCb((L"Nie mozna wczytac obrazu: " + fileName).c_str());
        }
        else if (task.exception) {
            if (logCb) logCb((L"Wyjatek podczas kompresji: " + fileName).c_str());
        }
        else if (task.outLen == 0) {
            if (logCb) logCb((L"Kompresja zwrocila 0 bajtow: " + fileName).c_str());
        }
        else {
            // Zapis pliku .lz77 (I/O — po stoperze).
            std::wstring outFile = std::wstring(outputFolder) + L"\\" + stem + L".lz77";
            if (!WriteCompressedFile(outFile, task.w, task.h,
                task.dst.data(), task.outLen)) {
                if (logCb) logCb((L"Blad zapisu: " + stem + L".lz77").c_str());
            }
            else {
                if (logCb) logCb((L"Skompresowano: " + fileName).c_str());
            }
        }

        ++processed;
        if (progressCb) progressCb((processed * 100) / totalFiles);
    }

    if (progressCb) progressCb(100);

    std::wstringstream rpt;
    rpt << L"--- Kompresja zakonczona ---\n"
        << L"Plikow: " << totalFiles << L"  |  "
        << L"Watkow: " << actualThreads << L"  |  "
        << L"Czas algorytmu LZ77: " << elapsedMs << L" ms";
    if (logCb) logCb(rpt.str().c_str());

    // WAŻNE: FreeLibrary po join() — wątki przestały używać kodu z DLL.
    FreeLibrary(hMod);
}

// ============================================================
// WAŻNE: StartDecompression — główna funkcja dekompresji, eksportowana do C#.
//
// Symetryczna architektura trójfazowa jak w StartCompression.
//
//   FAZA 1 — PRE-LOAD (przed stoperem):
//     Dla każdego pliku .lz77:
//       - odczyt nagłówka i danych skompresowanych (ReadCompressedFile),
//       - pre-alokacja bufora wyjściowego pixels (width * height pikseli).
//
//   FAZA 2 — MIERZONA (tstart … tend):
//     Obejmuje dokładnie i wyłącznie:
//       - tworzenie wątków roboczych (emplace_back),
//       - wywołania decompFn() we wszystkich wątkach,
//       - oczekiwanie na zakończenie wątków (join()).
//
//   FAZA 3 — POST (po stoperze):
//     Zapis zdekompresowanych obrazów (.bmp), logowanie, progress.
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
    LZ77CompressFunc   compFn = nullptr;   // wymagane przez LoadLZ77DLL, nieużywane tutaj
    LZ77DecompressFunc decompFn = nullptr;
    std::wstring dllError;

    if (!LoadLZ77DLL(useASM, hMod, compFn, decompFn, dllError)) {
        if (logCb) logCb((L"Blad ladowania DLL: " + dllError).c_str());
        return;
    }
    if (logCb) logCb(useASM ? L"Zaladowano DLL: AsmDll.dll"
        : L"Zaladowano DLL: CppDll.dll");

    // ============================================================
    // Struktura zadania dekompresji — przechowuje wszystko, czego
    // potrzebuje wątek roboczy (pre-wczytane dane + pre-alokowany bufor wyjściowy).
    // ============================================================
    struct DecompressTask {
        std::wstring          filePath;          // oryginalna ścieżka (do logowania i zapisu)
        std::vector<uint8_t>  compData;          // pre-wczytane tokeny LZ77
        uint32_t              w = 0;             // szerokość obrazu z nagłówka
        uint32_t              h = 0;             // wysokość obrazu z nagłówka
        std::vector<uint32_t> pixels;            // pre-alokowany bufor wyjściowy (piksele RGBA)
        size_t                pixelCount = 0;    // oczekiwana liczba pikseli (w * h)
        size_t                outLen = 0;        // [out] liczba odtworzonych pikseli po decompFn
        bool                  loadOk = false;    // czy odczyt .lz77 się powiódł
        bool                  exception = false; // czy decompFn rzuciła wyjątek
    };

    // ============================================================
    // FAZA 1: PRE-LOAD — odczyt plików .lz77 i alokacja buforów.
    // ============================================================
    std::vector<DecompressTask> tasks;

    try {
        for (auto& entry : fs::directory_iterator(sourceFolder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".lz77") continue;

            DecompressTask task;
            task.filePath = entry.path().wstring();

            // Odczyt pliku .lz77 (I/O — przed stoperem).
            task.loadOk = ReadCompressedFile(task.filePath,
                task.w, task.h, task.compData);

            if (task.loadOk) {
                task.pixelCount = static_cast<size_t>(task.w) * task.h;
                // Pre-alokacja bufora wyjściowego — zerowanie chroni przed śmieciami
                // w przypadku częściowej dekompresji.
                task.pixels.assign(task.pixelCount, 0u);
            }

            tasks.push_back(std::move(task));
        }
    }
    catch (const std::exception& ex) {
        std::string msg(ex.what());
        std::wstring wmsg(msg.begin(), msg.end());
        if (logCb) logCb((L"Blad enumeracji folderu: " + wmsg).c_str());
        FreeLibrary(hMod);
        return;
    }

    int totalFiles = static_cast<int>(tasks.size());
    if (totalFiles == 0) {
        if (logCb) logCb(L"Brak plikow .lz77 w folderze zrodlowym.");
        FreeLibrary(hMod);
        return;
    }

    CreateDirectoryW(outputFolder, nullptr);

    std::atomic<int> taskIndex{ 0 };

    // ============================================================
    // FAZA 2: MIERZONA — tworzenie wątków, decompFn, join.
    // ============================================================
    int actualThreads = std::max(1, numThreads);
    std::vector<std::thread> workers;
    workers.reserve(actualThreads);

    // Worker operuje wyłącznie na pre-alokowanych buforach — żadnego I/O.
    auto worker = [&]() {
        while (true) {
            int idx = taskIndex.fetch_add(1, std::memory_order_relaxed);
            if (idx >= totalFiles) break;

            DecompressTask& task = tasks[static_cast<size_t>(idx)];
            if (!task.loadOk) continue;  // plik nie załadowany — pomiń (wylogowane w FAZIE 3)

            try {
                decompFn(task.compData.data(), task.compData.size(),
                    task.pixels.data(), task.pixelCount,
                    &task.outLen);
            }
            catch (...) {
                task.exception = true;
            }
        }
        };

    // --- tstart: tuż przed pierwszym emplace_back()
    auto tstart = std::chrono::steady_clock::now();

    for (int i = 0; i < actualThreads; ++i)
        workers.emplace_back(worker);

    // WAŻNE: join() musi być przed tend — czekamy na zakończenie WSZYSTKICH wątków.
    for (auto& t : workers)
        if (t.joinable()) t.join();

    // --- tend: tuż po ostatnim join()
    auto tend = std::chrono::steady_clock::now();

    // ============================================================
    // FAZA 3: POST — zapis wyników, logowanie, progress.
    // ============================================================
    int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        tend - tstart).count();
    if (outElapsedMs) *outElapsedMs = elapsedMs;

    int processed = 0;
    for (auto& task : tasks) {
        std::wstring fileName = fs::path(task.filePath).filename().wstring();
        std::wstring stem = fs::path(task.filePath).stem().wstring();

        if (!task.loadOk) {
            if (logCb) logCb((L"Nie mozna wczytac lub uszkodzony: " + fileName).c_str());
        }
        else if (task.exception) {
            if (logCb) logCb((L"Wyjatek podczas dekompresji: " + fileName).c_str());
        }
        else if (task.outLen != task.pixelCount) {
            // WAŻNE: outLen musi dokładnie równać się pixelCount.
            // Niezgodność wskazuje na uszkodzone dane lub błąd w DLL.
            if (logCb) logCb((L"Niezgodna liczba pikseli po dekompresji: " + fileName).c_str());
        }
        else {
            // Zapis zdekompresowanego obrazu jako .bmp (I/O — po stoperze).
            std::wstring outFile = std::wstring(outputFolder) + L"\\" + stem + L".bmp";
            if (!SavePixelsAsBMP(outFile, task.pixels, task.w, task.h)) {
                if (logCb) logCb((L"Blad zapisu BMP: " + stem).c_str());
            }
            else {
                if (logCb) logCb((L"Zdekompresowano: " + stem + L".bmp").c_str());
            }
        }

        ++processed;
        if (progressCb) progressCb((processed * 100) / totalFiles);
    }

    if (progressCb) progressCb(100);

    std::wstringstream rpt;
    rpt << L"--- Dekompresja zakonczona ---\n"
        << L"Plikow: " << totalFiles << L"  |  "
        << L"Watkow: " << actualThreads << L"  |  "
        << L"Czas algorytmu LZ77: " << elapsedMs << L" ms";
    if (logCb) logCb(rpt.str().c_str());

    // WAŻNE: FreeLibrary po join() — wątki przestały używać kodu z DLL.
    FreeLibrary(hMod);
}