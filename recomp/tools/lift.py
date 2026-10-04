"""lift.py: statically recompile 32-bit x86 functions of DungeonSiege.exe into C (see runtime/rt.h for the model).

  lift.py <exe> <analysis.json> <outdir> [--only 0xADDR,... [--closure]] [--per-file N]

Writes <outdir>/funcs.h, <outdir>/fntab.c (address -> function table, import list) and <outdir>/lift_NNN.c.
Unsupported or junk instructions become rt_unhandled() traps; the report lists them."""
import sys, os, re, json, struct, argparse, collections, capstone
from capstone import x86 as X
sys.path.insert(0, os.path.dirname(__file__)); from pe import PE, code_range

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SZ = {1: 0, 2: 1, 4: 2}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
CC = {'o': 'CC_O', 'no': 'CC_NO', 'b': 'CC_B', 'ae': 'CC_AE', 'e': 'CC_E', 'ne': 'CC_NE', 'be': 'CC_BE', 'a': 'CC_A',
      's': 'CC_S', 'ns': 'CC_NS', 'p': 'CC_P', 'np': 'CC_NP', 'l': 'CC_L', 'ge': 'CC_GE', 'le': 'CC_LE', 'g': 'CC_G'}
# Points where the native build adds behaviour (runtime/win32/hooks.c). Each is an instruction boundary; the hook sees
# the registers and memory as the original code would, and does nothing unless its feature is turned on.
HOOKS = {
    0x0059000f: 'mood loaded: scale fog and frustum (draw distance)',
    0x0061d06c: 'Skrit compiler message (this, level, format, ...): printed with DS_SKRITLOG=1',
    0x004acb46: 'FuBi enum spec constructed: Legends of Aranna extends eJobAbstractType',
    0x005d1fcf: 'GoMind template jobs loaded: Legends of Aranna adds jat_approach',
    0x004036a8: 'FuBi enum spec constructed (second copy): Legends of Aranna extends eQueryTrait',
}
JUNK = {'in', 'out', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw', 'iretd', 'hlt', 'cli', 'sti', 'int1', 'into',
        'int', 'aaa', 'aas', 'aam', 'aad', 'daa', 'das', 'salc', 'les', 'lds', 'retf', 'ljmp', 'lcall', 'arpl', 'bound',
        'bndldx', 'bndstx', 'bndmov', 'bndcl', 'bndcu', 'bndmk', 'bnd call', 'bnd jmp', 'bnd ret', 'fbstp', 'fbld',
        'ud2', 'int3', 'sysenter', 'syscall', 'lock', 'wait'}

# Functions a native implementation can take over (runtime/win32/hooks.c rt_override): at the function's first
# instruction the runtime may perform the whole call (including the return) itself.
OVERRIDES = {
    0x005cfa0d: 'ToString(eJobAbstractType)',
    0x005cfa1e: 'FromString(const char*, eJobAbstractType&)',
    0x005cfa33: 'job type flag mask',
    0x0051f739: 'ToString(eQueryTrait)',
    0x005e179b: 'AIQuery::Is(Go const*, Go const*, eQueryTrait)',
    0x006dec5c: 'UIShell::ShowInterface(const gpstring&)',
    0x006dee75: 'UIShell::ShowGroup(const char*, bool, bool, const char*)',
    0x005d281f: 'GoMind::SDoJob(const JobReq&)',
    0x005abf1e: 'Rules::ChangeLife(Goid, float, DWORD)',
    0x004f0b34: 'UIGame GUI callback (const gpstring& message, UIWindow&)',
    0x004e72f3: 'UIGame: publish the in-game key commands to the input binder',
    0x00533eb3: 'GoAspect::Xfer(PersistContext&)',
    0x004997d7: 'UIGame: the campaign is won (end-of-game dialog)',
}

class Unsupported(Exception): pass

