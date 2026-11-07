// Lists references to addresses or imported functions and decompiles each referencing function once.
// Script args: <targetsFile> <outFile> <maxDecompLines>; a target line is a hex VA or a symbol name.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class Xrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        int maxLines = Integer.parseInt(a[2]);
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        Set<Function> done = new HashSet<>();
        try (PrintWriter w = new PrintWriter(a[1], "UTF-8")) {
            for (String t : Files.readAllLines(Paths.get(a[0]))) {
                t = t.trim();
                if (t.isEmpty()) continue;
                List<Address> targets = new ArrayList<>();
                if (t.matches("(0x)?[0-9a-fA-F]+")) {
                    targets.add(toAddr(Long.parseLong(t.replace("0x", ""), 16)));
                } else {
                    SymbolIterator it = currentProgram.getSymbolTable().getSymbols(t);
                    while (it.hasNext()) {
                        Symbol s = it.next();
                        targets.add(s.getAddress());
                        for (Reference r : getReferencesTo(s.getAddress())) targets.add(r.getFromAddress());
                    }
                }
                w.println("\n######## target " + t + " -> " + targets);
                for (Address target : targets) {
                    for (Reference r : getReferencesTo(target)) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        w.println("ref " + r.getFromAddress() + " " + r.getReferenceType() + " in "
                                + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                        if (f == null || maxLines <= 0 || !done.add(f)) continue;
                        DecompileResults res = dec.decompileFunction(f, 60, monitor);
                        if (res == null || !res.decompileCompleted()) continue;
                        String[] c = res.getDecompiledFunction().getC().split("\n");
                        int printed = 0;
                        for (String line : c) {
                            if (line.trim().isEmpty() || line.matches("\\s+(undefined|float|int|longlong|char|uint|byte|bool|ulonglong|short|ushort)\\d*\\s*\\*?\\s*\\w+(\\s*\\[\\d+\\])?;")) continue;
                            w.println("    " + line);
                            if (++printed >= maxLines) { w.println("    ..."); break; }
                        }
                    }
                }
            }
        }
        dec.dispose();
    }
}
