"""Regression for a static library call whose dumped Blueprint enum has a qualified name."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import genueapi


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
                with patch.object(genueapi, 'BP_ENUMS', set()), patch.object(genueapi, 'GAPS', []), \
                        patch.object(genueapi, 'ENUMS', {}):
                    genueapi.parse_structs(str(enums), 'ELogLevel')
                    self.assertEqual(genueapi.ENUMS, {})
                    found, _, _ = genueapi.parse_header(str(classes))
                    self.assertEqual(found[0].funcs, [(True, 'void', 'Log', [('uint8', 'Level')])])
                    self.assertTrue(any(row[2] == 'InstanceLog' for row in genueapi.GAPS))


if __name__ == '__main__':
    unittest.main()
