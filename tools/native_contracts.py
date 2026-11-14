"""Generate address-independent native contracts from a locally owned reference.

Generation requires pefile and Capstone. Neither is used by the shipped runtime.
Only hashes, reference locations and short leaf locators are emitted, not game bodies.
"""

import argparse
import bisect
import hashlib
import json
import re
import struct
from collections import defaultdict
from pathlib import Path


DATA_ROLES = {
    0x1ad8290, 0x1e09e58, 0x1e09e64, 0x1e0a234, 0x1ec1dd0, 0x1ffb0b0,
    0x2006f00, 0x20086d8, 0x20086f8, 0x200b5a8, 0x20119d8, 0x2011c08,
    0x201b8e0, 0x201e218, 0x201e310, 0x201e318, 0x201e370, 0x201e378, 0x201ebc0, 0x2020f38,
}


class Image:
    def __init__(self, path):
        import capstone
        import pefile

        self.cs = capstone
        self.pe = pefile.PE(str(path))
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.imports = {
            imported.address - self.base: (library.dll.decode("ascii").lower(),
                                           imported.name.decode("ascii") if imported.name else "",
                                           imported.ordinal or 0)
            for library in self.pe.DIRECTORY_ENTRY_IMPORT for imported in library.imports
        }
        self.absolute_relocations = {
            entry.rva for block in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", ()) for entry in block.entries if entry.type == 10
        }
        self.entries = {e.struct.BeginAddress: e.struct for e in self.pe.DIRECTORY_ENTRY_EXCEPTION}
        self.starts = sorted(self.entries)
        self.groups = defaultdict(list)
        self.owners = {}
        self.decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.decoder.detail = True
        for start, entry in self.entries.items():
            seen = set()
            parent = entry
            while True:
                if parent.UnwindData in seen:
                    raise ValueError("Cyclic reference unwind chain")
                seen.add(parent.UnwindData)
                header = struct.unpack("<I", self.pe.get_data(parent.UnwindData, 4))[0]
                if not (header >> 3) & 4:
                    break
                extra = parent.UnwindData + 4 + (((header >> 16 & 255) + 1) & ~1) * 2
                begin, end, unwind = struct.unpack("<III", self.pe.get_data(extra, 12))
                parent = self.entries[begin]
                if (parent.EndAddress, parent.UnwindData) != (end, unwind):
                    raise ValueError("Inexact reference unwind parent")
            self.owners[start] = parent.BeginAddress
            self.groups[parent.BeginAddress].append((start, entry.EndAddress))

    def section(self, rva):
        return next((s for s in self.pe.sections
                     if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)), None)

    def owner(self, rva):
        index = bisect.bisect_right(self.starts, rva) - 1
        if index < 0:
            return None
        start = self.starts[index]
        return self.owners[start] if rva < self.entries[start].EndAddress else None

    def immutable_string(self, rva):
        section = self.section(rva)
        if section is None or section.Characteristics & 0xe0000000 != 0x40000000 or rva in self.imports:
            return None
        available = section.SizeOfRawData - (rva - section.VirtualAddress)
        if available <= 0:
            return None
        data = self.pe.get_data(rva, min(512, available))
        for width in (2, 1):
            characters = 0
            for offset in range(0, len(data) - width + 1, width):
                value = int.from_bytes(data[offset:offset + width], "little")
                if value == 0:
                    if characters >= 4:
                        value = data[:offset + width]
                        return len(value), hashlib.sha256(value).hexdigest()
                    break
                if not (32 <= value <= 126 or value in (9, 10, 13)):
                    break
                characters += 1
        return None

    def ranges(self, root):
        if root in self.groups:
            return sorted(self.groups[root])
        next_entry = bisect.bisect_right(self.starts, root)
        limit = min(root + 4096, self.starts[next_entry] if next_entry < len(self.starts) else self.size)
        pending, instructions = [root], {}
        while pending:
            address = pending.pop()
            while address not in instructions:
                if not root <= address < limit:
                    raise ValueError(f"Unbounded leaf control flow at {root:x}")
                instruction = next(self.decoder.disasm(self.pe.get_data(address, 15),
                                                       self.base + address, count=1), None)
                if instruction is None or address + instruction.size > limit:
                    raise ValueError(f"Undecodable leaf at {root:x}")
                instructions[address] = instruction
                if instruction.group(self.cs.CS_GRP_RET):
                    break
                if instruction.group(self.cs.CS_GRP_JUMP):
                    operand = instruction.operands[0]
                    if operand.type == self.cs.x86.X86_OP_IMM:
                        target = operand.imm - self.base
                        if root <= target < limit and target not in self.entries:
                            pending.append(target)
                    if instruction.mnemonic == "jmp":
                        break
                address += instruction.size
        return [(root, max(address + instruction.size for address, instruction in instructions.items()))]

    def contract(self, root, roles):
        ranges = self.ranges(root)
        pending = [begin for begin, _ in ranges]
        decoded = {}
        while pending:
            address = pending.pop()
            while address not in decoded:
                containing = next(((a, b) for a, b in ranges if a <= address < b), None)
                if containing is None:
                    break
                instruction = next(self.decoder.disasm(self.pe.get_data(address, min(15, containing[1] - address)),
                                                       self.base + address, count=1), None)
                if instruction is None:
                    raise ValueError(f"Undecodable reachable reference instruction at {address:x}")
                decoded[address] = instruction
                if instruction.group(self.cs.CS_GRP_RET) or instruction.mnemonic in ("int3", "ud2", "hlt"):
                    break
                if instruction.group(self.cs.CS_GRP_JUMP):
                    operand = instruction.operands[0]
                    if operand.type == self.cs.x86.X86_OP_IMM:
                        target = operand.imm - self.base
                        if any(a <= target < b for a, b in ranges):
                            pending.append(target)
                    if instruction.mnemonic == "jmp":
                        break
                address += instruction.size
        output, references = bytearray(), []
        for begin, end in ranges:
            data = self.pe.get_data(begin, end - begin)
            instructions = [instruction for address, instruction in sorted(decoded.items()) if begin <= address < end]
            start = len(output)
            output.extend(data)
            for instruction in instructions:
                position = start + instruction.address - self.base - begin
                for operand in instruction.operands:
                    target, offset, width, kind = None, 0, 0, "relative"
                    if operand.type == self.cs.x86.X86_OP_MEM and operand.mem.base == self.cs.x86.X86_REG_RIP:
                        target = instruction.address + instruction.size + operand.mem.disp - self.base
                        # RIP displacement is always disp32, including operand-size-prefixed word accesses.
                        offset, width = instruction.disp_offset, 4
                    elif operand.type == self.cs.x86.X86_OP_IMM:
                        if instruction.group(self.cs.CS_GRP_CALL) or instruction.group(self.cs.CS_GRP_JUMP):
                            if not any(self.base + a <= operand.imm < self.base + b for a, b in ranges):
                                target = operand.imm - self.base
                                offset, width = instruction.imm_offset, instruction.imm_size
                        elif (self.base <= operand.imm < self.base + self.size and
                              instruction.address - self.base + instruction.imm_offset in self.absolute_relocations):
                            target = operand.imm - self.base
                            offset, width, kind = instruction.imm_offset, instruction.imm_size, "absolute"
                    if not width:
                        continue
                    section = self.section(target)
                    if section is None and target != 0:
                        raise ValueError(f"Unmapped reference {target:x} in {root:x}")
                    flags = section.Characteristics & 0xe0000000 if section else 0
                    references.append(dict(position=position + offset, instruction=position,
                                           target=target, source_target=target,
                                           flags=flags, width=width, instruction_size=instruction.size, kind=kind))
                    output[position + offset:position + offset + width] = bytes(width)
        return dict(entry=root, size=len(output), digest=hashlib.sha256(output).hexdigest(),
                    references=references, leaf=root not in self.groups, prefix=bytes(output[:16]), ranges=ranges)

    def data_anchors(self, roles):
        candidates = defaultdict(set)
        for section in self.pe.sections:
            if not section.Characteristics & 0x20000000:
                continue
            data = section.get_data()
            for phase in range(4):
                length=(len(data)-phase)//4*4
                for index, (displacement,) in enumerate(struct.iter_unpack("<i", data[phase:phase + length])):
                    address = section.VirtualAddress + phase + index * 4
                    for tail in (0, 1, 2, 4):
                        target = address + 4 + tail + displacement
                        if target in roles:
                            owner = self.owner(address)
                            if owner is not None:
                                candidates[target].add(owner)
        selected = set()
        for target in sorted(roles):
            valid = []
            for owner in candidates[target]:
                try:
                    contract = self.contract(owner, roles)
                except ValueError:
                    continue
                if any(reference["source_target"] == target for reference in contract["references"]):
                    valid.append((contract["size"] < 64, contract["size"], owner))
            selected.update(row[2] for row in sorted(valid)[:3])
        return selected


