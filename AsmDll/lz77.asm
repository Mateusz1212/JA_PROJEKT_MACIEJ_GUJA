; ============================================================
; LZ77 RGBA (u32 pixels) - MASM x64, SSE2, Windows x64 ABI
; Format tokenu: Token12 { offset_px:u32, length_px:u32, next_px:u32 }
; Brak alokacji pamieci. Brak zmiennych globalnych.
; Bezpieczne dla uzycia wielowatkowego.
;
; Sygnatura kompresji:
;   void lz77_rgba_compress(
;       const uint32_t* src_px,  RCX
;       size_t src_count_px,     RDX
;       uint8_t* dst,            R8
;       size_t dst_cap_bytes,    R9
;       void* work,              [rsp+40h] -- po prologu: [rsp+78h]
;       size_t work_cap_bytes,   [rsp+48h] -- po prologu: [rsp+80h]
;       size_t* out_len_bytes    [rsp+50h] -- po prologu: [rsp+88h]
;   )
;
; Sygnatura dekompresji:
;   void lz77_rgba_decompress(
;       const uint8_t* src,      RCX
;       size_t src_len_bytes,    RDX
;       uint32_t* dst_px,        R8
;       size_t dst_cap_px,       R9
;       size_t* out_len_px       [rsp+40h] -- po prologu: [rsp+68h]
;   )
;
; Obliczenie offsetow argumentow na stosie:
;   Prolog kompresji:  8x push (8*8=64B) + sub rsp,16 = 80B lacznie
;     arg5 (work)         = [rsp + 80 + 40] = [rsp + 78h]
;     arg6 (work_cap)     = [rsp + 80 + 48] = [rsp + 80h]
;     arg7 (out_len*)     = [rsp + 80 + 56] = [rsp + 88h]
;   Prolog dekompresji: 8x push (8*8=64B), brak sub rsp
;     arg5 (out_len_px*)  = [rsp + 64 + 40] = [rsp + 68h]
;
; SSE2 - trzy miejsca uzycia:
;   1. Inicjalizacja head[] (pcmpeqd + movdqu x4096 iteracji)
;   2. Porownanie kandydatow w kompresji (pcmpeqd + pmovmskb, bloki 4px=16B)
;   3. Kopiowanie dopasowania w dekompresji (movdqu, bloki 4px=16B)
;
; Analiza bezpieczenstwa SSE2 w kompresji (punkt 2):
;   Warunek wejscia do bloku 16B: curLen+4 <= maxMatch <= remaining-1.
;   Zatem indeks i+curLen+3 < src_count_px => odczyt 16B jest w granicach.
;   Kandydat: candidate+curLen+3 < i <= src_count_px => analogicznie bezpieczny.
;   Warto o tym pamietac przy zmianie MAX_MATCH_PX.
;
; Analiza bezpieczenstwa SSE2 w dekompresji (punkt 3):
;   Blok 16B jest uzywany tylko gdy offset_px >= 4 (16B dystansu miedzy
;   wskaznikiem zrodla a celem). Dzieki temu pierwszy odczyt 16B nie pokrywa
;   sie z pierwszym zapisem 16B. Kolejne iteracje moga czytac juz zapisane
;   piksele -- jest to zamierzone i daje poprawna semantyke LZ77/RLE.
;   Przy offset_px < 4 stosowane jest kopiowanie skalarne piksel-po-pikselu.
; ============================================================

OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

.code

; --------------------------------------------------
; Stale konfiguracyjne
; --------------------------------------------------
WINDOW_PX       EQU 4096               ; okno slizgowe w pikselach (potega 2)
HASH_SIZE       EQU 65536              ; liczba wpisow tablicy hash (potega 2)
HASH_MASK       EQU (HASH_SIZE - 1)
MAX_MATCH_PX    EQU 64                 ; maks. dlugosc dopasowania w pikselach
MAX_CANDIDATES  EQU 32                 ; maks. liczba krokow w lancuchu hash
TOKEN_SIZE      EQU 12                 ; rozmiar tokenu: 3 x sizeof(u32)
INVALID_POS     EQU 0FFFFFFFFh        ; znacznik pustego slotu w tablicach hash

