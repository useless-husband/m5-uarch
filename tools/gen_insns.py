#!/usr/bin/env python3
"""Expand insns/*.def into an assembly file and a C table.

The measurement engine never encodes a table instruction itself.  This script
turns every instruction template into concrete assembly text (one block for
throughput, one chain step per input->output path for latency); the system
assembler produces the machine code, and the C side copies those words into
its JIT buffer.

Definition syntax (one entry per line):

    # comment                           whole-line comments only ('#' also
                                        introduces immediates)
    group Integer / arithmetic          sticky section title
    ext LSE                             sticky extension label ("ext -" clears)
    for s in x w                        loop; $s is substituted in the body
    for T,e in 16b:b 8h:h               several variables, values joined by ':'
    end
    name | asm template | attributes

Operand placeholders in the template:

    {W0:x}    written only          {R1:x}   read only
    {RW0:v}   read and written      {A0}     address base (read, 64-bit)
    {off:8}   instance index * 8    {off:8:64}  64 + instance index * 8

Classes: x w (general purpose), b h s d q v z (FP/SIMD views; `v` and `z`
render the bare register name so the template adds `.16b` and so on).

`;` separates several instructions in one entry.  `||` separates two
alternatives that are used in turn, for instructions whose side effects must
cancel (post-index loads that walk a pointer forwards, then backwards).

Attributes (space separated key=value):

    flags=r|w|rw        the instruction reads / writes NZCV
    init=R0:5,R1:0x10   constants for operands
    ptr=RW1             operand is a pointer into the scratch buffer
    fp=d|s|h|i          lane pattern for all vector registers (default d)
    cell=A0:8           a latency chain through the address operand A0 is
                        possible: build a random cycle of nodes in scratch
                        memory whose cell at node+8 points to the next node
    cell=R1:0:8:8       same through an index operand: the cell at
                        scratch + 0 + index*8 holds the next index (8 bytes
                        wide; 1, 2, 4 for narrower loads)
    flip                the instruction selects on a condition: run it with
                        inverted flags between measurements so that a core
                        cannot treat the condition as constant
    extra=N             the template contains N extra one-cycle instructions
                        in its chain (subtracted from the latency)
    chain=R0,R1         measure latency only through these inputs
    nolat / notp        skip latency / throughput
    tpk=N               number of instances in the throughput block
    serial              throughput block is inherently a dependency chain
    pre=... post=...    assembly run once before / after the loop
    note="..."          remark carried into the results
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Register allocation.  x18 is the platform register; x27 is the scratch
# memory base, x28 the loop counter, x29/x30 belong to the trampoline.
GPR_POOL = list(range(0, 18))
GPR_SRC = [19, 20, 21, 22]
GPR_HELP = [23, 24]
# In throughput blocks, read-only integer operands rotate through these so
# that no single register is read by every instance (see DESIGN.md, "Shared
# sources").
GPR_ROT = [19, 20, 21, 22, 23, 24, 25, 26]
MEM_REG = 27
RESERVED_GPR = {18, 28, 29, 30, 31}
# Vector sources are v12-v15 so that they satisfy the "Rm < 16" restriction
# of the 16-bit indexed-element forms.
VEC_POOL = list(range(0, 12)) + list(range(16, 28))
VEC_SRC = [12, 13, 14, 15]
VEC_HELP = [28, 29, 30]

GPR_CLASSES = {"x", "w"}
VEC_CLASSES = {"b", "h", "s", "d", "q", "v", "z"}

HELP_NONE, HELP_CMP, HELP_CSINC, HELP_FMOV_XD, HELP_FMOV_DX, HELP_FCMP, HELP_FCSEL = range(7)
HELP_NAMES = ["UA_HELP_NONE", "UA_HELP_CMP", "UA_HELP_CSINC", "UA_HELP_FMOV_XD",
              "UA_HELP_FMOV_DX", "UA_HELP_FCMP", "UA_HELP_FCSEL"]

FP_PATTERNS = {
    "d": (0x3FF0000000000000, 0x3FF0000000000000),  # 1.0 in each 64-bit lane
    "s": (0x3F8000003F800000, 0x3F8000003F800000),  # 1.0f in each 32-bit lane
    "h": (0x3C003C003C003C00, 0x3C003C003C003C00),  # 1.0 in each 16-bit lane
    "i": (0x0706050403020100, 0x0F0E0D0C0B0A0908),  # byte indices, for tbl etc.
}

MAX_CHAINS = 8
CODE_MARK = 0x0BAD0000

PLACEHOLDER = re.compile(r"\{(RW|W|R|A)(\d)(?::([a-z]))?\}")
OFFSET = re.compile(r"\{off:(-?\d+)(?::(-?\d+))?\}")


class DefError(Exception):
    pass


@dataclass(frozen=True)
class Operand:
    role: str   # W, R, RW, A
    index: int
    cls: str    # x w b h s d q v z

    @property
    def key(self) -> str:
        return f"{self.role}{self.index}"

    @property
    def family(self) -> str:
        return "gpr" if self.cls in GPR_CLASSES else "vec"

    @property
    def reads(self) -> bool:
        return self.role in ("R", "RW", "A")

    @property
    def writes(self) -> bool:
        return self.role in ("W", "RW")


@dataclass
class Spec:
    name: str
    alts: list[list[str]]          # alternatives -> instruction templates
    attrs: dict[str, str]
    group: str
    ext: str
    where: str
    operands: dict[str, Operand] = field(default_factory=dict)


@dataclass
class Init:
    kind: str
    reg: int = 0
    off: int = 0
    val: int = 0
    val2: int = 0


@dataclass
class Chain:
    src: str
    dst: str
    lines: list[str]
    helper: int
    tied: bool
    steps: int
    inits: list[Init]
    extra: int = 0


@dataclass
class Insn:
    spec: Spec
    text: str
    tp_lines: list[str]
    tp_inst: int
    tp_chains: int
    tp_inits: list[Init]
    serial: bool
    pre: list[str]
    post: list[str]
    chains: list[Chain]


# --------------------------------------------------------------------------
# Parsing


def _substitute(text: str, env: dict[str, str]) -> str:
    # Longest names first so that $Tn is not read as $T followed by "n".
    for k in sorted(env, key=len, reverse=True):
        text = text.replace("$" + k, env[k])
    return text


def parse_defs(text: str, where: str = "<defs>") -> list[Spec]:
    specs: list[Spec] = []
    group, ext = "Ungrouped", ""
    lines = text.splitlines()

    def run(start: int, env: dict[str, str], depth: int) -> int:
        nonlocal group, ext
        i = start
        while i < len(lines):
            raw = lines[i]
            loc = f"{where}:{i + 1}"
            line = raw.strip()
            i += 1
            if not line or line.startswith("#"):
                continue
            if line == "end":
                if depth == 0:
                    raise DefError(f"{loc}: 'end' without 'for'")
                return i
            if line.startswith("for "):
                m = re.match(r"for\s+([\w,]+)\s+in\s+(.+)$", line)
                if not m:
                    raise DefError(f"{loc}: malformed 'for'")
                names = m.group(1).split(",")
                values = m.group(2).split()
                end = i
                for v in values:
                    parts = v.split(":")
                    if len(parts) != len(names):
                        raise DefError(f"{loc}: value '{v}' does not match {names}")
                    child = dict(env)
                    child.update({n: ("" if p == "-" else p) for n, p in zip(names, parts)})
                    end = run(i, child, depth + 1)
                i = end
                continue
            if line.startswith("group "):
                group = line[6:].strip()
                continue
            if line.startswith("ext "):
                ext = line[4:].strip()
                if ext == "-":
                    ext = ""
                continue
            line = _substitute(line, env)
            fields = [f.strip() for f in line.split("|")]
            # "||" inside the template produced empty fields; stitch them back.
            fields = _rejoin_alternatives(fields)
            if len(fields) < 2 or len(fields) > 3:
                raise DefError(f"{loc}: expected 'name | asm | attrs', got {line!r}")
            name, asm = fields[0], fields[1]
            attrs = _parse_attrs(fields[2] if len(fields) == 3 else "", loc)
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
                raise DefError(f"{loc}: bad entry name {name!r}")
            alts = [[ins.strip() for ins in alt.split(";") if ins.strip()]
                    for alt in asm.split("||")]
            if not alts or any(not a for a in alts) or len(alts) > 2:
                raise DefError(f"{loc}: empty or too many alternatives")
            spec = Spec(name, alts, attrs, group, ext, loc)
            _collect_operands(spec)
            specs.append(spec)
        if depth:
            raise DefError(f"{where}: 'for' without 'end'")
        return i

    run(0, {}, 0)
    seen: dict[str, str] = {}
    for s in specs:
        if s.name in seen:
            raise DefError(f"{s.where}: duplicate entry {s.name!r} (first at {seen[s.name]})")
        seen[s.name] = s.where
    return specs


def _rejoin_alternatives(fields: list[str]) -> list[str]:
    out: list[str] = []
    i = 0
    while i < len(fields):
        if fields[i] == "" and out and i + 1 < len(fields):
            out[-1] = out[-1] + " || " + fields[i + 1]
            i += 2
        else:
            out.append(fields[i])
            i += 1
    return out


def _parse_attrs(text: str, loc: str) -> dict[str, str]:
    attrs: dict[str, str] = {}
    try:
        tokens = shlex.split(text)
    except ValueError as e:
        raise DefError(f"{loc}: {e}") from None
    known = {"flags", "init", "ptr", "fp", "cell", "chain", "nolat", "notp", "tpk",
             "serial", "pre", "post", "note", "flip", "extra"}
    for tok in tokens:
        key, _, val = tok.partition("=")
        if key not in known:
            raise DefError(f"{loc}: unknown attribute {key!r}")
        if key == "cell" and key in attrs:
            attrs[key] += "," + val
        else:
            attrs[key] = val
    if attrs.get("flags", "") not in ("", "r", "w", "rw"):
        raise DefError(f"{loc}: flags must be r, w or rw")
    if attrs.get("fp", "d") not in FP_PATTERNS:
        raise DefError(f"{loc}: fp must be one of {sorted(FP_PATTERNS)}")
    return attrs


def _collect_operands(spec: Spec) -> None:
    for alt in spec.alts:
        for ins in alt:
            for m in PLACEHOLDER.finditer(ins):
                role, idx, cls = m.group(1), int(m.group(2)), m.group(3)
                if role == "A":
                    cls = "x"
                elif cls is None:
                    raise DefError(f"{spec.where}: {m.group(0)} needs a class")
                if cls not in GPR_CLASSES | VEC_CLASSES:
                    raise DefError(f"{spec.where}: unknown class in {m.group(0)}")
                op = Operand(role, idx, cls)
                prev = spec.operands.get(op.key)
                if prev and prev.family != op.family:
                    raise DefError(f"{spec.where}: {op.key} used as both integer and vector")
                spec.operands.setdefault(op.key, op)
            leftover = PLACEHOLDER.sub("", OFFSET.sub("", ins))
            if "{" in leftover or "}" in leftover:
                raise DefError(f"{spec.where}: unrecognised placeholder in {ins!r}")


# --------------------------------------------------------------------------
# Rendering


def _regname(cls: str, n: int) -> str:
    if cls in ("v", "z"):
        return f"{cls}{n}"
    return f"{cls}{n}"


def render(template: str, regs: dict[str, int], inst: int) -> str:
    def rep(m: re.Match) -> str:
        role, idx, cls = m.group(1), m.group(2), m.group(3)
        key = f"{role}{idx}"
        n = regs[key]
        if role == "A":
            cls = "x"
        if cls in GPR_CLASSES and n in RESERVED_GPR:
            raise DefError(f"reserved register x{n} in {template!r}")
        return _regname(cls, n)

    def off(m: re.Match) -> str:
        step = int(m.group(1))
        base = int(m.group(2)) if m.group(2) else 0
        return str(base + step * inst)

    return OFFSET.sub(off, PLACEHOLDER.sub(rep, template))


def _parse_int(s: str) -> int:
    return int(s, 0)


def _init_map(spec: Spec) -> dict[str, int]:
    out: dict[str, int] = {}
    if "init" in spec.attrs:
        for item in spec.attrs["init"].split(","):
            key, _, val = item.partition(":")
            if key not in spec.operands:
                raise DefError(f"{spec.where}: init for unknown operand {key!r}")
            out[key] = _parse_int(val)
    return out


def _ptr_map(spec: Spec) -> dict[str, int]:
    out: dict[str, int] = {}
    if "ptr" in spec.attrs:
        for item in spec.attrs["ptr"].split(","):
            key, _, val = item.partition(":")
            if key not in spec.operands:
                raise DefError(f"{spec.where}: ptr for unknown operand {key!r}")
            out[key] = _parse_int(val) if val else 0
    return out


def _cell_map(spec: Spec) -> dict[str, tuple[int, int, int]]:
    """operand key -> (offset, scale, size).  scale 0 means a pointer cycle."""
    out: dict[str, tuple[int, int, int]] = {}
    if "cell" in spec.attrs:
        for item in spec.attrs["cell"].split(","):
            parts = item.split(":")
            if len(parts) not in (2, 3, 4) or parts[0] not in spec.operands:
                raise DefError(f"{spec.where}: bad cell attribute {item!r}")
            off = _parse_int(parts[1])
            scale = _parse_int(parts[2]) if len(parts) >= 3 else 0
            size = _parse_int(parts[3]) if len(parts) == 4 else 8
            if scale not in (0, 1, 2, 4, 8, 16) or size not in (1, 2, 4, 8):
                raise DefError(f"{spec.where}: bad cell scale/size in {item!r}")
            out[parts[0]] = (off, scale, size)
    return out


def _has_memory(spec: Spec) -> bool:
    return any("[" in ins for alt in spec.alts for ins in alt)


def _in_address(spec: Spec, key: str) -> bool:
    """True if the operand appears inside a memory operand's brackets."""
    for alt in spec.alts:
        for ins in alt:
            for inner in re.findall(r"\[([^\]]*)\]", ins):
                for m in PLACEHOLDER.finditer(inner):
                    if f"{m.group(1)}{m.group(2)}" == key:
                        return True
    return False


