// Marks the imports found by tools/noreturn/noreturn.py as non-returning, before auto-analysis.
//
// Run it as a headless pre-script with the tool's output for the same file:
//   analyzeHeadless <project dir> <project> -import <elf2std output>
//       -scriptPath tools/noreturn/ghidra_scripts -preScript MarkNoReturn.java <list file>
// Each line of the list is "<PLT stub vaddr hex> <NID#library#module>". The script creates a
// function at every stub and marks it non-returning, and marks the external functions with those
// names too (the stubs become thunks of them during analysis).
//@category bloodborne-linux

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.Set;

public class MarkNoReturn extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("usage: MarkNoReturn.java <list file>");
        }
        Set<String> symbols = new HashSet<>();
        int stubs = 0;
        for (String line : Files.readAllLines(Paths.get(args[0]))) {
            line = line.strip();
            if (line.isEmpty()) {
                continue;
            }
            String[] parts = line.split(" ");
            if (parts.length != 2) {
                throw new IllegalArgumentException("bad line in " + args[0] + ": " + line);
            }
            Address stub = toAddr(Long.parseUnsignedLong(parts[0], 16));
            Function function = getFunctionAt(stub);
            if (function == null) {
                disassemble(stub);
                function = createFunction(stub, null);
            }
            if (function == null) {
                throw new IllegalStateException("cannot create a function at " + stub);
            }
            function.setNoReturn(true);
            symbols.add(parts[1]);
            stubs++;
        }
        int externals = 0;
        for (Function function : currentProgram.getFunctionManager().getExternalFunctions()) {
            if (symbols.contains(function.getName())) {
                function.setNoReturn(true);
                externals++;
            }
        }
        println("MarkNoReturn: " + stubs + " stubs, " + externals + " external functions, "
                + symbols.size() + " imports");
    }
}
