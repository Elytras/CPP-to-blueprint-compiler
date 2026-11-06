"""Blueprint interface contracts must survive the cooked-class to C++ header join."""
import unittest
from unittest.mock import patch

import genueapi


class BlueprintInterfaceTests(unittest.TestCase):
    def test_interfaces_are_joined_by_full_path(self):
        actor = genueapi.Klass('AActorBase_C', 'AActor', '/Game/Base/ActorBase', 'ActorBase_C', True)
        ours = genueapi.Klass('IIMod_C', '', '/Game/Api/IMod', 'IMod_C', True)
        other = genueapi.Klass('IIMod_0_C', '', '/Game/Other/IMod', 'IMod_C', True)
        classes = [actor, other, ours]
        got = genueapi.blueprint_interfaces(classes, {cls.cpp: cls for cls in classes}, {
            '/Game/Base/ActorBase.ActorBase_C': ['/Game/Api/IMod.IMod_C']})
        self.assertEqual(got, {'AActorBase_C': ['IIMod_C']})

    def test_child_does_not_redeclare_an_inherited_interface(self):
        parent = genueapi.Klass('AParent_C', 'AActor', '/Game/Parent', 'Parent_C', True)
        child = genueapi.Klass('AChild_C', 'AParent_C', '/Game/Child', 'Child_C', True)
        interface = genueapi.Klass('IIMod_C', '', '/Game/IMod', 'IMod_C', True)
        classes = [parent, child, interface]
        got = genueapi.blueprint_interfaces(classes, {cls.cpp: cls for cls in classes}, {
            '/Game/Parent.Parent_C': ['/Game/IMod.IMod_C'],
            '/Game/Child.Child_C': ['/Game/IMod.IMod_C']})
        self.assertEqual(got, {'AParent_C': ['IIMod_C']})

    def test_native_interface_uses_the_cpp_interface_not_its_uobject_class(self):
        actor = genueapi.Klass('AActorBase_C', 'AActor', '/Game/Base/ActorBase', 'ActorBase_C', True)
        interface = genueapi.Klass('IUsable', '', '/Script/FSD', 'Usable', False)
        object_class = genueapi.Klass('UUsable', 'UInterface', '/Script/FSD', 'Usable', False)
        classes = [actor, interface, object_class]
        got = genueapi.blueprint_interfaces(classes, {cls.cpp: cls for cls in classes}, {
            '/Game/Base/ActorBase.ActorBase_C': ['/Script/FSD.Usable']})
        self.assertEqual(got, {'AActorBase_C': ['IUsable']})

    def test_call_on_self_lists_the_native_interface_on_the_native_ancestor(self):
        pawn = genueapi.Klass('ASpiderEnemy', 'AActor', '/Script/FSD', 'SpiderEnemy', False)
        bp = genueapi.Klass('AENE_Spider', 'ASpiderEnemy', '/Game/ENE_Spider', 'ENE_Spider_C', True)
        listed = genueapi.Klass('AENE_Listed', 'ASpiderEnemy', '/Game/ENE_Listed', 'ENE_Listed_C', True)
        iface = genueapi.Klass('INetMontageAble', '', '/Script/FSD', 'NetMontageAble', False)
        iface.funcs = [(False, 'float', 'QueueMontage', [])]
        classes = [pawn, bp, listed, iface]
        by_name = {cls.cpp: cls for cls in classes}
        game = {'/Game/ENE_Spider.ENE_Spider_C': (0, 0, 0, '/Script/FSD.SpiderEnemy'),
                '/Game/ENE_Listed.ENE_Listed_C': (0, 0, 0, '/Script/FSD.SpiderEnemy')}
        calls = {('/Game/ENE_Spider.ENE_Spider_C', 'QueueMontage'), ('/Game/ENE_Spider.ENE_Spider_C', 'K2_DestroyActor')}
        self.assertEqual(genueapi.native_interfaces(classes, by_name, game, set(), calls), {'ASpiderEnemy': ['INetMontageAble']})
        # A Blueprint listing the interface itself says nothing of its native parent.
        with patch.object(genueapi, 'GAME_INTERFACES', {'/Game/ENE_Listed.ENE_Listed_C': ['/Script/FSD.NetMontageAble']}):
            self.assertEqual(genueapi.native_interfaces(classes, by_name, game, set(), {
                ('/Game/ENE_Listed.ENE_Listed_C', 'QueueMontage')}), {})
        # Nor does a call to a function of the Blueprint's own of that name.
        with patch.object(genueapi, 'GAME_FUNCTIONS', {'/Game/ENE_Listed.ENE_Listed_C': {'QueueMontage'}}):
            self.assertEqual(genueapi.native_interfaces(classes, by_name, game, set(), {
                ('/Game/ENE_Listed.ENE_Listed_C', 'QueueMontage')}), {})

    def test_missing_interface_is_reported_as_a_generation_gap(self):
        actor = genueapi.Klass('AActorBase_C', 'AActor', '/Game/Base/ActorBase', 'ActorBase_C', True)
        with patch.object(genueapi, 'GAPS', []):
            self.assertEqual(genueapi.blueprint_interfaces([actor], {actor.cpp: actor}, {
                '/Game/Base/ActorBase.ActorBase_C': ['/Game/Missing.IMissing_C']}), {})
            self.assertEqual(genueapi.GAPS[0][0], 'interface')
            self.assertEqual(genueapi.GAPS[0][3], '/Game/Missing.IMissing_C')


if __name__ == '__main__':
    unittest.main()
