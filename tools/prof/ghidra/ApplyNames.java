// Applies reversed names, comments and structures to the current program: ApplyNames.java <file> [<file>...]
// Lines (module = program name such as CryRenderD3D10.dll, or * for every program; # starts a comment):
//   F <module> <hex address> <Name or Class::Name> [comment]  function (created when missing), renamed
//   V <module> <hex address> <Class::vfN>                     vtable slot name, only for still unnamed functions
//   L <module> <hex address> <name> [comment]                 data label
//   S <module> <struct> <hex size> [comment]                  structure in /ce2
//   M <module> <struct> <hex offset> <type> <name> [comment]  member: u8 u16 u32 u64 i16 i32 f32 ptr f32xN u8xN
//@category CryMT
import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;

import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;

public class ApplyNames extends GhidraScript
{
    private final Map<String, StructureDataType> structs = new LinkedHashMap<>();

    private DataType type(String t)
    {
        int x = t.indexOf('x');
        if (x > 0 && !t.equals("ptr"))
            return new ArrayDataType(type(t.substring(0, x)), Integer.parseInt(t.substring(x + 1)), -1);
        switch (t)
        {
        case "u8": return ByteDataType.dataType;
        case "u16": return WordDataType.dataType;
        case "i16": return ShortDataType.dataType;
        case "u32": return DWordDataType.dataType;
        case "i32": return IntegerDataType.dataType;
        case "u64": return QWordDataType.dataType;
        case "f32": return FloatDataType.dataType;
        default: return PointerDataType.dataType;
        }
    }

    private void name(Function f, String full) throws Exception
    {
        String[] parts = full.split("::");
        Namespace ns = currentProgram.getGlobalNamespace();
        if (parts.length > 1)
            ns = NamespaceUtils.createNamespaceHierarchy(String.join("::", Arrays.copyOf(parts, parts.length - 1)), null,
                                                         currentProgram, SourceType.USER_DEFINED);
        f.setParentNamespace(ns);
        f.setName(parts[parts.length - 1], SourceType.USER_DEFINED);
    }

    @Override
    public void run() throws Exception
    {
        String prog = currentProgram.getName();
        int functions = 0, slots = 0, labels = 0, members = 0, failed = 0;
        for (String path : getScriptArgs())
            for (String raw : Files.readAllLines(new File(path).toPath()))
            {
                String line = raw.trim();
                if (line.isEmpty() || line.startsWith("#"))
                    continue;
                String[] p = line.split("\\s+", line.startsWith("M") ? 7 : 5);
                if (p.length < 4 || !(p[1].equals("*") || p[1].equalsIgnoreCase(prog)))
                    continue;
                try
                {
                    switch (p[0])
                    {
                    case "F":
                    case "V":
                    {
                        Address a = toAddr(Long.parseLong(p[2], 16));
                        Function f = getFunctionAt(a);
                        if (f == null)
                            f = createFunction(a, null);
                        if (f == null)
                        {
                            failed++;
                            break;
                        }
                        if (p[0].equals("V"))
                        {
                            if (f.getSymbol().getSource() != SourceType.DEFAULT)
                                break;
                            name(f, p[3]);
                            slots++;
                            break;
                        }
                        name(f, p[3]);
                        if (p.length > 4)
                            setPlateComment(a, p[4]);
                        functions++;
                        break;
                    }
                    case "L":
                    {
                        Address a = toAddr(Long.parseLong(p[2], 16));
                        createLabel(a, p[3], true);
                        if (p.length > 4)
                            setEOLComment(a, p[4]);
                        labels++;
                        break;
                    }
                    case "S":
                    {
                        StructureDataType s = new StructureDataType(new CategoryPath("/ce2"), p[2], Integer.parseInt(p[3], 16));
                        if (p.length > 4)
                            s.setDescription(p[4]);
                        structs.put(p[2], s);
                        break;
                    }
                    case "M":
                    {
                        StructureDataType s = structs.get(p[2]);
                        DataType t = type(p[4]);
                        s.replaceAtOffset(Integer.parseInt(p[3], 16), t, t.getLength(), p[5], p.length > 6 ? p[6] : null);
                        members++;
                        break;
                    }
                    }
                }
                catch (Exception e)
                {
                    failed++;
                    println("failed: " + line + " (" + e.getMessage() + ")");
                }
            }
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        for (StructureDataType s : structs.values())
            dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER);
        println(prog + ": functions " + functions + ", vtable slots " + slots + ", labels " + labels + ", structures " +
                structs.size() + " (" + members + " members), failed " + failed);
    }
}
