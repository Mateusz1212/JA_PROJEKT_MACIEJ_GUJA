; ============================================================
; LZ77 RGBA (u32 pixels) - MASM x64, SSE2, Windows x64 ABI
; Format tokenu: Token12 { offset_px:u32, length_px:u32, next_px:u32 }
;
; Poprawki zastosowane w tej wersji:
;  1) POPRAWNE offsety argumentow stosowych dla kompresji:
;       prolog = 8*push (0x40) + sub rsp,16 (0x10) = 0x50
;       arg5=[rsp+78h], arg6=[rsp+80h], arg7=[rsp+88h]
;  2) NIE uzywamy XMM6-15 (non-volatile). Uzywamy XMM2 (volatile) do init.
;  3) bestOff NIE jest trzymany w EDX (EDX jest scratch). bestOff w localu.
;  4) Bezpiecznik A: jesli bestLen>0, wymagaj bestOff!=0 i bestOff<=i, inaczej literal.
;  5) Bezpiecznik B: jesli bestLen>maxMatch, wymus literal (chroni next_px i spojnosc).
;  6) Bezpiecznik C: jesli bestLen>0 ale bestOff==0, literal (spojne z dekompresorem).
;  7) Zachowane sensowne SSE2:
;       - init head[] movdqu
;       - porownanie kandydatow 16B (4 px)
;       - kopiowanie w dekompresji 16B
; ============================================================

OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

.code

; Definicje sta³ych u¿ywanych w algorytmie
WINDOW_PX       EQU 4096           ; Rozmiar okna s³ownika w pikselach
HASH_SIZE       EQU 65536          ; Rozmiar tablicy haszuj¹cej (2^16)
HASH_MASK       EQU (HASH_SIZE - 1) ; Maska do operacji haszowania
MAX_MATCH_PX    EQU 64             ; Maksymalna d³ugoœæ dopasowania w pikselach
MAX_CANDIDATES  EQU 32             ; Maksymalna liczba kandydatów do sprawdzenia w ³añcuchu
TOKEN_SIZE      EQU 12             ; Rozmiar pojedynczego tokena w bajtach (3 x DWORD)
INVALID_POS     EQU 0FFFFFFFFh     ; Wartoœæ oznaczaj¹ca nieprawid³ow¹ pozycjê

; Rozmiary pamiêci roboczej
WORK_HEAD_BYTES EQU (HASH_SIZE * 4)     ; 262144 bajty dla tablicy head
WORK_PREV_BYTES EQU (WINDOW_PX * 4)     ; 16384 bajty dla tablicy prev
WORK_NEED_BYTES EQU (WORK_HEAD_BYTES + WORK_PREV_BYTES) ; 278528 ³¹cznie

; Offsety argumentów stosowych po prologu kompresji (0x50 bajtów)
COMP_ARG_WORK     EQU 078h         ; Offset dla wskaŸnika work
COMP_ARG_WORKCAP  EQU 080h         ; Offset dla pojemnoœci work_cap
COMP_ARG_OUTLEN   EQU 088h         ; Offset dla wskaŸnika out_len

; Zmienne lokalne na stosie (16 bajtów)
; [rsp+0]  DWORD chain_remaining   ; Pozosta³e iteracje ³añcucha
; [rsp+4]  DWORD candidate_save    ; Zapisany kandydat
; [rsp+8]  DWORD offset_save       ; Zapisany offset
; [rsp+12] DWORD bestoff_save       ; Zapisany najlepszy offset
LOCAL_CHAIN   EQU 0
LOCAL_CAND    EQU 4
LOCAL_OFFSET  EQU 8
LOCAL_BESTOFF EQU 12

; Offset argumentu stosowego dla dekompresji (po prologu 0x40 bajtów)
DECOMP_ARG_OUTLEN EQU 068h         ; Offset dla wskaŸnika out_len w dekompresji


