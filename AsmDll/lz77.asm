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

WINDOW_PX       EQU 4096
HASH_SIZE       EQU 65536
HASH_MASK       EQU (HASH_SIZE - 1)
MAX_MATCH_PX    EQU 64
MAX_CANDIDATES  EQU 32
TOKEN_SIZE      EQU 12
INVALID_POS     EQU 0FFFFFFFFh

WORK_HEAD_BYTES EQU (HASH_SIZE * 4)     ; 262144
WORK_PREV_BYTES EQU (WINDOW_PX * 4)     ; 16384
WORK_NEED_BYTES EQU (WORK_HEAD_BYTES + WORK_PREV_BYTES) ; 278528

; --- stack args after prolog (0x50 bytes) ---
COMP_ARG_WORK     EQU 078h
COMP_ARG_WORKCAP  EQU 080h
COMP_ARG_OUTLEN   EQU 088h

; --- locals: 16 bytes ---
; [rsp+0]  DWORD chain_remaining
; [rsp+4]  DWORD candidate_save
; [rsp+8]  DWORD offset_save
; [rsp+12] DWORD bestoff_save
LOCAL_CHAIN   EQU 0
LOCAL_CAND    EQU 4
LOCAL_OFFSET  EQU 8
LOCAL_BESTOFF EQU 12

; --- decompress arg5 after prolog (0x40 bytes) ---
DECOMP_ARG_OUTLEN EQU 068h


; ============================================================
; lz77_rgba_compress
; ============================================================
PUBLIC lz77_rgba_compress
lz77_rgba_compress PROC
    ; prolog
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 16

    ; args regs
    mov  rsi, rcx                 ; src_px
    mov  r14, rdx                 ; src_count_px
    mov  rdi, r8                  ; dst
    mov  r15, r9                  ; dst_cap_bytes

    ; args stack
    mov  rbx, QWORD PTR [rsp + COMP_ARG_WORK]      ; work
    mov  rax, QWORD PTR [rsp + COMP_ARG_WORKCAP]   ; work_cap
    mov  r13, QWORD PTR [rsp + COMP_ARG_OUTLEN]    ; out_len*

    xor  r12d, r12d               ; out_bytes=0

    ; dst_cap >= 1 token
    cmp  r15, TOKEN_SIZE
    jb   LZC_FAIL

    ; empty input
    test r14, r14
    jz   LZC_DONE

    ; no/too small work => literal only
    test rbx, rbx
    jz   LZC_LITERAL_ONLY
    cmp  rax, WORK_NEED_BYTES
    jb   LZC_LITERAL_ONLY

    lea  rbp, [rbx + WORK_HEAD_BYTES]   ; prev_base

    ; SSE2 init head[] = 0xFFFFFFFF (use XMM2 volatile)
    pcmpeqd xmm2, xmm2
    mov  rcx, WORK_HEAD_BYTES / 16
    mov  rax, rbx
LZC_INIT_HEAD:
    movdqu XMMWORD PTR [rax], xmm2
    add  rax, 16
    dec  rcx
    jnz  LZC_INIT_HEAD

    xor  r8d, r8d                 ; i = 0

; ============================================================
; main loop
; ============================================================
LZC_MAIN:
    cmp  r8, r14
    jae  LZC_DONE

    ; remaining = src_count - i
    mov  r9, r14
    sub  r9, r8

    ; remaining==1 => literal (do not insert)
    cmp  r9, 1
    je   LZC_EMIT_LITERAL

    ; maxMatch = min(MAX_MATCH_PX, remaining-1)
    mov  r10, r9
    dec  r10
    cmp  r10, MAX_MATCH_PX
    jbe  LZC_MAX_OK
    mov  r10, MAX_MATCH_PX
LZC_MAX_OK:                         ; r10 = maxMatch (u64), use r10d when comparing lengths

    ; hash(i) = (p0 XOR ROL(p1,5)) & MASK
    mov  eax, DWORD PTR [rsi + r8*4]
    mov  edx, DWORD PTR [rsi + r8*4 + 4]
    rol  edx, 5
    xor  eax, edx
    and  eax, HASH_MASK

    ; dictStart = max(0, i - WINDOW_PX)
    xor  r11d, r11d
    cmp  r8, WINDOW_PX
    jb   LZC_DICT_OK
    mov  r11, r8
    sub  r11, WINDOW_PX
