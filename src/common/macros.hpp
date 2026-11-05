// Preprocessor helpers.
#pragma once

// Pastes `a` and `b` after expanding them, e.g. BB_CONCAT(name_, __COUNTER__).
#define BB_CONCAT_(a, b) a##b
#define BB_CONCAT(a, b) BB_CONCAT_(a, b)
