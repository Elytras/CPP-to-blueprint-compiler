"""invariants.Package reads UE 5.6 packages: the tagged tag grammar (FPropertyTypeName trees), the unversioned property
header RogueCore's own packages carry, and 5.6's script operands. The last test runs over RC's extracted content
(`pak.py extract ... .uasset .uexp`; RC_CONTENT names the .../RogueCore/Content folder) and is skipped without it."""
import glob
import os
import struct
import unittest

import invariants


def fake(blob, names, unversioned=False):
    """A Package of one export holding blob, without a file: only what tags / struct read."""
    p = invariants.Package.__new__(invariants.Package)
    p.names, p.ue, p.total, p.ue5, p.unversioned_props = names, blob, 0, True, unversioned
    p.exports = [dict(off=0, size=len(blob), cls=0, outer=0, name='X')]
    p._structs, p._tags, p.imports = {}, {}, []
    return p


def n(names, text, num=0):
    return struct.pack('<ii', names.index(text), num)


class Tag5Tests(unittest.TestCase):
    NAMES = ['None', 'Foo', 'ArrayProperty', 'EnumProperty', 'EMode', '/Script/Game', 'Flag', 'BoolProperty',
             'StructProperty', 'FVec', 'MapProperty', 'IntProperty', 'NameProperty']

    def test_tagged_trees(self):
        nm = self.NAMES
        b = b'\x00'                                                      # SerializationControlExtensions
        # Foo: ArrayProperty(1) -> EnumProperty(1) -> EMode(1) -> /Script/Game(0); size 8, no flags
        b += n(nm, 'Foo') + n(nm, 'ArrayProperty') + struct.pack('<i', 1) + n(nm, 'EnumProperty') + struct.pack('<i', 1)
        b += n(nm, 'EMode') + struct.pack('<i', 1) + n(nm, '/Script/Game') + struct.pack('<i', 0) + struct.pack('<i', 8)
        b += b'\x00' + b'\xaa' * 8
        # Flag: BoolProperty(0), size 0, flags BoolTrue
        b += n(nm, 'Flag') + n(nm, 'BoolProperty') + struct.pack('<i', 0) + struct.pack('<i', 0) + b'\x10'
        # Foo: StructProperty(1) -> FVec(1) -> /Script/Game(0); size 4, HasArrayIndex then the index 2
        b += n(nm, 'Foo') + n(nm, 'StructProperty') + struct.pack('<i', 1) + n(nm, 'FVec') + struct.pack('<i', 1)
        b += n(nm, '/Script/Game') + struct.pack('<i', 0) + struct.pack('<i', 4) + b'\x01' + struct.pack('<i', 2) + b'\xbb' * 4
        # Foo: MapProperty(2) -> NameProperty(0), IntProperty(0); size 1
        b += n(nm, 'Foo') + n(nm, 'MapProperty') + struct.pack('<i', 2) + n(nm, 'NameProperty') + struct.pack('<i', 0)
        b += n(nm, 'IntProperty') + struct.pack('<i', 0) + struct.pack('<i', 1) + b'\x00' + b'\xcc'
        b += n(nm, 'None')
        tags = fake(b, nm).tags(0)
        self.assertEqual([t['type'] for t in tags], ['ArrayProperty', 'BoolProperty', 'StructProperty', 'MapProperty'])
        self.assertEqual(tags[0]['inner'], 'EnumProperty')
        self.assertEqual(tags[0]['enum'], 'EMode')                       # an array's enum element names its enum too
        self.assertEqual(tags[0]['value'], b'\xaa' * 8)
        self.assertEqual(tags[1]['bool'], 1)
        self.assertEqual((tags[2]['struct'], tags[2]['index'], tags[2]['value']), ('FVec', 2, b'\xbb' * 4))
        self.assertEqual((tags[3]['inner'], tags[3]['value_type'], tags[3]['value']), ('NameProperty', 'IntProperty', b'\xcc'))
        self.assertEqual(tags.end, len(b))

    def test_unversioned_header_has_no_tags(self):
        # fragments: skip 13 properties, last (0x010d), then the lazy-guid bool, as a UClass export of RC starts
        b = struct.pack('<H', 0x010d) + struct.pack('<i', 0)
        tags = fake(b, self.NAMES, unversioned=True).tags(0)
        self.assertEqual((list(tags), tags.end), ([], 2))


class RunscriptMathTests(unittest.TestCase):
    def test_runscript_evaluates_ue5_double_math(self):
        import runscript
        m = runscript.MATH
        self.assertEqual((m['Add_DoubleDouble'](1.5, 2.0), m['Percent_DoubleDouble'](5.0, 3.0)), (3.5, 2.0))
        self.assertEqual(m['Conv_DoubleToFloat'](0.1), 0.10000000149011612)      # the narrowing keeps a float's 24 bits
        self.assertEqual(m['Conv_FloatToDouble'](2), 2.0)


RC = os.environ.get('RC_CONTENT', 'D:/tmp/s40rc/content/RogueCore/Content')


@unittest.skipUnless(os.path.isdir(RC + '/AI'), 'RC content not extracted')
class RcPackagesTests(unittest.TestCase):
    def test_every_ai_class_and_script_reads_to_its_header(self):
        bad, count = [], 0
        for base in invariants.packages([RC + '/AI']):
            pkg = invariants.Package(base)
            for i, msg in invariants.struct_body_exact(pkg):
                bad.append((base, pkg.exports[i]['name'], msg))
            count += 1
        self.assertGreater(count, 20)
        self.assertEqual(bad[:5], [])

    def test_unversioned_enum_names_and_struct_defaults(self):
        import genueapi
        names = genueapi.cooked_enum_names([RC], '/Game/GameElements/Barrier/EBarrierState')
        self.assertEqual(names[0], 'NewEnumerator0')            # the header counts a values-map, so the body is searched for
        self.assertGreaterEqual(len(names), 4)
        pkg = invariants.Package(RC + '/GameElements/Objectives/Excavation/Struct_WeigtedActor')
        self.assertIsNone(pkg.struct(0).defaults)                # a UserDefinedStruct's unversioned default is opaque

    def test_uasset_tags_and_struct_read_a_5_6_package(self):
        import contextlib, io, uasset
        base = RC + '/AI/Decorators/DEC_CanGrabTarget'
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            uasset.main(['tags', base, '0'])
            uasset.main(['struct', base, '2'])
        self.assertIn('unversioned properties', out.getvalue())
        self.assertIn('SuperStruct', out.getvalue())
        self.assertIn('consumed', out.getvalue())


if __name__ == '__main__':
    unittest.main()
