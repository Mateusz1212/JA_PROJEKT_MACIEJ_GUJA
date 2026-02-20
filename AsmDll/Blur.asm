.data
    align 16                                    ; Wyrównanie do 16 bajtów

.code
; ----------------------------------------------------------------------------
;   width                               ; [RBP + 64]
;   height                              ; [RBP + 72]
;   y (center Y)                        ; [RBP + 80]
;   xStart (Iterator X)                 ; [RBP + 88]
;   xFinish                             ; [RBP + 96]
; ----------------------------------------------------------------------------

BLUR_KERNEL_ASM proc frame
    push rbp                                    ; Zapisz stary wskaŸnik bazy stosu na stosie
    .pushreg rbp                                ; Dyrektywa dla debuggera/unwindera o zapisaniu RBP
    mov rbp, rsp                                ; Ustaw nowy wskaŸnik bazy (RBP) na obecny szczyt stosu (RSP)
    .setframe rbp, 0                            ; Dyrektywa informuj¹ca, ¿e RBP jest teraz ramk¹ stosu
    
    push rbx                                    ; Zapisywanie rejestrów, zachowaj RBX (rejestr zachowywany przez wywo³ywanego)
    .pushreg rbx                                ; Dyrektywa unwind dla RBX
    push rsi                                    ; Zachowaj RSI (rejestr zachowywany przez wywo³ywanego)
    .pushreg rsi                                ; Dyrektywa unwind dla RSI
    push rdi                                    ; Zachowaj RDI (rejestr zachowywany przez wywo³ywanego)
    .pushreg rdi                                ; Dyrektywa unwind dla RDI
    push r12                                    ; Zachowaj R12 (rejestr ogólnego przeznaczenia)
    .pushreg r12                                ; Dyrektywa unwind dla R12
    push r13                                    ; Zachowaj R13 (rejestr ogólnego przeznaczenia)
    .pushreg r13                                ; Dyrektywa unwind dla R13
    push r14                                    ; Zachowaj R14 (rejestr ogólnego przeznaczenia)
    .pushreg r14                                ; Dyrektywa unwind dla R14
    push r15                                    ; Zachowaj R15 (rejestr ogólnego przeznaczenia)
    .pushreg r15                                ; Dyrektywa unwind dla R15
    
    sub rsp, 64                                 ; Rezerwacja miejsca na stosie dla YMM0 i YMM1 (64 bajty)
    vmovdqu ymmword ptr [rsp], ymm0             ; Zapisz zawartoœæ rejestru wektorowego YMM0 na stosie (zabezpieczenie)
    vmovdqu ymmword ptr [rsp+32], ymm1          ; Zapisz zawartoœæ rejestru wektorowego YMM1 na stosie (zabezpieczenie)
    
    .endprolog                                  ; Koniec prologu funkcji (informacja dla unwindera)

    mov r12d, [rbp + 64]                        ; Pobieranie argumentów, wczytaj 'width' do R12D
    mov r13d, [rbp + 72]                        ; Wczytaj 'height' do R13D
    mov r14d, [rbp + 80]                        ; Wczytaj 'y' (aktualny wiersz) do R14D
    mov r15d, [rbp + 88]                        ; Wczytaj 'xStart' (pocz¹tek pêtli X) do R15D

    mov rsi, r14                                ; Obliczanie zakresu Y (pionowo), skopiuj 'y' do RSI
    sub rsi, 20                                 ; Odejmij 20 (promieñ rozmycia) od pozycji Y, aby znaleŸæ górn¹ krawêdŸ okna
    jns y_min_ok                                ; Jeœli wynik nie jest ujemny (flaga znaku=0), skocz do y_min_ok
    xor esi, esi                                ; Jeœli wynik ujemny, wyzeruj ESI (ustaw doln¹ granicê na 0 - clamp to edge)

y_min_ok:
    mov rdi, r14                                ; Skopiuj 'y' do RDI
    add rdi, 20                                 ; Dodaj 20 (promieñ rozmycia), aby znaleŸæ doln¹ krawêdŸ okna
    mov eax, r13d                               ; Wczytaj 'height' do EAX
    dec eax                                     ; Zmniejsz 'height' o 1 (aby uzyskaæ maksymalny indeks: height-1)
    cmp rdi, rax                                ; Porównaj obliczon¹ doln¹ krawêdŸ z maksymalnym indeksem
    jle y_max_ok                                ; Jeœli krawêdŸ <= max indeks, skocz do y_max_ok (jest w zakresie)
    mov rdi, rax                                ; W przeciwnym razie ustaw krawêdŸ na max indeks (clamp to edge)

