"""The rules of invariants.py, one module per area of what UE 4.27 relies on in a cooked Blueprint class. Each module
imports the core (`from invariants import *`) and registers its rules in invariants.RULES as it loads; invariants.py
imports this package last. The areas came out of a survey of the engine source against the suite: each rule's
docstring cites the file:line that makes it an invariant, and each was calibrated on the game's own cooked packages.

_fn_sdk / _vm_sdk read the engine's own (/Script) classes off a Dumper-7 dump of the game (invariants.SDK_DUMP), which
the cooked packages do not carry; without one, the rules that need them skip native operands."""
from invariant_rules import (preload, tables, class_tail, functions, operands, operand_types, vm_semantics, ubergraph,
                             delegates, scs, components, properties, user_types, replication, latent, edits)