; Uklad bufora roboczego (dostarczanego przez wywolujacego):
;   [0              .. HASH_SIZE*4)  : head[HASH_SIZE] u32
;      hash    -> najnowsza pozycja piksela o tym hashu
;   [HASH_SIZE*4    .. +WINDOW_PX*4) : prev[WINDOW_PX] u32
;      slot(i) -> poprzednia pozycja o tym samym hashu (lancuch)
WORK_HEAD_BYTES EQU (HASH_SIZE * 4)   ; 262144 B = 256 KB
WORK_PREV_BYTES EQU (WINDOW_PX * 4)   ;  16384 B =  16 KB
WORK_NEED_BYTES EQU (WORK_HEAD_BYTES + WORK_PREV_BYTES)

; --------------------------------------------------
; Offsety argumentow na stosie po prologu - kompresja
; (8 push + sub rsp,16 = 80B przesuniecia)
; --------------------------------------------------
COMP_ARG_WORK     EQU 078h
COMP_ARG_WORKCAP  EQU 080h
COMP_ARG_OUTLEN   EQU 088h

; Zmienne lokalne w przestrzeni [rsp+0 .. rsp+15]:
;   [rsp+0]  DWORD chain_remaining -- ile krokow lancucha pozostalo
;   [rsp+4]  DWORD candidate_save  -- indeks biezacego kandydata
;   [rsp+8]  DWORD offset_px_save  -- offset biezacego kandydata (i - candidate)
;   [rsp+12] DWORD (padding)
LOCAL_CHAIN  EQU 0
LOCAL_CAND   EQU 4
LOCAL_OFFSET EQU 8

; --------------------------------------------------
; Offset argumentu na stosie po prologu - dekompresja
; (8 push = 64B przesuniecia, brak sub rsp)
; --------------------------------------------------
DECOMP_ARG_OUTLEN EQU 068h

; ============================================================
; lz77_rgba_compress
;
; Stala mapa rejestrów nieulotnych (przez cala funkcje):
;   rsi = src_px          wskaznik do tablicy pikseli wejsciowych (u32*)
;   r14 = src_count_px    liczba pikseli wejsciowych
;   rdi = dst             bufor wyjsciowy (u8*)
;   r15 = dst_cap_bytes   pojemnosc bufora wyjsciowego w bajtach
;   rbx = head_base       poczatek tablicy head[] w buforze roboczym (u32*)
;   rbp = prev_base       poczatek tablicy prev[] = head_base + WORK_HEAD_BYTES
;   r12 = out_bytes       liczba bajtow zapisanych do dst
;   r13 = out_len_ptr     wskaznik do *out_len_bytes
;   r8  = i               biezacy indeks piksela (u64)
;
; Rejestry tymczasowe: rax, rcx, rdx, r9, r10, r11, xmm0, xmm1, xmm7
; ============================================================
PUBLIC lz77_rgba_compress
lz77_rgba_compress PROC
    ; --- Prolog: zachowaj rejestry nieulotne ---
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 16                   ; 16B na zmienne lokalne (LOCAL_*)
    ; Lacznie przesuniecia RSP: 8*8 + 16 = 80B

    ; --- Zaladuj argumenty ---
    mov  rsi, rcx                               ; src_px
    mov  r14, rdx                               ; src_count_px
    mov  rdi, r8                                ; dst
    mov  r15, r9                                ; dst_cap_bytes
    mov  rbx, QWORD PTR [rsp + COMP_ARG_WORK]  ; work (head_base)
    mov  rax, QWORD PTR [rsp + COMP_ARG_WORKCAP] ; work_cap_bytes (temp)
    mov  r13, QWORD PTR [rsp + COMP_ARG_OUTLEN] ; out_len_bytes*

    xor  r12d, r12d                             ; out_bytes = 0

    ; Minimalny bufor wyjsciowy: 1 token
    cmp  r15, TOKEN_SIZE
    jb   LZC_FAIL

    ; Puste wejscie: sukces, 0 bajtow
    test r14, r14
    jz   LZC_DONE

    ; Brak bufora roboczego lub za maly => tryb samych literalow
    test rbx, rbx
    jz   LZC_LITERAL_ONLY
    cmp  rax, WORK_NEED_BYTES
    jb   LZC_LITERAL_ONLY

    ; prev_base lezy bezposrednio za head[] w buforze roboczym
    lea  rbp, [rbx + WORK_HEAD_BYTES]

    ; --- SSE2: wypelnij head[] wartoscia INVALID_POS (0xFFFFFFFF) ---
    ; xmm7 = { 0xFFFFFFFF x4 } -- rejestr SSE2 z samymi jedynkami
    pcmpeqd xmm7, xmm7                         ; all-ones przez porownanie ze soba
    mov  rcx, WORK_HEAD_BYTES / 16             ; 4096 blokow po 16B = 256 KB
    mov  rax, rbx                               ; wskaznik zapisu
