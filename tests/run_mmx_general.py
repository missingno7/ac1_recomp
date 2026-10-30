#!/usr/bin/env python3
"""Generate real audio/Bink functions and MMX instruction probes, then run native oracles."""
from __future__ import annotations
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "work/mmx_evidence/oracle"
sys.dont_write_bytecode = True


def run(*, flag_liveness='off', local_regs='off', x87_inline=False):
    if local_regs not in ('off','all'):
        raise ValueError('MMX oracle local_regs must be off or all')
    start = time.perf_counter()
    WORK.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("mmx_general_lift", ROOT / "tools/lift_ac1.py")
    lift = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lift)
    include = lift.prepare_test_runtime()
    sys.path[:0] = [str(lift.DUKE_DEFAULT / "tools/pe"), str(lift.DUKE_DEFAULT / "tools/disasm"),
                    str(lift.DUKE_DEFAULT / "tools/lift")]
    from pe_analyze import analyze_pe, build_iat_map
    from disasm32 import Disassembler
    from lift32_cpu import Lifter
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM
    cc, cxx = shutil.which("gcc"), shutil.which("g++")
    if not cc or not cxx or not shutil.which("objcopy"):
        raise RuntimeError("gcc, g++ and objcopy are required for native MMX oracle checks")

    inline_options = ['-DAC1_FORCE_INLINE_X87=1'] if x87_inline else []
    operations = []
    binary = ("movq paddb paddw paddd paddq psubb psubw psubd psubq paddsb paddsw psubsb psubsw "
              "paddusb paddusw psubusb psubusw pand pandn por pxor pcmpeqb pcmpeqw pcmpeqd "
              "pcmpgtb pcmpgtw pcmpgtd packsswb packssdw packuswb punpcklbw punpcklwd punpckldq "
              "punpckhbw punpckhwd punpckhdq pmullw pmulhw pmulhuw pmaddwd pmuludq "
              "pavgb pavgw pminub pmaxub pminsw pmaxsw psadbw psllw pslld psllq "
              "psrlw psrld psrlq psraw psrad").split()
    for name in binary:
        operations.append((name, f"{name} mm0, mm1", f"{name} %%mm1, %%mm0", "mm0"))
        operations.append((name+"_self", f"{name} mm0, mm0", f"{name} %%mm0, %%mm0", "mm0"))
    for name in "psllw pslld psllq psrlw psrld psrlq psraw psrad".split():
        for count in (0, 1, 15, 16, 31, 32, 63, 64, 255):
            operations.append((f"{name}_{count}", f"{name} mm0, {count}", f"{name} ${count}, %%mm0", "mm0"))
    for imm in (0, 27, 78, 255):
        operations.append((f"pshufw_{imm}",f"pshufw mm0, mm1, {imm}",f"pshufw ${imm}, %%mm1, %%mm0","mm0"))
        operations.append((f"pshufw_self_{imm}",f"pshufw mm0, mm0, {imm}",f"pshufw ${imm}, %%mm0, %%mm0","mm0"))
    operations += [
        ("movd_in", "movd mm0, eax", "movd %%eax, %%mm0", "mm0"),
        ("movd_out", "movd eax, mm0", "movd %%mm0, %%eax", "rax"),
        ("pinsrw", "pinsrw mm0, eax, 2", "pinsrw $2, %%eax, %%mm0", "mm0"),
        ("pextrw", "pextrw eax, mm1, 3", "pextrw $3, %%mm1, %%eax", "rax"),
        ("pmovmskb", "pmovmskb eax, mm1", "pmovmskb %%mm1, %%eax", "rax"),
        ("movdq2q", "movdq2q mm0, xmm0", "movdq2q %%xmm0, %%mm0", "mm0"),
        ("movq2dq", "movq2dq xmm0, mm0", "movq2dq %%mm0, %%xmm0", "xmm0"),
        ("cvtps2pi", "cvtps2pi mm0, xmm0", "cvtps2pi %%xmm0, %%mm0", "mm0"),
        ("cvttps2pi", "cvttps2pi mm0, xmm0", "cvttps2pi %%xmm0, %%mm0", "mm0"),
        ("cvtpi2ps", "cvtpi2ps xmm0, mm0", "cvtpi2ps %%mm0, %%xmm0", "xmm0"),
    ]
    probes = {
        "mem_movd_read": "movd mm0, dword ptr [eax]", "mem_movd_write": "movd dword ptr [eax], mm0",
        "mem_movq_read": "movq mm0, qword ptr [eax]", "mem_movq_write": "movq qword ptr [eax], mm0",
        "mem_pinsrw": "pinsrw mm0, word ptr [eax], 1", "mem_movntq": "movntq qword ptr [eax], mm0",
        "maskmovq": "maskmovq mm0, mm1", "emms": "emms", "pushal": "pushad", "popal": "popad",
        "fnstenv": "fnstenv [edi]", "fldenv": "fldenv [edi]", "fnsave": "fnsave [edi]", "frstor": "frstor [edi]",
    }
    assembly = ".intel_syntax noprefix\n.code32\n.text\n"
    for _, intel, _, _ in operations:
        assembly += intel + "\n.balign 16,0x90\n"
    for intel in probes.values():
        assembly += intel + "\n.balign 16,0x90\n"
    (WORK / "ops.S").write_text(assembly)
    commands = [[cc, "-c", str(WORK / "ops.S"), "-o", str(WORK / "ops.o")],
                ["objcopy", "-O", "binary", "--only-section=.text", str(WORK / "ops.o"), str(WORK / "ops.bin")]]
    for command in commands:
        subprocess.run(command, check=True, capture_output=True, text=True)
    raw = (WORK / "ops.bin").read_bytes()
    lifter = Lifter("instruction-probe", 0x10000, None)
    source = '#include "cpu.h"\n'
    declarations = []
    natives = []
    rows = []
    for index, (name, _, instruction, result_reg) in enumerate(operations):
        ins = next(lifter.md.disasm(raw[index*16:index*16+16], 0x400000+index*16, count=1))
        lowering = lift.ac1_mmx_translation(lifter, ins)
        if lowering is None:
            raise RuntimeError(f"MMX probe did not lower: {ins.mnemonic} {ins.op_str}")
        body=f"void probe_{name}(CPU *c)\n{{\n    " + " ".join(lowering) + "\n}"
        if local_regs=='all':body=lift.local_xmm_function(lift.local_register_function(body))
        source += body+'\n'
        declarations.append(f"void probe_{name}(CPU *);")
        asm = ("movq %1, %%mm0\n\tmovq %2, %%mm1\n\tmovq %2, %%xmm0\n\t"
               "movl %k3, %%eax\n\t" + instruction + f"\n\tmovq %%{result_reg}, %0\n\temms\n\t")
        natives.append(f"static uint64_t native_{name}(uint64_t a,uint64_t b) {{ uint64_t r; "
                       "unsigned old=_mm_getcsr(); _mm_setcsr(oracle_mxcsr); "
                       f"__asm__ volatile({json.dumps(asm)} : \"=m\"(r) : \"m\"(a),\"m\"(b),\"r\"((uint32_t)b) "
                       ': "rax","mm0","mm1","xmm0"); '
                       "oracle_flags=_mm_getcsr()&63u; _mm_setcsr(old); return r; }\n")
        result_kind = 0 if result_reg=="mm0" else 1 if result_reg=="rax" else 2
        rows.append(f'{{"{name}",native_{name},probe_{name},{result_kind}}}')
    for index, (name, _) in enumerate(probes.items(), len(operations)):
        ins = next(lifter.md.disasm(raw[index*16:index*16+16], 0x500000+index*16, count=1))
        lowering = lift.ac1_mmx_translation(lifter, ins)
        if lowering is None:
            lowering = lift.ac1_state_translation(lifter, ins)
        if lowering is None:
            raise RuntimeError(f"state probe did not lower: {ins.mnemonic}")
        body=f"void probe_{name}(CPU *c)\n{{\n    " + " ".join(lowering) + "\n}"
        if local_regs=='all':body=lift.local_xmm_function(lift.local_register_function(body))
        source += body+'\n'
        declarations.append(f"void probe_{name}(CPU *);")
    mix_raw = bytes.fromhex("d9e8c3")
    mix_spec = {"address":0x600000,"size":3,"blocks":[{"start":0x600000,"end":0x600003,
                "instructions":[(0x600000,2),(0x600002,1)]}]}
    mix_body = lift.lift_exact_function(lifter,mix_spec,lambda va,n:mix_raw[va-0x600000:va-0x600000+n],X86_OP_IMM,X86_OP_MEM)
    mix_body=lift.flag_liveness_function(mix_body,flag_liveness)
    if local_regs=='all':mix_body=lift.local_xmm_function(lift.local_register_function(mix_body))
    source += "#define AC1_SET_GUEST_EIP(c,va) ((c)->eip=(va))\n" + mix_body
    declarations += ["void L_00600000(CPU *);", "void L_00834A90(CPU *);", "void L_1801E500(CPU *);"]
    (WORK / "ops.c").write_text(source)
    (WORK / "ops_declarations.h").write_text("\n".join(declarations))
    (WORK / "ops_native.inc").write_text("\n".join(natives) + "\nstatic Operator operators[] = {\n" + ",\n".join(rows) + "\n};\n")

    units = []
    for config_name, address, tag in (("ac1_dx9.json",0x834A90,"audio"),("binkw32.json",0x1801E500,"bink")):
        config=json.loads((ROOT / "config" / config_name).read_text())
        binary=Path(config["binary"])
        if hashlib.sha256(binary.read_bytes()).hexdigest()!=config["sha256"]:
            raise RuntimeError(f"pinned binary changed: {binary}")
        info=analyze_pe(str(binary)); dis=Disassembler(binary.read_bytes(),info.image_base,info.sections)
        lift.install_ac1_jump_table_bounds(dis)
        fn=dis.disassemble_function(address,build_iat_map(info))
        spec={"address":address,"size":fn.size,"instruction_count":fn.num_instructions,"blocks":[{
            "start":block.start,"end":block.end,"successors":block.successors,"is_exit":block.is_exit,
            "instructions":[(i.address,i.size) for i in block.instructions]} for block in fn.blocks.values()]}
        output=WORK / tag
        result=lift.emit_lift(binary,lift.DUKE_DEFAULT,[spec],output,100,
                              flag_liveness=flag_liveness,local_regs=local_regs)
        if result["lift_failures"]:
            raise RuntimeError(str(result["lift_failures"]))
        units += [output / name for name in result["translation_units"]]
    objects=[]
    for index, unit in enumerate([WORK / "ops.c", *units]):
        obj=WORK / f"generated_{index}.o"
        command=[cc,*inline_options,"-std=c11","-O2","-Wall","-Wextra","-Werror","-fwrapv","-fno-strict-aliasing",
                 "-I",str(include),"-c",str(unit),"-o",str(obj)]
        result=subprocess.run(command,capture_output=True,text=True)
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
        commands.append(command);objects.append(obj)
    exe=WORK / "compare.exe"
    command=[cxx,*inline_options,"-std=c++17","-O2","-Wall","-Wextra","-Werror","-I",str(include),"-I",str(WORK),
             str(ROOT / "tests/mmx_general_native.cpp"),*[str(obj) for obj in objects],"-o",str(exe)]
    commands.append(command)
    result=subprocess.run(command,capture_output=True,text=True)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    (WORK / "native.log").write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    report={"operations":len(operations),"output":result.stdout,"commands":commands,
            "flag_liveness":flag_liveness,"local_regs":local_regs,"x87_inline":x87_inline,
            "seconds":round(time.perf_counter()-start,3)}
    (WORK / "report.json").write_text(json.dumps(report,indent=2)+"\n")
    print(result.stdout.strip())
    return report


if __name__ == "__main__":
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flag-liveness',choices=('off','straight','cfg'),default='off')
    parser.add_argument('--local-regs',choices=('off','all'),default='off')
    parser.add_argument('--x87-inline',action='store_true')
    args=parser.parse_args()
    run(flag_liveness=args.flag_liveness,local_regs=args.local_regs,x87_inline=args.x87_inline)
