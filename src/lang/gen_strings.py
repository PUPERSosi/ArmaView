# Builds src/core/Strings.cpp: Strings.template.cpp + the string table from table*.tsv (this folder).
# TSV columns: English, Russian, tooltip EN, tooltip RU, hotkey command; lines starting with "# " are comments.
# Run after editing a table:  python src/lang/gen_strings.py
import re, sys, glob, os
D = os.path.dirname(os.path.abspath(__file__))
core = open(os.path.join(D, 'Strings.template.cpp'), encoding='utf-8').read()

def lit(s):
    if s is None or s == '':
        return 'nullptr'
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\':
            if i + 1 < len(s) and s[i + 1] == 'n':
                out.append('\\n'); i += 2; continue
            out.append('\\\\')
        elif c == '"':
            out.append('\\"')
        else:
            out.append(c)
        i += 1
    return '"' + ''.join(out) + '"'

rows, seen = [], {}
bad = False
for f in sorted(glob.glob(os.path.join(D, 'table*.tsv'))):
    for ln, line in enumerate(open(f, encoding='utf-8').read().split('\n'), 1):
        if not line.strip() or line.startswith('# '):
            continue
        p = line.split('\t')
        while len(p) < 5:
            p.append('')
        en = p[0]
        if en in seen:
            print('duplicate', en, os.path.basename(f), ln, 'first', os.path.basename(seen[en][0]), seen[en][1]); bad = True; continue
        seen[en] = (f, ln)
        if len(p) > 5:
            print('too many columns', f, ln, p); sys.exit(1)
        if p[1]:
            spec = lambda x: (re.findall(r'%[-+ #0]*[0-9.*]*(?:hh|h|ll|l|z|j|t)?[diouxXeEfgGcsp%]', x), x.count('{}') + len(re.findall(r'\{:[^}]*\}', x)))
            if spec(en) != spec(p[1]):
                print('format mismatch', f, ln, en, '|', p[1], spec(en), spec(p[1])); sys.exit(1)
        rows.append(p)

if bad: sys.exit(1)
body = []
body.append('const Row kRows[] = {')
for en, ru, te, tr, cmd in rows:
    body.append('    { %s, %s, %s, %s, %s },' % (lit(en), lit(ru), lit(te), lit(tr), lit(cmd)))
body.append('};')
body.append('const size_t kRowCount = std::size(kRows);')
table = '\n'.join(body)
out = core + '\nnamespace {\n\n' + table + '\n\n} // namespace\n'
out = out.replace('\r\n', '\n').replace('\n', '\r\n')
open(os.path.join(D, '..', 'core', 'Strings.cpp'), 'wb').write(out.encode('utf-8'))
print(len(rows), 'rows')