LZC_INIT_HEAD:
    movdqu XMMWORD PTR [rax], xmm7             ; 4 x INVALID_POS naraz
    add  rax, 16
    dec  rcx
    jnz  LZC_INIT_HEAD
    ; prev[] nie wymaga inicjalizacji: kazdy slot jest nadpisywany
    ; starym head[hash] przed pierwszym odczytem.

    xor  r8d, r8d                               ; i = 0

; ============================================================
; Glowna petla kompresji
; i (r8) jest inkrementowane o (bestLen+1) po kazdej iteracji.
; ============================================================
LZC_MAIN:
    cmp  r8, r14                                ; i >= src_count_px?
    jae  LZC_DONE

    ; remaining = src_count_px - i
    mov  r9, r14
    sub  r9, r8

    ; Ostatni piksel (remaining==1): brak miejsca na next_px po dopasowaniu,
    ; wyslij jako literal (offset=0, length=0, next=src[i]).
    ; Uwaga: ten piksel NIE jest wstawiany do tablic hash, bo nie jest
    ; kandydatem dla przyszlych dopasowan (jest ostatni).
    cmp  r9, 1
    je   LZC_EMIT_LITERAL

    ; maxMatch = min(MAX_MATCH_PX, remaining-1)
    ; Odejmujemy 1: token zawsze przechowuje jawny "nastepny piksel" po dopasowaniu.
    mov  r10, r9
    dec  r10
    cmp  r10, MAX_MATCH_PX
    jbe  LZC_MAX_OK
    mov  r10, MAX_MATCH_PX
LZC_MAX_OK:                                    ; r10 = maxMatch

    ; --- Hash dwupikselowy: (src[i] XOR ROL(src[i+1], 5)) & HASH_MASK ---
    ; Dwa piksele sa dostepne (remaining >= 2).
    mov  eax, DWORD PTR [rsi + r8*4]           ; p0 = src[i]
    mov  edx, DWORD PTR [rsi + r8*4 + 4]       ; p1 = src[i+1]
    rol  edx, 5
    xor  eax, edx
    and  eax, HASH_MASK                         ; eax = hash

    ; dictStart = max(0, i - WINDOW_PX) -- najstarsza dopuszczalna pozycja
    xor  r11d, r11d                             ; dictStart = 0
    cmp  r8, WINDOW_PX
    jb   LZC_DICT_OK
    mov  r11, r8
    sub  r11, WINDOW_PX                         ; r11 = dictStart
LZC_DICT_OK:

    ; Pobierz pierwszego kandydata z head[hash]
    mov  ecx, DWORD PTR [rbx + rax*4]          ; ecx = head[hash]

    ; Wyzeruj najlepszy wynik
    xor  r9d, r9d                               ; bestLen = 0
    xor  edx, edx                               ; bestOff = 0

    ; Inicjuj licznik krokow lancucha w zmiennej lokalnej
    mov  DWORD PTR [rsp + LOCAL_CHAIN], MAX_CANDIDATES

