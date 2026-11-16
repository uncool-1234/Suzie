import unreal, os
lst = [l.strip().split('|')[0] for l in open(os.environ["SUZIE_LIST"]) if l.strip()]
prog = os.environ["SUZIE_PROG"]
tools = unreal.AssetToolsHelpers.get_asset_tools()
world = unreal.EditorLevelLibrary.get_editor_world()
sphere = unreal.load_asset("/Engine/BasicShapes/Sphere")
rt = unreal.RenderingLibrary.create_render_target2d(world, 128, 128, unreal.TextureRenderTargetFormat.RTF_RGBA8)
def mark(s):
    with open(prog, "a") as f: f.write(s + "\n"); f.flush(); os.fsync(f.fileno())
def spawn_capture(show_only):
    cap = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(-250,0,300), unreal.Rotator(0,0,0))
    c = cap.capture_component2d
    c.set_editor_property("capture_every_frame", False)
    c.primitive_render_mode = unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST
    if show_only: c.show_only_actor_components(show_only, True)
    c.texture_target = rt
    c.capture_scene()
    # Reading the target back waits for the render thread and the GPU, so a rendering crash happens while the current asset is recorded.
    return cap, unreal.RenderingLibrary.read_render_target(world, rt, False)
cap, background = spawn_capture(None)
cap.destroy_actor()
for i, pk in enumerate(lst):
    mark("START|%s" % pk)
    unreal.log("SZMAT|START|%s" % pk)
    actor = cap = None
    try:
        p = unreal.load_asset(pk)
        if p is None: mark("FAIL|%s|load returned None" % pk); continue
        mic = tools.create_asset("MICS_%d" % i, "/Game/Test", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mic.set_editor_property("parent", p)
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,300))
        comp = actor.static_mesh_component; comp.set_mobility(unreal.ComponentMobility.MOVABLE)
        comp.set_static_mesh(sphere); comp.set_material(0, mic)
        cap, px = spawn_capture(actor)
        diffs = [max(abs(q.r - b.r), abs(q.g - b.g), abs(q.b - b.b)) for q, b in zip(px, background)]
        changed = sum(1 for d in diffs if d > 6)
        mark("OK|%s|%d|%d" % (pk, changed, max(diffs, default=0)))
    except Exception as e:
        mark("FAIL|%s|%s" % (pk, str(e)[:200].replace("\n", " ")))
    finally:
        unreal.log("SZMAT|END|%s" % pk)
        if actor: actor.destroy_actor()
        if cap: cap.destroy_actor()
mark("DONE")
unreal.SystemLibrary.execute_console_command(None, "quit")