class Lifter:
    hooks = {}; overrides = {}
    def __init__(self, exe, analysis):
        self.pe = PE(exe); self.TLO, self.THI, self.code = code_range(self.pe)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); self.md.detail = True
        a = json.load(open(analysis)); self.funcs = set(a['funcs'])
        self.imports = sorted(self.pe.imports.items())                # [(iat_va, 'DLL!name')]
        self.imp_index = {va: k for k, (va, _) in enumerate(self.imports)}
        self.cache = {}; self.unsup = collections.Counter(); self.unres_jt = []; self.internal = []; self.discovered = []; self.tail_edges = collections.defaultdict(set); self.scc_of = {}
    def ins(self, va):
        if va in self.cache: return self.cache[va]
        r = None
        if self.TLO <= va < self.THI:
            for i in self.md.disasm(self.code[va - self.TLO:va - self.TLO + 16], va, 1): r = i
        self.cache[va] = r; return r

    # ---------- operands ----------
    def rreg(self, n):
        if n in R32: return n
        if n in R16: return '(%s & 0xffffu)' % R16[n]
        if n in R8L: return '(%s & 0xffu)' % R8L[n]
        if n in R8H: return '((%s >> 8) & 0xffu)' % R8H[n]
        raise Unsupported('reg ' + n)
    def wreg(self, n, v):
        if n in R32: return '%s = (uint32_t)(%s);' % (n, v)
        if n in R16: b = R16[n]; return '%s = (%s & 0xffff0000u) | ((uint32_t)(%s) & 0xffffu);' % (b, b, v)
        if n in R8L: b = R8L[n]; return '%s = (%s & 0xffffff00u) | ((uint32_t)(%s) & 0xffu);' % (b, b, v)
        if n in R8H: b = R8H[n]; return '%s = (%s & 0xffff00ffu) | (((uint32_t)(%s) & 0xffu) << 8);' % (b, b, v)
        raise Unsupported('reg ' + n)
    def addr(self, i, op, lea=False):
        m = op.mem; parts = []
        if m.base:
            n = i.reg_name(m.base)
            if n not in R32: raise Unsupported('16-bit address')
            parts.append(n)
        if m.index:
            n = i.reg_name(m.index)
            if n not in R32: raise Unsupported('16-bit address')
            parts.append(n if m.scale == 1 else '%s * %du' % (n, m.scale))
        if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = '(uint32_t)(%s)' % ' + '.join(parts)
        seg = i.reg_name(m.segment) if m.segment else None
        if seg == 'fs' and not lea: e = '(c->fs_base + %s)' % e
        elif seg in ('gs',) or (seg == 'fs' and lea): raise Unsupported('segment ' + seg)
        return e
    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG: return self.rreg(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM: return '0x%xu' % (op.imm & {1: 0xff, 2: 0xffff, 4: 0xffffffff}[size])
        return self.rmem(size, self.addr(i, op))
    def rmem(self, size, a):
        if size not in (1, 2, 4): raise Unsupported('operand size %d' % size)
        return {1: 'R8', 2: 'R16', 4: 'R32'}[size] + '(%s)' % a
    def wmem(self, size, a, v):
        if size not in (1, 2, 4): raise Unsupported('operand size %d' % size)
        return {1: 'W8', 2: 'W16', 4: 'W32'}[size] + '(%s, %s);' % (a, v)
    def wr(self, i, op, v):
        if op.type == X.X86_OP_REG: return self.wreg(i.reg_name(op.reg), v)
        return self.wmem(op.size, self.addr(i, op), v)
    def rmw(self, i, op):
        """(read expr, write fn, prelude) for a read-modify-write destination; memory address computed once"""
        if op.type == X.X86_OP_REG:
            n = i.reg_name(op.reg); return self.rreg(n), (lambda v: self.wreg(n, v)), ''
        a = self.addr(i, op); sz = op.size
        return self.rmem(sz, 'ea'), (lambda v: self.wmem(sz, 'ea', v)), 'uint32_t ea = %s; ' % a

    # ---------- function body ----------
    def collect(self, f, extra=()):
        insns = {}; stack = [f] + list(extra); own = {f} | set(extra)
        while stack:
            va = stack.pop()
            while va not in insns:
                if va not in own and va in self.funcs: break       # falls into another function: tail call
                i = self.ins(va)
                if i is None: break
                insns[va] = i; mn = i.mnemonic; nxt = va + i.size
                op0 = i.operands[0] if i.operands else None
                if mn == 'jmp':
                    if op0.type == X.X86_OP_IMM:
                        t = op0.imm & 0xffffffff
                        if t == f or t not in self.funcs: stack.append(t)
                    else:
                        jt = self.jumptable(i, insns, f)
                        if jt: stack.extend(jt[1])
                    break
                if mn in ('ret', 'retf', 'int3', 'hlt', 'ud2') or mn in JUNK and mn not in ('lock', 'wait'): break
                if (mn[0] == 'j' and mn != 'jmp') or mn.startswith('loop'):
                    t = op0.imm & 0xffffffff
                    if t == f or t not in self.funcs: stack.append(t)
                va = nxt
        return insns
    def jumptable(self, i, insns, f):
        op = i.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or op.mem.segment: return None
        if i.address in self.jt_cache: return self.jt_cache[i.address]
        table = op.mem.disp & 0xffffffff; idx = i.reg_name(op.mem.index); n = None
        # look back (in address order) for "cmp idx, N" + ja, or a byte-table "movzx idx, byte [x + table2]"
        va = i.address; back = []
        p = self.prev_insns(i.address, 8)
        for q in reversed(p):
            if q.mnemonic == 'cmp' and q.operands[0].type == X.X86_OP_REG and q.operands[1].type == X.X86_OP_IMM \
                    and q.reg_name(q.operands[0].reg) == idx:
                n = (q.operands[1].imm & 0xffffffff) + 1; break
            if q.mnemonic == 'and' and q.operands[0].type == X.X86_OP_REG and q.operands[1].type == X.X86_OP_IMM \
                    and q.reg_name(q.operands[0].reg) == idx and (q.operands[1].imm & 0xffffffff) < 256:
                n = (q.operands[1].imm & 0xffffffff) + 1; break
            if q.mnemonic == 'movzx' and q.operands[1].type == X.X86_OP_MEM and q.reg_name(q.operands[0].reg) == idx:
                # two-level: idx = byte_table[j]; the bound is the largest byte in that table range
                bt = q.operands[1].mem.disp & 0xffffffff; m = None
                for r in reversed(p[:p.index(q)]):
                    if r.mnemonic == 'cmp' and r.operands[1].type == X.X86_OP_IMM: m = (r.operands[1].imm & 0xffffffff) + 1; break
                if m and m < 4096: n = max(self.pe.bytes_at(bt, m)) + 1
                break
            if q.operands and q.operands[0].type == X.X86_OP_REG and q.reg_name(q.operands[0].reg) == idx and q.mnemonic not in ('cmp', 'test'):
                break                                   # the index was computed some other way: no usable bound
        def ok(v): return v is not None and self.TLO <= v < self.THI and abs(v - i.address) <= 0x2000
        if n is None or n > 4096:
            # no bound found (e.g. memcpy's masked or negative indices): take the valid entries around the table
            tg = []; bad = 0
            for k in range(-8, 256):
                v = self.pe.u32(table + 4 * k)
                if ok(v): tg.append(v); bad = 0
                elif k >= 0:
                    bad += 1
                    if bad >= 3: break
            self.unres_jt.append(i.address)
            self.jt_cache[i.address] = (table, tg) if tg else None
            return self.jt_cache[i.address]
        tg = [self.pe.u32(table + 4 * k) for k in range(n)]
        tg = [t for t in tg if t is not None and self.TLO <= t < self.THI]
        self.jt_cache[i.address] = (table, tg); return self.jt_cache[i.address]
    def prev_insns(self, va, n):
        # linear decode backwards is ambiguous; decode forward from a little before and keep the chain ending at va
        for back in range(4 * n, 6 * n * 8, 1):
            start = va - back
            if start < self.TLO: break
            chain = []; p = start
            while p < va:
                q = self.ins(p)
                if q is None: break
                chain.append(q); p += q.size
            if p == va and len(chain) >= n: return chain[-n:]
        return []

    SEH_HANDLER3 = 0x40a020                         # _except_handler3 (MSVC 6 C runtime)
    def seh_handlers(self, insns):
        """__except block addresses from the scope table pushed in the SEH prologue (push table; push _except_handler3)"""
        order = sorted(insns); out = []
        for k in range(len(order) - 1):
            a, b = insns[order[k]], insns[order[k + 1]]
            if a.mnemonic == 'push' and b.mnemonic == 'push' and a.operands[0].type == X.X86_OP_IMM and b.operands[0].type == X.X86_OP_IMM \
                    and (b.operands[0].imm & 0xffffffff) == self.SEH_HANDLER3:
                table = a.operands[0].imm & 0xffffffff
                for e in range(64):
                    enc, flt, hnd = (self.pe.u32(table + 12 * e + 4 * j) for j in range(3))
                    if enc is None or not (enc == 0xffffffff or enc < e) or not (self.TLO <= hnd < self.THI) or (flt and not (self.TLO <= flt < self.THI)): break
                    if flt: out.append(hnd)
        return sorted(set(out))
    def lift_function(self, f):
        insns = self.collect(f)
        seh = self.seh_handlers(insns)
        if seh: insns = self.collect(f, seh)
        self.cur_seh = set(seh)
        order = sorted(insns)
        labels = {f}
        for va in order:
            i = insns[va]; mn = i.mnemonic
            if i.operands and i.operands[0].type == X.X86_OP_IMM and (mn[0] == 'j' or mn.startswith('loop')) and mn != 'jmp' or mn == 'jmp' and i.operands[0].type == X.X86_OP_IMM:
                labels.add(i.operands[0].imm & 0xffffffff)
            if mn == 'jmp' and i.operands[0].type == X.X86_OP_MEM:
                jt = self.jt_cache.get(va)
                if jt: labels.update(jt[1])
        for k, va in enumerate(order):                          # fall-through into overlapping code needs a label too
            i = insns[va]; nxt = va + i.size
            if self.falls_through(i) and (k + 1 == len(order) or order[k + 1] != nxt): labels.add(nxt)
        out = ['void f_%08x(Ctx *c)\n{\n    REGS;\n' % f]
        if seh:
            labels.update(seh)
            out.append('    RtSehPad seh_pad;\n')
        if order and order[0] != f: out.append('    goto L_%08x;\n' % f)     # code before the entry point (reached by jumps)
        callees = set()
        for k, va in enumerate(order):
            i = insns[va]
            if va in labels: out.append('L_%08x:;\n' % va)
            try:
                body = self.emit(i, f, insns, callees)
            except (Unsupported, KeyError, IndexError, AttributeError) as e:
                if not isinstance(e, Unsupported): e = Unsupported('internal %s: %s' % (type(e).__name__, e)); self.internal.append((va, str(e)))
                self.unsup[str(e)] += 1
                if self.illegal_seh:     # DLLs: CPU-specific paths the code guards with __try (STATUS_ILLEGAL_INSTRUCTION)
                    body = 'SPILL; rt_exception(c, 0x%08xu, 0xc000001du); /* illegal here: %s; %s %s */ RELOAD;' % (va, str(e).replace('"', '').replace('*/', ''), i.mnemonic, i.op_str)
                else:                    # the game: a lifter gap, fatal with a clear message
                    body = 'SPILL; rt_unhandled(c, 0x%08xu, "%s"); /* %s %s */' % (va, str(e).replace('"', ''), i.mnemonic, i.op_str)
            if seh and i.mnemonic == 'mov' and i.op_str == 'dword ptr fs:[0], esp':
                cases = ' '.join('case 0x%08xu: goto L_%08x;' % (h, h) for h in seh)
                body += (' { static const uint32_t seh_h[] = {%s, 0}; seh_pad.frame = esp; seh_pad.handlers = seh_h; seh_pad.c = c; SPILL; rt_seh_push(&seh_pad);'
                         ' if (_setjmp(seh_pad.jb)) { RELOAD; switch (seh_pad.target) { %s } } }') % (', '.join('0x%08xu' % h for h in seh), cases)
            if seh:   # leaving the function (return or tail call) drops its landing pad
                body = re.sub(r'SPILL; ((?:f_[0-9a-f]{8}\(c\)|rt_call\(c, t\)|rt_import\(c, \d+\)); )?return;', lambda m: 'rt_seh_pop(&seh_pad); ' + m.group(0), body)
            if va in self.hooks:      # native features: runtime/win32/hooks.c runs before this instruction
                body = 'SPILL; rt_hook(c, 0x%08xu); RELOAD; /* hook: %s */ ' % (va, self.hooks[va]) + body
            if va == f and va in self.overrides:   # a native implementation may perform the whole call
                body = 'SPILL; if (rt_override(c, 0x%08xu)) return; RELOAD; /* override: %s */ ' % (va, self.overrides[va]) + body
            out.append('#line %d "x86"\n    /* %08x: %s %s */ %s\n' % (va, va, i.mnemonic, i.op_str, body))   # debug line = guest address
            nxt = va + i.size
            if self.falls_through(i) and (k + 1 == len(order) or order[k + 1] != nxt):
                g = self.goto(nxt, f, insns, callees)
                if seh: g = re.sub(r'SPILL; (f_[0-9a-f]{8}\(c\); )?return;', lambda m: 'rt_seh_pop(&seh_pad); ' + m.group(0), g)
                out.append('    ' + g + '\n')
        out.append('}\n')
        src = ''.join(out)
        if self.scc_of.get(f) is not None:
            src = src.replace('void f_%08x(Ctx *c)' % f, 'void b_%08x(Ctx *c)' % f, 1)
            members = sorted(m for m, k in self.scc_of.items() if k == self.scc_of[f])
            disp = ' '.join('case 0x%08xu: b_%08x(c); break;' % (m, m) for m in members)
            src = ''.join('void b_%08x(Ctx *c);\n' % m for m in members if m != f) + src + \
                  'void f_%08x(Ctx *c)\n{\n    uint32_t t = 0x%08xu;\n    do { c->tc = 0; switch (t) { %s } t = c->tc; } while (t);\n}\n' % (f, f, disp)
        return src, callees
    def falls_through(self, i):
        mn = i.mnemonic
        if mn in ('jmp', 'ret', 'retf', 'int3', 'hlt', 'ud2'): return False
        if mn in JUNK and mn not in ('lock', 'wait'): return False
        return True
    def goto(self, t, f, insns, callees):
        if t in insns and (t == f or t not in self.funcs or t in getattr(self, 'cur_seh', ())): return 'goto L_%08x;' % t
        if t in self.funcs:
            callees.add(t); self.tail_edges[f].add(t)
            if self.scc_of.get(f) is not None and self.scc_of.get(f) == self.scc_of.get(t):
                return 'c->tc = 0x%08xu; SPILL; return;' % t                # trampoline in f_<member>
            return 'SPILL; f_%08x(c); return;' % t
        return 'SPILL; rt_unhandled(c, 0x%08xu, "jump outside known code");' % t

    # ---------- instructions ----------
    def emit(self, i, f, insns, callees):
        mn = i.mnemonic; ops = i.operands; va = i.address; nxt = va + i.size
        rep = None
        for pfx in ('rep ', 'repe ', 'repne ', 'lock '):
            if mn.startswith(pfx): rep = pfx.strip(); mn = mn[len(pfx):]
        if i.opcode[0] in range(0xd8, 0xe0) or mn.startswith('f') and mn not in ('fs',):
            return self.x87(i)
        if mn == 'wait': return ';'                              # fwait: x87 exceptions are masked
        if mn == 'int3': return 'SPILL; rt_exception(c, 0x%08xu, 0x80000003u); RELOAD;' % i.address          # EXCEPTION_BREAKPOINT
        if mn == 'int': return 'SPILL; rt_exception(c, 0x%08xu, 0xc0000005u); RELOAD;' % i.address            # general protection
        if mn in JUNK: raise Unsupported(mn)
        if rep == 'lock': return self.locked(i, mn)
        if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                  'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd') and not (ops and ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg).startswith('xmm')):
            return self.string(mn, rep)
        if rep: raise Unsupported('prefix %s %s' % (rep, mn))
        o = ops
        if mn == 'mov':
            return self.wr(i, o[0], self.rd(i, o[1], o[0].size))
        if mn == 'movzx': return self.wr(i, o[0], self.rd(i, o[1]))
        if mn == 'movsx':
            t = {1: 'int8_t', 2: 'int16_t'}[o[1].size]; return self.wr(i, o[0], '(uint32_t)(int32_t)(%s)%s' % (t, self.rd(i, o[1])))
        if mn == 'lea': return self.wr(i, o[0], self.addr(i, o[1], lea=True))
        if mn == 'xchg':
            if o[0].type == X.X86_OP_REG and o[1].type == X.X86_OP_REG:
                return '{ uint32_t t0 = %s, t1 = %s; %s %s }' % (self.rd(i, o[0]), self.rd(i, o[1]), self.wr(i, o[0], 't1'), self.wr(i, o[1], 't0'))
            m, r = (o[0], o[1]) if o[0].type == X.X86_OP_MEM else (o[1], o[0])
            rx, wfn, pre = self.rmw(i, m)
            return '{ %suint32_t t0 = %s, t1 = %s; %s %s }' % (pre, rx, self.rd(i, r), wfn('t1'), self.wr(i, r, 't0'))
        if mn == 'bswap': n = i.reg_name(o[0].reg); return '%s = __builtin_bswap32(%s);' % (n, n)
        if mn == 'cdq': return 'edx = (uint32_t)((int32_t)eax >> 31);'
        if mn == 'cwde': return 'eax = (uint32_t)(int32_t)(int16_t)eax;'
        if mn == 'cbw': return 'eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);'
        if mn == 'cwd': return 'edx = (edx & 0xffff0000u) | (((int16_t)eax < 0) ? 0xffffu : 0);'
        if mn == 'push' and o[0].type == X.X86_OP_REG and i.reg_name(o[0].reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
            sel = {'cs': 0x1b, 'fs': 0x3b}.get(i.reg_name(o[0].reg), 0x23)        # flat user-mode selectors
            return 'esp -= 4; W32(esp, 0x%xu);' % sel
        if mn == 'pop' and o[0].type == X.X86_OP_REG and i.reg_name(o[0].reg) in ('ds', 'es', 'ss', 'fs', 'gs'):
            return 'esp += 4;'
        if mn == 'cpuid': return 'rt_cpuid(c, &eax, &ebx, &ecx, &edx);'
        if mn == 'push':
            if o[0].size != 4: raise Unsupported('push16')
            return '{ uint32_t v = %s; esp -= 4; W32(esp, v); }' % self.rd(i, o[0])
        if mn == 'pop':
            if o[0].size != 4: raise Unsupported('pop16')
            return '{ uint32_t v = R32(esp); esp += 4; %s }' % self.wr(i, o[0], 'v')
        if mn == 'pushal':
            return '{ uint32_t s = esp; esp -= 32; W32(esp + 28, eax); W32(esp + 24, ecx); W32(esp + 20, edx); W32(esp + 16, ebx); W32(esp + 12, s); W32(esp + 8, ebp); W32(esp + 4, esi); W32(esp, edi); }'
        if mn == 'popal':
            return 'edi = R32(esp); esi = R32(esp + 4); ebp = R32(esp + 8); ebx = R32(esp + 16); edx = R32(esp + 20); ecx = R32(esp + 24); eax = R32(esp + 28); esp += 32;'
        if mn == 'pushfd': return '{ uint32_t v = flags_get(&f); esp -= 4; W32(esp, v); }'
        if mn == 'popfd': return '{ uint32_t v = R32(esp); esp += 4; flags_set(&f, v, 0xcd5u | F_DF); }'
        if mn == 'lahf': return 'eax = (eax & 0xffff00ffu) | ((flags_get(&f) & 0xd5u | 2u) << 8);'
        if mn == 'sahf': return 'flags_set(&f, (eax >> 8) & 0xffu, F_CF | F_PF | F_AF | F_ZF | F_SF);'
        if mn == 'clc': return 'flags_set(&f, 0, F_CF);'
        if mn == 'stc': return 'flags_set(&f, F_CF, F_CF);'
        if mn == 'cmc': return 'flags_set(&f, flag_cf(&f) ? 0 : F_CF, F_CF);'
        if mn == 'cld': return 'f.df = 0;'
        if mn == 'std': return 'f.df = 1;'
        if mn == 'nop': return ';'
        if mn == 'leave': return 'esp = ebp; ebp = R32(esp); esp += 4;'
        if mn == 'enter':
            if o[1].imm & 0x1f: raise Unsupported('enter nesting')
            return 'esp -= 4; W32(esp, ebp); ebp = esp; esp -= 0x%xu;' % (o[0].imm & 0xffff)
        if mn == 'xlatb': return 'eax = (eax & 0xffffff00u) | R8(ebx + (eax & 0xffu));'
        if mn == 'rdtsc': return '{ uint64_t t = rt_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }'
        if mn in ('add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'cmp', 'test'):
            return self.alu(i, mn)
        if mn in ('inc', 'dec'):
            sz = o[0].size; rx, wfn, pre = self.rmw(i, o[0]); opch = '+' if mn == 'inc' else '-'
            return '{ %suint32_t a = %s, r = (a %s 1u) & %s; f.cin = flag_cf(&f); %s LF(%s, %d, a, 1, r); }' % (
                pre, rx, opch, MASK[sz], wfn('r'), 'LF_INC' if mn == 'inc' else 'LF_DEC', SZ[sz])
        if mn == 'neg':
            sz = o[0].size; rx, wfn, pre = self.rmw(i, o[0])
            return '{ %suint32_t a = %s, r = (0u - a) & %s; %s LF(LF_SUB, %d, 0, a, r); }' % (pre, rx, MASK[sz], wfn('r'), SZ[sz])
        if mn == 'not':
            sz = o[0].size; rx, wfn, pre = self.rmw(i, o[0])
            return '{ %suint32_t r = ~%s & %s; %s }' % (pre, rx, MASK[sz], wfn('r'))
        if mn in ('shl', 'sal', 'shr', 'sar'): return self.shift(i, 'shl' if mn == 'sal' else mn)
        if mn in ('rol', 'ror', 'rcl', 'rcr'): return self.rotate(i, mn)
        if mn == 'shld' or mn == 'shrd':
            d, s, cnt = o; n = self.rd(i, cnt, 1) if cnt.type == X.X86_OP_REG else '%du' % (cnt.imm & 31)
            rx, wfn, pre = self.rmw(i, d)
            if mn == 'shld':
                return '{ %suint32_t n = %s & 31u; if (n) { uint32_t a = %s, r = (a << n) | (%s >> (32u - n)); %s LF(LF_SHL, 2, a, n, r); } }' % (pre, n, rx, self.rd(i, s), wfn('r'))
            return '{ %suint32_t n = %s & 31u; if (n) { uint32_t a = %s, r = (a >> n) | (%s << (32u - n)); %s LF(LF_SHR, 2, a, n, r); } }' % (pre, n, rx, self.rd(i, s), wfn('r'))
        if mn in ('mul', 'imul', 'div', 'idiv'): return self.muldiv(i, mn)
        if mn in ('bsr', 'bsf'):
            fn = '31 - __builtin_clz(s)' if mn == 'bsr' else '__builtin_ctz(s)'
            return '{ uint32_t s = %s; if (s) { %s flags_set(&f, 0, F_ZF); } else flags_set(&f, F_ZF, F_ZF); }' % (self.rd(i, o[1]), self.wr(i, o[0], fn))
        if mn in ('bt', 'bts', 'btr', 'btc'):
            b = o[1]
            if o[0].type == X.X86_OP_MEM and b.type == X.X86_OP_REG:
                a = self.addr(i, o[0]); idx = self.rd(i, b)
                pre = 'uint32_t ea = %s + (uint32_t)(((int32_t)%s >> 5) * 4); uint32_t bit = %s & 31u; ' % (a, idx, idx)
                rx = 'R32(ea)'; wfn = lambda v: 'W32(ea, %s);' % v
            else:
                rx, wfn, pre = self.rmw(i, o[0]); pre += 'uint32_t bit = %s & %du; ' % (self.rd(i, b), o[0].size * 8 - 1)
            upd = {'bt': '', 'bts': wfn('v | (1u << bit)'), 'btr': wfn('v & ~(1u << bit)'), 'btc': wfn('v ^ (1u << bit)')}[mn]
            return '{ %suint32_t v = %s; flags_set(&f, (v >> bit) & 1u, F_CF); %s }' % (pre, rx, upd)
        if mn.startswith('set'):
            return self.wr(i, o[0], '(%s) ? 1u : 0u' % CC[mn[3:]])
        if mn.startswith('cmov'):
            return 'if (%s) { %s }' % (CC[mn[4:]], self.wr(i, o[0], self.rd(i, o[1])))
        if mn == 'call': return self.call(i, callees)
        if mn == 'ret':
            n = (o[0].imm & 0xffff) if o else 0
            return 'esp += %du; SPILL; return;' % (4 + n)
        if mn == 'jmp': return self.jmp(i, f, insns, callees)
        if mn == 'jecxz':
            return 'if (ecx == 0) { %s }' % self.goto(o[0].imm & 0xffffffff, f, insns, callees)
        if mn in ('loop', 'loope', 'loopne'):
            cond = {'loop': 'ecx != 0', 'loope': 'ecx != 0 && CC_E', 'loopne': 'ecx != 0 && CC_NE'}[mn]
            return 'ecx--; if (%s) { %s }' % (cond, self.goto(o[0].imm & 0xffffffff, f, insns, callees))
        if mn[0] == 'j':
            return 'if (%s) { %s }' % (CC[mn[1:]], self.goto(o[0].imm & 0xffffffff, f, insns, callees))
        raise Unsupported(mn)

    def alu(self, i, mn):
        d, s = i.operands; sz = d.size
        b = self.rd(i, s, sz)
        if mn in ('cmp', 'test'):
            a = self.rd(i, d)
            if mn == 'cmp': return '{ uint32_t a = %s, b = %s; LF(LF_SUB, %d, a, b, (a - b) & %s); }' % (a, b, SZ[sz], MASK[sz])
            return '{ uint32_t r = %s & %s; LF(LF_LOGIC, %d, 0, 0, r); }' % (a, b, SZ[sz])
        rx, wfn, pre = self.rmw(i, d)
        if mn in ('and', 'or', 'xor'):
            op = {'and': '&', 'or': '|', 'xor': '^'}[mn]
            if mn == 'xor' and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
                return '{ %s LF(LF_LOGIC, %d, 0, 0, 0); }' % (wfn('0'), SZ[sz])
            return '{ %suint32_t r = (%s %s %s) & %s; %s LF(LF_LOGIC, %d, 0, 0, r); }' % (pre, rx, op, b, MASK[sz], wfn('r'), SZ[sz])
        if mn == 'add':
            return '{ %suint32_t a = %s, b = %s, r = (a + b) & %s; %s LF(LF_ADD, %d, a, b, r); }' % (pre, rx, b, MASK[sz], wfn('r'), SZ[sz])
        if mn == 'sub':
            return '{ %suint32_t a = %s, b = %s, r = (a - b) & %s; %s LF(LF_SUB, %d, a, b, r); }' % (pre, rx, b, MASK[sz], wfn('r'), SZ[sz])
        if mn == 'adc':
            return '{ %suint32_t a = %s, b = %s, ci = (uint32_t)flag_cf(&f), r = (a + b + ci) & %s; %s f.cin = ci; LF(LF_ADC, %d, a, b, r); }' % (pre, rx, b, MASK[sz], wfn('r'), SZ[sz])
        if mn == 'sbb':
            return '{ %suint32_t a = %s, b = %s, ci = (uint32_t)flag_cf(&f), r = (a - b - ci) & %s; %s f.cin = ci; LF(LF_SBB, %d, a, b, r); }' % (pre, rx, b, MASK[sz], wfn('r'), SZ[sz])

    def shift(self, i, mn):
        d = i.operands[0]; sz = d.size; bits = sz * 8
        cnt = i.operands[1] if len(i.operands) > 1 else None
        rx, wfn, pre = self.rmw(i, d)
        if cnt is None: n = '1u'
        elif cnt.type == X.X86_OP_IMM:
            k = cnt.imm & 31
            if k == 0: return ';'
            n = '%du' % k
        else: n = '(%s & 31u)' % self.rd(i, cnt, 1)
        if mn == 'shl': expr = '(uint32_t)(((uint64_t)a << n) & %s)' % MASK[sz]; lf = 'LF_SHL'
        elif mn == 'shr': expr = '(a >> n)'; lf = 'LF_SHR'
        else:
            t = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}[sz]
            expr = '((uint32_t)((int32_t)(%s)a >> (n > 31 ? 31 : n)) & %s)' % (t, MASK[sz]); lf = 'LF_SAR'
        body = 'uint32_t a = %s, r = %s; %s LF(%s, %d, a, n, r);' % (rx, expr, wfn('r'), lf, SZ[sz])
        return '{ %suint32_t n = %s; if (n) { %s } }' % (pre, n, body)

    def rotate(self, i, mn):
        d = i.operands[0]; sz = d.size; bits = sz * 8
        cnt = i.operands[1] if len(i.operands) > 1 else None
        rx, wfn, pre = self.rmw(i, d)
        n = '1u' if cnt is None else ('%du' % (cnt.imm & 31) if cnt.type == X.X86_OP_IMM else '(%s & 31u)' % self.rd(i, cnt, 1))
        msb = '%du' % (bits - 1)
        if mn in ('rol', 'ror'):
            if mn == 'rol':
                core = 'uint32_t k = n %% %d; uint32_t r = k ? ((a << k) | (a >> (%d - k))) & %s : a; uint32_t cf = r & 1u; uint32_t of = ((r >> %s) & 1u) ^ cf;' % (bits, bits, MASK[sz], msb)
            else:
                core = 'uint32_t k = n %% %d; uint32_t r = k ? ((a >> k) | (a << (%d - k))) & %s : a; uint32_t cf = (r >> %s) & 1u; uint32_t of = cf ^ ((r >> %du) & 1u);' % (bits, bits, MASK[sz], msb, bits - 2)
            return '{ %suint32_t n = %s; if (n) { uint32_t a = %s; %s %s flags_set(&f, (cf ? F_CF : 0) | (of ? F_OF : 0), F_CF | F_OF); } }' % (pre, n, rx, core, wfn('r'))
        # rcl / rcr through carry, bit by bit (rare)
        if mn == 'rcl':
            step = 'uint32_t o = (r >> %s) & 1u; r = ((r << 1) | cf) & %s; cf = o;' % (msb, MASK[sz])
            of = 'uint32_t of = ((r >> %s) & 1u) ^ cf;' % msb
        else:
            step = 'uint32_t o = r & 1u; r = (r >> 1) | (cf << %s); cf = o;' % msb
            of = 'uint32_t of = ((r >> %s) & 1u) ^ ((r >> %du) & 1u);' % (msb, bits - 2)
        return '{ %suint32_t n = %s %% %d; if (n) { uint32_t r = %s, cf = (uint32_t)flag_cf(&f); for (uint32_t k = 0; k < n; k++) { %s } %s %s flags_set(&f, (cf ? F_CF : 0) | (of ? F_OF : 0), F_CF | F_OF); } }' % (
            pre, n, bits + 1, rx, step, of, wfn('r'))

    def muldiv(self, i, mn):
        o = i.operands; pc = i.address
        if mn == 'imul' and len(o) >= 2:
            s1 = self.rd(i, o[1] if len(o) == 2 else o[1]); s2 = self.rd(i, o[0]) if len(o) == 2 else self.rd(i, o[2], o[0].size)
            if o[0].size != 4: raise Unsupported('imul16')
            return '{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; uint32_t r = (uint32_t)p; %s flags_set(&f, (p != (int64_t)(int32_t)r) ? (F_CF | F_OF) : 0, F_CF | F_OF); }' % (s1, s2, self.wr(i, o[0], 'r'))
        s = o[0]; sz = s.size; v = self.rd(i, s)
        if sz == 4:
            if mn == 'mul':
                return '{ uint64_t p = (uint64_t)eax * %s; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); flags_set(&f, edx ? (F_CF | F_OF) : 0, F_CF | F_OF); }' % v
            if mn == 'imul':
                return '{ int64_t p = (int64_t)(int32_t)eax * (int32_t)%s; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); flags_set(&f, (p != (int64_t)(int32_t)eax) ? (F_CF | F_OF) : 0, F_CF | F_OF); }' % v
            if mn == 'div':
                return '{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = %s; if (!d || n / d > 0xffffffffull) { SPILL; rt_div_fault(c, 0x%08xu); } eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % (v, pc)
            return '{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t)%s; if (!d || (n == INT64_MIN && d == -1)) { SPILL; rt_div_fault(c, 0x%08xu); } int64_t q = n / d; if (q != (int32_t)q) { SPILL; rt_div_fault(c, 0x%08xu); } eax = (uint32_t)q; edx = (uint32_t)(n %% d); }' % (v, pc, pc)
        if sz == 1:
            if mn == 'mul':
                return '{ uint32_t p = (eax & 0xffu) * %s; eax = (eax & 0xffff0000u) | (p & 0xffffu); flags_set(&f, (p >> 8) ? (F_CF | F_OF) : 0, F_CF | F_OF); }' % v
            if mn == 'imul':
                return '{ int32_t p = (int32_t)(int8_t)eax * (int8_t)%s; eax = (eax & 0xffff0000u) | ((uint32_t)p & 0xffffu); flags_set(&f, (p != (int8_t)p) ? (F_CF | F_OF) : 0, F_CF | F_OF); }' % v
            if mn == 'div':
                return '{ uint32_t n = eax & 0xffffu, d = %s; if (!d || n / d > 0xff) { SPILL; rt_div_fault(c, 0x%08xu); } eax = (eax & 0xffff0000u) | ((n %% d) << 8) | (n / d); }' % (v, pc)
            return '{ int32_t n = (int16_t)eax, d = (int8_t)%s; if (!d || n / d != (int8_t)(n / d)) { SPILL; rt_div_fault(c, 0x%08xu); } eax = (eax & 0xffff0000u) | (((uint32_t)(n %% d) & 0xffu) << 8) | ((uint32_t)(n / d) & 0xffu); }' % (v, pc)
        raise Unsupported('%s16' % mn)

    def locked(self, i, mn):
        o = i.operands
        if mn in ('inc', 'dec') and o[0].type == X.X86_OP_MEM and o[0].size == 4:
            fn = '__atomic_add_fetch' if mn == 'inc' else '__atomic_sub_fetch'
            return '{ uint32_t ea = %s; uint32_t r = %s((uint32_t *)(M + ea), 1u, __ATOMIC_SEQ_CST); f.cin = flag_cf(&f); LF(%s, 2, %s, 1, r); }' % (
                self.addr(i, o[0]), fn, 'LF_INC' if mn == 'inc' else 'LF_DEC', 'r - 1u' if mn == 'inc' else 'r + 1u')
        raise Unsupported('lock ' + mn)

    def string(self, mn, rep):
        k = {'b': 1, 'w': 2, 'd': 4}[mn[-1]]; base = mn[:-1]
        R = {1: 'R8', 2: 'R16', 4: 'R32'}[k]; W = {1: 'W8', 2: 'W16', 4: 'W32'}[k]
        acc = {1: '(eax & 0xffu)', 2: '(eax & 0xffffu)', 4: 'eax'}[k]
        setacc = {1: 'eax = (eax & 0xffffff00u) | v;', 2: 'eax = (eax & 0xffff0000u) | v;', 4: 'eax = v;'}[k]
        step = '(f.df ? (uint32_t)-%d : %du)' % (k, k)
        one = {'movs': '%s(edi, %s(esi)); esi += d; edi += d;' % (W, R),
               'stos': '%s(edi, %s); edi += d;' % (W, acc),
               'lods': '{ uint32_t v = %s(esi); %s } esi += d;' % (R, setacc),
               'scas': '{ uint32_t a = %s, b = %s(edi); LF(LF_SUB, %d, a, b, (a - b) & %s); } edi += d;' % (acc, R, SZ[k], MASK[k]),
               'cmps': '{ uint32_t a = %s(esi), b = %s(edi); LF(LF_SUB, %d, a, b, (a - b) & %s); } esi += d; edi += d;' % (R, R, SZ[k], MASK[k])}[base]
        if not rep: return '{ uint32_t d = %s; %s }' % (step, one)
        if base in ('movs', 'stos', 'lods') or rep == 'rep' and base not in ('scas', 'cmps'):
            if base == 'movs' and k == 4:
                fast = 'if (!f.df && (edi >= esi + ecx * 4u || edi + ecx * 4u <= esi)) { memmove(M + edi, M + esi, (size_t)ecx * 4u); esi += ecx * 4u; edi += ecx * 4u; ecx = 0; } else '
            elif base == 'movs' and k == 1:
                fast = 'if (!f.df && (edi >= esi + ecx || edi + ecx <= esi)) { memmove(M + edi, M + esi, ecx); esi += ecx; edi += ecx; ecx = 0; } else '
            elif base == 'stos' and k == 1:
                fast = 'if (!f.df) { memset(M + edi, (int)(eax & 0xffu), ecx); edi += ecx; ecx = 0; } else '
            else: fast = ''
            return '{ uint32_t d = %s; %swhile (ecx) { %s ecx--; } }' % (step, fast, one)
        stop = 'if (!CC_E) break;' if rep in ('repe', 'rep') else 'if (CC_E) break;'
        return '{ uint32_t d = %s; while (ecx) { %s ecx--; %s } }' % (step, one, stop)

    def call(self, i, callees):
        o = i.operands[0]; ret = i.address + i.size
        push = 'esp -= 4; W32(esp, 0x%08xu); SPILL; ' % ret
        if o.type == X.X86_OP_IMM:
            t = o.imm & 0xffffffff
            if not (self.TLO <= t < self.THI): return push + 'rt_unhandled(c, 0x%08xu, "call outside code");' % t
            callees.add(t); return '%sf_%08x(c); RELOAD;' % (push, t)
        if o.type == X.X86_OP_MEM and not o.mem.base and not o.mem.index and (o.mem.disp & 0xffffffff) in self.imp_index:
            k = self.imp_index[o.mem.disp & 0xffffffff]
            return '%srt_import(c, %d); RELOAD; /* %s */' % (push, k, self.pe.imports[o.mem.disp & 0xffffffff])
        return '{ uint32_t t = %s; %srt_call(c, t); RELOAD; }' % (self.rd(i, o), push)

    def jmp(self, i, f, insns, callees):
        o = i.operands[0]
        if o.type == X.X86_OP_IMM: return self.goto(o.imm & 0xffffffff, f, insns, callees)
        if o.type == X.X86_OP_MEM and not o.mem.base and not o.mem.index and (o.mem.disp & 0xffffffff) in self.imp_index:
            k = self.imp_index[o.mem.disp & 0xffffffff]
            return 'SPILL; rt_import(c, %d); return; /* %s */' % (k, self.pe.imports[o.mem.disp & 0xffffffff])
        jt = self.jt_cache.get(i.address)
        if jt:
            cases = ' '.join('case 0x%08xu: %s' % (t, self.goto(t, f, insns, callees)) for t in sorted(set(jt[1])))
            return '{ uint32_t t = %s; switch (t) { %s default: SPILL; rt_call(c, t); return; } }' % (self.rd(i, o), cases)
        return '{ uint32_t t = %s; SPILL; rt_call(c, t); return; }' % self.rd(i, o)

    # ---------- x87 ----------
    def x87(self, i):
        b = bytes(i.bytes); k = 0
        while b[k] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf0, 0xf2, 0xf3): k += 1
        op = b[k]
        if not 0xd8 <= op <= 0xdf: raise Unsupported(i.mnemonic)
        modrm = b[k + 1]; sub = (modrm >> 3) & 7; r = modrm & 7
        if modrm >= 0xc0: return self.x87reg(op, modrm, sub, r)
        if i.prefix[2] or i.prefix[3]: raise Unsupported('x87 prefix')
        mops = [o for o in i.operands if o.type == X.X86_OP_MEM]
        a = self.addr(i, mops[0])
        arith = ['ST(0) = ST(0) + v;', 'ST(0) = ST(0) * v;', 'rt_fcom(c, ST(0), v);', 'rt_fcom(c, ST(0), v); FPOP();',
                 'ST(0) = ST(0) - v;', 'ST(0) = v - ST(0);', 'ST(0) = ST(0) / v;', 'ST(0) = v / ST(0);']
        if op in (0xd8, 0xdc, 0xda, 0xde):
            v = {0xd8: '(double)RF32(ea)', 0xdc: 'RF64(ea)', 0xda: '(double)(int32_t)R32(ea)', 0xde: '(double)(int16_t)R16(ea)'}[op]
            return '{ uint32_t ea = %s; double v = %s; %s }' % (a, v, arith[sub])
        tbl = {
            (0xd9, 0): 'FPUSH((double)RF32(ea));', (0xd9, 2): 'WF32(ea, (float)ST(0));', (0xd9, 3): 'WF32(ea, (float)ST(0)); FPOP();',
            (0xd9, 4): 'rt_fldenv(c, ea);', (0xd9, 5): 'c->fcw = R16(ea);', (0xd9, 6): 'rt_fnstenv(c, ea);', (0xd9, 7): 'W16(ea, c->fcw);',
            (0xdd, 0): 'FPUSH(RF64(ea));', (0xdd, 2): 'WF64(ea, ST(0));', (0xdd, 3): 'WF64(ea, ST(0)); FPOP();',
            (0xdd, 4): 'rt_frstor(c, ea);', (0xdd, 6): 'rt_fnsave(c, ea);', (0xdd, 7): 'W16(ea, rt_fnstsw(c));',
            (0xdb, 0): 'FPUSH((double)(int32_t)R32(ea));', (0xdb, 2): 'W32(ea, rt_fist32(c, ST(0)));', (0xdb, 3): 'W32(ea, rt_fist32(c, ST(0))); FPOP();',
            (0xdb, 5): 'FPUSH(rt_f80_load(M, ea));', (0xdb, 7): 'rt_f80_store(M, ea, ST(0)); FPOP();',
            (0xdf, 0): 'FPUSH((double)(int16_t)R16(ea));', (0xdf, 2): 'W16(ea, rt_fist16(c, ST(0)));', (0xdf, 3): 'W16(ea, rt_fist16(c, ST(0))); FPOP();',
            (0xdf, 5): 'FPUSH((double)(int64_t)R64(ea));', (0xdf, 7): 'W64(ea, rt_fist64(c, ST(0))); FPOP();',
        }
        if (op, sub) not in tbl: raise Unsupported('x87 %02x/%d' % (op, sub))
        return '{ uint32_t ea = %s; %s }' % (a, tbl[(op, sub)])
    def x87reg(self, op, modrm, sub, r):
        if op == 0xd8:
            return ['ST(0) = ST(0) + ST(%d);', 'ST(0) = ST(0) * ST(%d);', 'rt_fcom(c, ST(0), ST(%d));', 'rt_fcom(c, ST(0), ST(%d)); FPOP();',
                    'ST(0) = ST(0) - ST(%d);', 'ST(0) = ST(%d) - ST(0);', 'ST(0) = ST(0) / ST(%d);', 'ST(0) = ST(%d) / ST(0);'][sub] % r
        if op in (0xdc, 0xde):
            pop = ' FPOP();' if op == 0xde else ''
            if op == 0xde and modrm == 0xd9: return 'rt_fcom(c, ST(0), ST(1)); FPOP(); FPOP();'
            e = ['ST(%d) = ST(%d) + ST(0);', 'ST(%d) = ST(%d) * ST(0);', None, None,
                 'ST(%d) = ST(0) - ST(%d);', 'ST(%d) = ST(%d) - ST(0);', 'ST(%d) = ST(0) / ST(%d);', 'ST(%d) = ST(%d) / ST(0);'][sub]
            if e is None:
                return 'rt_fcom(c, ST(0), ST(%d));%s' % (r, ' FPOP();' if sub == 3 or op == 0xde else '')
            return (e % (r, r)) + pop
        if op == 0xd9:
            if sub == 0: return '{ double v = ST(%d); FPUSH(v); }' % r
            if sub == 1: return '{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (r, r)
            one = {0xd0: ';', 0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabs(ST(0));', 0xe4: 'rt_fcom(c, ST(0), 0.0);', 0xe5: 'rt_fxam(c);',
                   0xe8: 'FPUSH(1.0);', 0xe9: 'FPUSH(3.321928094887362347870);', 0xea: 'FPUSH(1.442695040888963407360);',
                   0xeb: 'FPUSH(3.141592653589793238463);', 0xec: 'FPUSH(0.301029995663981195214);', 0xed: 'FPUSH(0.693147180559945309417);',
                   0xee: 'FPUSH(0.0);', 0xf0: 'ST(0) = expm1(ST(0) * 0.693147180559945309417);', 0xf1: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                   0xf2: 'ST(0) = tan(ST(0)); FPUSH(1.0); c->fsw &= ~(uint32_t)FSW_C2;', 0xf3: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                   0xf5: 'rt_fprem(c, 1);', 0xf6: 'c->top = (c->top - 1) & 7;', 0xf7: 'c->top = (c->top + 1) & 7;', 0xf8: 'rt_fprem(c, 0);',
                   0xf9: 'ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();', 0xfa: 'ST(0) = sqrt(ST(0));',
                   0xfb: '{ double t = ST(0); ST(0) = sin(t); FPUSH(cos(t)); c->fsw &= ~(uint32_t)FSW_C2; }',
                   0xfc: 'ST(0) = rt_frnd(c, ST(0));', 0xfd: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                   0xfe: 'ST(0) = sin(ST(0)); c->fsw &= ~(uint32_t)FSW_C2;', 0xff: 'ST(0) = cos(ST(0)); c->fsw &= ~(uint32_t)FSW_C2;'}
            if modrm in one: return one[modrm]
        if op == 0xdd:
            if sub == 0: return ';'                                  # ffree: tags are not tracked
            if sub == 2: return 'ST(%d) = ST(0);' % r
            if sub == 3: return 'ST(%d) = ST(0); FPOP();' % r
            if sub == 4: return 'rt_fcom(c, ST(0), ST(%d));' % r
            if sub == 5: return 'rt_fcom(c, ST(0), ST(%d)); FPOP();' % r
        if op == 0xda and modrm == 0xe9: return 'rt_fcom(c, ST(0), ST(1)); FPOP(); FPOP();'
        if op == 0xdb and modrm == 0xe2: return 'c->fsw &= 0x7f00u & ~0x3800u;'
        if op == 0xdb and modrm == 0xe3: return 'c->fcw = 0x37f; c->fsw = 0; c->top = 0;'
        if op == 0xdf and modrm == 0xe0: return 'eax = (eax & 0xffff0000u) | rt_fnstsw(c);'
        raise Unsupported('x87 %02x %02x' % (op, modrm))

