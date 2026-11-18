"""apisurface.check on hand-made surfaces: which change needs which bump. python test_apisurface.py"""
import copy
from apisurface import check

FN = dict(flags=['Static'], params=[['X', 'Int']], returns=[])
SURF = dict(classes={'/Game/A.A_C': dict(super='/Script/Engine.Actor', interfaces=[], functions={'F': FN},
                                          properties={'P': 'Int'})},
            structs={'/Game/S.S': {'X_1': 'Int'}}, enums={'/Game/E.E': ['E::A=0', 'E::B=1']})
BASE = {'Core': dict(version=[1, 0], surface=SURF)}


def problems(edit, version):
    now = copy.deepcopy(SURF)
    edit(now)
    return check(BASE, {'Core': now}, {'Core': version})


def main():
    same = lambda s: None
    add_fn = lambda s: s['classes']['/Game/A.A_C']['functions'].update(G=FN)
    add_enum = lambda s: s['enums']['/Game/E.E'].append('E::C=2')
    drop_fn = lambda s: s['classes']['/Game/A.A_C']['functions'].pop('F')
    retype = lambda s: s['classes']['/Game/A.A_C']['properties'].update(P='Float')
    add_field = lambda s: s['structs']['/Game/S.S'].update(Y_2='Int')
    assert not problems(same, [1, 0])
    for additive in (add_fn, add_enum):
        assert problems(additive, [1, 0]) and problems(additive, [1, 0])[-1].endswith('bump CoreMinor')
        assert not problems(additive, [1, 1]) and not problems(additive, [2, 0])
    for breaking in (drop_fn, retype, add_field):
        assert problems(breaking, [1, 1])[-1].endswith('bump CoreMajor')
        assert not problems(breaking, [2, 0])
    assert 'older than' in problems(same, [0, 9])[0]
    print('test_apisurface: ok')


if __name__ == '__main__':
    main()