; ============================================================
; Procedura kompresji LZ77 dla danych RGBA
; Wejœcie:
;   RCX - wskaŸnik do tablicy Ÿród³owej (src_px)
;   RDX - liczba pikseli Ÿród³owych (src_count_px)
;   R8  - wskaŸnik do bufora docelowego (dst)
;   R9  - pojemnoœæ bufora docelowego w bajtach (dst_cap_bytes)
;   [RSP+78h] - wskaŸnik do pamiêci roboczej (work)
;   [RSP+80h] - pojemnoœæ pamiêci roboczej (work_cap)
;   [RSP+88h] - wskaŸnik do zmiennej wyjœciowej out_len
; ============================================================
PUBLIC lz77_rgba_compress
lz77_rgba_compress PROC
    ; Prolog - zachowano nietrwa³e rejestry na stosie
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 16                     ; Zarezerwowano miejsce na zmienne lokalne

    ; Przypisano argumenty rejestrowe do rejestrów trwa³ych
    mov  rsi, rcx                 ; src_px
    mov  r14, rdx                 ; src_count_px
    mov  rdi, r8                  ; dst
    mov  r15, r9                  ; dst_cap_bytes

    ; Pobrano argumenty stosowe
    mov  rbx, QWORD PTR [rsp + COMP_ARG_WORK]      ; work
    mov  rax, QWORD PTR [rsp + COMP_ARG_WORKCAP]   ; work_cap
    mov  r13, QWORD PTR [rsp + COMP_ARG_OUTLEN]    ; out_len*

    xor  r12d, r12d               ; Wyzerowano licznik zapisanych bajtów (out_bytes=0)

    ; Sprawdzono czy bufor docelowy pomieœci co najmniej jeden token
    cmp  r15, TOKEN_SIZE
    jb   LZC_FAIL                 ; Jeœli nie, zakoñczono niepowodzeniem

    ; Sprawdzono czy dane wejœciowe nie s¹ puste
    test r14, r14
    jz   LZC_DONE                 ; Jeœli puste, zakoñczono sukcesem

    ; Sprawdzono czy przekazano wystarczaj¹c¹ pamiêæ robocz¹
    test rbx, rbx
    jz   LZC_LITERAL_ONLY         ; Brak pamiêci - u¿yto trybu tylko literalnego
    cmp  rax, WORK_NEED_BYTES
    jb   LZC_LITERAL_ONLY         ; Zbyt ma³a pamiêæ - u¿yto trybu tylko literalnego

    ; Obliczono adres bazowy tablicy prev (znajduje siê za tablic¹ head)
    lea  rbp, [rbx + WORK_HEAD_BYTES]   ; prev_base

    ; Zainicjalizowano tablicê head wartoœciami 0xFFFFFFFF (INVALID_POS)
    ; U¿yto rejestru XMM2 jako tymczasowego (volatile)
    pcmpeqd xmm2, xmm2            ; Wype³niono xmm2 jedynkami (0xFFFFFFFF)
    mov  rcx, WORK_HEAD_BYTES / 16 ; Liczba 16-bajtowych bloków
    mov  rax, rbx                 ; Pocz¹tek tablicy head
LZC_INIT_HEAD:
    movdqu XMMWORD PTR [rax], xmm2 ; Zapisano 16 bajtów
    add  rax, 16                  ; Przejœcie do nastêpnego bloku
    dec  rcx
    jnz  LZC_INIT_HEAD            ; Powtórzono dla ca³ej tablicy

    xor  r8d, r8d                 ; Inicjalizacja indeksu i = 0

; ============================================================
; G³ówna pêtla kompresji
; ============================================================
LZC_MAIN:
    cmp  r8, r14                  ; Czy przetworzono wszystkie piksele?
    jae  LZC_DONE                 ; Jeœli tak, zakoñczono

    ; Obliczono remaining = src_count - i
    mov  r9, r14
    sub  r9, r8

    ; Jeœli remaining == 1, wyemitowano literal i nie wstawiono do s³ownika
    cmp  r9, 1
    je   LZC_EMIT_LITERAL

    ; Obliczono maxMatch = min(MAX_MATCH_PX, remaining-1)
    mov  r10, r9
    dec  r10                      ; remaining-1
    cmp  r10, MAX_MATCH_PX
    jbe  LZC_MAX_OK
    mov  r10, MAX_MATCH_PX
