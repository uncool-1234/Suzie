# Runs inside the editor (-ExecutePythonScript): loads DataTable assets from the asset registry and reports, per DataTable whose row struct is
# GENERATED (a struct that came from the profile, i.e. its package is /Script/<module> but unknown to the stock engine), whether it loaded.
# Env: SUZIE_DT_MAX (default 300), SUZIE_DT_OUT (result file). Exits the editor when done; a missing DONE line means a crash.
import unreal, os, random
mx = int(os.environ.get("SUZIE_DT_MAX", "300")); out = os.environ["SUZIE_DT_OUT"]
excl = set(l.strip() for l in open(os.environ["SUZIE_DT_EXCLUDE"])) if os.environ.get("SUZIE_DT_EXCLUDE") and os.path.exists(os.environ["SUZIE_DT_EXCLUDE"]) else set()
def mark(s):
    with open(out, "a") as f: f.write(s + "\n"); f.flush(); os.fsync(f.fileno())
reg = unreal.AssetRegistryHelpers.get_asset_registry()
flt = unreal.ARFilter(class_paths=[unreal.TopLevelAssetPath("/Script/Engine", "DataTable")])
assets = list(reg.get_assets(flt))
mark("FOUND|%d" % len(assets))
random.seed(7); random.shuffle(assets)
assets = [a for a in assets if str(a.package_name) not in excl]
ok = fail = 0; structs = {}
for a in assets[:mx]:
    pk = str(a.package_name)
    mark("START|" + pk)
    try:
        dt = unreal.load_asset(pk)
        if dt is None: fail += 1; mark("FAIL|%s|None" % pk); continue
        rs = dt.get_editor_property("row_struct")
        rn = rs.get_path_name() if rs else "None"
        n = 0
        try: n = len(unreal.DataTableFunctionLibrary.get_data_table_row_names(dt))
        except Exception as e: pass
        structs[rn] = structs.get(rn, 0) + 1; ok += 1
        mark("OK|%s|%s|rows=%d" % (pk, rn, n))
    except Exception as e:
        fail += 1; mark("FAIL|%s|%s" % (pk, str(e)[:160]))
mark("SUMMARY|ok=%d fail=%d structs=%d" % (ok, fail, len(structs)))
for k, v in sorted(structs.items()): mark("STRUCT|%s|%d" % (k, v))
mark("DONE")
unreal.SystemLibrary.execute_console_command(None, "quit")
