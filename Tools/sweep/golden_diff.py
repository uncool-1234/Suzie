#!/usr/bin/env python3
"""golden_diff.py old.golden new.golden [--whitelist file] [--show N]
Compares two Suzie.DumpAll dumps block by block (a block starts at a line without leading blanks: ENUM/STRUCT/CLASS/... <path> ...).
Prints added / removed / changed blocks (with the first differing lines) and exits 1 when something is not covered by the whitelist.
Whitelist lines: `<KIND> <path>  # reason` (matches added, removed or changed blocks of that type); `#` comments allowed."""
import sys, re, argparse, collections

def blocks(path):
    out = collections.OrderedDict(); cur = None; head = {}
    for l in open(path, encoding='utf8', errors='replace'):
        l = l.rstrip('\n')
        if l and not l[0].isspace():
            m = re.match(r'(\S+)\s+(\S+)', l)
            if l.startswith('SUMMARY'):
                out['SUMMARY'] = [l]; cur = None; continue
            key = (m.group(1) + ' ' + m.group(2)) if m else l
            cur = key; out[cur] = [l]
        elif cur is not None:
            out[cur].append(l)
    return out

ap = argparse.ArgumentParser(); ap.add_argument('old'); ap.add_argument('new'); ap.add_argument('--whitelist'); ap.add_argument('--show', type=int, default=3)
a = ap.parse_args()
O, N = blocks(a.old), blocks(a.new)
wl = set()
if a.whitelist:
    for l in open(a.whitelist, encoding='utf8'):
        l = l.split('#')[0].strip()
        if l: wl.add(l)
added = [k for k in N if k not in O]; removed = [k for k in O if k not in N]
changed = [k for k in N if k in O and N[k] != O[k]]
bad = 0
def report(title, keys, detail):
    global bad
    print('%s: %d' % (title, len(keys)))
    for k in keys:
        ok = k in wl
        if not ok: bad += 1
        print('  %s%s' % ('(whitelisted) ' if ok else '', k))
        if detail and not ok:
            o, n = O[k], N[k]; shown = 0
            so, sn = set(o), set(n)
            for l in o:
                if l not in sn and shown < a.show: print('      - ' + l[:200]); shown += 1
            for l in n:
                if l not in so and shown < 2 * a.show: print('      + ' + l[:200]); shown += 1
report('added', added, False); report('removed', removed, False); report('changed', changed, True)
print('blocks old=%d new=%d, not whitelisted=%d' % (len(O), len(N), bad))
sys.exit(1 if bad else 0)