LZC_DICT_OK:

    ; candidate = head[hash]
    mov  ecx, DWORD PTR [rbx + rax*4]

    ; bestLen=0
    xor  r9d, r9d
    ; bestOff=0
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0

    mov  DWORD PTR [rsp + LOCAL_CHAIN], MAX_CANDIDATES

; ============================================================
; chain loop
; ============================================================
LZC_CHAIN_LOOP:
    cmp  ecx, INVALID_POS
    je   LZC_CHAIN_DONE

    cmp  rcx, r11
    jb   LZC_CHAIN_DONE

    ; offset = i - candidate
    mov  eax, r8d
    sub  eax, ecx

    mov  DWORD PTR [rsp + LOCAL_CAND],   ecx
    mov  DWORD PTR [rsp + LOCAL_OFFSET], eax

    lea  rcx, [rsi + rcx*4]          ; ptrB
    xor  eax, eax                    ; curLen=0

; SSE2 16B blocks (4 px)
LZC_CMP_BLOCK:
    lea  edx, [eax + 4]
    cmp  edx, r10d                   ; curLen+4 > maxMatch ?
    ja   LZC_CMP_SCALAR

    lea  rdx, [r8 + rax]             ; i + curLen
    movdqu xmm0, XMMWORD PTR [rsi + rdx*4]
    movdqu xmm1, XMMWORD PTR [rcx + rax*4]
    pcmpeqd xmm0, xmm1
    pmovmskb edx, xmm0
    cmp  edx, 0FFFFh
    jne  LZC_CMP_SCALAR
    add  eax, 4
    jmp  LZC_CMP_BLOCK

; scalar tail
LZC_CMP_SCALAR:
    cmp  eax, r10d
    jae  LZC_CMP_DONE
    lea  rdx, [r8 + rax]
    mov  edx, DWORD PTR [rsi + rdx*4]
    cmp  edx, DWORD PTR [rcx + rax*4]
    jne  LZC_CMP_DONE
    inc  eax
    jmp  LZC_CMP_SCALAR

LZC_CMP_DONE:
    cmp  eax, r9d
    jle  LZC_NO_IMPROVE
    mov  r9d, eax
    mov  edx, DWORD PTR [rsp + LOCAL_OFFSET]
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], edx
LZC_NO_IMPROVE:

    ; next candidate via prev[slot]
    mov  ecx, DWORD PTR [rsp + LOCAL_CAND]
    mov  eax, ecx
    and  eax, (WINDOW_PX - 1)
    mov  ecx, DWORD PTR [rbp + rax*4]

    dec  DWORD PTR [rsp + LOCAL_CHAIN]
    jnz  LZC_CHAIN_LOOP

LZC_CHAIN_DONE:

    ; ============================================================
    ; Bezpiecznik B: bestLen <= maxMatch
    ; (chroni odczyt next_px i semantyke)
    ; ============================================================
    cmp  r9d, r10d
    jbe  LZC_LEN_OK
    xor  r9d, r9d
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0
LZC_LEN_OK:

    ; ============================================================
    ; Bezpiecznik A/C: dla match bestLen>0 wymagaj:
    ;   bestOff != 0 i bestOff <= i
    ; inaczej literal
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
    xor  r9d, r9d
    mov  DWORD PTR [rsp + LOCAL_BESTOFF], 0

; ============================================================
; emit token
; ============================================================
LZC_EMIT_TOKEN:
    ; space check
    mov  rax, r15
    sub  rax, r12
    cmp  rax, TOKEN_SIZE
    jb   LZC_FAIL

    ; next_px
    test r9d, r9d
    jz   LZC_NEXT_LIT

    ; (bezpiecznie: bestLen <= maxMatch <= remaining-1, wiec i+bestLen < src_count)
    lea  rax, [r8 + r9]
    mov  eax, DWORD PTR [rsi + rax*4]
    jmp  LZC_NEXT_READY
LZC_NEXT_LIT:
    mov  eax, DWORD PTR [rsi + r8*4]
LZC_NEXT_READY:

    mov  edx, DWORD PTR [rsp + LOCAL_BESTOFF]

    lea  rcx, [rdi + r12]
    mov  DWORD PTR [rcx + 0], edx
    mov  DWORD PTR [rcx + 4], r9d
    mov  DWORD PTR [rcx + 8], eax
    add  r12, TOKEN_SIZE

    ; insert positions [i .. i+bestLen]
    mov  r10d, r9d
    xor  eax, eax