def find_cycles(L):
    """strongly connected components of the tail-call graph with more than one member"""
    idx = {}; low = {}; st = []; on = set(); sccs = []; n = [0]
    sys.setrecursionlimit(100000)
    def sc(v):
        idx[v] = low[v] = n[0]; n[0] += 1; st.append(v); on.add(v)
        for w in L.tail_edges.get(v, ()):
            if w not in idx: sc(w); low[v] = min(low[v], low[w])
            elif w in on: low[v] = min(low[v], idx[w])
        if low[v] == idx[v]:
            comp = []
            while True:
                w = st.pop(); on.discard(w); comp.append(w)
                if w == v: break
            if len(comp) > 1: sccs.append(sorted(comp))
    for v in list(L.tail_edges):
        if v not in idx: sc(v)
    return sccs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('analysis'); ap.add_argument('outdir')
    ap.add_argument('--only', default=None); ap.add_argument('--closure', action='store_true')
    ap.add_argument('--per-file', type=int, default=400)
    ap.add_argument('--imp-base', type=int, default=0, help='index of this image\'s first import in the global import table')
    ap.add_argument('--tag', default='', help='multi-image builds: file name prefix; tables come from tools/link_tables.py')
    a = ap.parse_args()
    L = Lifter(a.exe, a.analysis); L.jt_cache = {}
    L.imp_index = {va: a.imp_base + k for k, (va, _) in enumerate(L.imports)}
    L.illegal_seh = bool(a.tag)
    L.hooks = {} if a.tag else dict(HOOKS)
    L.overrides = {} if a.tag else dict(OVERRIDES)
    # development: DS_TRACE_HOOKS="0x48bf51,0x713158" adds hooks that log registers and stack arguments (DS_HOOKTRACE=1)
    for h in filter(None, os.environ.get('DS_TRACE_HOOKS', '').split(',')):
        if not a.tag: L.hooks.setdefault(int(h, 16), 'trace')
    os.makedirs(a.outdir, exist_ok=True)
    pre = a.tag + '_' if a.tag else ''
    for old in os.listdir(a.outdir):                             # stale files from a previous run
        if old.startswith(pre + 'lift_') and old.endswith('.c'): os.remove(os.path.join(a.outdir, old))
    todo = sorted(L.funcs) if not a.only else [int(x, 16) for x in a.only.split(',')]
    for t in todo: L.funcs.add(t)
    done = {}; queue = list(todo)
    while queue:
        fva = queue.pop(0)
        if fva in done: continue
        src, callees = L.lift_function(fva); done[fva] = src
        for t in sorted(callees):                                # call targets the analysis missed become functions
            if t not in L.funcs and not a.only: L.funcs.add(t); queue.append(t); L.discovered.append(t)
            elif a.closure and t not in done: queue.append(t)
    sccs = find_cycles(L)
    if sccs:
        for k, comp in enumerate(sccs):
            for m in comp: L.scc_of[m] = k
        for m in L.scc_of: done[m] = L.lift_function(m)[0]
    print('tail-call cycles: %d (members run through trampolines)' % len(sccs))
    fns = sorted(done)
    with open(os.path.join(a.outdir, pre + 'funcs.h'), 'w') as h:
        h.write('#include "rt.h"\n'); h.writelines('void f_%08x(Ctx *c);\n' % x for x in fns)
    json.dump({'base': L.pe.base, 'entry': L.pe.entry, 'imp_base': a.imp_base, 'functions': fns,
               'imports': [[va, n] for va, n in L.imports]}, open(os.path.join(a.outdir, pre + 'image.json'), 'w'))
    if pre: fns_tab = None
    else:
     with open(os.path.join(a.outdir, 'fntab.c'), 'w') as t:
        t.write('#include "funcs.h"\nconst struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[] = {\n')
        t.writelines('    {0x%08xu, f_%08x},\n' % (x, x) for x in fns)
        t.write('};\nconst unsigned rt_fntab_n = %d;\n' % len(fns))
        t.write('const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[] = {\n')
        t.writelines('    {0x%08xu, "%s"},\n' % (va, n) for va, n in L.imports)
        t.write('};\nconst unsigned rt_imptab_n = %d;\n' % len(L.imports))
        t.write('const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[] = {{"", 0x%xu, 0, %d}};\nconst unsigned rt_images_n = 1;\n' % (L.pe.base, len(L.imports)))
    for k in range(0, len(fns), a.per_file):
        with open(os.path.join(a.outdir, pre + 'lift_%03d.c' % (k // a.per_file)), 'w') as o:
            o.write('#include "' + pre + 'funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n#pragma clang diagnostic ignored "-Wunused-variable"\n\n')
            for x in fns[k:k + a.per_file]: o.write(done[x]); o.write('\n')
    print('lifted %d functions -> %s (%d files)' % (len(fns), a.outdir, (len(fns) + a.per_file - 1) // a.per_file))
    print('unsupported:', dict(L.unsup.most_common(40)))
    print('jump tables bounded heuristically: %d' % len(set(L.unres_jt)))
    print('functions discovered while lifting: %d' % len(L.discovered))
    print('internal errors: %d %s' % (len(L.internal), L.internal[:10]))
if __name__ == "__main__": main()
