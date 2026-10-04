"""analyze.py <exe> <out.json> : function discovery + sizing report for the recompiler.
Seeds: entry point, direct call targets, .rdata code pointers (vtables etc.), validated .data code pointers, function
starts after int3 padding. Recursive descent per function; jump tables resolved for MSVC patterns."""
import struct, sys, os, json, collections, capstone
from capstone import x86 as X
sys.path.insert(0, os.path.dirname(__file__)); from pe import PE, code_range
pe = PE(sys.argv[1]); TLO, THI, code = code_range(pe)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
cache = {}
def ins_at(va):
    if va in cache: return cache[va]
    r = None
    if TLO <= va < THI:
        for i in md.disasm(code[va - TLO:va - TLO + 16], va, 1): r = i
    cache[va] = r; return r
BAD = {'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'iretd', 'hlt', 'cli', 'sti', 'int1', 'into', 'aaa', 'aas', 'aam',
       'aad', 'salc', 'les', 'lds', 'retf', 'ljmp', 'lcall', 'arpl', 'bound', 'daa', 'das'}
def plausible(va, n=12):          # a clean decode of the first n instructions with no privileged/obsolete ops
    for _ in range(n):
        i = ins_at(va)
        if i is None or i.mnemonic in BAD: return False
        if i.mnemonic in ('ret', 'jmp'): return True
        va += i.size
    return True
def in_text(blob, k):             # the dword at k sits inside an ASCII/UTF-16 string, not a pointer table
    w = blob[max(0, k - 8):k + 12]
    def u16(o): return o + 4 <= len(blob) and blob[o + 1] == 0 and blob[o + 3] == 0 and all(32 <= blob[o + j] < 127 or blob[o + j] == 0 for j in (0, 2))
    if u16(k) and 32 <= blob[k] < 127 and (u16(k + 4) or (k >= 4 and u16(k - 4))): return True   # UTF-16 text
    def alnum3(j): return all(chr(b).isalnum() and b < 128 for b in blob[j:j + 3])   # e.g. "ENI\0", a 3-letter code
    ptrs = sum(1 for j in range(max(0, k - 8), min(len(blob) - 3, k + 12), 4)
               if TLO <= int.from_bytes(blob[j:j + 4], 'little') < THI and not alnum3(j))
    if ptrs >= 3: return False                    # a table of code pointers, not ASCII text
    return len(w) >= 16 and all(32 <= b < 127 or b in (0, 9, 10, 13) for b in w) and sum(32 <= b < 127 for b in w) >= len(w) // 2
seeds = {pe.entry: 'entry'}; cands = {}
# exported functions: the engine's function binder (FuBi) calls them through the export address table
_erva = pe.dirs[0][0]
if _erva:
    _ed = pe.base + _erva
    for _k in range(pe.u32(_ed + 20)):
        _v = pe.u32(pe.base + pe.u32(_ed + 28) + 4 * _k)
        if _v and TLO <= pe.base + _v < THI: seeds.setdefault(pe.base + _v, 'export')
OBS = os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), 'observed_targets.json')
if os.path.exists(OBS):                      # indirect-branch targets observed at run time (tests/startup.py DISCOVER=)
    for v in json.load(open(OBS)): seeds.setdefault(v, 'observed')
for name, sva, vs, ra, rs, fl in pe.sections:
    if name not in ('.rdata', '.data'): continue
    blob = pe.bytes_at(sva, min(vs, rs))
    for k in range(0, len(blob) - 3, 4):
        v = int.from_bytes(blob[k:k + 4], 'little')
        if TLO <= v < THI and v not in seeds and plausible(v): cands.setdefault(v, 'ptr' + name)
funcs = {}; work = list(seeds); ptrsrc = dict(seeds)
jumptables = []; jt_unresolved = []; decode_fail = []
def jumptable(i, prev):            # jmp dword ptr [reg*4 + table]: bound from a preceding cmp reg, N / ja
    op = i.operands[0]
    if op.type != X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0: return None
    table = op.mem.disp & 0xffffffff; n = None
    for p in reversed(prev[-6:]):
        if p.mnemonic == 'cmp' and p.operands[1].type == X86_OP_IMM: n = p.operands[1].imm + 1; break
    if n is None or n > 2048: return table, None
    return table, [pe.u32(table + 4 * k) for k in range(n)]