LZC_MAX_OK:                         ; r10 = maxMatch

    ; Obliczono funkcjê haszuj¹c¹: hash = (p0 XOR ROL(p1,5)) & MASK
    mov  eax, DWORD PTR [rsi + r8*4]        ; Pobrano bie¿¹cy piksel
    mov  edx, DWORD PTR [rsi + r8*4 + 4]    ; Pobrano nastêpny piksel
    rol  edx, 5                              ; Rotacja w lewo o 5 bitów
    xor  eax, edx                            ; XOR
    and  eax, HASH_MASK                      ; Maska

    ; Obliczono dictStart = max(0, i - WINDOW_PX)
    xor  r11d, r11d
    cmp  r8, WINDOW_PX
    jb   LZC_DICT_OK
    mov  r11, r8
    sub  r11, WINDOW_PX
LZC_DICT_OK:

    ; Pobrano pierwszego kandydata z tablicy head dla danego hasha
    mov  ecx, DWORD PTR [rbx + rax*4]

    ; Inicjalizacja najlepszego dopasowania
    xor  r9d, r9d                    ; bestLen = 0
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0  ; bestOff = 0

    ; Ustawiono licznik przeszukiwania ³añcucha
    mov  DWORD PTR [rsp + LOCAL_CHAIN], MAX_CANDIDATES

; ============================================================
; Pêtla przeszukiwania ³añcucha kandydatów
; ============================================================
LZC_CHAIN_LOOP:
    cmp  ecx, INVALID_POS          ; Czy kandydat jest prawid³owy?
    je   LZC_CHAIN_DONE

    cmp  rcx, r11                  ; Czy kandydat mieœci siê w oknie?
    jb   LZC_CHAIN_DONE

    ; Obliczono offset = i - candidate
    mov  eax, r8d
    sub  eax, ecx
    mov  DWORD PTR [rsp + LOCAL_CAND],   ecx   ; Zapisano kandydata
    mov  DWORD PTR [rsp + LOCAL_OFFSET], eax   ; Zapisano offset

    lea  rcx, [rsi + rcx*4]          ; WskaŸnik do danych kandydata
    xor  eax, eax                    ; curLen = 0

; Porównanie blokami 16-bajtowymi (4 piksele) z u¿yciem SSE2
LZC_CMP_BLOCK:
    lea  edx, [eax + 4]
    cmp  edx, r10d                   ; curLen+4 > maxMatch ?
    ja   LZC_CMP_SCALAR               ; Jeœli tak, przejœcie do porównania skalarnego

    lea  rdx, [r8 + rax]             ; i + curLen
    movdqu xmm0, XMMWORD PTR [rsi + rdx*4]  ; Za³adowano 4 piksele z bie¿¹cej pozycji
    movdqu xmm1, XMMWORD PTR [rcx + rax*4]  ; Za³adowano 4 piksele z kandydata
    pcmpeqd xmm0, xmm1                       ; Porównanie - wynik 0xFFFFFFFF dla równych
    pmovmskb edx, xmm0                       ; Maska bitowa wyniku porównania
    cmp  edx, 0FFFFh                          ; Czy wszystkie 16 bajtów równe?
    jne  LZC_CMP_SCALAR
    add  eax, 4                               ; Zwiêkszono d³ugoœæ o 4
    jmp  LZC_CMP_BLOCK                         ; Kontynuacja

; Porównanie skalarne dla pozosta³ych pikseli
LZC_CMP_SCALAR:
    cmp  eax, r10d
    jae  LZC_CMP_DONE
    lea  rdx, [r8 + rax]
    mov  edx, DWORD PTR [rsi + rdx*4]        ; Pobrano piksel z bie¿¹cej pozycji
    cmp  edx, DWORD PTR [rcx + rax*4]        ; Porównanie z pikselem kandydata
    jne  LZC_CMP_DONE
    inc  eax                                   ; Zwiêkszono d³ugoœæ o 1
    jmp  LZC_CMP_SCALAR

