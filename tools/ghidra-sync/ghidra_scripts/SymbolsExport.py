# Writes the function names and signatures we set in Ghidra to a JSON file for ghidrasync.py.
# Run by tools/ghidra-sync/ghidrasync.py, not by hand. Args: <out.json>
# @runtime PyGhidra
import json

from ghidra.program.model.symbol import SourceType

USER = SourceType.USER_DEFINED
prog = currentProgram  # noqa: F821 (GhidraScript globals)
base = prog.getImageBase()
rows = []
for func in prog.getFunctionManager().getFunctions(True):
    if func.isThunk() or func.isExternal():
        continue
    user_name = func.getSymbol().getSource() == USER
    user_sig = func.getSignatureSource() == USER
    if user_name or user_sig:
        rows.append({
            "address": int(func.getEntryPoint().subtract(base)),
            "name": str(func.getName()) if user_name else "",
            "signature": str(func.getPrototypeString(True, False)) if user_sig else "",
        })
with open(getScriptArgs()[0], "w") as out:  # noqa: F821
    json.dump({"functions": rows}, out)
