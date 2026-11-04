# Applies the function names and signatures from a JSON file written by ghidrasync.py.
# Run by tools/ghidra-sync/ghidrasync.py, not by hand. Args: <in.json> <done.json>
# @runtime PyGhidra
import json

from ghidra.app.cmd.function import ApplyFunctionSignatureCmd, FunctionRenameOption
from ghidra.app.util.cparser.C import CParserUtils
from ghidra.app.util.headless import HeadlessScript
from ghidra.program.model.symbol import SourceType

USER = SourceType.USER_DEFINED
prog = currentProgram  # noqa: F821 (GhidraScript globals)
args = getScriptArgs()  # noqa: F821
base = prog.getImageBase()
functions = prog.getFunctionManager()


def import_functions():
    with open(args[0]) as src:
        rows = json.load(src)["functions"]
    for row in rows:
        addr = base.add(row["address"])
        func = functions.getFunctionAt(addr) or createFunction(addr, None)  # noqa: F821
        if func is None:
            raise RuntimeError(f"cannot create a function at {addr}")
        if row["signature"]:
            sig = CParserUtils.parseSignature(None, prog, row["signature"], True)
            cmd = ApplyFunctionSignatureCmd(addr, sig, USER, True, FunctionRenameOption.NO_CHANGE)
            if not cmd.applyTo(prog, monitor):  # noqa: F821
                raise RuntimeError(f"{addr}: {cmd.getStatusMsg()}")
        if row["name"]:
            func.setName(row["name"], USER)
    with open(args[1], "w") as out:
        json.dump({"functions": len(rows)}, out)


try:
    import_functions()
except Exception:
    # analyzeHeadless logs a failed Python script and goes on with the next one, then saves the
    # program. Stop instead: no later script runs and the program is not saved, so the types
    # imported before this script are dropped as well and the project stays as it was.
    setHeadlessContinuationOption(HeadlessScript.HeadlessContinuationOption.ABORT)  # noqa: F821
    prog.setTemporary(True)
    raise