LZC_CMP_DONE:
    cmp  eax, r9d                            ; Czy nowe dopasowanie jest lepsze?
    jle  LZC_NO_IMPROVE
    mov  r9d, eax                              ; Aktualizacja bestLen
    mov  edx, DWORD PTR [rsp + LOCAL_OFFSET]
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], edx  ; Aktualizacja bestOff
LZC_NO_IMPROVE:

    ; Przejœcie do nastêpnego kandydata przez tablicê prev
    mov  ecx, DWORD PTR [rsp + LOCAL_CAND]
    mov  eax, ecx
    and  eax, (WINDOW_PX - 1)                ; Obliczenie indeksu w tablicy prev
    mov  ecx, DWORD PTR [rbp + rax*4]        ; Pobrano nastêpnego kandydata

    dec  DWORD PTR [rsp + LOCAL_CHAIN]       ; Zmniejszono licznik
    jnz  LZC_CHAIN_LOOP

LZC_CHAIN_DONE:

    ; ============================================================
    ; Bezpiecznik B: bestLen <= maxMatch
    ; Zabezpiecza przed odczytem poza zakres i zapewnia spójnoœæ tokena
    ; ============================================================
    cmp  r9d, r10d
    jbe  LZC_LEN_OK
    xor  r9d, r9d                              ; Wymuszenie literala
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0
LZC_LEN_OK:

    ; ============================================================
    ; Bezpiecznik A/C: dla match bestLen>0 wymagane:
    ;   bestOff != 0 i bestOff <= i
    ; W przeciwnym razie wyemitowano literal
    ; ============================================================
    test r9d, r9d
    jz   LZC_EMIT_TOKEN

    mov  edx, DWORD PTR [rsp + LOCAL_BESTOFF]
    test edx, edx
    jz   LZC_FORCE_LITERAL
    cmp  edx, r8d
    ja   LZC_FORCE_LITERAL
    jmp  LZC_EMIT_TOKEN

LZC_FORCE_LITERAL:
    xor  r9d, r9d                              ; Wymuszenie literala
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0

; ============================================================
; Emisja tokena do bufora wyjœciowego
; ============================================================
LZC_EMIT_TOKEN:
    ; Sprawdzono dostêpne miejsce w buforze
    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    ; Pobrano next_px w zale¿noœci od typu tokena
    test r9d, r9d
    jz   LZC_NEXT_LIT
    ; Dla tokena z dopasowaniem: next_px = src[i + bestLen]
    lea  rax, [r8 + r9]
    mov  eax, DWORD PTR [rsi + rax*4]
    jmp  LZC_NEXT_READY
LZC_NEXT_LIT:
    ; Dla literala: next_px = src[i]
    mov  eax, DWORD PTR [rsi + r8*4]
LZC_NEXT_READY:

    mov  edx, DWORD PTR [rsp + LOCAL_BESTOFF]  ; offset

    ; Zapisano token (offset, length, next_px)
    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], edx
    mov  DWORD PTR [rcx + 4], r9d
    mov  DWORD PTR [rcx + 8], eax
    add  r12, TOKEN_SIZE                        ; Zwiêkszono licznik wyjœciowy

    ; Wstawiono pozycje [i .. i+bestLen] do s³ownika
    mov  r10d, r9d
    xor  eax, eax

