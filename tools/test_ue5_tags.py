"""S40: the 5.6 tag grammar of what assetgen writes for a UE5 target, read back with invariants.Package. Compiles
BpMods/RC/TagsRc.cpp against RC's UeApi (RC_UEAPI, a folder named UeApi made by genueapi on the RC dump) with the
assetgen in ASSETGEN_EXE; skipped where either is missing."""
import glob
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

import invariants

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
EXE = os.environ.get('ASSETGEN_EXE', os.path.join(REPO, 'x64', 'Release', 'assetgen.exe'))
API = os.environ.get('RC_UEAPI', 'D:/tmp/s40rc/rcapi/UeApi')


@unittest.skipUnless(os.path.exists(EXE) and os.path.isdir(API), 'needs assetgen.exe and an RC UeApi')
class TagTreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp()
        # compiled from a copy: BpMods/RC's own UeApi/ (two tracked headers) would shadow the generated one
        src = os.path.join(cls.out, 'src', 'TagsRc.cpp')
        os.makedirs(os.path.dirname(src))
        shutil.copy(os.path.join(REPO, 'BpMods', 'RC', 'TagsRc.cpp'), src)
        run = subprocess.run([EXE, 'compile', src, API, os.path.join(cls.out, 'out')],
                             capture_output=True, text=True)
        assert run.returncode == 0, run.stdout + run.stderr
        base = glob.glob(os.path.join(cls.out, 'out', '**', 'TagsRc.uasset'), recursive=True)[0][:-len('.uasset')]
        cls.pkg = invariants.Package(base)
        cdo = cls.pkg.find('Default__TagsRc_C') or cls.pkg.find('Default__TagsRc')
        cls.tags = {t['name']: t for t in cls.pkg.tags(cdo)}

    def test_array_of_structs_names_its_struct_and_has_no_inner_tag(self):
        self.assertEqual(self.tags['Points']['inner'], 'StructProperty')
        # the count and two FVectors of three doubles: 5.6 writes no inner tag between them
        self.assertEqual(len(self.tags['Points']['value']), 4 + 2 * 24)
        self.assertEqual((self.tags['Points']['struct'], self.tags['Rows']['struct']), ('Vector', 'FTagRow'))

    def test_array_of_enum_names_its_enum(self):
        self.assertEqual(self.tags['Modes']['inner'], 'ByteProperty')
        self.assertEqual(self.tags['Modes']['enum'], 'ETagMode')

    def test_map_has_a_key_and_a_value_tree(self):
        self.assertEqual((self.tags['ByName']['inner'], self.tags['ByName']['value_type']), ('NameProperty', 'StructProperty'))

    def test_soft_path_is_a_top_level_asset_path_and_a_sub_path(self):
        value = self.tags['Icon']['value']
        names = self.pkg.names
        package, asset = struct.unpack_from('<ii', value, 0)[0], struct.unpack_from('<ii', value, 8)[0]
        self.assertEqual((names[package], names[asset], struct.unpack_from('<i', value, 16)[0], len(value)),
                         ('/Engine/EngineResources/DefaultTexture', 'DefaultTexture', 0, 20))


if __name__ == '__main__':
    unittest.main()
