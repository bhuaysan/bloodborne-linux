/* Ghidra category /bb/text: written by tools/ghidra-sync, see symbols/README.md */

#pragma pack(push, 1)

struct CharRange;

struct CharRange {
    /* 0x0000 */ char *begin;
    /* 0x0008 */ char *end;
};
_Static_assert(sizeof(struct CharRange) == 0x10, "CharRange");

#pragma pack(pop)
