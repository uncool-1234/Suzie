"""usage: make_sweep_csv.py <tag> <out.csv> --dir <runlist OutDir> [--list <asset|Type list>]: per asset status CLEAN / DIRTY (loads, but error lines in its load slice) / CRASH / FAIL."""
import sys, re, glob, csv, os, collections, argparse
ap = argparse.ArgumentParser(); ap.add_argument('tag'); ap.add_argument('out'); ap.add_argument('--dir', required=True); ap.add_argument('--list', default='')
A = ap.parse_args()
D, tag, out = A.dir, A.tag, A.out
noise = re.compile(r'DirectoryWatcher|Failed to read file|DerivedDataCache|XGE|LogStats|FDebug::EnsureFailed|CVar|LogGameplayTags: Warning|Missing Dependency, missing export|Skipped failed export|Script Stack|LogShaderCompilers|LogMemoryImage|LogMaterial: Warning|LogTexture|LogSlate|LogAudio')
bad = re.compile(r'Error:|Fatal|Ensure condition failed|Assertion failed|missing script import|Could not find|Serial size mismatch|ObjectSerializationError')
slices = {}
for f in sorted(glob.glob(os.path.join(D, tag + '.L*.log'))):
    cur = None
    for l in open(f, encoding='utf8', errors='ignore'):
        m = re.search(r'SZTEST\|START\|(\S+)', l)
        if m: cur = m.group(1); slices.setdefault(cur, []); continue
        if 'SZTEST|END|all' in l: cur = None
        if cur and bad.search(l) and not noise.search(l):
            slices[cur].append(re.sub(r'^\[[^\]]*\]\[[^\]]*\]', '', l.strip())[:260])
types = {}
for l in (open(A.list, encoding='utf8') if A.list else []):
    a, t = l.strip().split('|'); types[a] = t
rows = list(csv.DictReader(open(os.path.join(D, tag + '.results.csv'), encoding='utf8')))
w = csv.writer(open(out, 'w', newline='', encoding='utf8'))
w.writerow(['asset', 'type', 'status', 'n_error_lines', 'first_error'])
c = collections.Counter()
for r in rows:
    v = slices.get(r['asset'], [])
    st = r['status']
    if st == 'PASS': st = 'CLEAN' if not v else 'DIRTY'
    first = r['info'] if st == 'CRASH' else (v[0] if v else '')
    c[st] += 1
    w.writerow([r['asset'], types.get(r['asset'], ''), st, len(v), first.replace('\n', ' ')[:300]])
print(c)
