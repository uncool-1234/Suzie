import unreal, os
lst = [l.strip().split('|')[0] for l in open(os.environ["SUZIE_LIST"]) if l.strip()]
prog = os.environ["SUZIE_PROG"]
tools = unreal.AssetToolsHelpers.get_asset_tools()
world = unreal.EditorLevelLibrary.get_editor_world()
sphere = unreal.load_asset("/Engine/BasicShapes/Sphere")
def mark(s):
    with open(prog, "a") as f: f.write(s + "\n"); f.flush(); os.fsync(f.fileno())
for i, pk in enumerate(lst):
    mark("START|%s" % pk)
    try:
        p = unreal.load_asset(pk)
        if p is None: mark("FAIL|%s|load returned None" % pk); continue
        mic = tools.create_asset("MICS_%d" % i, "/Game/Test", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mic.set_editor_property("parent", p)
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(0,0,300))
        comp = actor.static_mesh_component; comp.set_mobility(unreal.ComponentMobility.MOVABLE)
        comp.set_static_mesh(sphere); comp.set_material(0, mic)
        cap = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.SceneCapture2D, unreal.Vector(-250,0,300), unreal.Rotator(0,0,0))
        c = cap.capture_component2d
        c.primitive_render_mode = unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST
        c.show_only_actor_components(actor, True)
        rt = unreal.RenderingLibrary.create_render_target2d(world, 128, 128, unreal.TextureRenderTargetFormat.RTF_RGBA8)
        c.texture_target = rt
        c.capture_scene()
        actor.destroy_actor(); cap.destroy_actor()
        mark("OK|%s" % pk)
    except Exception as e:
        mark("FAIL|%s|%s" % (pk, str(e)[:200]))
mark("DONE")
unreal.SystemLibrary.execute_console_command(None, "quit")