; ============================================================
; Przeszukiwanie lancucha hash (hash-chain traversal)
; Przechodzi przez kolejnych kandydatow az do MAX_CANDIDATES krokow.
; ============================================================
LZC_CHAIN_LOOP:
    cmp  ecx, INVALID_POS
    je   LZC_CHAIN_DONE                         ; koniec lancucha

    ; Kandydat poza oknem? (rcx=zero-extend(ecx) umozliwia porownanie u64)
    cmp  rcx, r11
    jb   LZC_CHAIN_DONE                         ; wyszlismy poza WINDOW_PX

    ; offset = i - candidate (zawsze > 0, bo candidate < i)
    mov  eax, r8d
    sub  eax, ecx                               ; eax = offset_px

    ; Zapisz kandydata i offset w zmiennych lokalnych (rcx/eax beda zmienione)
    mov  DWORD PTR [rsp + LOCAL_CAND],   ecx
    mov  DWORD PTR [rsp + LOCAL_OFFSET], eax

    ; ptrB = &src[candidate] -- rcx: mov ecx,ecx zero-extend jest tu zbedne,
    ; bo 'mov ecx, DWORD PTR [...]' juz zero-extenduje do 64b w rcx.
    lea  rcx, [rsi + rcx*4]                    ; rcx = ptrB (bajty)

    ; curLen = 0 -- liczba pasujacych pikseli od pozycji i i candidate
    xor  eax, eax

    ; --- SSE2: porownanie blokow 4 pikseli (16 bajtow) ---
    ; Bezpieczne granicznie: curLen+4 <= maxMatch <= remaining-1,
    ; wiec i+curLen+3 < src_count_px.
LZC_CMP_BLOCK:
    lea  edx, [eax + 4]
    cmp  edx, r10d                              ; curLen+4 > maxMatch?
    ja   LZC_CMP_SCALAR

    ; ptrA = &src[i + curLen]; nie mamy wolnego rejestru na staly ptrA,
    ; wiec obliczamy indeks przez lea i uzywamy rsi jako bazy.
    lea  rdx, [r8 + rax]                        ; rdx = i + curLen (indeks px)
    movdqu xmm0, XMMWORD PTR [rsi + rdx*4]    ; 4 piksele ze zrodla (ptrA)
    movdqu xmm1, XMMWORD PTR [rcx + rax*4]    ; 4 piksele z kandydata (ptrB)
    pcmpeqd xmm0, xmm1                         ; porownaj DWORD-parami
    pmovmskb edx, xmm0                         ; maska: bit=1 gdy bajt zgodny
    cmp  edx, 0FFFFh                            ; wszystkie 16 bajtow pasuja?
    jne  LZC_CMP_SCALAR                         ; niezgodnosc -- doczyszcz skalarnie
    add  eax, 4                                 ; curLen += 4 piksele
    jmp  LZC_CMP_BLOCK

LZC_CMP_SCALAR:
    ; Skalarne porownanie: piksel po pikselu dla reszty (<4 px) lub
    ; po wyjsciu z bloku SSE2 z czesciowa niezgodnoscia.
    cmp  eax, r10d                              ; curLen >= maxMatch?
    jae  LZC_CMP_DONE
    lea  rdx, [r8 + rax]
    mov  edx, DWORD PTR [rsi + rdx*4]          ; src[i + curLen]
    cmp  edx, DWORD PTR [rcx + rax*4]          ; src[candidate + curLen]
    jne  LZC_CMP_DONE
    inc  eax
    jmp  LZC_CMP_SCALAR

LZC_CMP_DONE:
    ; eax = curLen tego kandydata; zaktualizuj najlepszy wynik
    cmp  eax, r9d                               ; curLen > bestLen?
    jle  LZC_NO_IMPROVE
    mov  r9d, eax                               ; bestLen = curLen
    mov  edx, DWORD PTR [rsp + LOCAL_OFFSET]   ; bestOff = offset tego kandydata
LZC_NO_IMPROVE:

    ; Przejdz do nastepnego kandydata przez tablice prev[]
    mov  ecx, DWORD PTR [rsp + LOCAL_CAND]     ; przywroc indeks biezacego kand.
    mov  eax, ecx
    and  eax, (WINDOW_PX - 1)                  ; slot = candidate & (WINDOW_PX-1)
    mov  ecx, DWORD PTR [rbp + rax*4]          ; candidate = prev[slot]

    dec  DWORD PTR [rsp + LOCAL_CHAIN]
    jnz  LZC_CHAIN_LOOP