LZC_INSERT_LOOP:
    cmp  eax, r10d
    ja   LZC_INSERT_DONE

    mov  ecx, r8d
    add  ecx, eax                ; pozycja do wstawienia

    ; Sprawdzono czy nastêpna pozycja istnieje (potrzebna do haszowania)
    lea  rdx, [rcx + 1]
    cmp  rdx, r14
    jae  LZC_INSERT_DONE

    ; Obliczono hash dla pozycji
    mov  r9d,  DWORD PTR [rsi + rcx*4]        ; piksel bie¿¹cy
    mov  r11d, DWORD PTR [rsi + rcx*4 + 4]    ; piksel nastêpny
    rol  r11d, 5
    xor  r9d,  r11d
    and  r9d,  HASH_MASK

    ; Obliczono indeks w tablicy prev
    mov  edx, ecx
    and  edx, (WINDOW_PX - 1)    ; slot

    ; Aktualizacja ³añcucha: prev[slot] = head[hash], head[hash] = pos
    mov  r11d, DWORD PTR [rbx + r9*4]    ; Stara wartoœæ head
    mov  DWORD PTR [rbp + rdx*4], r11d   ; Zapisano do prev
    mov  DWORD PTR [rbx + r9*4], ecx     ; Aktualizacja head

    inc  eax
    jmp  LZC_INSERT_LOOP

LZC_INSERT_DONE:
    add  r8, r10                              ; i += bestLen
    inc  r8                                    ; i++ (przesuniêcie o next_px)
    jmp  LZC_MAIN

; ============================================================
; Obs³uga ostatniego piksela jako literala
; ============================================================
LZC_EMIT_LITERAL:
    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], 0               ; offset = 0
    mov  DWORD PTR [rcx + 4], 0               ; length = 0
    mov  eax, DWORD PTR [rsi + r8*4]          ; next_px = bie¿¹cy piksel
    mov  DWORD PTR [rcx + 8], eax
    add  r12, TOKEN_SIZE
    inc  r8
    jmp  LZC_MAIN

; ============================================================
; Tryb tylko literalny - u¿yty gdy brak pamiêci roboczej
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
; Zakoñczenie procedury kompresji
; ============================================================
LZC_FAIL:
    mov  QWORD PTR [r13], 0       ; Zapisano 0 w out_len (sygnalizacja b³êdu)
    jmp  LZC_EXIT

LZC_DONE:
    mov  QWORD PTR [r13], r12      ; Zapisano rzeczywist¹ d³ugoœæ wyjœcia

LZC_EXIT:
    add  rsp, 16                   ; Zwolniono zmienne lokalne
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
; Procedura dekompresji LZ77 dla danych RGBA
; Wejœcie:
;   RCX - wskaŸnik do skompresowanych danych (src)
;   RDX - rozmiar skompresowanych danych w bajtach (src_len)
;   R8  - wskaŸnik do bufora wyjœciowego (dst)
;   R9  - pojemnoœæ bufora wyjœciowego w pikselach (dst_cap_px)
;   [RSP+68h] - wskaŸnik do zmiennej wyjœciowej out_len_px
; ============================================================
PUBLIC lz77_rgba_decompress
lz77_rgba_decompress PROC
    ; Prolog - zachowano nietrwa³e rejestry
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    ; Przypisano argumenty rejestrowe
    mov  rsi, rcx                 ; src - skompresowane dane
    mov  r14, rdx                 ; src_len - rozmiar danych w bajtach
    mov  rdi, r8                  ; dst - bufor wyjœciowy
    mov  r15, r9                  ; dst_cap_px - pojemnoœæ w pikselach
    mov  r13, QWORD PTR [rsp + DECOMP_ARG_OUTLEN] ; out_len*

    xor  r12d, r12d               ; Licznik zapisanych pikseli (out_px)
    xor  ebx,  ebx                 ; Licznik odczytanych bajtów (in_bytes)

