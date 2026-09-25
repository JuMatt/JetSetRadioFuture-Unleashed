"""
A jcc whose flags come from a predecessor further down the function.

Run: py -3 -m pytest tools/recomp/test_flag_state_back_edge.py

Blocks are emitted in address order. A block reached both by falling in from
above and by a jump back up from below used to be lifted before the lower
predecessor had any flag state, and "unknown" became the unassigned _flags
fallback -- a condition that is always false.

JSRF's stage-trigger ops 0x04 and 0x05 share exactly this tail: op 0x04 falls
into `test eax, eax` / `je skip`, op 0x05 does `test ecx, ecx` and jumps back up
to the `je`. The je never fired, "has the player walked into Gum's zone?" always
answered yes, and her lesson started the moment Corn stopped talking -- with the
camera parked on her for the whole tutorial. 119 jcc sites came out this way.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000

#  +0  85 C0   test eax, eax
#  +2  75 08   jne  +12           (reaches the arm below)
#  +4  85 D2   test edx, edx
#  +6  74 03   je   +11           <- also the target of the jmp at +14
#  +8  B0 01   mov  al, 1
# +10  C3      ret
# +11  C3      ret
# +12  85 C9   test ecx, ecx
# +14  EB F6   jmp  +6            (back up, into the je)
CODE = bytes.fromhex("85C0" "7508" "85D2" "7403" "B001" "C3" "C3" "85C9" "EBF6")


def _translate(code):
    config._install(
        [config.Section(".text", BASE, len(code), 0x0000, len(code), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-back-edge-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(code),
                 "_addr": BASE, "size": len(code)}}
    return FunctionTranslator(code, db).translate_function(BASE, db[BASE])


def test_back_edge_predecessor_supplies_the_flags():
    c = _translate(CODE)
    je = [l for l in c.splitlines() if "je:" in l and "goto" in l]
    assert je, c
    assert "_flags" not in je[0], je[0]
    assert "TEST_Z(_fa, _fb)" in je[0], je[0]


def test_both_arms_leave_a_snapshot_for_it():
    c = _translate(CODE)
    assert "/* test edx, edx" in c, c
    assert "/* test ecx, ecx" in c, c


if __name__ == "__main__":
    test_back_edge_predecessor_supplies_the_flags()
    test_both_arms_leave_a_snapshot_for_it()
    print("all passed")