LZC_INSERT_LOOP:
    cmp  eax, r10d
    ja   LZC_INSERT_DONE

    mov  ecx, r8d
    add  ecx, eax                ; pos

    lea  rdx, [rcx + 1]
    cmp  rdx, r14
    jae  LZC_INSERT_DONE

    mov  r9d,  DWORD PTR [rsi + rcx*4]
    mov  r11d, DWORD PTR [rsi + rcx*4 + 4]
    rol  r11d, 5
    xor  r9d,  r11d
    and  r9d,  HASH_MASK

    mov  edx, ecx
    and  edx, (WINDOW_PX - 1)    ; slot

    mov  r11d, DWORD PTR [rbx + r9*4]
    mov  DWORD PTR [rbp + rdx*4], r11d
    mov  DWORD PTR [rbx + r9*4], ecx

    inc  eax
    jmp  LZC_INSERT_LOOP

LZC_INSERT_DONE:
    add  r8, r10
    inc  r8
    jmp  LZC_MAIN

; ============================================================
; last pixel literal
; ============================================================
LZC_EMIT_LITERAL:
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
    jmp  LZC_MAIN

; ============================================================
; literal-only mode
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
; exits
; ============================================================
LZC_FAIL:
    mov  QWORD PTR [r13], 0
    jmp  LZC_EXIT

LZC_DONE:
    mov  QWORD PTR [r13], r12

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
; ============================================================
PUBLIC lz77_rgba_decompress
lz77_rgba_decompress PROC
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15

    mov  rsi, rcx
    mov  r14, rdx
    mov  rdi, r8
    mov  r15, r9
    mov  r13, QWORD PTR [rsp + DECOMP_ARG_OUTLEN]

    xor  r12d, r12d
    xor  ebx,  ebx

LZD_LOOP:
    lea  rax, [rbx + TOKEN_SIZE]
    cmp  rax, r14
    ja   LZD_DONE

    mov  eax, DWORD PTR [rsi + rbx]          ; offset_px
    mov  ebp, DWORD PTR [rsi + rbx + 4]      ; length_px
    mov  edx, DWORD PTR [rsi + rbx + 8]      ; next_px
    add  rbx, TOKEN_SIZE

    test eax, eax
    jnz  LZD_MATCH
    test ebp, ebp
    jnz  LZD_MATCH

    cmp  r12, r15
    jae  LZD_FAIL

    mov  DWORD PTR [rdi + r12*4], edx
    inc  r12
    jmp  LZD_LOOP

LZD_MATCH:
    mov  rcx, r12
    add  rcx, rbp
    inc  rcx
    cmp  rcx, r15
    ja   LZD_FAIL

    test eax, eax
    jz   LZD_FAIL

    cmp  rax, r12
    ja   LZD_FAIL

    mov  r8, r12
    sub  r8, rax

    lea  r9,  [rdi + r8*4]
    lea  r10, [rdi + r12*4]

    mov  r11d, ebp
    shl  r11d, 2

    cmp  eax, 4
    jb   LZD_SCALAR_COPY

    xor  ecx, ecx
LZD_SSE2_LOOP:
    mov  eax, r11d
    sub  eax, ecx
    cmp  eax, 16
    jb   LZD_SSE2_TAIL

    movdqu xmm0, XMMWORD PTR [r9 + rcx]
    movdqu XMMWORD PTR [r10 + rcx], xmm0
    add  ecx, 16
    jmp  LZD_SSE2_LOOP

LZD_SSE2_TAIL:
    cmp  ecx, r11d
    jae  LZD_COPY_DONE
    mov  eax, DWORD PTR [r9 + rcx]
    mov  DWORD PTR [r10 + rcx], eax
    add  ecx, 4
    jmp  LZD_SSE2_TAIL

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
    add  r12, rbp
    mov  DWORD PTR [rdi + r12*4], edx
    inc  r12
    jmp  LZD_LOOP

LZD_FAIL:
    mov  QWORD PTR [r13], 0
    jmp  LZD_EXIT

LZD_DONE:
    mov  QWORD PTR [r13], r12

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