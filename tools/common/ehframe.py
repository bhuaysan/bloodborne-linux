"""Reader for .eh_frame_hdr, found through PT_GNU_EH_FRAME.

The header holds a pointer to .eh_frame and a search table of (initial location, FDE address)
pairs sorted by location. Its layout is in the LSB Core specification (".eh_frame_hdr").

Accepted pointer encodings (DW_EH_PE_*), on purpose the same for every pointer field
(eh_frame_ptr, the search table, and the FDE pointers that tools decode with read_pointer):
- formats absptr (8 bytes), udata4, udata8, sdata4 and sdata8. LEB128 and 2-byte formats are not
  used by x86-64 linkers.
- applications absptr, pcrel (relative to the field itself) and datarel (relative to the start
  of .eh_frame_hdr; only inside the header, where that base is defined). indirect, textrel and
  funcrel are rejected.
fde_count is a count, not a pointer, so it takes the formats above with application absptr.
fde_count or table encoding omit (0xff) means the header has no search table. Every pinned file
uses version 1, pcrel|sdata4, udata4, datarel|sdata4.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from ps4elf import PT_GNU_EH_FRAME, Phdr, file_offset, need

EH_PE_FORMATS = {0x00: "<Q", 0x03: "<I", 0x04: "<Q", 0x0B: "<i", 0x0C: "<q"}
EH_PE_ABSPTR, EH_PE_PCREL, EH_PE_DATAREL = 0x00, 0x10, 0x30
EH_PE_OMIT = 0xFF

ADDRESS_MASK = 0xFFFFFFFFFFFFFFFF


def pointer_format(enc: int, what: str, datarel: bool = False) -> str:
    """struct format of a pointer with encoding enc; rejects encodings this module doesn't read.

    datarel: whether the field has a data base (only fields inside .eh_frame_hdr do)."""
    fmt = EH_PE_FORMATS.get(enc & 0x0F)
    apps = (EH_PE_ABSPTR, EH_PE_PCREL) + ((EH_PE_DATAREL,) if datarel else ())
    need(fmt is not None and enc & 0xF0 in apps, f"unsupported {what} encoding {enc:#x}")
    return fmt


def read_pointer(data: bytes, offset: int, vaddr: int, enc: int, data_base: int = 0) -> int:
    """Pointer at file offset offset (vaddr in memory), encoded as enc.

    The encoding must have passed pointer_format. data_base is the datarel base."""
    (value,) = struct.unpack_from(EH_PE_FORMATS[enc & 0x0F], data, offset)
    base = {EH_PE_ABSPTR: 0, EH_PE_PCREL: vaddr, EH_PE_DATAREL: data_base}[enc & 0xF0]
    return (base + value) & ADDRESS_MASK


@dataclass
class EhFrameHdr:
    phdr: Phdr  # PT_GNU_EH_FRAME
    load: Phdr  # the PT_LOAD that holds it
    eh_frame: int  # vaddr of .eh_frame
    table: list[tuple[int, int]]  # (initial location, FDE vaddr) of the search table


def read_eh_frame_hdr(data: bytes, phdrs: list[Phdr], loads: list[Phdr]) -> EhFrameHdr | None:
    """The .eh_frame_hdr of an ELF, or None without PT_GNU_EH_FRAME.

    Every field must lie inside the PT_GNU_EH_FRAME file size, which must lie in one PT_LOAD."""
    hdrs = [ph for ph in phdrs if ph.p_type == PT_GNU_EH_FRAME]
    if not hdrs:
        return None
    need(len(hdrs) == 1, "more than one PT_GNU_EH_FRAME")
    (phdr,) = hdrs
    need(phdr.p_filesz >= 4, "PT_GNU_EH_FRAME is too small")
    hdr, load = file_offset(data, loads, phdr.p_vaddr, phdr.p_filesz)
    version, ptr_enc, count_enc, table_enc = data[hdr: hdr + 4]
    need(version == 1, f"unsupported .eh_frame_hdr version {version}")
    pos = 4

    def field(fmt: str) -> int:
        nonlocal pos
        need(pos + struct.calcsize(fmt) <= phdr.p_filesz, "PT_GNU_EH_FRAME is too small")
        start = pos
        pos += struct.calcsize(fmt)
        return start

    def pointer(at: int, enc: int) -> int:
        return read_pointer(data, hdr + at, phdr.p_vaddr + at, enc, phdr.p_vaddr)

    eh_frame = pointer(field(pointer_format(ptr_enc, "eh_frame_ptr", datarel=True)), ptr_enc)
    table: list[tuple[int, int]] = []
    if count_enc != EH_PE_OMIT and table_enc != EH_PE_OMIT:
        count_fmt = EH_PE_FORMATS.get(count_enc)  # application absptr only
        need(count_fmt is not None, f"unsupported fde_count encoding {count_enc:#x}")
        (count,) = struct.unpack_from(count_fmt, data, hdr + field(count_fmt))
        need(count >= 0, f"negative fde_count {count}")
        entry_fmt = pointer_format(table_enc, "search table", datarel=True)
        need(pos + 2 * struct.calcsize(entry_fmt) * count <= phdr.p_filesz,
             "PT_GNU_EH_FRAME is too small")
        for _ in range(count):
            loc_at = field(entry_fmt)
            fde_at = field(entry_fmt)
            table.append((pointer(loc_at, table_enc), pointer(fde_at, table_enc)))
    return EhFrameHdr(phdr, load, eh_frame, table)