LZC_CHAIN_DONE:
    ; r9d = bestLen (0 = brak dopasowania => literal)
    ; edx = bestOff (0 gdy brak dopasowania)

; ============================================================
; Emisja tokenu { bestOff, bestLen, next_px }
; ============================================================
LZC_EMIT_TOKEN:
    ; Sprawdz wolne miejsce w dst
    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    ; next_px = src[i + bestLen] gdy bestLen>0, lub src[i] gdy literal
    ; Oblicz przed uzyciem rcx do adresowania tokenu.
    test r9d, r9d
    jz   LZC_NEXT_LIT
    lea  rax, [r8 + r9]                         ; i + bestLen (64-bit, bez overflow)
    mov  eax, DWORD PTR [rsi + rax*4]           ; src[i + bestLen]
    jmp  LZC_NEXT_READY
LZC_NEXT_LIT:
    mov  eax, DWORD PTR [rsi + r8*4]            ; src[i]
LZC_NEXT_READY:                                 ; eax = next_px

    ; Zapisz token w dst[out_bytes]
    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], edx               ; token.offset_px = bestOff
    mov  DWORD PTR [rcx + 4], r9d               ; token.length_px = bestLen
    mov  DWORD PTR [rcx + 8], eax               ; token.next_px
    add  r12, TOKEN_SIZE                         ; out_bytes += 12

    ; --- Aktualizacja tablic hash dla pozycji [i .. i+bestLen] ---
    ; Wstawiamy bestLen+1 wpisow: kazda nowa pozycja startowa dopasowania
    ; staje sie poczatkiem lancucha dla swojego hasha.
    ; Uwaga edge-case: jesli bestLen==0, wstawiamy dokladnie 1 pozycje (i),
    ; co jest poprawne -- literal rowniez ma sens jako kandydat.
    mov  r10d, r9d                              ; r10d = bestLen (zachowaj)
    xor  eax, eax                               ; k = 0

LZC_INSERT_LOOP:
    cmp  eax, r10d                              ; k > bestLen?
    ja   LZC_INSERT_DONE

    ; pos = i + k
    mov  ecx, r8d
    add  ecx, eax                               ; ecx = pos (u32)

    ; Do hasha potrzeba par (src[pos], src[pos+1]): sprawdz granice
    lea  rdx, [rcx + 1]                         ; pos+1 jako u64
    cmp  rdx, r14
    jae  LZC_INSERT_DONE                        ; pos+1 >= src_count => koniec

    ; hash(pos) = (src[pos] XOR ROL(src[pos+1], 5)) & HASH_MASK
    mov  r9d,  DWORD PTR [rsi + rcx*4]
    mov  r11d, DWORD PTR [rsi + rcx*4 + 4]
    rol  r11d, 5
    xor  r9d,  r11d
    and  r9d,  HASH_MASK                        ; r9d = hash

    ; slot = pos & (WINDOW_PX-1)
    mov  edx, ecx
    and  edx, (WINDOW_PX - 1)                  ; edx = slot

    ; prev[slot] = head[hash]   (stary head wchodzi na lancuch)
    mov  r11d, DWORD PTR [rbx + r9*4]
    mov  DWORD PTR [rbp + rdx*4], r11d

    ; head[hash] = pos          (pos staje sie nowa glowa lancucha)
    mov  DWORD PTR [rbx + r9*4], ecx

    inc  eax
    jmp  LZC_INSERT_LOOP

LZC_INSERT_DONE:
    ; Przesuniecie: i += bestLen + 1
    add  r8, r10                                ; i += bestLen
    inc  r8                                     ; i += 1
    jmp  LZC_MAIN

; ============================================================
; Literal: emisja ostatniego piksela (remaining==1)
; ============================================================
LZC_EMIT_LITERAL:
    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], 0                ; offset = 0
    mov  DWORD PTR [rcx + 4], 0                ; length = 0
    mov  eax, DWORD PTR [rsi + r8*4]
    mov  DWORD PTR [rcx + 8], eax              ; next_px = src[i]
    add  r12, TOKEN_SIZE
    inc  r8
    jmp  LZC_MAIN