def generate(reference, runtime, output):
    image = Image(reference)
    roots = {int(value, 16) for value in re.findall(r"\(image,(0x[0-9a-f]+)\)", runtime.read_text())}
    roots = {rva for rva in roots if image.section(rva).Characteristics & 0x20000000}
    roots.add(image.pe.OPTIONAL_HEADER.AddressOfEntryPoint)
    allocation_header = runtime.parent / "outlast_target_roles.h"
    for address in re.findall(r"\{(0x[0-9a-f]+),OutlastTargetRole::", allocation_header.read_text()):
        roots.add(image.owner(int(address, 16)) or int(address, 16))
    roots.add(0x8bb6f0)
    for address in (0xc47aed, 0x2fa4f0, 0x61f840):
        roots.add(image.owner(address) or address)
    constructor = image.contract(0x8c7670, set())
    for reference in constructor["references"]:
        target = reference["source_target"]
        if 0x800000 <= target < 0xa00000 and image.section(target).Characteristics & 0x20000000:
            roots.add(image.owner(target) or target)
    print("Finding native global-layout anchors...", flush=True)
    roots.update(image.data_anchors(DATA_ROLES))
    for root in sorted(roots):
        contract = image.contract(root, set())
        if contract["size"] <= 128:
            for reference in contract["references"]:
                target = reference["source_target"]
                section = image.section(target)
                if section is not None and section.Characteristics & 0x20000000:
                    roots.add(image.owner(target) or target)
    roles = roots | DATA_ROLES | image.imports.keys()
    contracts = [image.contract(root, roles) for root in sorted(roots)]
    immutable = {}
    for contract in contracts:
        for ref in contract["references"]:
            target = ref["source_target"]
            identity = image.immutable_string(target)
            if identity is not None:
                immutable[target] = identity
    lines = ['#pragma once', '#include "outlast_native_contract.h"', '',
             '// Generated by tools/native_contracts.py from a locally owned reference; no game bodies.',
             'namespace olvr {']
    for slot, (dll, name, ordinal) in sorted(image.imports.items()):
        lines.append(f"inline constexpr NativeImportContract native_import_{slot:x}"
                     f"={{{json.dumps(dll)},{json.dumps(name)},{ordinal}}};")
    for target, (size, digest) in sorted(immutable.items()):
        lines.append(f'inline constexpr NativeImmutableContract native_immutable_{target:x}={{{size},"{digest}"}};')
    lines.append("inline constexpr NativeCodeRange outlast_native_initial_globals[]={")
    for target in sorted(DATA_ROLES - {0x1ad8290}):
        size = 16 if target in (0x201e310, 0x201e370) else 4 if target in (
            0x1e09e58, 0x1e09e64, 0x1e0a234, 0x201e318, 0x201e378) else 8
        if any(image.pe.get_data(target, size)):
            raise ValueError(f"Initial global {target:x} is not source-bound zero data")
        lines.append(f"    {{{hex(target)},{size}}},")
    lines.append("};")
    for contract in contracts:
        lines.append(f"inline constexpr NativeCodeRange native_ranges_{contract['entry']:x}[]={{"+
                     ",".join("{"+hex(a)+","+hex(b-a)+"}" for a,b in contract["ranges"])+"};")
        name = f"native_references_{contract['entry']:x}"
        if contract["references"]:
            lines.append(f"inline constexpr NativeCodeReference {name}[]={{")
            for ref in contract["references"]:
                imported = f"&native_import_{ref['source_target']:x}" if ref["source_target"] in image.imports else "nullptr"
                identity = f"&native_immutable_{ref['source_target']:x}" if ref["source_target"] in immutable else "nullptr"
                lines.append("    {%s,%s,%s,%s,%s,%s,NativeReferenceKind::%s,%s,%s}," % (
                    hex(ref["position"]), hex(ref["instruction"]), hex(ref["target"]), hex(ref["flags"]),
                    ref["width"], ref["instruction_size"], ref["kind"], imported, identity))
            lines.append("};")
    lines.append("inline constexpr NativeFunctionContract outlast_native_contracts[]={")
    for contract in contracts:
        count = len(contract["references"])
        refs = f"native_references_{contract['entry']:x}" if count else "nullptr"
        suffix = ",false,{},0"
        if contract["leaf"]:
            prefix = ",".join(hex(byte) for byte in contract["prefix"])
            suffix = ",true,{" + prefix + "}," + str(len(contract["prefix"]))
        lines.append('    {%s,%s,"%s",%s,%s%s,native_ranges_%x,%s},' % (
            hex(contract["entry"]), hex(contract["size"]), contract["digest"], refs, count, suffix,
            contract["entry"], len(contract["ranges"])))
    lines.extend(["};", "}", ""])
    output.write_text("\n".join(lines))
    print("Generated", len(contracts), "native contracts and",
          sum(len(contract["references"]) for contract in contracts), "validated reference locations", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, default=Path(__file__).resolve().parents[1] / "src" / "outlast_runtime.cpp")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    generate(arguments.reference, arguments.runtime, arguments.output)
