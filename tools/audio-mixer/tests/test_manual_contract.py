from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(repo))
from tools.recomp.manual_scan import scan


def test_mixer_wrappers_are_visible_to_clean_generation():
    skip, wrap, referenced = scan(repo / "port/recomp_manual.c")
    assert {0x0013AEB0, 0x0013EB30} <= skip & wrap & referenced


if __name__ == "__main__":
    test_mixer_wrappers_are_visible_to_clean_generation()
    print("PASS: clean generation discovers both mixer wrappers and generated bodies")