LZD_LOOP:
    ; Sprawdzono czy pozosta³o miejsce na kolejny token
    lea  rax, [rbx + TOKEN_SIZE]
    cmp  rax, r14
    ja   LZD_DONE                  ; Jeœli nie, zakoñczono

    ; Odczytano token (offset, length, next_px)
    mov  eax, DWORD PTR [rsi + rbx]          ; offset_px
    mov  ebp, DWORD PTR [rsi + rbx + 4]      ; length_px
    mov  edx, DWORD PTR [rsi + rbx + 8]      ; next_px
    add  rbx, TOKEN_SIZE                       ; Przesuniêto wskaŸnik wejœcia

    ; Sprawdzono czy to token literalny (offset=0 i length=0)
    test eax, eax
    jnz  LZD_MATCH
    test ebp, ebp
    jnz  LZD_MATCH

    ; Obs³uga literala - zapis pojedynczego piksela
    cmp  r12, r15
    jae  LZD_FAIL                    ; Sprawdzenie przepe³nienia bufora

    mov  DWORD PTR [rdi + r12*4], edx
    inc  r12
    jmp  LZD_LOOP

LZD_MATCH:
    ; Obs³uga tokena z dopasowaniem
    ; Obliczono docelow¹ pozycjê po skopiowaniu dopasowania
    mov  rcx, r12
    add  rcx, rbp
    inc  rcx
    cmp  rcx, r15
    ja   LZD_FAIL                    ; Sprawdzenie przepe³nienia

    ; Walidacja offsetu
    test eax, eax
    jz   LZD_FAIL
    cmp  rax, r12                     ; Offset nie mo¿e byæ wiêkszy ni¿ bie¿¹ca pozycja
    ja   LZD_FAIL

    ; Obliczono adres Ÿród³a dopasowania
    mov  r8, r12
    sub  r8, rax                      ; src_match = dst + (out_px - offset)

    ; Przygotowano wskaŸniki do kopiowania
    lea  r9,  [rdi + r8*4]            ; ród³o dopasowania
    lea  r10, [rdi + r12*4]            ; Miejsce docelowe

    ; Konwersja d³ugoœci z pikseli na bajty
    mov  r11d, ebp
    shl  r11d, 2                       ; length_bytes = length_px * 4

    ; Wybór metody kopiowania w zale¿noœci od offsetu
    cmp  eax, 4
    jb   LZD_SCALAR_COPY               ; Dla ma³ych offsetów u¿yto kopiowania skalarnego

    ; Kopiowanie z u¿yciem SSE2 dla offsetów >= 4
    xor  ecx, ecx
LZD_SSE2_LOOP:
    mov  eax, r11d
    sub  eax, ecx
    cmp  eax, 16
    jb   LZD_SSE2_TAIL

    movdqu xmm0, XMMWORD PTR [r9 + rcx]    ; Za³adowano 16 bajtów
    movdqu XMMWORD PTR [r10 + rcx], xmm0   ; Zapisano 16 bajtów
    add  ecx, 16
    jmp  LZD_SSE2_LOOP

LZD_SSE2_TAIL:
    ; Kopiowanie pozosta³ych bajtów skalarnie
    cmp  ecx, r11d
    jae  LZD_COPY_DONE
    mov  eax, DWORD PTR [r9 + rcx]
    mov  DWORD PTR [r10 + rcx], eax
    add  ecx, 4
    jmp  LZD_SSE2_TAIL

; Kopiowanie skalarne dla ma³ych offsetów
LZD_SCALAR_COPY:
    xor  ecx, ecx
LZD_SCALAR_LOOP:
    cmp  ecx, r11d
    jae  LZD_COPY_DONE
    mov  eax, DWORD PTR [r9 + rcx]
    mov  DWORD PTR [r10 + rcx], eax
    add  ecx, 4
    jmp  LZD_SCALAR_LOOP

LZD_COPY_DONE:
    ; Aktualizacja pozycji po skopiowaniu dopasowania
    add  r12, rbp
    ; Zapisano next_px
    mov  DWORD PTR [rdi + r12*4], edx
    inc  r12
    jmp  LZD_LOOP

LZD_FAIL:
    mov  QWORD PTR [r13], 0        ; Zapisano 0 w out_len (sygnalizacja b³êdu)
    jmp  LZD_EXIT

LZD_DONE:
    mov  QWORD PTR [r13], r12       ; Zapisano rzeczywist¹ liczbê pikseli

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