
/********************************************************************************
 * TEMAT PROJEKTU: Algorytm LZ77 do kompresji obrazków
 * OPIS ALGORYTMU: Implementacja algorytmu LZ77 do kompresji obrazków – odpowiednik kodu asemblerowego MASM x64 napisany w C++
 * DATA WYKONANIA: luty 2026 r.
 * SEMESTR / ROK AKADEMICKI: Semestr Zimowy 2025/2026
 * AUTOR: Maciej Guja
 * AKTUALNA WERSJA: 1.1
 ********************************************************************************/

#include "lz77.h"
#include <string.h>

 // ============================================================
 // Stałe konfiguracyjne (identyczne jak w wersji ASM)
 // ============================================================
static const uint32_t WINDOW_PX = 4096;           // okno ślizgowe w pikselach (potęga 2)
static const uint32_t HASH_SIZE = 65536;           // liczba wpisów tablicy hash (potęga 2)
static const uint32_t HASH_MASK = HASH_SIZE - 1;
static const uint32_t MAX_MATCH_PX = 64;              // maks. długość dopasowania w pikselach
static const uint32_t MAX_CANDIDATES = 32;              // maks. liczba kroków w łańcuchu hash
static const uint32_t TOKEN_SIZE = 12;              // rozmiar tokenu: 3 x sizeof(uint32_t)
static const uint32_t INVALID_POS = 0xFFFFFFFFu;    // znacznik pustego slotu

// Rozmiar bufora roboczego:
//   head[HASH_SIZE]  = 65536 * 4 = 262144 B (256 KB)
//   prev[WINDOW_PX]  =  4096 * 4 =  16384 B ( 16 KB)
static const size_t WORK_HEAD_BYTES = HASH_SIZE * sizeof(uint32_t);
static const size_t WORK_PREV_BYTES = WINDOW_PX * sizeof(uint32_t);
static const size_t WORK_NEED_BYTES = WORK_HEAD_BYTES + WORK_PREV_BYTES;

// ============================================================
// Struktura tokenu
// ============================================================
struct Token12 {
    uint32_t offset_px;   // odległość wstecz (0 = literal)
    uint32_t length_px;   // długość dopasowania (0 = literal)
    uint32_t next_px;     // piksel następujący po dopasowaniu (lub sam literal)
};

// ============================================================
// Hash dwupikselowy: (src[i] XOR ROL(src[i+1], 5)) & HASH_MASK
// ============================================================
static inline uint32_t pixel_hash(uint32_t p0, uint32_t p1)
{
    uint32_t rot = (p1 << 5) | (p1 >> 27);   // ROL p1, 5
    return (p0 ^ rot) & HASH_MASK;
}

