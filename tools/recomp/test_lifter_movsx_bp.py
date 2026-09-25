import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _insn(mnemonic, op_str, operands):
    instruction = Instruction(0, 4, mnemonic, op_str, "")
    instruction.operands = operands
    return instruction


def _reg(name):
    return Operand(type="reg", reg=name)


class MovsxWordRegisterTest(unittest.TestCase):
    """movsx from a 16-bit register sign-extends for every one of them. bp
    and sp were missing from the list, so `movsx ecx, bp` came out as a plain
    16-bit read -- a zero extension -- and JSRF's keyframe angle interpolator
    (sub_0005D3B0) returned every negative X angle 65536 too large."""

    def test_every_word_register_is_sign_extended(self):
        for r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            with self.subTest(reg=r):
                out = Lifter().lift_instruction(
                    _insn("movsx", "ecx, " + r, [_reg("ecx"), _reg(r)]))
                self.assertEqual(len(out), 1)
                self.assertIn("SX16(", out[0])

    def test_movzx_still_zero_extends(self):
        out = Lifter().lift_instruction(
            _insn("movzx", "ecx, bp", [_reg("ecx"), _reg("bp")]))
        self.assertIn("ZX16(", out[0])
