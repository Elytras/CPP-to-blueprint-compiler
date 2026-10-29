"""Regressions for the game's Blueprint enums (UserDefinedEnum): a static library call whose dumped enum has a
qualified name, and an enum genueapi gives a C++ type with the members that use it (BP_DropPod_Base_C's DoorState)."""
from contextlib import ExitStack
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import genueapi

DOOR_ENUM = ('// UserDefinedEnum DroppodDoorState.DroppodDoorState\n'
             '// NumValues: 0x0004\n'
             'enum class EDroppodDoorState : uint8\n{\n'
             '\tClosed                                   = 0,\n'
             '\tOuterOpen                                = 1,\n'
             '\tOpen                                     = 2,\n'
             '\tDroppodDoorState_MAX                     = 3,\n};\n')
# The dump's own lines (CL 158958), the delegate and function as Dumper-7 writes them.
DROP_POD = ('// BlueprintGeneratedClass /Game/LevelElements/Droppod/BP_DropPod_Base.BP_DropPod_Base_C\n'
            'class ABP_DropPod_Base_C : public ADropPod\n{\npublic:\n'
            '\tEDroppodDoorState                             DoorState;                                         '
            '// 0x07A2(0x0001)(Edit, BlueprintVisible, Net, ZeroConstructor, DisableEditOnInstance, IsPlainOldData, '
            'RepNotify, NoDestructor, HasGetValueTypeHash, RepNotifyFunc=OnRep_DoorState)\n'
            '\tTMulticastInlineDelegate<void(EDroppodDoorState DoorState_0)> DoorStateChanged;                  '
            '// 0x07B8(0x0010)(Edit, BlueprintVisible, ZeroConstructor, DisableEditOnInstance, BlueprintAssignable, '
            'BlueprintCallable)\n'
            '\tvoid SetDoorState(EDroppodDoorState DoorState);\n};\n')
COOKED = {0: 'NewEnumerator0', 1: 'NewEnumerator1', 2: 'NewEnumerator2', 3: 'DroppodDoorState_MAX'}


def fresh():
    """genueapi's tables, empty for one test."""
    stack = ExitStack()
    for name, empty in (('BP_ENUMS', set()), ('GAPS', []), ('ENUMS', {}), ('GAME_ENUMS', {}), ('GAME_ENUM_PATHS', {}),
                        ('QUALIFIED', {}), ('STRUCTS', {}), ('GAME_STRUCT_NAMES', {})):
        stack.enter_context(patch.object(genueapi, name, empty))
    return stack


def parse(text, name='Types_structs.hpp'):
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / name
        path.write_text(text)
        if name.endswith('_classes.hpp'):
            return genueapi.parse_header(str(path))[0]
        genueapi.parse_structs(str(path), name.split('_')[0])


