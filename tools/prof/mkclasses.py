# census_vtables.txt (vtable, method count, class name, offset) -> RendererClasses6156.h compiled into CryMT.dll.
import sys
src, dst = sys.argv[1], sys.argv[2]
rows = []
for line in open(src, encoding='ascii'):
    parts = line.split()
    if len(parts) == 4:
        rows.append((int(parts[0], 16), int(parts[1]), parts[2]))
longest = max(len(r[2]) for r in rows)
out = ['#pragma once',
       '// Classes with vtables in CryRenderD3D10.dll (Crysis 1.2.1 build 6156, base 0x38000000): vtable, method count,',
       '// RTTI name, in file order (the fill window numbers the methods in this order). Generated from',
       '// logs\\mt\\census_vtables.txt (tools\\prof\\all_vtables.py) by mkclasses.py; ' + str(len(rows)) + ' classes, longest name ' + str(longest) + '.',
       '#include <cstdint>',
       '',
       'struct RendererClassInfo',
       '{',
       '    uint32_t vtable;',
       '    uint32_t count;',
       '    const char* name;',
       '};',
       '',
       'constexpr RendererClassInfo kRendererClassList[] = {']
for vt, n, name in rows:
    out.append('    {0x%08X, %d, "%s"},' % (vt, n, name))
out.append('};')
open(dst, 'w', encoding='ascii', newline='\n').write('\n'.join(out) + '\n')
print(len(rows), 'classes, longest name', longest, 'methods', sum(r[1] for r in rows))
