// External sampling profiler: suspends one thread of a running process, walks its stack
// through the module .pdata tables, and aggregates exclusive/inclusive time per function.
// Usage: CrySampler <pid> <seconds> <hz> <outPrefix>
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

static class CrySampler
{
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
    [DllImport("kernel32", SetLastError = true)] static extern IntPtr OpenThread(int a, bool i, int tid);
    [DllImport("kernel32")] static extern int SuspendThread(IntPtr h);
    [DllImport("kernel32")] static extern int ResumeThread(IntPtr h);
    [DllImport("kernel32", SetLastError = true)] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("kernel32")] static extern IntPtr GetModuleHandle(string n);
    [DllImport("kernel32")] static extern IntPtr LoadLibrary(string n);
    [DllImport("winmm")] static extern int timeBeginPeriod(int ms);

    [DllImport("dbghelp", SetLastError = true)] static extern bool SymInitialize(IntPtr p, string search, bool invade);
    [DllImport("dbghelp", SetLastError = true)] static extern IntPtr SymFunctionTableAccess64(IntPtr p, ulong addr);
    [DllImport("dbghelp", SetLastError = true)] static extern ulong SymGetModuleBase64(IntPtr p, ulong addr);
    [DllImport("dbghelp", SetLastError = true)]
    static extern bool StackWalk64(int machine, IntPtr p, IntPtr t, IntPtr frame, IntPtr ctx,
        IntPtr readMem, IntPtr funcTable, IntPtr modBase, IntPtr translate);

    const int AMD64 = 0x8664;
    const int MaxDepth = 96;

    class Mod { public string Name; public ulong Base, End; }
    static List<Mod> mods = new List<Mod>();
    static Mod ModOf(ulong a) { foreach (var m in mods) if (a >= m.Base && a < m.End) return m; return null; }

    static IntPtr hProc;
    static string FuncKey(ulong addr)
    {
        var m = ModOf(addr);
        if (m == null) return "?!0x" + addr.ToString("X");
        IntPtr rf = SymFunctionTableAccess64(hProc, addr);
        ulong rva = rf != IntPtr.Zero ? (ulong)(uint)Marshal.ReadInt32(rf) : addr - m.Base;
        return m.Name + "!+0x" + rva.ToString("X");
    }

    static int Main(string[] a)
    {
        int pid = int.Parse(a[0]), seconds = int.Parse(a[1]), hz = int.Parse(a[2]);
        string prefix = a[3];
        var proc = Process.GetProcessById(pid);
        foreach (ProcessModule pm in proc.Modules)
        {
            ulong b = (ulong)pm.BaseAddress.ToInt64();
            mods.Add(new Mod { Name = pm.ModuleName, Base = b, End = b + (ulong)pm.ModuleMemorySize });
        }
        hProc = OpenProcess(0x1F0FFF, false, pid);
        if (hProc == IntPtr.Zero) { Console.WriteLine("OpenProcess failed"); return 1; }
        if (!SymInitialize(hProc, null, true)) { Console.WriteLine("SymInitialize failed " + Marshal.GetLastWin32Error()); return 1; }
        IntPtr dbg = GetModuleHandle("dbghelp.dll");
        IntPtr ftab = GetProcAddress(dbg, "SymFunctionTableAccess64"), mbase = GetProcAddress(dbg, "SymGetModuleBase64");

        var main = proc.Threads.Cast<ProcessThread>().OrderBy(t => t.StartTime).First();
        IntPtr hT = OpenThread(0x0002 | 0x0008 | 0x0040, false, main.Id); // suspend, get-context, query
        if (hT == IntPtr.Zero) { Console.WriteLine("OpenThread failed"); return 1; }
        Console.WriteLine("sampling tid=" + main.Id + " hz=" + hz + " seconds=" + seconds);

        IntPtr raw = Marshal.AllocHGlobal(1232 + 32);
        IntPtr ctx = new IntPtr((raw.ToInt64() + 15) & ~15L);
        IntPtr frame = Marshal.AllocHGlobal(1024);

        var excl = new Dictionary<string, int>();
        var incl = new Dictionary<string, int>();
        var exclMod = new Dictionary<string, int>();
        var inclMod = new Dictionary<string, int>();
        var stacks = new Dictionary<string, int>();
        int total = 0, failed = 0;
        Action<Dictionary<string, int>, string> bump = (d, k) => { int v; d.TryGetValue(k, out v); d[k] = v + 1; };

        timeBeginPeriod(1);
        var sw = Stopwatch.StartNew();
        double period = 1000.0 / hz, next = 0;
        var frames = new List<ulong>(MaxDepth);
        while (sw.Elapsed.TotalSeconds < seconds && !proc.HasExited)
        {
            frames.Clear();
            if (SuspendThread(hT) == -1) break;
            try
            {
                for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(ctx, i, 0);
                Marshal.WriteInt32(ctx, 0x30, 0x100003); // CONTEXT_CONTROL | CONTEXT_INTEGER
                if (GetThreadContext(hT, ctx))
                {
                    for (int i = 0; i < 1024; i += 8) Marshal.WriteInt64(frame, i, 0);
                    Marshal.WriteInt64(frame, 0, Marshal.ReadInt64(ctx, 0xF8));   // AddrPC = Rip
                    Marshal.WriteInt32(frame, 12, 3);
                    Marshal.WriteInt64(frame, 32, Marshal.ReadInt64(ctx, 0xA0));  // AddrFrame = Rbp
                    Marshal.WriteInt32(frame, 44, 3);
                    Marshal.WriteInt64(frame, 48, Marshal.ReadInt64(ctx, 0x98));  // AddrStack = Rsp
                    Marshal.WriteInt32(frame, 60, 3);
                    for (int d = 0; d < MaxDepth; d++)
                    {
                        if (!StackWalk64(AMD64, hProc, hT, frame, ctx, IntPtr.Zero, ftab, mbase, IntPtr.Zero)) break;
                        ulong pc = (ulong)Marshal.ReadInt64(frame, 0);
                        if (pc == 0) break;
                        frames.Add(pc);
                    }
                }
            }
            finally { ResumeThread(hT); }

            if (frames.Count == 0) failed++;
            else
            {
                total++;
                var keys = frames.Select(FuncKey).ToList();
                bump(excl, keys[0]);
                bump(exclMod, keys[0].Split('!')[0]);
                foreach (var k in keys.Distinct()) bump(incl, k);
                foreach (var k in keys.Select(s => s.Split('!')[0]).Distinct()) bump(inclMod, k);
                bump(stacks, string.Join(";", Enumerable.Reverse(keys)));
            }
            next += period;
            int wait = (int)(next - sw.Elapsed.TotalMilliseconds);
            if (wait > 0) Thread.Sleep(wait);
        }

        Func<Dictionary<string, int>, int, IEnumerable<string>> top = (d, n) =>
            d.OrderByDescending(kv => kv.Value).Take(n).Select(kv =>
                string.Format("{0,7:F2}%  {1}", 100.0 * kv.Value / Math.Max(total, 1), kv.Key));
        using (var w = new StreamWriter(prefix + ".txt"))
        {
            w.WriteLine("samples=" + total + " failed=" + failed + " elapsed=" + sw.Elapsed.TotalSeconds.ToString("F1"));
            w.WriteLine("\n== inclusive by module"); foreach (var s in top(inclMod, 30)) w.WriteLine(s);
            w.WriteLine("\n== exclusive by module"); foreach (var s in top(exclMod, 30)) w.WriteLine(s);
            w.WriteLine("\n== inclusive by function"); foreach (var s in top(incl, 150)) w.WriteLine(s);
            w.WriteLine("\n== exclusive by function"); foreach (var s in top(excl, 100)) w.WriteLine(s);
        }
        using (var w = new StreamWriter(prefix + ".folded"))
            foreach (var kv in stacks.OrderByDescending(k => k.Value)) w.WriteLine(kv.Key + " " + kv.Value);
        Console.WriteLine("samples=" + total + " failed=" + failed);
        return 0;
    }
}