y_max_ok:

                                                
main_loop_x:                                    ; Pêtla G³ówna X
    mov eax, [rbp + 96]                         ; xFinish, wczytaj koniec zakresu X do EAX
    cmp r15d, eax                               ; Porównaj iterator X (R15D) z xFinish
    jg end_proc                                 ; Jeœli R15D > xFinish, zakoñcz procedurê

    vxorps ymm0, ymm0, ymm0                     ; Zeruj sumy: YMM0 (AVX 256-bit register), wyzeruj akumulator sumy kolorów
                                                ; Obliczanie zakresu X (poziomo)
    mov rbx, r15                                ; Skopiuj aktualny 'x' (R15) do RBX
    sub rbx, 20                                 ; Odejmij 20 (promieñ), aby znaleŸæ lew¹ krawêdŸ okna
    jns kx_min_ok                               ; Jeœli wynik nieujemny, skocz do kx_min_ok
    xor ebx, ebx                                ; Jeœli ujemny, wyzeruj EBX (clamp to edge 0)

    kx_min_ok:
        mov eax, r15d                           ; Wczytaj aktualny 'x' do EAX
        add eax, 20                             ; Dodaj 20 (promieñ), aby znaleŸæ praw¹ krawêdŸ okna
        mov r10d, r12d                          ; Wczytaj 'width' do R10D
        dec r10d                                ; Zmniejsz szerokoœæ o 1 (width-1)
        cmp eax, r10d                           ; Porównaj praw¹ krawêdŸ z maksymalnym indeksem
        jle kx_max_ok                           ; Jeœli w zakresie, skocz do kx_max_ok
        mov eax, r10d                           ; W przeciwnym razie przytnij do (width-1)
    
    kx_max_ok:
        mov r13, rax                            ; R13 = kx_end, zapisz obliczony koniec pêtli kernela X w R13

                                                
    mov r10, rsi                                ; Pêtla Kernela Y, ustaw iterator 'ky' (R10) na pocz¹tek zakresu Y (RSI)
                                                ; R10 = ky iterator
    loop_ky:
        cmp r10d, edi                           ; Porównaj iterator 'ky' z koñcem zakresu Y (RDI)
        jg end_loop_ky                          ; Jeœli ky > ky_end, wyjdŸ z pêtli Y

        mov r11d, r10d                          ; Skopiuj 'ky' do R11D
        imul r11d, r12d                         ; R11 = row_offset, pomnó¿ 'ky' przez 'width' (oblicz offset wiersza)

                                                ; Pêtla Kernela X
        mov rax, rbx                            ; RAX = kx iterator, ustaw iterator 'kx' na pocz¹tek zakresu X (RBX)
        loop_kx:
            cmp eax, r13d                       ; Porównaj iterator 'kx' z koñcem zakresu X (R13D)
            jg end_loop_kx                      ; Jeœli kx > kx_end, wyjdŸ z pêtli X

            mov r14d, r11d                              ; Index = row_offset + kx, skopiuj offset wiersza do R14D
            add r14d, eax                               ; Dodaj kolumnê (kx) do offsetu wiersza
            movsxd r14, r14d                            ; R14 = 64-bit index, rozszerz 32-bitowy indeks do 64 bitów (ze znakiem)

                                                        ; £adowanie pikseli
            push rax                                    ; Zapisz kx, od³ó¿ iterator kx na stos (potrzebujemy RAX do operacji)

                                                
                                                
            movzx eax, byte ptr [rcx + r14]             ; R (RCX) - £adowanie do XMM1 (dolna czêœæ YMM1), wczytaj bajt kana³u R
            vmovd xmm1, eax                             ; Instrukcja vmovd automatycznie zeruje górne bity YMM1, przenieœ R do najni¿szego dword XMM1
                                        
            movzx eax, byte ptr [rdx + r14]             ; G (RDX), wczytaj bajt kana³u G
            vpinsrd xmm1, xmm1, eax, 1                  ; Wstaw wartoœæ G na pozycjê 1 w wektorze XMM1
                                          
            movzx eax, byte ptr [r8 + r14]              ; B (R8), wczytaj bajt kana³u B
            vpinsrd xmm1, xmm1, eax, 2                  ; Wstaw wartoœæ B na pozycjê 2 w wektorze XMM1

            mov eax, 1                                  ; Count (1), ustaw licznik pikseli na 1
            vpinsrd xmm1, xmm1, eax, 3                  ; Wstaw licznik (1) na pozycjê 3 w wektorze XMM1 (u¿ywane do œredniej)

            pop rax                                     ; Przywróæ oryginaln¹ wartoœæ iteratora kx ze stosu
                                      
            vpaddd ymm0, ymm0, ymm1                     ; Sumowanie AVX, dodaj wektor piksela (R, G, B, 1) do akumulatora YMM0
                                                        ; XMM1 jest doln¹ czêœci¹ YMM1. Górna czêœæ YMM1 to same zera (dziêki vmovd).
                                                        ; Sumowanie u¿ywaj¹c rejestrów 256-bitowych.
            inc eax                                     ; Zwiêksz iterator kx
            jmp loop_kx                                 ; Skocz na pocz¹tek pêtli kernela X

    end_loop_kx:
        inc r10d                                        ; Zwiêksz iterator ky
        jmp loop_ky                                     ; Skocz na pocz¹tek pêtli kernela Y

