// Timestamped sampling of the busiest threads of a process.
// Usage: CrySampler2 <pid> <seconds> <hz> <outPrefix> [threadCount]
// Writes <out>.threads.txt (index, tid, start module, cpu) and <out>.samples.txt
// (thread index, QPC timestamp, frames root-first as module!+0xRVA joined by ';').
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

static class CrySampler2
{
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenThread(int a, bool i, int tid);
    [DllImport("kernel32")] static extern int SuspendThread(IntPtr h);
    [DllImport("kernel32")] static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("kernel32")] static extern IntPtr GetModuleHandle(string n);
    [DllImport("kernel32")] static extern bool QueryPerformanceCounter(out long v);
    [DllImport("winmm")] static extern int timeBeginPeriod(int ms);
    [DllImport("ntdll")] static extern int NtQueryInformationThread(IntPtr h, int c, out IntPtr v, int l, out int r);
    [DllImport("dbghelp", SetLastError = true)] static extern bool SymInitialize(IntPtr p, string search, bool invade);
    [DllImport("dbghelp", SetLastError = true)] static extern IntPtr SymFunctionTableAccess64(IntPtr p, ulong addr);
    [DllImport("dbghelp", SetLastError = true)]
    static extern bool StackWalk64(int machine, IntPtr p, IntPtr t, IntPtr frame, IntPtr ctx,
        IntPtr readMem, IntPtr funcTable, IntPtr modBase, IntPtr translate);

    class Mod { public string Name; public ulong Base, End; }
    static List<Mod> mods = new List<Mod>();
    static IntPtr hProc;

    static string Key(ulong addr)
    {
        foreach (var m in mods)
            if (addr >= m.Base && addr < m.End)
            {
                IntPtr rf = SymFunctionTableAccess64(hProc, addr);
                ulong rva = rf != IntPtr.Zero ? (ulong)(uint)Marshal.ReadInt32(rf) : addr - m.Base;
                return m.Name + "!+0x" + rva.ToString("X");
            }
        return "?";
    }

    static int Main(string[] a)
    {
        int pid = int.Parse(a[0]), seconds = int.Parse(a[1]), hz = int.Parse(a[2]);
        string prefix = a[3];
        int count = a.Length > 4 ? int.Parse(a[4]) : 4;
        var proc = Process.GetProcessById(pid);
        foreach (ProcessModule pm in proc.Modules)
        {
            ulong b = (ulong)pm.BaseAddress.ToInt64();
            mods.Add(new Mod { Name = pm.ModuleName, Base = b, End = b + (ulong)pm.ModuleMemorySize });
        }
        hProc = OpenProcess(0x1F0FFF, false, pid);
        SymInitialize(hProc, null, true);
        IntPtr dbg = GetModuleHandle("dbghelp.dll");
        IntPtr ftab = GetProcAddress(dbg, "SymFunctionTableAccess64"), mbase = GetProcAddress(dbg, "SymGetModuleBase64");

        // Pick the busiest threads over one second.
        var before = new Dictionary<int, double>();
        foreach (ProcessThread t in proc.Threads) { try { before[t.Id] = t.TotalProcessorTime.TotalMilliseconds; } catch { } }
        Thread.Sleep(1000);
        proc.Refresh();
        var busy = new List<Tuple<int, double>>();
        foreach (ProcessThread t in proc.Threads)
        {
            try
            {
                double b0; before.TryGetValue(t.Id, out b0);
                busy.Add(Tuple.Create(t.Id, t.TotalProcessorTime.TotalMilliseconds - b0));
            }
            catch { }
        }
        var chosen = busy.OrderByDescending(x => x.Item2).Take(count).ToList();
        var handles = new List<IntPtr>();
        using (var w = new StreamWriter(prefix + ".threads.txt"))
        {
            for (int i = 0; i < chosen.Count; i++)
            {
                IntPtr h = OpenThread(0x0002 | 0x0008 | 0x0040, false, chosen[i].Item1);
                handles.Add(h);
                IntPtr start; int r;
                NtQueryInformationThread(h, 9, out start, IntPtr.Size, out r);
                w.WriteLine(i + "\t" + chosen[i].Item1 + "\t" + Key((ulong)start.ToInt64()) + "\tcpuMsPerSec=" + chosen[i].Item2.ToString("F0"));
            }
        }

        IntPtr raw = Marshal.AllocHGlobal(1232 + 32);
        IntPtr ctx = new IntPtr((raw.ToInt64() + 15) & ~15L);
        IntPtr frame = Marshal.AllocHGlobal(1024);
        timeBeginPeriod(1);
        var sw = Stopwatch.StartNew();
        double period = 1000.0 / hz, next = 0;
        int samples = 0;
        using (var w = new StreamWriter(prefix + ".samples.txt"))
        {
            while (sw.Elapsed.TotalSeconds < seconds && !proc.HasExited)
            {
                for (int i = 0; i < handles.Count; i++)
                {
                    var keys = new List<string>();
                    long qpc;
                    if (SuspendThread(handles[i]) == -1) continue;
                    try
                    {
                        QueryPerformanceCounter(out qpc);
                        for (int k = 0; k < 1232; k += 8) Marshal.WriteInt64(ctx, k, 0);
                        Marshal.WriteInt32(ctx, 0x30, 0x100003);
                        if (GetThreadContext(handles[i], ctx))
                        {
                            for (int k = 0; k < 1024; k += 8) Marshal.WriteInt64(frame, k, 0);
                            Marshal.WriteInt64(frame, 0, Marshal.ReadInt64(ctx, 0xF8));
                            Marshal.WriteInt32(frame, 12, 3);
                            Marshal.WriteInt64(frame, 32, Marshal.ReadInt64(ctx, 0xA0));
                            Marshal.WriteInt32(frame, 44, 3);
                            Marshal.WriteInt64(frame, 48, Marshal.ReadInt64(ctx, 0x98));
                            Marshal.WriteInt32(frame, 60, 3);
                            for (int d = 0; d < 80; d++)
                            {
                                if (!StackWalk64(0x8664, hProc, handles[i], frame, ctx, IntPtr.Zero, ftab, mbase, IntPtr.Zero)) break;
                                ulong pc = (ulong)Marshal.ReadInt64(frame, 0);
                                if (pc == 0) break;
                                keys.Add(Key(pc));
                            }
                        }
                    }
                    finally { ResumeThread(handles[i]); }
                    keys.Reverse();
                    w.WriteLine(i + "\t" + qpc + "\t" + string.Join(";", keys));
                    samples++;
                }
                next += period;
                int wait = (int)(next - sw.Elapsed.TotalMilliseconds);
                if (wait > 0) Thread.Sleep(wait);
            }
        }
        Console.WriteLine("samples=" + samples + " threads=" + handles.Count);
        return 0;
    }
}
