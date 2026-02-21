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

 // Okno ślizgowe: 4096 pikseli wstecz, tablice hash: 65536 wpisów (potęga 2 umożliwia maskowanie bitowe).
static const uint32_t WINDOW_PX = 4096;
static const uint32_t HASH_SIZE = 65536;
static const uint32_t HASH_MASK = HASH_SIZE - 1;
// Jedno dopasowanie nie może objąć więcej niż 64 piksele; łańcuch hash przeszukuje co najwyżej 32 kandydatów.
static const uint32_t MAX_MATCH_PX = 64;
static const uint32_t MAX_CANDIDATES = 32;
// Token ma 12 bajtów: trzy pola uint32_t (offset_px, length_px, next_px).
static const uint32_t TOKEN_SIZE = 12;
// Wartość oznaczająca pusty slot w tablicy head[] lub prev[].
static const uint32_t INVALID_POS = 0xFFFFFFFFu;

// Bufor roboczy: head[65536] zajmuje 256 KB, prev[4096] zajmuje 16 KB.
static const size_t WORK_HEAD_BYTES = HASH_SIZE * sizeof(uint32_t);
static const size_t WORK_PREV_BYTES = WINDOW_PX * sizeof(uint32_t);
static const size_t WORK_NEED_BYTES = WORK_HEAD_BYTES + WORK_PREV_BYTES;

struct Token12 {
    uint32_t offset_px;   // odległość wstecz do początku dopasowania; 0 oznacza literal
    uint32_t length_px;   // liczba skopiowanych pikseli; 0 oznacza literal
    uint32_t next_px;     // piksel bezpośrednio po dopasowaniu lub wartość literalu
};

// Hash z dwóch sąsiednich pikseli: XOR pierwszego z rotacją drugiego o 5 bitów w lewo.
// Dwa piksele wejściowe zwiększają selektywność i zmniejszają liczbę fałszywych trafień.
static inline uint32_t pixel_hash(uint32_t p0, uint32_t p1)
{
    uint32_t rot = (p1 << 5) | (p1 >> 27);
    return (p0 ^ rot) & HASH_MASK;
}

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

    if (dst_cap < TOKEN_SIZE)
        return;

    if (src_count == 0) {
        *out_len = 0;
        return;
    }

    // Brak bufora roboczego lub zbyt mały: każdy piksel emitowany jako oddzielny literal.
    // Strumień jest poprawny i dekompresuje się bez błędów, lecz bez kompresji.
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

    // head[] wskazuje najnowszą pozycję dla każdego hashu; prev[] łączy starsze pozycje w łańcuch.
    uint32_t* head = reinterpret_cast<uint32_t*>(work);
    uint32_t* prev = head + HASH_SIZE;

    // Wszystkie sloty head[] ustawione na INVALID_POS (0xFF..FF); prev[] nie wymaga inicjalizacji.
    memset(head, 0xFF, WORK_HEAD_BYTES);

    size_t out_bytes = 0;

    size_t i = 0;
    while (i < src_count) {

        size_t remaining = src_count - i;

        // Ostatni piksel nie ma sąsiada potrzebnego do obliczenia hashu, więc zawsze trafia jako literal.
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

        // Odejmowanie 1: token zawsze przechowuje jawny piksel następujący po dopasowaniu (next_px),
        // więc nie można dopasować do ostatniego piksela w oknie.
        uint32_t maxMatch = (uint32_t)(remaining - 1);
        if (maxMatch > MAX_MATCH_PX)
            maxMatch = MAX_MATCH_PX;

        uint32_t h = pixel_hash(src_px[i], src_px[i + 1]);

        // Pozycje starsze niż WINDOW_PX od bieżącej są poza oknem i nie mogą być kandydatami.
        uint32_t dictStart = (i >= WINDOW_PX) ? (uint32_t)(i - WINDOW_PX) : 0u;

        uint32_t candidate = head[h];

        uint32_t bestLen = 0;
        uint32_t bestOff = 0;

        uint32_t chainLeft = MAX_CANDIDATES;

        // Przeszukiwanie łańcucha hash: iteracja po kandydatach od najnowszego do najstarszego.
        // Pętla kończy się po napotkaniu INVALID_POS, kandydata spoza okna lub wyczerpaniu limitu.
        while (candidate != INVALID_POS && candidate >= dictStart && chainLeft > 0) {

            uint32_t offset = (uint32_t)i - candidate;

            const uint32_t* ptrA = src_px + i;
            const uint32_t* ptrB = src_px + candidate;
            uint32_t curLen = 0;

            // Porównanie blokami 4 pikseli (odpowiednik instrukcji SSE2 pcmpeqd/pmovmskb w wersji ASM).
            // Pętla blokowa przerywa się, gdy którykolwiek z czterech pikseli się różni.
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
            // Ogon: mniej niż 4 piksele lub nieudany blok — kontynuacja skalarna piksel po pikselu.
            while (curLen < maxMatch && ptrA[curLen] == ptrB[curLen])
                curLen++;

            if (curLen > bestLen) {
                bestLen = curLen;
                bestOff = offset;
            }

            // Przejście do następnego kandydata przez tablicę prev[]; slot wyznaczany modulo WINDOW_PX.
            uint32_t slot = candidate & (WINDOW_PX - 1);
            candidate = prev[slot];

            chainLeft--;
        }

        if (dst_cap - out_bytes < TOKEN_SIZE) {
            *out_len = 0;
            return;
        }

        // Dla dopasowania next_px to piksel tuż za skopiowanym ciągiem; dla literalu to sam piksel src[i].
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

        // Aktualizacja tablic hash dla wszystkich pozycji objętych tokenem [i .. i+bestLen].
        // Wstawianych jest bestLen+1 wpisów, by kolejne tokeny mogły odwoływać się do tych pozycji.
        for (uint32_t k = 0; k <= bestLen; k++) {
            uint32_t pos = (uint32_t)i + k;

            // Para (src[pos], src[pos+1]) jest wymagana do obliczenia hashu; brak sąsiada kończy wstawianie.
            if ((size_t)pos + 1 >= src_count)
                break;

            uint32_t nh = pixel_hash(src_px[pos], src_px[pos + 1]);
            uint32_t slot = pos & (WINDOW_PX - 1);

            prev[slot] = head[nh];
            head[nh] = pos;
        }

        i += bestLen + 1;
    }

    *out_len = out_bytes;
}