class BlueprintEnumTests(unittest.TestCase):
    def test_static_enum_fallback_accepts_qualified_and_plain_names(self):
        for spelling in ('ELogLevel', 'ELogLevel::ELogLevel'):
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                enums = root / 'ELogLevel_structs.hpp'
                enums.write_text('// UserDefinedEnum ELogLevel.ELogLevel\n'
                                 f'enum class {spelling} : uint8\n{{\n\tInfo = 1,\n}};\n'
                                 '// Enum Engine.ENative\nenum class Engine::ENative : uint8\n{\n};\n')
                classes = root / 'EmmApi_classes.hpp'
                classes.write_text('// BlueprintGeneratedClass /Game/_ElytrasMods/Api/EmmApi.EmmApi_C\n'
                                   'class UEmmApi_C\n{\n'
                                   f'\tstatic void Log({spelling} Level);\n'
                                   f'\tvoid InstanceLog({spelling} Level);\n}};\n')
                with fresh():
                    genueapi.parse_structs(str(enums), 'ELogLevel')
                    self.assertEqual(list(genueapi.ENUMS), ['ENative'])     # a qualified native keeps its plain name
                    found, _, _ = genueapi.parse_header(str(classes))
                    self.assertEqual(found[0].funcs, [(True, 'void', 'Log', [('uint8', 'Level')])])
                    self.assertTrue(any(row[2] == 'InstanceLog' for row in genueapi.GAPS))

    def drop_pod(self, content):
        """parse_structs + game_enum_types + parse_header over the drop pod's enum and class: (Klass, the enum or None)."""
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'DroppodDoorState_structs.hpp').write_text(DOOR_ENUM)
            (root / 'BP_DropPod_Base_classes.hpp').write_text(DROP_POD)
            genueapi.parse_structs(str(root / 'DroppodDoorState_structs.hpp'), 'DroppodDoorState')
            genueapi.GAME_ENUM_PATHS[('DroppodDoorState', 'DroppodDoorState')] = ['/Game/Art/DropPod/DroppodDoorState']
            with patch.object(genueapi, 'cooked_enum_names', lambda c, p: COOKED if c else None):
                genueapi.game_enum_types(('/Game/_ElytrasMods/Mine/',), content)
            found, _, _ = genueapi.parse_header(str(root / 'BP_DropPod_Base_classes.hpp'))
            return found[0], genueapi.ENUMS.get('EDroppodDoorState')

    def test_game_enum_typed_with_its_members(self):
        with patch.object(genueapi, 'BP_ENUMS', set()), patch.object(genueapi, 'GAPS', []), \
                patch.object(genueapi, 'ENUMS', {}), patch.object(genueapi, 'GAME_ENUMS', {}), \
                patch.object(genueapi, 'GAME_ENUM_PATHS', {}):
            k, en = self.drop_pod('Content')
            self.assertEqual((en.path, en.pkg, en.ue_name), ('/Game/Art/DropPod/DroppodDoorState', 'Game/EDroppodDoorState',
                                                             'DroppodDoorState'))
            self.assertEqual(en.values, [('Closed', 0), ('OuterOpen', 1), ('Open', 2)])     # no _MAX: the engine adds it
            text = genueapi.emit_enum(en)
            self.assertIn('UE_ENUM_IN(EDroppodDoorState, "/Game/Art/DropPod");', text)
            self.assertIn('EDroppodDoorState__UeName = "DroppodDoorState";', text)
            self.assertIn('EDroppodDoorState__UeEnumNames = '
                          '"Closed=NewEnumerator0|OuterOpen=NewEnumerator1|Open=NewEnumerator2";', text)
            self.assertIn(('EDroppodDoorState', 'DoorState'), k.fields)
            self.assertIn(('TMulticastInlineDelegate<void(EDroppodDoorState DoorState_0)>', 'DoorStateChanged'), k.fields)
            self.assertIn((False, 'void', 'SetDoorState', [('EDroppodDoorState', 'DoorState')]), k.funcs)
            self.assertEqual(genueapi.GAPS, [])
            self.assertEqual(genueapi.game_enum_words('TArray<EDroppodDoorState>'), ['EDroppodDoorState'])

    def test_game_enum_max_named_off_prefix(self):
        """Minehead_State's sentinel is Minehead_MAX (the engine's prefix), not Minehead_State_MAX: still left out."""
        with tempfile.TemporaryDirectory() as temp, patch.object(genueapi, 'BP_ENUMS', set()), \
                patch.object(genueapi, 'GAPS', []), patch.object(genueapi, 'ENUMS', {}), \
                patch.object(genueapi, 'GAME_ENUMS', {}), patch.object(genueapi, 'GAME_ENUM_PATHS', {}):
            path = Path(temp) / 'Minehead_State_structs.hpp'
            path.write_text('// UserDefinedEnum Minehead_State.Minehead_State\nenum class EMinehead_State : uint8\n{\n'
                            '\tUnfolded = 0,\n\tFolded = 1,\n\tMinehead_MAX = 2,\n};\n')
            genueapi.parse_structs(str(path), 'Minehead_State')
            genueapi.GAME_ENUM_PATHS[('Minehead_State', 'Minehead_State')] = ['/Game/LevelElements/Minehead/Minehead_State']
            cooked = {0: 'NewEnumerator0', 1: 'NewEnumerator1', 2: 'Minehead_MAX'}
            with patch.object(genueapi, 'cooked_enum_names', lambda c, p: cooked):
                genueapi.game_enum_types((), 'Content')
            self.assertEqual(genueapi.ENUMS['EMinehead_State'].values, [('Unfolded', 0), ('Folded', 1)])

    def test_game_enum_needs_game(self):
        """Without --game the cooked names are unknown: the enum and its members stay out, as before."""
        with patch.object(genueapi, 'BP_ENUMS', set()), patch.object(genueapi, 'GAPS', []), \
                patch.object(genueapi, 'ENUMS', {}), patch.object(genueapi, 'GAME_ENUMS', {}), \
                patch.object(genueapi, 'GAME_ENUM_PATHS', {}):
            k, en = self.drop_pod(None)
            self.assertIsNone(en)
            self.assertEqual(k.fields, [])
            self.assertIn(('game enum', 'no --game'), [(g[0], g[4]) for g in genueapi.GAPS])


