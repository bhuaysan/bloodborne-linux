# Parses the headers in symbols/types/ (as prepared by ghidrasync.py) and puts their types into
# /bb/<subsystem>, replacing types of the same name there.
# Run by ghidrasync.py, not by hand. Args: <in.json> <done.json>
# @runtime PyGhidra
import json

from ghidra.app.util.cparser.C import CParser
from ghidra.app.util.headless import HeadlessScript
from ghidra.program.model.data import (CategoryPath, DataTypeConflictHandler, Enum, EnumDataType,
                                       StandAloneDataTypeManager, Structure, Union)

prog = currentProgram  # noqa: F821 (GhidraScript globals)
dtm = prog.getDataTypeManager()
args = getScriptArgs()  # noqa: F821


def import_headers():
    with open(args[0]) as src:
        headers = json.load(src)["headers"]

    # Parse into a scratch manager first: the parser puts every type at the root category, which
    # could clash with types of the same name in the program.
    scratch = StandAloneDataTypeManager("bb", dtm.getDataOrganization())
    tx = scratch.startTransaction("parse headers")
    parser = CParser(scratch, True, None)
    ours = []
    for header in headers:
        where = f"types/{header['subsystem']}.h"
        parser.setParseFileName(where)
        parser.parse(header["text"])
        if not parser.didParseSucceed():
            raise RuntimeError(f"{where}: {parser.getParseMessages()}")
        new = [dt for dt in (*parser.getComposites().values(), *parser.getEnums().values(),
                             *parser.getTypes().values()) if dt.getCategoryPath().isRoot()]
        sizes = header["sizes"]
        for dt in new:
            dt.setCategoryPath(CategoryPath(f"/bb/{header['subsystem']}"))
        # The parser makes every enum 4 bytes with 32-bit values. Rebuild them first, so the
        # structs that use them repack.
        for dt in new:
            name = str(dt.getName())
            if isinstance(dt, Enum) and name in header["enums"]:
                enum = EnumDataType(dt.getCategoryPath(), name, sizes.get(name, 4), scratch)
                for value_name, value in header["enums"][name]:
                    enum.add(value_name, value)
                dt.replaceWith(enum)
        # The headers spell out every gap as a _pad_ field under pack(1). Ghidra keeps such a
        # layout as an unpacked struct with undefined bytes in the gaps.
        for dt in new:
            if isinstance(dt, (Structure, Union)):
                dt.setPackingEnabled(False)
                for comp in reversed(dt.getDefinedComponents()):
                    if str(comp.getFieldName() or "").startswith("_pad_"):
                        dt.clearComponent(comp.getOrdinal())
            size = sizes.get(str(dt.getName()))
            if size is not None and size != dt.getLength():
                raise RuntimeError(f"{where}: {dt.getName()} has {dt.getLength()} bytes, "
                                   f"the header asserts {size}")
        ours += new
    scratch.endTransaction(tx, True)

    for dt in ours:
        dtm.resolve(dt, DataTypeConflictHandler.REPLACE_HANDLER)
    scratch.close()
    with open(args[1], "w") as out:
        json.dump({"types": len(ours)}, out)


try:
    import_headers()
except Exception:
    # analyzeHeadless logs a failed Python script and goes on with the next one, then saves the
    # program. Stop instead: the scripts after this one (the functions) do not run, and the
    # program is not saved, so the project stays as it was.
    setHeadlessContinuationOption(HeadlessScript.HeadlessContinuationOption.ABORT)  # noqa: F821
    prog.setTemporary(True)
    raise