end_loop_ky:

                                                        ; Uœrednianie (AVX)
                                                        ; YMM0 zawiera wynik w dolnych 128 bitach.
                                                        ; Wyci¹gamy doln¹ po³ówkê do XMM1, ¿eby wykonaæ dzielenie (float).
    
    vextractf128 xmm1, ymm0, 0                          ; Wyci¹gnij dolne 128 bitów z YMM0 do XMM1 (zawiera sumy R, G, B, Count)
    
    
    vcvtdq2ps xmm1, xmm1                                ; Konwersja int -> float, zamieñ liczby ca³kowite w XMM1 na zmiennoprzecinkowe
    
    
    vshufps xmm2, xmm1, xmm1, 11111111b                 ; Rozpropaguj Count (indeks 3), skopiuj wartoœæ z index 3 (licznik) do wszystkich pól XMM2
    
    
    vdivps xmm1, xmm1, xmm2                             ; Dzielenie, podziel sumy (R, G, B, Count) przez Count (XMM1 / XMM2)
    
                                                        ; Konwersja float -> int32
    vcvttps2dq xmm0, xmm1                               ; XMM0 = [IntCount, IntB, IntG, IntR], konwertuj float z powrotem na int (obciêcie)

    
    mov eax, [rbp + 80]                                 ; Zapis, wczytaj 'y' (numer wiersza)
    imul eax, r12d                                      ; Pomnó¿ przez 'width'
    add eax, r15d                                       ; Dodaj 'x' (aktualna kolumna) -> oblicz indeks piksela docelowego
    movsxd r14, eax                                     ; Rozszerz indeks do 64 bitów w R14

    
    vpextrb eax, xmm0, 0                                ; Zapis R (Index 0), wyci¹gnij bajt (wynik R) z pozycji 0 wektora XMM0
    mov [r9 + r14], al                                  ; Zapisz obliczony bajt R do tablicy wyjœciowej (R9 to wskaŸnik na wyjœcie R)
    
    
    vpextrb eax, xmm0, 4                                ; Zapis G (Index 4), wyci¹gnij bajt (wynik G) z pozycji 4 (bajtowo to 4. bajt inta z index 1)
    mov r11, [rbp + 48]                                 ; Wczytaj adres bufora wyjœciowego Green ze stosu
    mov [r11 + r14], al                                 ; Zapisz obliczony bajt G
    
    
    vpextrb eax, xmm0, 8                                ; Zapis B (Index 8), wyci¹gnij bajt (wynik B) z pozycji 8 (bajtowo to 8. bajt inta z index 2)
    mov r11, [rbp + 56]                                 ; Wczytaj adres bufora wyjœciowego Blue ze stosu
    mov [r11 + r14], al                                 ; Zapisz obliczony bajt B

    inc r15d                                            ; Zwiêksz g³ówny iterator X
    jmp main_loop_x                                     ; Wróæ do pêtli g³ównej

end_proc:
    vmovdqu ymm1, ymmword ptr [rsp+32]                  ; Przywróæ oryginaln¹ wartoœæ YMM1 ze stosu
    vmovdqu ymm0, ymmword ptr [rsp]                     ; Przywróæ oryginaln¹ wartoœæ YMM0 ze stosu
    add rsp, 64                                         ; Zwolnij zarezerwowane miejsce na stosie
    
    pop r15                                             ; Przywróæ R15
    pop r14                                             ; Przywróæ R14
    pop r13                                             ; Przywróæ R13
    pop r12                                             ; Przywróæ R12
    pop rdi                                             ; Przywróæ RDI
    pop rsi                                             ; Przywróæ RSI
    pop rbx                                             ; Przywróæ RBX
    pop rbp                                             ; Przywróæ RBP
    ret                                                 ; Powrót z funkcji

BLUR_KERNEL_ASM endp
end