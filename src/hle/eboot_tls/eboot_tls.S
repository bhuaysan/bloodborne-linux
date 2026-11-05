// The reservation for the eboot's static TLS block (docs/design/m3-core-os.md, "TLS: one thread
// pointer for game and runtime"): 0x750 bytes, 16-aligned, that must end exactly at the thread
// pointer.
//
// glibc puts the executable's TLS block directly below the thread pointer, so the reservation has
// to be the last thing in the executable's TLS segment. A thread_local would land in .tbss in link
// order, before the TLS of any static library linked after it. The input section .tcommon instead
// comes after every .tbss input section in GNU ld's default linker script ("*(.tbss .tbss.* ...)
// *(.tcommon)"), whatever the link order. Nothing else in the executable uses .tcommon: it holds
// TLS common symbols, which neither C++ nor C with -fno-common (GCC's default) produces.
// check_eboot_tls verifies the placement at start-up and in every thread.

    .section .tcommon, "awT", @nobits
    .balign 16
bb_eboot_tls:
    .skip 0x750
    .size bb_eboot_tls, 0x750

    .text
    // std::uintptr_t bb_eboot_tls_block(): the start of the calling thread's reservation. Local
    // exec TLS, so this object only links into executables.
    .globl bb_eboot_tls_block
    .type bb_eboot_tls_block, @function
bb_eboot_tls_block:
    .cfi_startproc
    mov %fs:0, %rax
    lea bb_eboot_tls@tpoff(%rax), %rax
    ret
    .cfi_endproc
    .size bb_eboot_tls_block, . - bb_eboot_tls_block

    .section .note.GNU-stack, "", @progbits