; ============================================================
; Tryb literalow: brak lub za maly bufor roboczy
; Kazdy piksel -> osobny token (0, 0, src[i]).
; Prawidlowy wynik -- brak kompresji, ale poprawna dekompresja.
; ============================================================
LZC_LITERAL_ONLY:
    xor  r8d, r8d
LZC_LIT_LOOP:
    cmp  r8, r14
    jae  LZC_DONE

    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], 0
    mov  DWORD PTR [rcx + 4], 0
    mov  eax, DWORD PTR [rsi + r8*4]
    mov  DWORD PTR [rcx + 8], eax
    add  r12, TOKEN_SIZE
    inc  r8
    jmp  LZC_LIT_LOOP

; ============================================================
; Wyjscia
; ============================================================
LZC_FAIL:
    mov  QWORD PTR [r13], 0                    ; *out_len_bytes = 0
    jmp  LZC_EXIT

LZC_DONE:
    mov  QWORD PTR [r13], r12                  ; *out_len_bytes = out_bytes

LZC_EXIT:
    add  rsp, 16
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rbx
    ret
lz77_rgba_compress ENDP


; ============================================================
; lz77_rgba_decompress
;
; Stala mapa rejestrów nieulotnych:
;   rsi = src           skompresowany strumien (u8*)
;   r14 = src_len_bytes dlugosc strumienia w bajtach
;   rdi = dst_px        bufor wyjsciowy (u32*)
;   r15 = dst_cap_px    pojemnosc wyjscia w pikselach
;   r12 = out_px        liczba wypisanych pikseli
;   r13 = out_len_ptr   wskaznik do *out_len_px
;   rbx = src_byte_pos  biezaca pozycja odczytu (bajty)
;   rbp = length_px     pole length z biezacego tokenu
;
; Rejestry tymczasowe: rax, rcx, rdx, r8..r11, xmm0
; ============================================================
PUBLIC lz77_rgba_decompress
lz77_rgba_decompress PROC
    ; --- Prolog ---
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    ; Przesuniecie RSP: 8*8 = 64B
    ; arg5 out_len_px* = [rsp + 64 + 40] = [rsp + 68h]

    ; --- Zaladuj argumenty ---
    mov  rsi, rcx
    mov  r14, rdx
    mov  rdi, r8
    mov  r15, r9
    mov  r13, QWORD PTR [rsp + DECOMP_ARG_OUTLEN]

    xor  r12d, r12d                             ; out_px = 0
    xor  ebx,  ebx                              ; src_byte_pos = 0

; ============================================================
; Glowna petla dekompresji
; ============================================================
LZD_LOOP:
    ; Wymaga pelnego tokenu (12 bajtow)
    lea  rax, [rbx + TOKEN_SIZE]
    cmp  rax, r14
    ja   LZD_DONE

    ; Wczytaj pola tokenu
    mov  eax, DWORD PTR [rsi + rbx]            ; eax = offset_px
    mov  ebp, DWORD PTR [rsi + rbx + 4]        ; ebp = length_px
    mov  edx, DWORD PTR [rsi + rbx + 8]        ; edx = next_px

    add  rbx, TOKEN_SIZE                        ; przesun kursor strumienia

    ; --- Literal: offset==0 && length==0 ---
    test eax, eax
    jnz  LZD_MATCH
    test ebp, ebp
    jnz  LZD_MATCH

    cmp  r12, r15                               ; out_px >= dst_cap_px?
    jae  LZD_FAIL

    mov  DWORD PTR [rdi + r12*4], edx          ; dst[out_px] = next_px
    inc  r12
    jmp  LZD_LOOP