// ============================================================
// lz77_rgba_compress
//
// Format tokenu: Token12 { offset_px, length_px, next_px }
// Literal: offset==0, length==0, next==src[i]
// Dopasowanie: offset>0, length>0, next==src[i+length]
//
// Bufor roboczy work:
//   head[HASH_SIZE] : head[hash] -> najnowsza pozycja piksela o tym hashu
//   prev[WINDOW_PX] : prev[slot] -> poprzednia pozycja (łańcuch hash)
// ============================================================
void lz77_rgba_compress(
    const uint32_t* src_px,
    size_t          src_count,
    uint8_t* dst,
    size_t          dst_cap,
    void* work,
    size_t          work_cap,
    size_t* out_len)
{
    *out_len = 0;

    // Minimalny bufor wyjściowy: co najmniej 1 token
    if (dst_cap < TOKEN_SIZE)
        return;

    // Puste wejście – sukces, 0 bajtów
    if (src_count == 0) {
        *out_len = 0;
        return;
    }

    // -------------------------------------------------------
    // Tryb samych literałów: brak lub za mały bufor roboczy
    // Każdy piksel -> osobny token (0, 0, src[i]).
    // Wynik poprawny – brak kompresji, ale dekompresja działa.
    // -------------------------------------------------------
    if (work == nullptr || work_cap < WORK_NEED_BYTES) {
        size_t out_bytes = 0;
        for (size_t i = 0; i < src_count; i++) {
            if (dst_cap - out_bytes < TOKEN_SIZE) {
                *out_len = 0;
                return;
            }
            Token12* tok = reinterpret_cast<Token12*>(dst + out_bytes);
            tok->offset_px = 0;
            tok->length_px = 0;
            tok->next_px = src_px[i];
            out_bytes += TOKEN_SIZE;
        }
        *out_len = out_bytes;
        return;
    }

    // -------------------------------------------------------
    // Inicjalizacja tablic hash w buforze roboczym
    // -------------------------------------------------------
    uint32_t* head = reinterpret_cast<uint32_t*>(work);
    uint32_t* prev = head + HASH_SIZE;

    // Wypełnij head[] wartością INVALID_POS
    memset(head, 0xFF, WORK_HEAD_BYTES);
    // prev[] nie wymaga inicjalizacji – każdy slot jest nadpisywany
    // starym head[hash] przed pierwszym odczytem.

    size_t out_bytes = 0;

    // ============================================================
    // Główna pętla kompresji
    // ============================================================
    size_t i = 0;
    while (i < src_count) {

        size_t remaining = src_count - i;

        // Ostatni piksel (remaining==1): wyślij jako literal,
        // NIE wstawiaj do tablic hash (brak sąsiada do hasha).
        if (remaining == 1) {
            if (dst_cap - out_bytes < TOKEN_SIZE) {
                *out_len = 0;
                return;
            }
            Token12* tok = reinterpret_cast<Token12*>(dst + out_bytes);
            tok->offset_px = 0;
            tok->length_px = 0;
            tok->next_px = src_px[i];
            out_bytes += TOKEN_SIZE;
            i++;
            continue;
        }

        // maxMatch = min(MAX_MATCH_PX, remaining - 1)
        // Odejmujemy 1: token zawsze przechowuje jawny "następny piksel".
        uint32_t maxMatch = (uint32_t)(remaining - 1);
        if (maxMatch > MAX_MATCH_PX)
            maxMatch = MAX_MATCH_PX;

        // Hash dwupikselowy dla pozycji i
        uint32_t h = pixel_hash(src_px[i], src_px[i + 1]);

        // Najstarsza dopuszczalna pozycja w oknie
        uint32_t dictStart = (i >= WINDOW_PX) ? (uint32_t)(i - WINDOW_PX) : 0u;

        // Pobierz pierwszego kandydata z head[hash]
        uint32_t candidate = head[h];

        uint32_t bestLen = 0;
        uint32_t bestOff = 0;

        uint32_t chainLeft = MAX_CANDIDATES;

        // -------------------------------------------------------
        // Przeszukiwanie łańcucha hash
        // -------------------------------------------------------
        while (candidate != INVALID_POS && candidate >= dictStart && chainLeft > 0) {

            uint32_t offset = (uint32_t)i - candidate;   // zawsze > 0

            // Porównaj src[i..] z src[candidate..]
            const uint32_t* ptrA = src_px + i;
            const uint32_t* ptrB = src_px + candidate;
            uint32_t curLen = 0;

            // SSE2 w oryginale: bloki 4 pikseli (16 bajtów).
            // W C++ kompilator sam wektoryzuje pętlę – zachowujemy semantykę.
            while (curLen + 4 <= maxMatch) {
                if (ptrA[curLen] == ptrB[curLen] &&
                    ptrA[curLen + 1] == ptrB[curLen + 1] &&
                    ptrA[curLen + 2] == ptrB[curLen + 2] &&
                    ptrA[curLen + 3] == ptrB[curLen + 3])
                {
                    curLen += 4;
                }
                else {
                    break;
                }
            }
            // Dogranicz skalarne (ogon < 4 pikseli lub nieudany blok)
            while (curLen < maxMatch && ptrA[curLen] == ptrB[curLen])
                curLen++;

            if (curLen > bestLen) {
                bestLen = curLen;
                bestOff = offset;
            }

            // Przejdź do następnego kandydata przez prev[]
            uint32_t slot = candidate & (WINDOW_PX - 1);
            candidate = prev[slot];

            chainLeft--;
        }

        // -------------------------------------------------------
        // Emisja tokenu { bestOff, bestLen, next_px }
        // -------------------------------------------------------
        if (dst_cap - out_bytes < TOKEN_SIZE) {
            *out_len = 0;
            return;
        }

        uint32_t next_px;
        if (bestLen > 0)
            next_px = src_px[i + bestLen];
        else
            next_px = src_px[i];

        Token12* tok = reinterpret_cast<Token12*>(dst + out_bytes);
        tok->offset_px = bestOff;
        tok->length_px = bestLen;
        tok->next_px = next_px;
        out_bytes += TOKEN_SIZE;

        // -------------------------------------------------------
        // Aktualizacja tablic hash dla pozycji [i .. i+bestLen]
        // Wstawiamy bestLen+1 wpisów.
        // -------------------------------------------------------
        for (uint32_t k = 0; k <= bestLen; k++) {
            uint32_t pos = (uint32_t)i + k;

            // Do hasha potrzeba pary (src[pos], src[pos+1]) – sprawdź granicę
            if ((size_t)pos + 1 >= src_count)
                break;

            uint32_t nh = pixel_hash(src_px[pos], src_px[pos + 1]);
            uint32_t slot = pos & (WINDOW_PX - 1);

            prev[slot] = head[nh];   // stary head wchodzi na łańcuch
            head[nh] = pos;        // pos staje się nową głową łańcucha
        }

        // Przesuniecie: i += bestLen + 1
        i += bestLen + 1;
    }

    *out_len = out_bytes;
}

