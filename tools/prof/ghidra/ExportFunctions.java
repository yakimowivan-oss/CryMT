// Writes "<hex entry> <hex size> <name>" for every function of the current program: ExportFunctions.java <out dir>
// (file <out dir>\<program>.funcs.txt), for symbolizing sampler stacks.
//@category CryMT
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript
{
    @Override
    public void run() throws Exception
    {
        File out = new File(getScriptArgs()[0], currentProgram.getName() + ".funcs.txt");
        int n = 0;
        try (PrintWriter w = new PrintWriter(out))
        {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            {
                w.printf("%x %x %s%n", f.getEntryPoint().getOffset(), f.getBody().getNumAddresses(), f.getName(true));
                n++;
            }
        }
        println(currentProgram.getName() + ": " + n + " functions");
    }
}
