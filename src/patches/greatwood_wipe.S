/* Exact original DLL only. Called at RVA 0x4b5b9, after the host's
 * spectator selection has found no surviving target. See analysis notes.
 * RDI is the original root object. No game-executable absolute addresses.
 * Flags are dead at this call site (next consumer is CMP at 0x4b5d9).
 * All live registers are preserved; RAX implements the displaced MOV.
 */
.intel_syntax noprefix
.text
.global greatwood_wipe
greatwood_wipe:
.ifdef GUEST_RELOAD
    push rdi
    sub rsp, 0xd0
.else
    sub rsp, 0xd8
.endif
    mov [rsp+32], rcx
    mov [rsp+40], rdx
    mov [rsp+48], r8
    mov [rsp+56], r9
    mov [rsp+64], r10
    mov [rsp+72], r11
    movdqu [rsp+80], xmm0
    movdqu [rsp+96], xmm1
    movdqu [rsp+112], xmm2
    movdqu [rsp+128], xmm3
    movdqu [rsp+144], xmm4
    movdqu [rsp+160], xmm5
.ifdef GUEST_RELOAD
    /* Guest call site has R15=root, RDI=world wrapper, RBX=world manager. */
    mov [rsp+200], rax
    mov rdi, r15
.endif

    mov rax, [rdi+0xb0]
    test rax, rax
    jz done
    mov eax, [rax]
    and eax, 7
.ifdef GUEST_RELOAD
    cmp eax, 1
.else
    cmp eax, 3
.endif
    jne done

    mov rax, [rdi+0x60]
    test rax, rax
    jz done
    mov rax, [rax]
    test rax, rax
    jz done
    mov rax, [rax]
    test rax, rax
    jz done
    mov [rsp+176], rax
    /* Match only a known Greatwood entity/name in the original HUD pairs. */
    .irp off, 0x1af8, 0x1afc, 0x1b90, 0x1b94, 0x1c28, 0x1c2c
        cmp DWORD PTR [rax+\off], 3100800
        je matched
        cmp DWORD PTR [rax+\off], 905180
        je matched
    .endr
    jmp done
matched:
    /* Require the original reload controller before touching encounter state. */
    mov rax, [rdi+0x10]
    test rax, rax
    jz done
    mov rax, [rax+0x28]
    test rax, rax
    jz done
    mov rax, [rax]
    test rax, rax
    jz done
    cmp DWORD PTR [rax+0x65c], 0
    jne done
    mov rax, [rdi+8]
    test rax, rax
    jz done
    cmp QWORD PTR [rax+0x40], 0
    je done
    mov rax, [rdi+0x48]
    test rax, rax
    jz done
    mov rcx, [rax+0x20]
    test rcx, rcx
    jz done
    mov rcx, [rcx]
    test rcx, rcx
    jz done
    cmp BYTE PTR [rcx+0x228], 0
    je done
    mov [rsp+184], rcx
    mov rax, [rax+0x40]
    test rax, rax
    jz done
    mov edx, 31
    xor r8d, r8d
    xor r9d, r9d
    call rax
    test eax, eax
    js done
    mov eax, eax
    imul rdx, rax, 0xa8
    mov rcx, [rsp+184]
    mov r8, [rcx+0x40]
    test r8, r8
    jz done
    add r8, rdx
    mov rax, [r8]
    test rax, rax
    jz done
    /* Permanent defeat flag 13100800: never resurrect a completed boss. */
    test DWORD PTR [rax+100], 0x80000000
    jnz done
    mov r8, [r8+0x50]
    test r8, r8
    jz done
    mov [rsp+192], r8
    xor r9d, r9d
    mov rax, [rcx+0x218]
    test rax, rax
    jz banks_ready
    mov rax, [rax+0x18]
    test rax, rax
    jz done
    add rax, rdx
    mov r10, [rax]
    test r10, r10
    jz done
    test DWORD PTR [r10+100], 0x80000000
    jnz done
    mov r9, [rax+0x50]
    test r9, r9
    jz done
banks_ready:
    /* Only transient 13105800..13105899; retain 13100801/13100820.
     * Words are MSB-first, as in original routines 0x4f3b0/0x88f90.
     * Validate both banks before changing either (they may alias).
     */
    .irp reg, r8, r9
        test \reg, \reg
        jz skip_\reg
        mov DWORD PTR [\reg+100], 0
        mov DWORD PTR [\reg+104], 0
        mov DWORD PTR [\reg+108], 0
        and DWORD PTR [\reg+112], 0x0fffffff
    skip_\reg:
    .endr
    mov rax, [rsp+176]
    .irp off, 0x1af8, 0x1b90, 0x1c28
        cmp DWORD PTR [rax+\off], 3100800
        je clear_\off
        cmp DWORD PTR [rax+\off], 905180
        je clear_\off
        cmp DWORD PTR [rax+\off+4], 3100800
        je clear_\off
        cmp DWORD PTR [rax+\off+4], 905180
        jne skip_\off
    clear_\off:
        mov QWORD PTR [rax+\off], -1
    skip_\off:
    .endr
    /* Reuse the original host rest/reload handler, including cache invalidation
     * and message 0x14 to peers. The previous patch only cleared flag words;
     * that does not reconstruct NPC parts or restart encounter script instances.
     * 0x4e070 reads only hook_object+0x28; provide that field in our stack frame.
     */
.ifndef GUEST_RELOAD
    mov [rsp+192], rdi
    lea rcx, [rsp+152]
    call original_reload
.endif
done:
    mov rcx, [rsp+32]
    mov rdx, [rsp+40]
    mov r8, [rsp+48]
    mov r9, [rsp+56]
    mov r10, [rsp+64]
    mov r11, [rsp+72]
    movdqu xmm0, [rsp+80]
    movdqu xmm1, [rsp+96]
    movdqu xmm2, [rsp+112]
    movdqu xmm3, [rsp+128]
    movdqu xmm4, [rsp+144]
    movdqu xmm5, [rsp+160]
.ifdef GUEST_RELOAD
    mov rax, [rsp+200]
    /* Preserve the validated host's original reload even if our guards fail. */
    mov DWORD PTR [rbx+0x65c], 1
    add rsp, 0xd0
    pop rdi
.else
    mov rax, [rdi+0xb0]
    add rsp, 0xd8
.endif
    ret