def _fixed_regs(spec: Spec) -> dict[str, int]:
    """Registers for read-only operands when they are not part of a chain."""
    regs: dict[str, int] = {}
    for op in spec.operands.values():
        if op.role == "A":
            regs[op.key] = MEM_REG
        elif op.role == "R":
            src = GPR_SRC if op.family == "gpr" else VEC_SRC
            if op.index >= len(src):
                raise DefError(f"{spec.where}: too many read operands")
            regs[op.key] = src[op.index]
    return regs


def _base_inits(spec: Spec, regs: dict[str, int], skip: set[str]) -> list[Init]:
    """Initialisers shared by the throughput block and every chain."""
    inits: list[Init] = []
    lo, hi = FP_PATTERNS[spec.attrs.get("fp", "d")]
    inits.append(Init("VEC_ALL", val=lo, val2=hi))
    consts = _init_map(spec)
    ptrs = _ptr_map(spec)
    for key, op in spec.operands.items():
        if key in skip or key not in regs:
            continue
        if key in ptrs:
            if op.family != "gpr":
                raise DefError(f"{spec.where}: ptr on a vector operand")
            inits.append(Init("GPR_PTR", reg=regs[key], val=ptrs[key]))
        elif key in consts:
            if op.family == "gpr":
                inits.append(Init("GPR", reg=regs[key], val=consts[key]))
            else:
                v = consts[key]
                inits.append(Init("VEC", reg=regs[key], val=v & (2**64 - 1), val2=v >> 64))
    return inits


