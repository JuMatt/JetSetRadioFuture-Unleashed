"""Reproduce small, audited injections without editing the generated source.

The ignored derived include is a faithful clone of the input function, with local
register aliases expanded. Keep original guest calls/flags/history and tail.
Fail closed on any preimage drift. Run --check in builds/tests.
"""
from pathlib import Path
import argparse, hashlib, re
root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gen-dir', type=Path, default=root/'recomp/gen')
parser.add_argument('--check', action='store_true')
parser.add_argument('--output', type=Path, default=root/'jsrf_camera_native.inc')
args = parser.parse_args()
source_path = args.gen_dir/'recomp_0002.c'
if not source_path.is_file():
    parser.error(f'Generate code from your own executable first: {source_path} is missing')
source = source_path.read_text(encoding='utf-8')
def extract(name):
    signature = f'void {name}(void)\n{{'
    generated_signature = f'void {name}_gen(void)\n{{'
    actual = signature if signature in source else generated_signature
    if actual not in source:
        parser.error(f'Missing generated function {name} (or its _gen alias)')
    start = source.index(actual)
    end = source.index('\n}\n',start)+3
    return source[start:end].replace(actual,signature,1)
movement = extract('sub_000A76E0')
preimages = {'sub_000A76E0': '17357e159d94ed7904f3e1689633a54a500b84a0eb7384aa22797d60ea2b72a6'}
for name, body in [('sub_000A76E0',movement)]:
    sha = hashlib.sha256(body.encode()).hexdigest()
    if sha != preimages[name]: raise SystemExit(f'Unexpected preimage for {name}: {sha}')
def once(text, old, new):
    assert text.count(old)==1,old
    return text.replace(old,new)
movement = once(movement,'void sub_000A76E0(void)\n{','static void jcx_native_input(void)\n{\n    uint32_t jcx_heading=0; int jcx_override=0;')
movement = once(movement,'loc_000A7990: ;','''loc_000A7990: ;
    jcx_override = jcx_input_heading(esi, ebp, &jcx_heading);
    if (jcx_override) goto loc_000A7A91; /* local selection; no guest camera writes */''')
movement = once(movement,'loc_000A7A91: ;\n    eax = MEM32(ebp + 0x228);','loc_000A7A91: ;\n    eax = jcx_override ? jcx_heading : MEM32(ebp + 0x228);')
movement = once(movement,'loc_000A7AEB: ;\n    SET_LO16(ebp, MEM16(ebp + 0x228));','loc_000A7AEB: ;\n    SET_LO16(ebp, jcx_override ? (uint16_t)jcx_heading : MEM16(ebp + 0x228));')
out = '/* Reproduced by prepare_camera_native.py. See preimage hashes and local injections there. */\n#include "recomp/gen/recomp_funcs.h"\n'+movement
out=re.sub(r'\b(eax|ecx|edx|esp|ebx|esi|edi)\b',lambda m:'g_'+m[0],out)
path=args.output
if args.check:
    if not path.is_file() or path.read_text(encoding='utf-8') != out:
        raise SystemExit('Camera native include is stale; rerun prepare_camera_native.py')
else: path.write_text(out,encoding='utf-8')