void lz77_rgba_decompress(
    const uint8_t* src,
    size_t          src_len,
    uint32_t* dst_px,
    size_t          dst_cap,
    size_t* out_len)
{
    *out_len = 0;

    size_t src_pos = 0;
    size_t out_px = 0;

    while (true) {

        // Pętla kończy się, gdy w strumieniu nie ma już pełnego tokenu (12 bajtów).
        if (src_pos + TOKEN_SIZE > src_len)
            break;

        const Token12* tok = reinterpret_cast<const Token12*>(src + src_pos);
        uint32_t offset_px = tok->offset_px;
        uint32_t length_px = tok->length_px;
        uint32_t next_px = tok->next_px;

        src_pos += TOKEN_SIZE;

        // Literal: oba pola offset i length równe zero — zapisywany jest wyłącznie piksel next_px.
        if (offset_px == 0 && length_px == 0) {
            if (out_px >= dst_cap) {
                *out_len = 0;
                return;
            }
            dst_px[out_px++] = next_px;
            continue;
        }

        // Dopasowanie wymaga miejsca na length_px skopiowanych pikseli oraz dodatkowego next_px.
        if (out_px + length_px + 1 > dst_cap) {
            *out_len = 0;
            return;
        }

        // offset_px == 0 przy length_px > 0 jest stanem nieprawidłowym — strumień uszkodzony.
        if (offset_px == 0) {
            *out_len = 0;
            return;
        }

        // offset_px > out_px oznaczałoby odwołanie do obszaru jeszcze niezapisanego — strumień uszkodzony.
        if (offset_px > out_px) {
            *out_len = 0;
            return;
        }

        size_t src_start = out_px - offset_px;

        // Wybór ścieżki kopiowania zależy od odległości między źródłem a miejscem zapisu:
        //   offset_px >= 4: odstęp co najmniej 16 bajtów — pierwsze 16 bajtów nie nachodzi na zapis,
        //                   co pozwala na bezpieczne kopiowanie blokami 4-pikselowymi (SSE2 movdqu w ASM).
        //                   Późniejsze bloki mogą czytać już zapisane piksele — to zamierzona semantyka RLE.
        //   offset_px < 4:  odstęp mniejszy niż 16 bajtów — wymagane kopiowanie skalarne piksel po pikselu,
        //                   gdyż odczyt i zapis nachodzą na siebie od samego początku.
        if (offset_px >= 4) {
            const uint32_t* s = dst_px + src_start;
            uint32_t* d = dst_px + out_px;
            uint32_t bytes_to_copy = length_px * 4;
            uint32_t byte_off = 0;

            while (byte_off + 16 <= bytes_to_copy) {
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
            // Ogon poniżej 16 bajtów kopiowany po jednym pikselu (4 bajty).
            while (byte_off < bytes_to_copy) {
                uint32_t px = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(s) + byte_off);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(d) + byte_off) = px;
                byte_off += 4;
            }
        }
        else {
            // Kopiowanie skalarne: odczyt może wyprzedzać zapis o mniej niż 4 piksele,
            // co realizuje poprawną semantykę run-length (powielanie wzorca in-place).
            for (uint32_t k = 0; k < length_px; k++)
                dst_px[out_px + k] = dst_px[src_start + k];
        }

        out_px += length_px;

        // Piksel next_px zapisywany bezpośrednio po skopiowanym ciągu.
        dst_px[out_px++] = next_px;
    }

    *out_len = out_px;
}