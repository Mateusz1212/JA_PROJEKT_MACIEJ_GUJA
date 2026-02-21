
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
// Architektura wielowątkowa — wzorzec "thread pool z kolejką zadań":
//   - Jeden współdzielony std::deque<wstring> przechowuje ścieżki do przetworzenia.
//   - Każdy worker w pętli pobiera jeden plik z kolejki (sekcja krytyczna),
//     przetwarza go całkowicie, a potem wraca po następny.
//   - Brak dedykowanych kolejek per-wątek — naturalne równoważenie obciążenia
//     (duże pliki automatycznie nie blokują innych wątków).
//
// Bezpieczeństwo wątkowe:
//   - queue_mtx  — mutex chroniący deque (pobieranie + push_back).
//   - log_mtx    — mutex chroniący wywołania logCb (P/Invoke nie jest thread-safe).
//   - processed  — std::atomic<int> — bezpieczny inkrementor bez muteksu.
//   - algoNs     — std::atomic<long long> — bezpieczne sumowanie czasu ze wszystkich wątków.
//
// Ważny szczegół pomiaru czasu:
//   Mierzymy TYLKO czas wywołania compFn(), pomijając I/O (odczyt obrazu, zapis .lz77).
//   Dzięki temu wynik outElapsedMs odzwierciedla rzeczywistą wydajność algorytmu
//   LZ77, a nie obciążenie dysku.
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
    // WAŻNE: Enumeracja odbywa się PRZED uruchomieniem wątków — unika warunków wyścigu
    // na dostęp do systemu plików. Wszystkie ścieżki są z góry znane.
    std::deque<std::wstring> queue;
    try {
        for (auto& entry : fs::directory_iterator(sourceFolder)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            // Normalizacja do małych liter — obsługa plików .PNG, .JPG itp.
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

    // CreateDirectoryW nie zwraca błędu jeśli katalog już istnieje — zachowanie poprawne.
    CreateDirectoryW(outputFolder, nullptr);

    // --- Przygotowanie zmiennych wspoldzielonych miedzy workerami
    std::mutex       queue_mtx;   // chroni kolejke zadan
    std::mutex       log_mtx;     // chroni wywolania logCb
    std::atomic<int> processed(0);

    // Suma czasow wywolan compFn ze wszystkich watkow [nanosekundy].
    // Kazdy worker dodaje tu czas TYLKO wywolania compFn (nie I/O).
    std::atomic<long long> algoNs{ 0 };

    // --- Worker lambda — kazdy worker przetwarza jeden plik na raz
    // WAŻNE: Przechwytujemy przez referencję [&] wszystkie zmienne współdzielone.
    // Lambda jest bezpieczna, bo zmienne outlive'ują wątki (join() przed końcem funkcji).
    auto worker = [&](int workerId) {
        while (true) {
            // WAŻNE: Sekcja krytyczna — pobieranie pliku z kolejki.
            // Lock trzymany jak najkrócej (tylko na czas pobrania ścieżki),
            // aby inne wątki mogły pobierać swoje zadania równolegle podczas
            // przetwarzania bieżącego pliku.
            std::wstring filePath;
            {
                std::lock_guard<std::mutex> lk(queue_mtx);
                if (queue.empty()) break;   // brak zadan — koniec petli workera
                filePath = std::move(queue.front());  // move() unika kopiowania wstring
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

            // WAŻNE: Rozmiar bufora wyjściowego — pesymistyczny worst-case LZ77.
            // Jeśli algorytm nie znajdzie żadnych dopasowań (np. obraz szumu losowego),
            // każdy piksel jest kodowany jako token literalny.
            // Token literalny LZ77 dla piksela RGBA zajmuje 1 bajt flagi + 4 bajty piksela
            // + overhead kodowania = ~12 bajtów. Dodatkowe 64 bajty to margines na nagłówek
            // wewnętrzny strumienia (jeśli DLL go używa).
            size_t dstCap = pixelCount * 12u + 64u;
            std::vector<uint8_t> dst(dstCap);

            // WAŻNE: Bufor roboczy LZ77 (head[] + prev[]).
            // Tablice head[] i prev[] są wymagane przez algorytm LZ77 do szybkiego
            // wyszukiwania dopasowań w oknie historii.
            // Rozmiar LOGIC_LZ77_WORK_BYTES = (65536+4096)*4 = 272 KB per wątek.
            // Każdy wątek ma WŁASNY bufor roboczy (lokalny w lambdzie) —
            // brak współdzielenia = brak konieczności synchronizacji tych buforów.
            std::vector<uint8_t> work(LOGIC_LZ77_WORK_BYTES);

            // WAŻNE: Pomiar czasu TYLKO wywołania compFn (bez I/O).
            // steady_clock — monotonicznie rosnący zegar; nie cofa się przy
            // zmianie czasu systemowego (w odróżnieniu od system_clock).
            // try/catch łapie ewentualne wyjątki z DLL (np. access violation
            // przechwycony przez SEH jako C++ exception z /EHa).
            size_t outLen = 0;
            try {
                auto t0 = std::chrono::steady_clock::now();
                compFn(pixels.data(), pixelCount,
                    dst.data(), dstCap,
                    work.data(), work.size(),
                    &outLen);
                auto t1 = std::chrono::steady_clock::now();
                // WAŻNE: fetch_add jest niejawne w operator+=; operacja jest atomowa.
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

            // Walidacja — outLen == 0 oznacza, że compFn nie wytworzyła żadnych danych.
            if (outLen == 0) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Kompresja zwrocila 0 bajtow: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            // Zapisz plik .lz77 (naglowek + dane) do outputFolder
            // stem() — nazwa pliku bez rozszerzenia (np. "foto" z "foto.png").
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
            // processed.load() gwarantuje odczyt aktualnej wartości (nie ze stale rejestru).
            processed++;
            if (progressCb) progressCb((processed.load() * 100) / totalFiles);
        }
        };

    // --- Uruchom numThreads workerow (min. 1 zgodnie z suwakiiem GUI)
    // std::max(1, numThreads) zabezpiecza przed wartością 0 lub ujemną z GUI.
    int actualThreads = std::max(1, numThreads);
    std::vector<std::thread> workers;
    workers.reserve(actualThreads);
    for (int i = 0; i < actualThreads; ++i) {
        workers.emplace_back(worker, i);  // i = workerId przekazywany do lambdy
    }

    // --- Czekaj na zakonczenie wszystkich watkow
    // WAŻNE: join() blokuje wątek wywołujący (wątek C#) do czasu zakończenia
    // każdego workera. Bez join() zmienne lokalne (queue, algoNs itp.) zostałyby
    // zniszczone przed zakończeniem wątków — undefined behavior (UB).
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    // WAŻNE: Konwersja ns -> ms przez dzielenie całkowitoliczbowe.
    // algoNs zawiera SUMĘ czasów ze wszystkich wątków (nie czas ścienny wall-clock).
    // Dla n wątków działających równolegle algoNs ≈ n * czas_wall_clock.
    // Odpowiedź na pytanie "ile zajął algorytm" to algoNs/n, ale tu zwracamy sumę
    // zgodnie z kontraktem (C# wyświetla jak chce).
    int64_t elapsedMs = static_cast<int64_t>(algoNs.load() / 1'000'000LL);
    if (outElapsedMs) *outElapsedMs = elapsedMs;

    if (progressCb) progressCb(100);

    std::wstringstream rpt;
    rpt << L"--- Kompresja zakonczona ---\n"
        << L"Plikow: " << totalFiles << L"  |  "
        << L"Watkow: " << actualThreads << L"  |  "
        << L"Czas algorytmu LZ77: " << elapsedMs << L" ms";
    if (logCb) logCb(rpt.str().c_str());

    // WAŻNE: FreeLibrary musi być wywołane PO join() na wszystkich wątkach.
    // Gdyby wątki nadal korzystały z funkcji z DLL po FreeLibrary, nastąpiłby crash
    // (wykonywanie kodu ze zwolnionej pamięci).
    FreeLibrary(hMod);
}

// ============================================================
// WAŻNE: StartDecompression — główna funkcja dekompresji, eksportowana do C#.
//
// Symetryczna do StartCompression — ta sama architektura wielowątkowa
// (kolejka + N workerów + muteksy + atomic).
//
// Różnice względem StartCompression:
//   - Wejście: pliki .lz77 (zamiast obrazów)
//   - Wyjście: pliki .bmp (zamiast .lz77)
//   - Wywołuje decompFn zamiast compFn
//   - Brak bufora roboczego (dekompresja LZ77 nie wymaga head[]/prev[])
//   - Walidacja: outLen musi == pixelCount (width*height), inaczej dane są uszkodzone
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
    LZ77CompressFunc   compFn = nullptr;  // wymagane przez LoadLZ77DLL (nie używane tutaj)
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
            // Normalizacja rozszerzenia do małych liter (np. ".LZ77" -> ".lz77").
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
            // Sekcja krytyczna — pobieranie następnego pliku .lz77 z kolejki.
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

            // Alokacja bufora wyjściowego — dokładnie pixelCount pikseli RGBA (uint32_t).
            size_t pixelCount = static_cast<size_t>(w) * h;
            std::vector<uint32_t> pixels(pixelCount, 0);  // zerowanie = ochrona przed śmieciami

            // WAŻNE: Pomiar czasu TYLKO wywołania decompFn.
            // Analogicznie do StartCompression — steady_clock + try/catch.
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

            // WAŻNE: Weryfikacja spójności dekompresji.
            // outLen musi dokładnie równać się pixelCount (width * height).
            // Niezgodność wskazuje na uszkodzone dane wejściowe lub błąd w DLL.
            // Jeśli odtworzylibyśmy za mało pikseli, obraz BMP miałby błędną zawartość.
            if (outLen != pixelCount) {
                std::lock_guard<std::mutex> lk(log_mtx);
                if (logCb) logCb((L"[Watek " + std::to_wstring(workerId) +
                    L"] Niezgodna liczba pikseli po dekompresji: " + fileName).c_str());
                processed++;
                if (progressCb) progressCb((processed.load() * 100) / totalFiles);
                continue;
            }

            // Zapisz zdekompresowany obraz jako .bmp w outputFolder
            // stem() — nazwa bez rozszerzenia, np. "foto" z "foto.lz77"
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

    // --- Czekaj na zakonczenie wszystkich watkow (patrz komentarz w StartCompression).
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

    // WAŻNE: FreeLibrary po join() — patrz komentarz w StartCompression.
    FreeLibrary(hMod);
}