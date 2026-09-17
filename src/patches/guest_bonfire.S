/* The original guest hook subtracts 1000 from the received last-bonfire
 * player spawn ID to look up the bonfire object in a parameter table. It
 * incorrectly writes that object ID back to GameMan.LastBonfire (+0xacc).
 * Keep the lookup unchanged; convert back ONLY at the successful write.
 * Original RVA 0x4d125: mov [rax+0xacc], ebp. All live registers preserved.
 */
.intel_syntax noprefix
.text
.global guest_bonfire
guest_bonfire:
    sub rsp, 0x18
    mov [rsp], rdx
    lea edx, [rbp+1000]
    mov [rax+0xacc], edx
    mov rdx, [rsp]
    add rsp, 0x18
    ret