class SharedNameTests(unittest.TestCase):
    """Types whose name another one takes, which Dumper-7 qualifies, named by the user's rule: a native keeps the plain
    name, a game Blueprint's goes in its /Game path's namespace, as a game Blueprint class sharing a name does."""

    def test_native_keeps_its_name_blueprint_namespaced(self):
        with fresh():
            parse('// Enum Engine.EModuleType\nenum class Engine::EModuleType : uint8\n{\n\tEMT_A = 0,\n};\n'
                  '// UserDefinedEnum EModuleType.EModuleType\nenum class EModuleType::EModuleType : uint8\n{\n'
                  '\tNone = 0,\n\tCarver = 1,\n\tEModuleType_MAX = 2,\n};\n')
            genueapi.GAME_ENUM_PATHS[('EModuleType', 'EModuleType')] = ['/Game/Mods/Enemy/EModuleType']
            cooked = {0: 'NewEnumerator0', 1: 'NewEnumerator1', 2: 'EModuleType_MAX'}
            with patch.object(genueapi, 'cooked_enum_names', lambda c, p: cooked):
                genueapi.game_enum_types((), 'Content')
            self.assertEqual(sorted(genueapi.ENUMS), ['EModuleType', 'Game::Mods::Enemy::EModuleType'])
            k = parse('// BlueprintGeneratedClass /Game/Mods/Enemy/Module.Module_C\nclass AModule_C : public AActor\n{\n'
                      '\tEngine::EModuleType                           Native;                                            '
                      '// 0x0228(0x0001)(Edit, BlueprintVisible)\n'
                      '\tEModuleType::EModuleType                      Bp;                                                '
                      '// 0x0229(0x0001)(Edit, BlueprintVisible)\n};\n', 'Module_classes.hpp')[0]
            self.assertEqual(k.fields, [('EModuleType', 'Native'), ('Game::Mods::Enemy::EModuleType', 'Bp')])
            text = genueapi.emit_enum(genueapi.ENUMS['Game::Mods::Enemy::EModuleType'])
            self.assertIn('namespace Game { namespace Mods { namespace Enemy {\nenum class EModuleType : uint8', text)
            self.assertIn('UE_ENUM_IN(EModuleType, "/Game/Mods/Enemy");', text)
            self.assertEqual(genueapi.ENUMS['Game::Mods::Enemy::EModuleType'].pkg, 'Game/EModuleType')
            # Spelled from the root in a header: inside a Blueprint's own namespace a bare Game:: may be another one.
            self.assertEqual(genueapi.rooted('TEnum<Game::Mods::Enemy::EModuleType>'),
                             'TEnum<::Game::Mods::Enemy::EModuleType>')

    def test_blueprint_enum_pair_by_full_path(self):
        """Dumper-7's bFullAssetPaths comment gives each one's package, so each takes its own folder's namespace."""
        with fresh():
            parse('// UserDefinedEnum /Game/A/ELevel.ELevel\nenum class ELevel_0::ELevel : uint8\n{\n\tLow = 0,\n'
                  '\tELevel_MAX = 1,\n};\n'
                  '// UserDefinedEnum /Game/B/ELevel.ELevel\nenum class ELevel::ELevel : uint8\n{\n\tHigh = 0,\n'
                  '\tELevel_MAX = 1,\n};\n')
            with patch.object(genueapi, 'cooked_enum_names', lambda c, p: {0: 'NewEnumerator0', 1: 'ELevel_MAX'}):
                genueapi.game_enum_types((), 'Content')
            self.assertEqual(genueapi.QUALIFIED, {'ELevel_0::ELevel': 'Game::A::ELevel', 'ELevel::ELevel': 'Game::B::ELevel'})
            self.assertEqual(genueapi.ENUMS['Game::A::ELevel'].values, [('Low', 0)])
            self.assertEqual(genueapi.map_type('ELevel::ELevel'), 'Game::B::ELevel')

    def test_blueprint_enum_pair_without_paths_left_out(self):
        """Without bFullAssetPaths a pair's short name names both packages, and `_0` follows no package order."""
        with fresh():
            parse('// UserDefinedEnum ELevel.ELevel\nenum class ELevel_0::ELevel : uint8\n{\n\tLow = 0,\n};\n')
            genueapi.GAME_ENUM_PATHS[('ELevel', 'ELevel')] = ['/Game/A/ELevel', '/Game/B/ELevel']
            genueapi.game_enum_types((), 'Content')
            self.assertEqual(genueapi.ENUMS, {})
            self.assertEqual([g[4] for g in genueapi.GAPS], ['shared name'])

    def test_blueprint_struct_pair(self):
        with fresh():
            parse('// UserDefinedStruct /Game/A/Shared/Pair.Pair\n// 0x0004 (0x0004 - 0x0000)\nstruct Pair_0::FPair final\n'
                  '{\npublic:\n\tint32 Count_2_0123;  // 0x0000(0x0004)(Edit)\n};\n'
                  '// UserDefinedStruct /Game/B/Pair.Pair\n// 0x0004 (0x0004 - 0x0000)\nstruct Pair::FPair final\n'
                  '{\npublic:\n\tint32 Count_2_4567;  // 0x0000(0x0004)(Edit)\n};\n'
                  '// UserDefinedStruct /Game/9Lives/Pair.Pair\n// 0x0004 (0x0004 - 0x0000)\nstruct Pair_1::FPair final\n'
                  '{\npublic:\n\tint32 Count_2_89AB;  // 0x0000(0x0004)(Edit)\n};\n')
            # /Game/9Lives is no C++ namespace, so that one has no type.
            self.assertEqual(sorted(genueapi.STRUCTS), ['Game::A::Shared::FPair', 'Game::B::FPair'])
            self.assertEqual({s.pkg for s in genueapi.STRUCTS.values()}, {'Game/FPair'})
            self.assertEqual(genueapi.map_type('struct Pair_0::FPair'), 'Game::A::Shared::FPair')
            self.assertEqual(genueapi.type_words('TArray<Game::B::FPair>', 'F'), ['Game::B::FPair'])


if __name__ == '__main__':
    unittest.main()
