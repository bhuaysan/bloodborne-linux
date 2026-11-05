// The common entry of all function stubs (docs/design/runtime.md, "Function stubs"). A thunk
// jumps here with r11 = its StubInfo* and the caller's argument registers untouched.

    .text
    .globl bb_hle_stub_entry
    .type bb_hle_stub_entry, @function
bb_hle_stub_entry:
    .cfi_startproc
    // On entry rsp = 8 mod 16 (the caller's return address). 48 bytes of arguments plus 8 of
    // padding make it 16-byte aligned for the call.
    sub $56, %rsp
    .cfi_adjust_cfa_offset 56
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    mov %r11, %rdi
    mov %rsp, %rsi
    // bb::hle::on_stub_call(const StubInfo*, const std::uint64_t*)
    call _ZN2bb3hle12on_stub_callEPKNS0_8StubInfoEPKm@PLT
    add $56, %rsp
    .cfi_adjust_cfa_offset -56
    xor %eax, %eax
    pxor %xmm0, %xmm0
    ret
    .cfi_endproc
    .size bb_hle_stub_entry, . - bb_hle_stub_entry

    .section .note.GNU-stack,"",@progbits