def expand(spec: Spec) -> Insn:
    a = spec.attrs
    ops = spec.operands
    fixed = _fixed_regs(spec)
    written = [op for op in ops.values() if op.writes]
    w_gpr = [op for op in written if op.family == "gpr"]
    w_vec = [op for op in written if op.family == "vec"]
    rw = [op for op in written if op.role == "RW"]
    flags = a.get("flags", "")
    consts = _init_map(spec)
    ptrs = _ptr_map(spec)

    # ---- throughput block ------------------------------------------------
    k = 16
    limits = []
    if w_gpr:
        limits.append(len(GPR_POOL) // len(w_gpr))
    if w_vec:
        limits.append(len(VEC_POOL) // len(w_vec))
    if limits:
        k = min(limits)
    if "tpk" in a:
        k = int(a["tpk"])
    if k < 1:
        raise DefError(f"{spec.where}: too many written operands")

    rotating = [op for op in ops.values()
                if op.role == "R" and op.family == "gpr" and op.key not in consts
                and op.key not in ptrs and not _in_address(spec, op.key)]

    def tp_regs(i: int) -> dict[str, int]:
        regs = dict(fixed)
        for op in rotating:
            regs[op.key] = GPR_ROT[(i * (2 * op.index + 1) + op.index) % len(GPR_ROT)]
        for j, op in enumerate(w_gpr):
            regs[op.key] = GPR_POOL[(i * len(w_gpr) + j) % len(GPR_POOL)]
        for j, op in enumerate(w_vec):
            regs[op.key] = VEC_POOL[(i * len(w_vec) + j) % len(VEC_POOL)]
        return regs

    tp_lines: list[str] = []
    tp_inits: list[Init] = []
    text = "; ".join(render(t, tp_regs(0), 0) for t in spec.alts[0])
    if "notp" not in a:
        for alt in spec.alts:
            for i in range(k):
                for t in alt:
                    tp_lines.append(render(t, tp_regs(i), i))
        tp_inits = _base_inits(spec, fixed, set())
        for i in range(k):
            regs = tp_regs(i)
            for op in written:
                if op.key in ptrs:
                    tp_inits.append(Init("GPR_PTR", reg=regs[op.key], val=ptrs[op.key]))
                elif op.key in consts and op.role == "RW":
                    if op.family == "gpr":
                        tp_inits.append(Init("GPR", reg=regs[op.key], val=consts[op.key]))
                    else:
                        v = consts[op.key]
                        tp_inits.append(Init("VEC", reg=regs[op.key],
                                             val=v & (2**64 - 1), val2=v >> 64))
    tp_inst = k * len(spec.alts) if tp_lines else 0
    tp_chains = k * len(rw) if rw and "tpk" not in a else (1 if rw else 0)
    serial = "serial" in a or flags == "rw"

    # ---- latency chains --------------------------------------------------
    chains: list[Chain] = []
    if "nolat" not in a:
        chains = _expand_chains(spec, fixed)

    pre = [s.strip() for s in a.get("pre", "").split(";") if s.strip()]
    post = [s.strip() for s in a.get("post", "").split(";") if s.strip()]
    return Insn(spec, text, tp_lines, tp_inst, tp_chains, tp_inits, serial, pre, post, chains)


def _expand_chains(spec: Spec, fixed: dict[str, int]) -> list[Chain]:
    a = spec.attrs
    ops = spec.operands
    flags = a.get("flags", "")
    cells = _cell_map(spec)
    consts = _init_map(spec)
    only = set(a["chain"].split(",")) if "chain" in a else None
    if only:
        for key in only:
            if key != "nzcv" and key not in ops:
                raise DefError(f"{spec.where}: chain= names unknown operand {key!r}")
    has_mem = _has_memory(spec)

    srcs: list[str] = [op.key for op in ops.values() if op.reads]
    dsts: list[str] = [op.key for op in ops.values() if op.writes]
    if "r" in flags:
        srcs.append("nzcv")
    if "w" in flags:
        dsts.append("nzcv")

    chains: list[Chain] = []
    for src in srcs:
        if only is not None and src not in only:
            continue
        for dst in dsts:
            chain = _one_chain(spec, fixed, src, dst, cells, consts, has_mem)
            if chain is not None:
                chains.append(chain)
    if len(chains) > MAX_CHAINS:
        raise DefError(f"{spec.where}: {len(chains)} latency chains; restrict with chain=")
    return chains


def _one_chain(spec: Spec, fixed: dict[str, int], src: str, dst: str,
               cells: dict, consts: dict[str, int], has_mem: bool) -> Chain | None:
    ops = spec.operands
    s_op = ops.get(src)
    d_op = ops.get(dst)
    s_fam = "flags" if src == "nzcv" else s_op.family
    d_fam = "flags" if dst == "nzcv" else d_op.family

    # An address or index operand of a memory instruction can only carry a
    # chain if the definition says what the memory cell must contain.
    if (s_op is not None and has_mem and s_op.role != "RW" and src not in cells
            and _in_address(spec, src)):
        return None
    if s_op is not None and src in cells and d_fam == "flags":
        return None

    tied = False
    if s_op is not None and d_op is not None and src != dst:
        if s_op.role == "RW":
            return None  # a different read-write operand cannot be tied to the output
        if d_op.role == "RW":
            if s_fam != d_fam:
                return None
            tied = True

    regs = dict(fixed)
    gpr_free = list(GPR_POOL)
    vec_free = list(VEC_POOL)
    gpr_chain, vec_chain = gpr_free.pop(0), vec_free.pop(0)
    chain_reg = {"gpr": gpr_chain, "vec": vec_chain}

    if s_op is not None:
        regs[src] = chain_reg[s_fam]
    if d_op is not None:
        regs[dst] = chain_reg[d_fam]
    for op in ops.values():
        if op.writes and op.key not in (src, dst):
            regs[op.key] = (gpr_free if op.family == "gpr" else vec_free).pop(0)

    helper, helper_line = HELP_NONE, None
    if s_fam == d_fam:
        pass
    elif (s_fam, d_fam) == ("gpr", "flags"):
        helper, helper_line = HELP_CSINC, f"csinc x{gpr_chain}, x{GPR_HELP[0]}, x{GPR_HELP[1]}, ne"
    elif (s_fam, d_fam) == ("flags", "gpr"):
        helper, helper_line = HELP_CMP, f"cmp x{gpr_chain}, x{GPR_HELP[0]}"
    elif (s_fam, d_fam) == ("vec", "flags"):
        helper, helper_line = HELP_FCSEL, f"fcsel d{vec_chain}, d{VEC_HELP[0]}, d{VEC_HELP[1]}, ne"
    elif (s_fam, d_fam) == ("flags", "vec"):
        helper, helper_line = HELP_FCMP, f"fcmp d{vec_chain}, d{VEC_HELP[0]}"
    elif (s_fam, d_fam) == ("gpr", "vec"):
        helper, helper_line = HELP_FMOV_XD, f"fmov x{gpr_chain}, d{vec_chain}"
    elif (s_fam, d_fam) == ("vec", "gpr"):
        helper, helper_line = HELP_FMOV_DX, f"fmov d{vec_chain}, x{gpr_chain}"

    lines: list[str] = []
    for alt in spec.alts:
        for t in alt:
            lines.append(render(t, regs, 0))
        if helper_line:
            lines.append(helper_line)

    skip = set()
    inits = _base_inits(spec, regs, skip)
    # Keep the value flowing round a flags chain stable: the helper selects
    # between two copies of the operand's initial value.
    if helper == HELP_CSINC:
        v = consts.get(src)
        if v is not None:
            inits.append(Init("GPR", reg=GPR_HELP[0], val=v))
            inits.append(Init("GPR", reg=GPR_HELP[1], val=v))
    if src in cells:
        off, scale, size = cells[src]
        if scale == 0:
            inits.append(Init("CYCLE_PTR", reg=regs[src], off=off))
        else:
            inits.append(Init("CYCLE_IDX", reg=regs[src], off=off, val=scale, val2=size))

    def label(key: str) -> str:
        if key == "nzcv":
            return "nzcv"
        op = ops[key]
        return _regname("x" if op.role == "A" else op.cls, regs[key])

    return Chain(src=f"{src}:{label(src)}", dst=f"{dst}:{label(dst)}", lines=lines,
                 helper=helper, tied=tied, steps=len(spec.alts), inits=inits,
                 extra=int(spec.attrs.get("extra", "0")))


# --------------------------------------------------------------------------
# Output


def _c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _count_words(lines: list[str]) -> int:
    """Instruction lines only: labels and directives occupy no words."""
    n = 0
    for ln in lines:
        body = ln.strip()
        while re.match(r"^[0-9A-Za-z_.$]+:", body):
            body = body.split(":", 1)[1].strip()
        if body and not body.startswith("."):
            n += 1
    return n


def emit(insns: list[Insn]) -> tuple[str, str]:
    asm: list[str] = [
        "// Generated by tools/gen_insns.py - do not edit.",
        "    .section __TEXT,__const",
        "    .p2align 2",
        "    .globl _ua_code",
        "_ua_code:",
    ]
    c: list[str] = [
        "/* Generated by tools/gen_insns.py - do not edit. */",
        '#include "insn.h"',
        "",
    ]
    off = 0
    marks: list[int] = []

    def blob(lines: list[str], what: str) -> tuple[int, int]:
        nonlocal off
        if not lines:
            return 0, 0
        asm.append(f"    // {what}")
        asm.append(f"    .long {CODE_MARK | (len(marks) & 0xFFFF):#010x}")
        marks.append(off)
        off += 1
        start = off
        for ln in lines:
            asm.append("    " + ln)
        n = _count_words(lines)
        off += n
        return start, n

    def init_array(name: str, inits: list[Init]) -> str:
        if not inits:
            return "NULL"
        rows = ", ".join(
            f"{{UA_INIT_{i.kind}, {i.reg}, {i.off}, {i.val:#x}ull, {i.val2:#x}ull}}"
            for i in inits)
        c.append(f"static const ua_init {name}[] = {{{rows}}};")
        return name

    table: list[str] = []
    for idx, ins in enumerate(insns):
        s = ins.spec
        tp_off, tp_n = blob(ins.tp_lines, f"{s.name}: throughput")
        pre_off, pre_n = blob(ins.pre, f"{s.name}: pre")
        post_off, post_n = blob(ins.post, f"{s.name}: post")
        tp_init = init_array(f"tpi_{idx}", ins.tp_inits)
        chain_rows: list[str] = []
        for j, ch in enumerate(ins.chains):
            ch_off, ch_n = blob(ch.lines, f"{s.name}: latency {ch.src} -> {ch.dst}")
            ci = init_array(f"chi_{idx}_{j}", ch.inits)
            chain_rows.append(
                f"{{{_c_str(ch.src)}, {_c_str(ch.dst)}, {_c_str('; '.join(ch.lines))}, "
                f"{HELP_NAMES[ch.helper]}, {int(ch.tied)}, {ch.steps}, {ch.extra}, {ch_off}, {ch_n}, "
                f"{ci}, {len(ch.inits)}}}")
        chains_name = "NULL"
        if chain_rows:
            chains_name = f"chains_{idx}"
            c.append(f"static const ua_chain {chains_name}[] = {{")
            c.extend("    " + r + "," for r in chain_rows)
            c.append("};")
        flag_bits = []
        if ins.serial:
            flag_bits.append("UA_INSN_TP_SERIAL")
        if "flip" in s.attrs:
            flag_bits.append("UA_INSN_FLIP")
        flags = " | ".join(flag_bits) if flag_bits else "0"
        table.append(
            f"    {{{_c_str(s.name)}, {_c_str(s.group)}, {_c_str(s.ext)}, {_c_str(ins.text)}, "
            f"{_c_str(s.attrs.get('note', ''))}, {flags}, {tp_off}, {tp_n}, {ins.tp_inst}, "
            f"{ins.tp_chains}, {tp_init}, {len(ins.tp_inits)}, {pre_off}, {post_off}, "
            f"{pre_n}, {post_n}, {chains_name}, {len(ins.chains)}}},")

    asm += ["    .globl _ua_code_end", "_ua_code_end:", "    .long 0", ""]
    c.append("")
    c.append("const ua_insn ua_insns[] = {")
    c.extend(table)
    c.append("};")
    c.append(f"const size_t ua_n_insns = {len(insns)};")
    c.append(f"const uint32_t ua_code_words = {off};")
    c.append("const uint32_t ua_code_marks[] = {" + ", ".join(str(m) for m in marks) + "};")
    c.append(f"const size_t ua_n_code_marks = {len(marks)};")
    c.append("")
    return "\n".join(asm), "\n".join(c)


def load(paths: list[Path]) -> list[Insn]:
    specs: list[Spec] = []
    for p in paths:
        specs.extend(parse_defs(p.read_text(), str(p.name)))
    names: dict[str, str] = {}
    for s in specs:
        if s.name in names:
            raise DefError(f"{s.where}: duplicate entry {s.name!r} (first at {names[s.name]})")
        names[s.name] = s.where
    return [expand(s) for s in specs]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("defs", nargs="+", type=Path)
    ap.add_argument("--asm", type=Path, required=True)
    ap.add_argument("--c", type=Path, required=True)
    args = ap.parse_args(argv)
    try:
        insns = load(sorted(args.defs))
    except DefError as e:
        print(f"gen_insns: {e}", file=sys.stderr)
        return 1
    asm, c = emit(insns)
    args.asm.write_text(asm)
    args.c.write_text(c)
    n_chains = sum(len(i.chains) for i in insns)
    print(f"gen_insns: {len(insns)} entries, {n_chains} latency chains")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
