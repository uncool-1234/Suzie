import unreal, os
lst = [l.strip().split('|')[0] for l in open(os.environ["Game_LIST"]) if l.strip()]
prog = os.environ["Game_PROG"]
def mark(s):
    with open(prog, "a") as f: f.write(s + "\n"); f.flush(); os.fsync(f.fileno())
unreal.SystemLibrary.execute_console_command(None, "Suzie.VerifyNativeTypes")
for pk in lst:
    mark("START|%s" % pk)
    unreal.log("GameTEST|START|%s" % pk)
    try:
        o = unreal.load_asset(pk)
        if o is None:
            o = unreal.load_class(None, pk + '.' + pk.split('/')[-1] + '_C')
        if o:
            extra = ""
            try:
                c = o if isinstance(o, unreal.Class) else o.generated_class()
                extra = " class=%s" % c.get_name()
                chain = []
                s = c
                while s and len(chain) < 12:
                    chain.append(s.get_name()); s = s.get_super_struct() if hasattr(s, 'get_super_struct') else None
                extra += " chain=" + ">".join(chain)
            except Exception as e:
                extra += " (chain err %s)" % str(e)[:80]
            mark("OK|%s|%s%s" % (pk, type(o).__name__, extra))
        else:
            mark("FAIL|%s|load returned None" % pk)
    except Exception as e:
        mark("FAIL|%s|%s" % (pk, str(e)[:200]))
unreal.log("GameTEST|END|all")
for pk in (lst if os.environ.get("Game_DUMP") else []):
    cn = pk + '.' + pk.split('/')[-1] + '_C'
    unreal.SystemLibrary.execute_console_command(None, "Suzie.DumpClass " + cn)
if os.environ.get("Game_GC"):
    unreal.SystemLibrary.collect_garbage()
    mark("GC_OK")
mark("DONE")
unreal.SystemLibrary.execute_console_command(None, "quit")
