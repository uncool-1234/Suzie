# Runs inside the editor (-ExecutePythonScript): verify + tag probe + canonical reflection dump, then quit.
import unreal, os
for cmd in ("Game.VerifyNativeTypes", "Suzie.VerifyNativeTypes", "Suzie.TagInfo", "Game.TagInfo"):
    unreal.SystemLibrary.execute_console_command(None, cmd)
unreal.SystemLibrary.execute_console_command(None, "Suzie.DumpAll " + os.environ["SUZIE_DUMP_OUT"])
unreal.SystemLibrary.execute_console_command(None, "quit")
