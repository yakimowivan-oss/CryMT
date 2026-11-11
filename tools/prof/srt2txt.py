"""Plain text from a YouTube subtitle file (yt-dlp --convert-subs srt): no numbers, times or tags, the repeated lines of
auto captions dropped.
srt2txt.py <in.srt> <out.txt>"""
import re
import sys

lines = []
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.strip()
    if not line or line.isdigit() or '-->' in line:
        continue
    line = re.sub(r'<[^>]+>', '', line)
    if lines and (line == lines[-1] or lines[-1].endswith(line)):
        continue
    lines.append(line)
text = re.sub(r'\s+', ' ', ' '.join(lines))
open(sys.argv[2], 'w', encoding='utf-8').write(text)
print(len(text), 'characters')