// ============================================================
// lz77_rgba_decompress
// ============================================================
void lz77_rgba_decompress(
    const uint8_t* src,
    size_t          src_len,
    uint32_t* dst_px,
    size_t          dst_cap,
    size_t* out_len)
{
    *out_len = 0;

    size_t src_pos = 0;   // bieżąca pozycja odczytu w bajtach
    size_t out_px = 0;   // liczba wypisanych pikseli

    // ============================================================
    // Główna pętla dekompresji
    // ============================================================
    while (true) {

        // Wymaga pełnego tokenu (12 bajtów)
        if (src_pos + TOKEN_SIZE > src_len)
            break;

        const Token12* tok = reinterpret_cast<const Token12*>(src + src_pos);
        uint32_t offset_px = tok->offset_px;
        uint32_t length_px = tok->length_px;
        uint32_t next_px = tok->next_px;

        src_pos += TOKEN_SIZE;

        // -------------------------------------------------------
        // Literal: offset==0 && length==0
        // -------------------------------------------------------
        if (offset_px == 0 && length_px == 0) {
            if (out_px >= dst_cap) {
                *out_len = 0;
                return;
            }
            dst_px[out_px++] = next_px;
            continue;
        }

        // -------------------------------------------------------
        // Dopasowanie: skopiuj length_px pikseli, następnie next_px
        // -------------------------------------------------------

        // Potrzeba length_px + 1 pikseli miejsca
        if (out_px + length_px + 1 > dst_cap) {
            *out_len = 0;
            return;
        }

        // offset=0 przy length>0 jest nieprawidłowy
        if (offset_px == 0) {
            *out_len = 0;
            return;
        }

        // offset <= out_px (referencja do już zapisanego obszaru)
        if (offset_px > out_px) {
            *out_len = 0;
            return;
        }

        size_t src_start = out_px - offset_px;

        // -----------------------------------------------------------
        // Wybór ścieżki kopiowania (identyczna logika jak w ASM):
        //
        //   offset_px >= 4: dystans src_ptr -> dst_ptr >= 16 B
        //     => pierwsze 16 B nie nachodzi na pierwszy zapis,
        //        bezpieczne użycie bloków 4-pikselowych (memcpy-style).
        //        Późniejsze bloki mogą czytać już-zapisane piksele –
        //        to zamierzone: realizuje semantykę RLE dla LZ77.
        //
        //   offset_px < 4: dystans < 16 B
        //     => kopiowanie piksel po pikselu (prawidłowa semantyka RLE).
        // -----------------------------------------------------------
        if (offset_px >= 4) {
            // Kopiowanie blokami 4 pikseli (16 B), reszta po 1 pikselu
            const uint32_t* s = dst_px + src_start;
            uint32_t* d = dst_px + out_px;
            uint32_t bytes_to_copy = length_px * 4;
            uint32_t byte_off = 0;

            while (byte_off + 16 <= bytes_to_copy) {
                // 4 piksele naraz (odpowiednik movdqu w ASM)
                uint32_t p0 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off);
                uint32_t p1 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off + 4);
                uint32_t p2 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off + 8);
                uint32_t p3 = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off + 12);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off) = p0;
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off + 4) = p1;
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off + 8) = p2;
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off + 12) = p3;
                byte_off += 16;
            }
            // Ogon: mniej niż 16 B => po jednym pikselu (4 B)
            while (byte_off < bytes_to_copy) {
                uint32_t px = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off) = px;
                byte_off += 4;
            }
        }
        else {
            // Kopiowanie skalarne piksel po pikselu (offset_px < 4)
            // Odczyt może nastąpić po zapisie w tej samej pętli –
            // to zamierzone: poprawna semantyka run-length.
            for (uint32_t k = 0; k < length_px; k++)
                dst_px[out_px + k] = dst_px[src_start + k];
        }

        out_px += length_px;

        // Zapisz next_px
        dst_px[out_px++] = next_px;
    }

    *out_len = out_px;
}