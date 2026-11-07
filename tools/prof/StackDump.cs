// Prints the call stack of every thread in a running process.
// Usage: StackDump <pid> <outFile>
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

static class StackDump
{
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenThread(int a, bool i, int tid);
    [DllImport("kernel32")] static extern int SuspendThread(IntPtr h);
    [DllImport("kernel32")] static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("kernel32")] static extern IntPtr GetModuleHandle(string n);
    [DllImport("dbghelp", SetLastError = true)] static extern bool SymInitialize(IntPtr p, string search, bool invade);
    [DllImport("dbghelp", SetLastError = true)]
    static extern bool StackWalk64(int machine, IntPtr p, IntPtr t, IntPtr frame, IntPtr ctx,
        IntPtr readMem, IntPtr funcTable, IntPtr modBase, IntPtr translate);

    class Mod { public string Name; public ulong Base, End; }

    static int Main(string[] a)
    {
        int pid = int.Parse(a[0]);
        var proc = Process.GetProcessById(pid);
        var mods = new List<Mod>();
        foreach (ProcessModule pm in proc.Modules)
        {
            ulong b = (ulong)pm.BaseAddress.ToInt64();
            mods.Add(new Mod { Name = pm.ModuleName, Base = b, End = b + (ulong)pm.ModuleMemorySize });
        }
        Func<ulong, string> name = addr =>
        {
            foreach (var m in mods) if (addr >= m.Base && addr < m.End) return m.Name + "+0x" + (addr - m.Base).ToString("X");
            return "0x" + addr.ToString("X");
        };
        IntPtr hProc = OpenProcess(0x1F0FFF, false, pid);
        SymInitialize(hProc, null, true);
        IntPtr dbg = GetModuleHandle("dbghelp.dll");
        IntPtr ftab = GetProcAddress(dbg, "SymFunctionTableAccess64"), mbase = GetProcAddress(dbg, "SymGetModuleBase64");
        IntPtr raw = Marshal.AllocHGlobal(1232 + 32);
        IntPtr ctx = new IntPtr((raw.ToInt64() + 15) & ~15L);
        IntPtr frame = Marshal.AllocHGlobal(1024);
        using (var w = new StreamWriter(a[1]))
        {
            foreach (ProcessThread t in proc.Threads)
            {
                IntPtr hT = OpenThread(0x0002 | 0x0008 | 0x0040, false, t.Id);
                if (hT == IntPtr.Zero) continue;
                var frames = new List<string>();
                SuspendThread(hT);
                try
                {
                    for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(ctx, i, 0);
                    Marshal.WriteInt32(ctx, 0x30, 0x100003);
                    if (GetThreadContext(hT, ctx))
                    {
                        for (int i = 0; i < 1024; i += 8) Marshal.WriteInt64(frame, i, 0);
                        Marshal.WriteInt64(frame, 0, Marshal.ReadInt64(ctx, 0xF8));
                        Marshal.WriteInt32(frame, 12, 3);
                        Marshal.WriteInt64(frame, 32, Marshal.ReadInt64(ctx, 0xA0));
                        Marshal.WriteInt32(frame, 44, 3);
                        Marshal.WriteInt64(frame, 48, Marshal.ReadInt64(ctx, 0x98));
                        Marshal.WriteInt32(frame, 60, 3);
                        for (int d = 0; d < 48; d++)
                        {
                            if (!StackWalk64(0x8664, hProc, hT, frame, ctx, IntPtr.Zero, ftab, mbase, IntPtr.Zero)) break;
                            ulong pc = (ulong)Marshal.ReadInt64(frame, 0);
                            if (pc == 0) break;
                            frames.Add(name(pc));
                        }
                    }
                }
                finally { ResumeThread(hT); }
                double cpu = 0;
                try { cpu = t.TotalProcessorTime.TotalSeconds; } catch { }
                w.WriteLine("=== tid " + t.Id + " cpu=" + cpu.ToString("F1") + "s prio=" + t.BasePriority);
                foreach (var f in frames) w.WriteLine("  " + f);
            }
        }
        return 0;
    }
}