X86_OP_MEM, X86_OP_IMM = X.X86_OP_MEM, X.X86_OP_IMM
def lift_func(f):
    blocks = set(); insns = {}; stack = [f]; prev = []
    while stack:
        va = stack.pop(); prev = []
        while va not in insns:
            i = ins_at(va)
            if i is None: decode_fail.append(va); break
            insns[va] = i; prev.append(i); nxt = va + i.size
            mn = i.mnemonic; op0 = i.operands[0] if i.operands else None
            if mn not in ('call', 'jmp') and not mn.startswith('j') and not mn.startswith('loop'):
                for o in i.operands:                       # code addresses as immediates: callbacks, thread procs, window procs
                    v = (o.imm if o.type == X86_OP_IMM else o.mem.disp if o.type == X86_OP_MEM and not o.mem.base and not o.mem.index else 0) & 0xffffffff
                    if o.type == X86_OP_IMM and TLO <= v < THI and v not in funcs and v not in cands and plausible(v): cands[v] = 'imm'
            if mn == 'call':
                if op0.type == X86_OP_IMM:
                    tgt = op0.imm & 0xffffffff
                    if tgt not in funcs and tgt not in ptrsrc: ptrsrc[tgt] = 'call'; work.append(tgt)
            elif mn == 'jmp':
                if op0.type == X86_OP_IMM: stack.append(op0.imm & 0xffffffff)
                else:
                    jt = jumptable(i, prev)
                    if jt and jt[1]:
                        jumptables.append({'at': va, 'table': jt[0], 'targets': jt[1]}); stack.extend(t for t in jt[1] if TLO <= t < THI)
                    elif jt: jt_unresolved.append(va)
                break
            elif mn[0] == 'j' or mn.startswith('loop') or mn == 'jecxz':
                stack.append(op0.imm & 0xffffffff)
            elif mn in ('ret', 'int3', 'hlt', 'ud2'): break
            va = nxt
    return insns
while work:
    f = work.pop()
    if f in funcs or not (TLO <= f < THI): continue
    funcs[f] = lift_func(f)
def validate_candidates():
    starts = set(); cov = set()
    for ins in funcs.values():
        for va, i in ins.items(): starts.add(va); cov.update(range(va, va + i.size))
    added = 0
    for v, src in sorted(cands.items()):
        if v in funcs: continue
        if v in starts: ok = True                                      # an instruction boundary of known code
        elif v in cov: ok = False                                      # inside an instruction: not a code pointer
        else: ok = True                                                # undiscovered code: keep (junk is harmless)
        if ok: ptrsrc[v] = src; work.append(v); added += 1
    return added
while True:
    n = validate_candidates()
    while work:
        f = work.pop()
        if f in funcs or not (TLO <= f < THI): continue
        funcs[f] = lift_func(f)
    if not n: break
IS_DLL = bool(struct.unpack_from("<H", pe.data, struct.unpack_from("<I", pe.data, 0x3c)[0] + 22)[0] & 0x2000)
for gap_round in range(256 if IS_DLL else 1):   # the executable: one pass (its analysis is verified as is)
    nf = len(funcs)
    covered = set()
    for ins in funcs.values():
        for va, i in ins.items(): covered.update(range(va, va + i.size))
    # gaps: int3-padding-delimited candidates
    gap_starts = []
    va = TLO
    while va < THI:
        if va in covered: va += 1; continue
        s = va
        while va < THI and va not in covered: va += 1
        g = code[s - TLO:va - TLO]
        if g.strip(b'\xcc\x90'): gap_starts.append((s, va - s))
    pad_cands = []
    for s, n in gap_starts:
        p = s
        while p < s + n and code[p - TLO] in (0xcc, 0x90): p += 1
        if p < s + n and plausible(p): pad_cands.append(p)
    for p in pad_cands:
        if p not in funcs: ptrsrc[p] = 'gap'; work.append(p)
    while work:
        f = work.pop()
        if f in funcs or not (TLO <= f < THI): continue
        funcs[f] = lift_func(f)
    if len(funcs) == nf: break
covered = set(); mix = collections.Counter(); m80 = []; seh_set = []; seh_push = []; raise_calls = []
for f, ins in funcs.items():
    for va, i in ins.items():
        covered.update(range(va, va + i.size)); mix[i.mnemonic] += 1
        s = i.op_str
        if 'xword' in s or 'tbyte' in s: m80.append((va, i.mnemonic, s))
        if 'fs:[0]' in s:
            if i.mnemonic == 'mov' and s.startswith('dword ptr fs:[0]'): seh_set.append(f)
            if i.mnemonic == 'push': seh_push.append(f)
        if i.mnemonic == 'call' and i.operands[0].type == X86_OP_MEM and i.operands[0].mem.base == 0:
            imp = pe.imports.get(i.operands[0].mem.disp & 0xffffffff, '')
            if imp.endswith('!RaiseException'): raise_calls.append((f, va))
print('functions %d  coverage %.1f%% of .text  instructions %d  distinct mnemonics %d' % (len(funcs), 100.0 * len(covered) / (THI - TLO), sum(len(v) for v in funcs.values()), len(mix)))
print('seed sources:', dict(collections.Counter(ptrsrc[f] for f in funcs if f in ptrsrc)))
print('decode failures: %d at %s' % (len(set(decode_fail)), sorted(set(decode_fail))[:10]))
print('jump tables resolved %d, unresolved %d %s' % (len(jumptables), len(jt_unresolved), [hex(x) for x in jt_unresolved[:10]]))
print('SEH: functions writing fs:[0] %d, pushing fs:[0] %d; RaiseException call sites %d in functions %s' % (len(set(seh_set)), len(set(seh_push)), len(raise_calls), sorted({hex(f) for f, _ in raise_calls})))
print('80-bit memory operands: %d %s' % (len(m80), m80[:8]))
print('mnemonics:', ' '.join('%s:%d' % kv for kv in mix.most_common()))
json.dump({'funcs': sorted(funcs), 'jumptables': jumptables, 'raise': raise_calls, 'seh_funcs': sorted(set(seh_push))}, open(sys.argv[2], 'w'))
