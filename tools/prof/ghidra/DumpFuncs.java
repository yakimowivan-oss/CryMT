// Dumps the function containing each RVA: entry, size, caller count, referenced strings, decompilation.
// Script args: <rvaListFile> <outFile> <maxDecompLines>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.LinkedHashSet;
import java.util.Set;

public class DumpFuncs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int maxLines = Integer.parseInt(a[2]);
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> seen = new LinkedHashSet<>();
        try (PrintWriter w = new PrintWriter(a[1], "UTF-8")) {
            for (String line : Files.readAllLines(Paths.get(a[0]))) {
                line = line.trim();
                if (line.isEmpty()) continue;
                Address addr = currentProgram.getImageBase().add(Long.parseLong(line.replace("0x", ""), 16));
                Function f = getFunctionContaining(addr);
                w.println("\n######## rva " + line + " -> " + (f == null ? "NO FUNCTION" : f.getName() + " @ " + f.getEntryPoint()));
                if (f == null || !seen.add(f)) continue;
                w.println("size=" + f.getBody().getNumAddresses() + " callers=" + f.getCallingFunctions(monitor).size()
                        + " callees=" + f.getCalledFunctions(monitor).size());
                Set<String> strings = new LinkedHashSet<>();
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    for (Reference r : ins.getReferencesFrom()) {
                        Data d = getDataAt(r.getToAddress());
                        if (d != null && d.hasStringValue()) strings.add(String.valueOf(d.getValue()));
                    }
                }
                w.println("strings=" + strings);
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    String[] c = res.getDecompiledFunction().getC().split("\n");
                    for (int i = 0; i < Math.min(c.length, maxLines); i++) w.println(c[i]);
                    if (c.length > maxLines) w.println("... (" + c.length + " lines total)");
                }
            }
        }
        dec.dispose();
    }
}
