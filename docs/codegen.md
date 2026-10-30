# Code generation: flag liveness and register locals

This experiment uses the fingerprint in `config/ac1_dx9.json`, starting at
`786ecf1`. Original/generated C, object disassembly, captures, executables and
logs stay under ignored `work/codegen/`, `work/gameplay/` and `build/`.
No reference checkout or installed game file is changed.

## Baseline facts

Five functions from `docs/hot-functions.md` cover nonleaf transforms, SSE,
integer search, integer audio, and x87 quantization. These are static sites,
not execution counts. Source counts exclude comments. `CPU refs` counts
explicit `c->` expressions; direct stores are their assignment sites, excluding
macro/helper effects. Thus subtraction is not a complete load count: compound
assignments and subregister macros can read and write the same field.

| VA | Role | CPU refs | Direct CPU stores | Eager arithmetic helper sites | Eager lanes | x87 helper sites |
|---|---|---:|---:|---:|---:|---:|
| `004F5A70` | nonleaf transform/quantization | 838 | 339 | 27 | 158 | 42 |
| `00428540` | SSE quaternion/matrix | 423 | 165 | 2 | 11 | 0 |
| `0051DD60` | integer binary search | 68 | 21 | 10 | 55 | 0 |
| `00841BA0` | integer audio decoder | 126 | 39 | 20 | 113 | 0 |
| `004F2390` | x87 quantization | 115 | 51 | 4 | 23 | 18 |

The helper-site column covers `flags_add/sub/adc/sbb/logicz/incs/decs`.
Logical helpers write five lanes and preserve AF; arithmetic writes six;
INC/DEC preserve CF. Shift helpers/direct flag assignments are additional.
Every listed eager helper computes PF. The x87 counts include checks, integer
conversion, `fpush`, `fpop`, and `fst`; they do not mean out-of-line host calls.

Objects are built with GCC 12.2, RelWithDebInfo (`-O2 -g -DNDEBUG`), direct
memory, EIP tracing and forced accessor inlining. Disassembly uses
`C:/msys64/mingw64/bin/objdump.exe -dr -Mintel --insn-width=16` on the real
`CMakeFiles/ac1_runtime.dir/work/lift_initial/initial_NNNN.c.obj` files.
Private source/object inventories and extracted functions are
`work/codegen/{baseline-facts,object-facts}.json` and `baseline-<VA>.{c,asm}`.
CPU-memory counts below are conservative static counts through proven CPU
pointer bases; derived/indirect pointers can add accesses. EIP, flags and FPU
environment stores are included. A vector store counts once, even if it writes
multiple fields. Parity counts recognize the byte-wide shift-by-four starting
the XOR-fold recipe, rather than counting arbitrary shifts or host PF use.

| VA | Host instructions | CPU loads/stores (proved) | Flag store sites / lanes | Parity folds | x87 helper calls |
|---|---:|---:|---:|---:|---:|
| `004F5A70` | 5,320 | 648 / 861 | 102 / 105 | 18 | 0 |
| `00428540` | 1,397 | 255 / 200 | 6 / 6 | 1 | 0 |
| `0051DD60` | 435 | 26 / 78 | 34 / 34 | 6 | 0 |
| `00841BA0` | 911 | 71 / 143 | 59 / 60 | 10 | 0 |
| `004F2390` | 961 | 121 / 182 | 18 / 18 | 3 | 0 |

The first two functions retain their CPU pointer in RBX throughout their
bodies; the quantizer also derives a pointer into the FP state. x87 helpers
inline into environment/tag checks and stores. GCC already eliminates some
overwritten flags: the two source helpers in `00428540` leave only six lane
stores and one parity fold in the object. Source helper counts alone therefore
overstate the remaining opportunity.

