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

// The common entry of all trace thunks (debug builds, hle.cpp). A thunk jumps here with r11 = its
// TraceInfo*, whose first member is the implementation. Every argument register is saved around
// the log call: the six integer ones, rax (al counts the vector registers of a variadic call)
// and xmm0-xmm7. Then it jumps to the implementation with the caller's return address on the
// stack, so the implementation returns straight to the caller.
    .globl bb_hle_trace_entry
    .type bb_hle_trace_entry, @function
bb_hle_trace_entry:
    .cfi_startproc
    // 6 integer registers, rax and r11 (64 bytes), 8 xmm registers (128 bytes), and 8 bytes of
    // padding: 200 bytes take rsp from 8 mod 16 to 0 mod 16.
    sub $200, %rsp
    .cfi_adjust_cfa_offset 200
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    mov %rax, 48(%rsp)
    mov %r11, 56(%rsp)
    movdqu %xmm0, 64(%rsp)
    movdqu %xmm1, 80(%rsp)
    movdqu %xmm2, 96(%rsp)
    movdqu %xmm3, 112(%rsp)
    movdqu %xmm4, 128(%rsp)
    movdqu %xmm5, 144(%rsp)
    movdqu %xmm6, 160(%rsp)
    movdqu %xmm7, 176(%rsp)
    mov %r11, %rdi
    mov %rsp, %rsi
    // bb::hle::on_traced_call(const void*, const std::uint64_t*)
    call _ZN2bb3hle14on_traced_callEPKvPKm@PLT
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    mov 24(%rsp), %rcx
    mov 32(%rsp), %r8
    mov 40(%rsp), %r9
    mov 48(%rsp), %rax
    mov 56(%rsp), %r11
    movdqu 64(%rsp), %xmm0
    movdqu 80(%rsp), %xmm1
    movdqu 96(%rsp), %xmm2
    movdqu 112(%rsp), %xmm3
    movdqu 128(%rsp), %xmm4
    movdqu 144(%rsp), %xmm5
    movdqu 160(%rsp), %xmm6
    movdqu 176(%rsp), %xmm7
    add $200, %rsp
    .cfi_adjust_cfa_offset -200
    jmp *(%r11)
    .cfi_endproc
    .size bb_hle_trace_entry, . - bb_hle_trace_entry

    .section .note.GNU-stack,"",@progbits
