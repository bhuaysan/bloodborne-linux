// shadPS4's guest TLS header. Our kernel gives every guest thread its TLS
// (src/hle/kernel/tls.cpp), and the imported libraries use none of upstream's TLS calls, so this
// is empty. ime_dialog_ui.cpp and libs.h include it.
#pragma once
