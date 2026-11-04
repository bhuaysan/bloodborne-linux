# Writes our data types (category /bb/<subsystem>) to a JSON file for ghidrasync.py, which turns
# it into symbols/types/<subsystem>.h. Run by ghidrasync.py, not by hand. Args: <out.json>
# @runtime PyGhidra
import json

from ghidra.program.model.data import (Array, BuiltInDataType, CategoryPath, Enum, Pointer,
                                       Structure, TypeDef, Union)

ROOT = CategoryPath("/bb")
dtm = currentProgram.getDataTypeManager()  # noqa: F821 (GhidraScript globals)


def fail(where, why):
    raise RuntimeError(f"{where}: {why}")


def ref(dt, user):
    """A reference to a data type: a builtin or one of ours, maybe behind pointers and arrays."""
    if isinstance(dt, Pointer):
        if dt.getLength() != dtm.getDataOrganization().getPointerSize() \
                or isinstance(dt.getDataType(), Array):
            # Ghidra's C parser reads a pointer to an array as an array
            fail(user.getPathName(), f"{dt.getName()}: pointers to arrays and pointers of "
                 "another size are not supported")
        return {"pointer": ref(dt.getDataType(), user) if dt.getDataType() else None}
    if isinstance(dt, Array):
        return {"array": ref(dt.getDataType(), user), "count": dt.getNumElements()}
    if isinstance(dt, BuiltInDataType):
        return {"name": str(dt.getName())}
    path = dt.getCategoryPath()
    if path.getParent() != ROOT or not isinstance(dt, (Structure, Union, Enum, TypeDef)):
        fail(user.getPathName(), f"refers to {dt.getPathName()}, which is not a struct, union, "
             "enum or typedef in /bb/<subsystem>")
    tag = "struct" if isinstance(dt, Structure) else "union" if isinstance(dt, Union) \
        else "enum" if isinstance(dt, Enum) else ""
    return {"name": str(dt.getName()), "tag": tag, "subsystem": str(path.getName())}


def describe(dt):
    if isinstance(dt, (Pointer, Array)):
        return None  # derived types, written where they are used
    out = {"name": str(dt.getName())}
    if isinstance(dt, Enum):
        out.update(kind="enum", size=dt.getLength(),
                   values=[[str(n), int(dt.getValue(n))] for n in dt.getNames()])
    elif isinstance(dt, TypeDef):
        out.update(kind="typedef", type=ref(dt.getDataType(), dt))
    elif isinstance(dt, (Structure, Union)):
        fields = []
        for comp in dt.getDefinedComponents():
            if comp.isBitFieldComponent() or comp.getLength() != comp.getDataType().getLength():
                fail(dt.getPathName(), f"field at {comp.getOffset():#x}: bit fields and fields "
                     "of variable or zero length are not supported")
            name = str(comp.getFieldName() or comp.getDefaultFieldName())
            if name.startswith("_pad_"):
                fail(dt.getPathName(), f"field name {name}: the prefix _pad_ is reserved for gaps")
            fields.append({"offset": comp.getOffset(), "size": comp.getLength(), "name": name,
                           "type": ref(comp.getDataType(), dt)})
        out.update(kind="struct" if isinstance(dt, Structure) else "union",
                   size=dt.getLength(), fields=fields)
    else:
        fail(dt.getPathName(), f"{dt.getClass().getSimpleName()} types are not supported")
    return out


subsystems = {}
root = dtm.getCategory(ROOT)
for sub in root.getCategories() if root else []:
    if sub.getCategories():
        fail(sub.getCategoryPathName(), "nested categories are not supported")
    types = [d for d in map(describe, sub.getDataTypes()) if d]
    if types:
        subsystems[str(sub.getName())] = types
if root and root.getDataTypes():
    fail("/bb", "types directly in /bb need a subsystem category")
with open(getScriptArgs()[0], "w") as out:  # noqa: F821
    json.dump({"types": subsystems}, out)
