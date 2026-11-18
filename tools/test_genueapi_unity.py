"""The unity dump (SDK.unity, Dumper-7's fork): genueapi reads each file out of it as it reads the file off disk, and
its game section as JSON."""
import json
import os
import tempfile
import unittest
from unittest.mock import patch

import genueapi

FILES = {"SDK/SDK/Engine_classes.hpp": b"\xef\xbb\xbf// Class /Script/Engine.Actor\r\nclass AActor\r\n{\r\n};\r\n",
         "SDK/SDK/Engine_structs.hpp": "// é \xff\n".encode("utf-8") + b"\xff\xfe tail",
         "GObjects-Dump-WithProperties.txt": b"[00000001] {0x1} Class /Script/Engine.Actor\n"}
GAME = {"classes": {"/Game/A.A_C": [0, "/Script/CoreUObject.Object", "Engine", "/Script/Engine.Actor", []]}}


def unity(files, game):
    out = b""
    for name, body in list(files.items()) + [("@game", json.dumps(game).encode())]:
        out += b"//@file %d %s\n" % (len(body), name.encode()) + body
    return out


class Unity(unittest.TestCase):
    def test_files_and_game_section(self):
        with tempfile.TemporaryDirectory() as dump, patch.object(genueapi, "SDK_FILES", {}), \
                patch.object(genueapi, "SDK_DIRS", {}):
            for name, body in FILES.items():
                os.makedirs(os.path.dirname(os.path.join(dump, name)), exist_ok=True)
                with open(os.path.join(dump, name), "wb") as f:
                    f.write(body)
            on_disk = dict((n, genueapi.io.open(os.path.join(dump, n), encoding="utf-8", errors="replace").read())
                           for n in FILES)
            with open(os.path.join(dump, "SDK.unity"), "wb") as f:
                f.write(unity(FILES, GAME))
            for n in FILES:                      # the unity dump must win over the files: spoil them
                with open(os.path.join(dump, n), "wb") as f:
                    f.write(b"stale")
            self.assertEqual(genueapi.open_unity(dump), GAME)
            sdk = os.path.join(dump, "SDK", "SDK")
            self.assertEqual(genueapi.sdk_names(sdk), ["Engine_classes.hpp", "Engine_structs.hpp"])
            for n, text in on_disk.items():
                self.assertEqual(genueapi.sdk_text(os.path.join(dump, n)), text, n)
            self.assertEqual(list(genueapi.sdk_lines(os.path.join(sdk, "Engine_classes.hpp")))[1], "class AActor\n")

    def test_subobjects_sorted_in_the_dump_spelling(self):
        dump_list = (b"[00000001] {0x1} Class /Script/Engine.SceneComponent\n"
                     b"[00000002] {0x2} SceneComponent /Game/A.A_C.particle_Trail_GEN_VARIABLE\n"
                     b"[00000004] {0x4} SceneComponent /Game/A.A_C.Terrain scanner mesh_GEN_VARIABLE\n"
                     b"[00000003] {0x3} SceneComponent /Game/A.Default__A_C.temperature\n")
        cdo = {"/Game/A.A_C": ["/Script/Engine.SceneComponent Temperature", "/Script/Engine.scenecomponent Root"]}
        ich = {"/Game/A.A_C": ["Particle_Trail", "Terrain scanner Mesh"]}
        with tempfile.TemporaryDirectory() as dump, patch.object(genueapi, "SDK_FILES", {}), \
                patch.object(genueapi, "SDK_DIRS", {}), patch.object(genueapi, "GAME_SUBOBJECTS", dict(cdo)), \
                patch.object(genueapi, "GAME_ICH_RECORDS", dict(ich)), \
                patch.object(genueapi, "GAME_TEMPLATE_SUBOBJECTS", {}):
            with open(os.path.join(dump, "GObjects-Dump-WithProperties.txt"), "wb") as f:
                f.write(dump_list)
            genueapi.settle_subobjects(os.path.join(dump, "SDK", "SDK"), True)
            self.assertEqual(genueapi.GAME_SUBOBJECTS["/Game/A.A_C"],
                             ["/Script/Engine.SceneComponent Root", "/Script/Engine.SceneComponent temperature"])
            self.assertEqual(genueapi.GAME_ICH_RECORDS["/Game/A.A_C"], ["particle_Trail", "Terrain scanner mesh"])

    def test_no_unity(self):
        with tempfile.TemporaryDirectory() as dump:
            self.assertIsNone(genueapi.open_unity(dump))


if __name__ == "__main__":
    unittest.main()