; ============================================================
; Dopasowanie: skopiuj length_px pikseli, nastepnie next_px
; ============================================================
LZD_MATCH:
    ; Waliduj output: potrzeba length+1 pikseli miejsca
    mov  rcx, r12
    add  rcx, rbp                               ; out_px + length_px
    inc  rcx                                    ; + 1 dla next_px
    cmp  rcx, r15
    ja   LZD_FAIL

    ; Waliduj offset: offset=0 przy length>0 jest nieprawidlowy
    ; (wskazywaloby na niezapisany obszar wyjscia)
    test eax, eax
    jz   LZD_FAIL

    ; Waliduj: offset <= out_px (referencja do juz zapisanego obszaru)
    cmp  rax, r12                               ; offset (u64) > out_px?
    ja   LZD_FAIL

    ; Indeks startowy zrodla: src_start = out_px - offset_px
    mov  r8, r12
    sub  r8, rax                                ; r8 = src_start (indeks px)

    ; Wskazniki bajtowe
    lea  r9,  [rdi + r8*4]                     ; r9  = src_ptr  (u8*)
    lea  r10, [rdi + r12*4]                    ; r10 = dst_ptr  (u8*)

    ; Liczba bajtow do skopiowania = length_px * 4
    mov  r11d, ebp
    shl  r11d, 2                                ; r11d = bytes_to_copy

    ; --- Wybor sciezki kopiowania ---
    ; offset_px >= 4: dystans src_ptr -> dst_ptr >= 16B
    ;   => pierwszy blok SSE2 (16B) nie zachodzi na pierwszy blok zapisu => bezpieczne
    ;   Pozniejsze bloki moga czytac juz-zapisane piksele: to poprawna semantyka
    ;   LZ77 i realizuje run-length (np. offset=4, length=100 powtarza 4px x25).
    ; offset_px <  4: dystans < 16B => SSE2 bylby niepoprawny
    ;   => kopiowanie skalarne piksel-po-pikselu (rowniez poprawna semantyka RLE)
    cmp  eax, 4
    jb   LZD_SCALAR_COPY

    ; --- SSE2: kopiowanie blokow 4 pikseli (16 bajtow) ---
    xor  ecx, ecx                               ; byte_offset = 0
LZD_SSE2_LOOP:
    mov  eax, r11d
    sub  eax, ecx                               ; pozostale bajty
    cmp  eax, 16
    jb   LZD_SSE2_TAIL

    movdqu xmm0, XMMWORD PTR [r9 + rcx]        ; czytaj 16B ze zrodla
    movdqu XMMWORD PTR [r10 + rcx], xmm0       ; zapisz 16B do celu
    add  ecx, 16
    jmp  LZD_SSE2_LOOP

LZD_SSE2_TAIL:
    ; Ogon: mniej niz 16B => kopiuj po jednym pikselu (4B)
    cmp  ecx, r11d
    jae  LZD_COPY_DONE
    mov  eax, DWORD PTR [r9 + rcx]
    mov  DWORD PTR [r10 + rcx], eax
    add  ecx, 4
    jmp  LZD_SSE2_TAIL

    ; --- Skalarne kopiowanie (offset_px < 4) ---
    ; Odczyt moze nastapic po zapisie w tej samej petli -- to zamierzone:
    ; implementuje poprawna semantyke run-length.
LZD_SCALAR_COPY:
    xor  ecx, ecx                               ; byte_offset = 0
LZD_SCALAR_LOOP:
    cmp  ecx, r11d
    jae  LZD_COPY_DONE
    mov  eax, DWORD PTR [r9 + rcx]             ; czytaj piksel (moze byc wlasnie zapisany)
    mov  DWORD PTR [r10 + rcx], eax
    add  ecx, 4
    jmp  LZD_SCALAR_LOOP

LZD_COPY_DONE:
    ; out_px += length_px
    add  r12, rbp                               ; 64-bit (rbp upper bits = 0)

    ; Zapisz next_px
    mov  DWORD PTR [rdi + r12*4], edx          ; dst[out_px] = next_px
    inc  r12
    jmp  LZD_LOOP

LZD_FAIL:
    mov  QWORD PTR [r13], 0                    ; *out_len_px = 0
    jmp  LZD_EXIT

LZD_DONE:
    mov  QWORD PTR [r13], r12                  ; *out_len_px = out_px

LZD_EXIT:
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rdi
    pop  rsi
    pop  rbp
    pop  rbx
    ret
lz77_rgba_decompress ENDP

END