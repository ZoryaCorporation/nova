# Nova Optimizer Blueprint

Design reference for `nova_opt.c`. Externalizes all design decisions
so the implementation can be written without lengthy planning pauses.

## File Structure

```
nova_opt.c (~1200 lines estimated)
├── Part 0: File header + includes
├── Part 1: Internal helpers (NOP-ify, reachability bitmap)
├── Part 2: Peephole pass
├── Part 3: Constant folding pass
├── Part 4: Jump optimization pass
├── Part 5: Tail call detection pass
├── Part 6: Dead code elimination pass  
├── Part 7: Return specialization pass
├── Part 8: NOP squeeze (compaction)
├── Part 9: Line info rebuild after squeeze
├── Part 10: Public API (nova_optimize, nova_optimize_stats, nova_opt_dump_stats)
```

## Part 0: Includes

```c
#include "nova/nova_opt.h"    /* Our header */
#include "nova/nova_opcode.h" /* Decode/encode macros, opcode_info */
#include "nova/nova_proto.h"  /* NovaProto, NovaConstant, etc. */
#include "nova/nova_conf.h"   /* Limits */
#include <string.h>           /* memcpy, memmove */
#include <stdio.h>            /* fprintf for dump_stats */
```

## Part 1: Internal Helpers

### NOP-ify instruction
Replace an instruction with NOP, preserving its line number.
```c
static void novai_nopify(NovaProto *proto, uint32_t pc)
```
Just sets `proto->code[pc] = NOVA_ENCODE_ABC(NOVA_OP_NOP, 0, 0, 0)`.

### Build reachability bitmap
Allocate a `uint8_t` array of `code_count` bytes. Walk forward from pc=0, 
following all jump targets. Mark each reachable instruction. Return bitmap.
Used by dead code elimination.

```c
static uint8_t *novai_build_reachable(const NovaProto *proto)
```

Algorithm:
1. Allocate `uint8_t reachable[code_count]` (calloc → zeroed)
2. Use a simple worklist: `uint32_t worklist[MAX_WORKLIST]` stack
3. Push pc=0
4. While worklist not empty:
   - Pop pc
   - If already marked, skip
   - Mark reachable[pc] = 1
   - Get opcode at pc
   - If JMP: push target (pc + 1 + sBx), do NOT push pc+1 (unconditional)
   - If comparison/test (testflag=1): push pc+1 AND pc+2 (skip variant)
   - If FORPREP: push target AND pc+1
   - If FORLOOP/TFORLOOP: push target AND pc+1
   - If RETURN/RETURN0/RETURN1/TAILCALL: do not push successors
   - Otherwise: push pc+1
5. Return bitmap

Worklist size: Use dynamic array or cap at code_count.

### Get jump target
```c
static uint32_t novai_get_jmp_target(const NovaProto *proto, uint32_t pc)
```
Returns `pc + 1 + NOVA_GET_SBX(proto->code[pc])` for JMP-class opcodes.
Returns UINT32_MAX if out of bounds.

### Is NOP check
```c
static int novai_is_nop(NovaInstruction instr)
```
Returns 1 if `NOVA_GET_OPCODE(instr) == NOVA_OP_NOP`.

### Get constant value for folding
```c
static int novai_get_const_integer(const NovaProto *proto, uint8_t rk, nova_int_t *out)
```
If rk >= NOVA_RK_CONST_BASE, look up constant. If it's NOVA_CONST_INTEGER, write to *out and return 1. Else return 0.

```c
static int novai_get_const_number(const NovaProto *proto, uint8_t rk, nova_number_t *out)
```
Same for NOVA_CONST_NUMBER. Also accept NOVA_CONST_INTEGER (promote to number).

## Part 2: Peephole Pass

Single forward sweep. Pattern-match local instruction sequences and rewrite.

```c
static void novai_pass_peephole(NovaProto *proto, NovaOptStats *stats)
```

### Patterns to detect:

**P1: Redundant MOVE**
```
MOVE R(A), R(A)  →  NOP
```
Check: A == B, opcode == NOVA_OP_MOVE.

**P2: MOVE then overwrite**
```
MOVE R(A), R(B)   ; pc
<any>  R(A) = ... ; pc+1 (sets register A without reading it)
```
If the next instruction sets A (check setsflag) and doesn't read B=A in its own B/C fields, the MOVE is dead. NOP it.
This is tricky -- only do the simple "next instr sets A" check.

**P3: LOADBOOL skip optimization**
```
LOADBOOL R(A), 0, 1    ; skip next
LOADBOOL R(A), 1, 0    ; no skip
```
This is already the canonical form from the compiler. No change needed, but verify.

**P4: Double NOT elimination**
```
NOT R(A), R(B)
NOT R(C), R(A)    ; where C == B
```
If A is only used by the second NOT, collapse to MOVE R(B), R(B) → NOP, or just NOP both.
Simpler: if two consecutive NOTs where second reads result of first and writes to original source, NOP both.

**P5: MOVE into RETURN**
```
MOVE R(A), R(B)     ; pc
RETURN1 R(A)        ; pc+1
```
Can become: `RETURN1 R(B)` and NOP the MOVE.
Also for RETURN: if A of return matches A of MOVE, rewrite RETURN's A to B.

**P6: Redundant LOADNIL**
```
LOADNIL R(A), B     ; pc
LOADNIL R(A), C     ; pc+1, where C >= B
```
Second LOADNIL subsumes the first. NOP the first if range is contained.
Simpler: if same A and second B >= first B, NOP first.

## Part 3: Constant Folding Pass

Forward sweep looking for arithmetic on constants.

```c
static void novai_pass_const_fold(NovaProto *proto, NovaOptStats *stats)
```

### Patterns:

**CF1: ADDK/SUBK/MULK/DIVK/MODK with constant B**
If R(B) is a LOADK or LOADINT immediately preceding, AND R(B) isn't used elsewhere
between the LOADK and the arithmetic, we can fold.

Actually, the simpler and more reliable approach: look for arithmetic opcodes
where BOTH operands are RK constants. E.g.:
```
ADD R(A), RK(B), RK(C)  where both B and C are constants
```
If both are NOVA_CONST_INTEGER, compute result, add to constant pool as new
constant, replace with LOADK R(A), new_const_index.

Similarly for SUB, MUL, IDIV, MOD (integer only for safety).
For ADD/SUB/MUL/DIV with NOVA_CONST_NUMBER, fold as well (but be careful
about floating point semantics -- skip div-by-zero).

**CF2: ADDI with LOADINT**
```
LOADINT R(A), sBx    ; small integer
ADDI    R(A), sBx2   ; add immediate
```
Can fold to single LOADINT R(A), sBx+sBx2 if result fits in sBx range.

**CF3: UNM on constant**
```
LOADK   R(A), K(idx)  ; K is integer or number
UNM     R(B), R(A)    ; B could equal A
```
If K is integer, add -K to pool, replace with LOADK R(B), new_idx, NOP the UNM.

## Part 4: Jump Optimization Pass

```c
static void novai_pass_jump_opt(NovaProto *proto, NovaOptStats *stats)
```

### Patterns:

**J1: Jump to NOP chain**
If a JMP targets a NOP, skip forward to the next non-NOP instruction.
(Useful after other passes create NOPs.)

**J2: Jump chain collapse**
If JMP targets another JMP (with A==0, no upvalue close), retarget to
the final destination. Follow chains up to a limit (16 hops) to avoid
infinite loops in malformed bytecode.

```
JMP  +5   ; target is another JMP +3
→ JMP +8  ; skip the intermediate
```

**J3: Jump to next instruction**
```
JMP +0  (sBx == 0, target is pc+1)  →  NOP
```
The jump goes nowhere. NOP it (if A==0, no upvalue close needed).
If A != 0, keep it — it's a CLOSE+JMP.

**J4: Comparison + JMP to next**
```
EQ  A, R(B), R(C)   ; skip next if test fails
JMP +0               ; the jump goes nowhere
```
Both the comparison and JMP are dead. NOP both.
But be careful: only if the JMP's sBx is 0.

**J5: Conditional skip over NOP**
```
EQ A, R(B), R(C)     ; skip next on fail
NOP                   ; was something that got NOPed
```
The skip is now useless. NOP the comparison too. But this is dangerous 
if the comparison has side effects (it doesn't in Nova). NOP both.

Patch procedure for retargeting jumps:
- Compute new sBx = (new_target - (pc + 1))
- Validate range: NOVA_MIN_SBX <= new_sBx <= NOVA_MAX_SBX
- Use NOVA_SET_SBX or rebuild instruction

## Part 5: Tail Call Detection

```c
static void novai_pass_tailcall(NovaProto *proto, NovaOptStats *stats)
```

### Pattern:
```
CALL R(A), B, C       ; pc
RETURN R(A), C        ; pc+1, returning what CALL produced
```
If CALL at pc is immediately followed by RETURN at pc+1, AND the
return register == call register, AND return count matches, convert
CALL to TAILCALL and NOP the RETURN.

More precisely:
- `code[pc]`   = CALL R(A), B, C
- `code[pc+1]` = RETURN R(A2), D
  - A2 == A (returning from same register CALL writes to)
  - D == C (same number of return values), OR D == 0 (return all)

Rewrite: 
- `code[pc]` = TAILCALL R(A), B, 0   (C=0 means return all results)
- `code[pc+1]` = NOP (or RETURN0 as safety for VM fallthrough)

Also handle RETURN1:
```
CALL R(A), B, 2       ; expects 1 result
RETURN1 R(A)          ; return that one result  
```
→ TAILCALL R(A), B, 0

And RETURN0 after void CALL:
```
CALL R(A), B, 1       ; C=1 means 0 results
RETURN0               ; return nothing
```
This is NOT a tail call (the call discards results). Skip.

## Part 6: Dead Code Elimination

```c
static void novai_pass_dead_code(NovaProto *proto, NovaOptStats *stats)
```

Use the reachability bitmap from Part 1. Any unreachable instruction
that isn't already a NOP gets NOPed.

Simple approach: after all other passes, build reachability graph,
NOP everything unreachable.

Edge case: Don't NOP the very last instruction if it's a RETURN variant
(it's the function's epilogue and must exist).

## Part 7: Return Specialization

```c
static void novai_pass_return_spec(NovaProto *proto, NovaOptStats *stats)
```

### Patterns:

**R1: RETURN R(A), 1 → RETURN0**
RETURN with B=1 means 0 return values. Convert to RETURN0.
```
RETURN R(A), 1, 0  →  RETURN0
```

**R2: RETURN R(A), 2 → RETURN1 R(A)**
RETURN with B=2 means 1 return value. Convert to RETURN1.
```
RETURN R(A), 2, 0  →  RETURN1 R(A)
```

These specialized forms are slightly faster in the VM (no B-field decode).

## Part 8: NOP Squeeze (Compaction)

After all passes, many NOPs may be scattered through the code.
Squeeze them out and adjust all jump targets.

```c
static void novai_pass_squeeze(NovaProto *proto, NovaOptStats *stats)
```

Algorithm:
1. Build a `remap[]` array: `uint32_t remap[code_count]`
   - For each pc, remap[pc] = new_pc after removing NOPs.
   - Walk forward, maintaining a write cursor. Skip NOPs.
   
2. Walk all instructions and adjust jump offsets:
   - For JMP: new_sBx = remap[old_target] - (remap[pc] + 1)
   - For FORPREP/FORLOOP/TFORLOOP: same treatment
   - For comparisons (testflag=1): the skip is always +1 relative,
     but we need to make sure the instruction after is still the JMP.
     Actually, comparisons skip the NEXT instruction — since we're
     compacting, the comparison and its JMP move together (both are
     non-NOP). So no adjustment needed for comparison skip semantics?
     
     Wait — comparisons say "if test fails, pc++". The pc++ is relative.
     If both the comparison and the JMP survive (neither is NOP), they
     stay adjacent. If the JMP was NOPed (pattern J4/J5), the comparison
     was also NOPed. So comparisons should be fine.

3. Compact: move instructions to their remap positions.
   - Walk `write = 0`, for each pc: if not NOP, code[write] = code[pc], write++.
   - Update code_count = write.

4. Update line_numbers array similarly.

## Part 9: Line Info Rebuild

After the squeeze, rebuild `proto->lines.line_numbers` to match the
compacted instruction array.

```c
static void novai_rebuild_lines(NovaProto *proto, const uint32_t *remap, 
                                uint32_t old_count)
```

Walk old line_numbers, copy line_numbers[pc] to line_numbers[remap[pc]]
for each non-NOP instruction.

Actually simpler: during the squeeze in Part 8, just compact line_numbers
in the same loop as instructions.

## Part 10: Public API

### nova_optimize
```c
void nova_optimize(NovaProto *proto, int level) {
    nova_optimize_stats(proto, level, NULL);
}
```

### nova_optimize_stats
```c
void nova_optimize_stats(NovaProto *proto, int level, NovaOptStats *stats) {
    NovaOptStats local_stats;
    memset(&local_stats, 0, sizeof(local_stats));
    
    if (proto == NULL || level <= NOVA_OPT_NONE) return;
    if (proto->code == NULL || proto->code_count == 0) return;
    
    local_stats.total_before = proto->code_count;
    
    /* Level 1+: basic passes */
    novai_pass_peephole(proto, &local_stats);
    novai_pass_jump_opt(proto, &local_stats);
    novai_pass_return_spec(proto, &local_stats);
    
    /* Level 2+: full passes */
    if (level >= NOVA_OPT_FULL) {
        novai_pass_const_fold(proto, &local_stats);
        novai_pass_tailcall(proto, &local_stats);
        novai_pass_dead_code(proto, &local_stats);
    }
    
    /* Always: compact NOPs */
    novai_pass_squeeze(proto, &local_stats);
    
    local_stats.total_after = proto->code_count;
    
    /* Recurse into sub-protos */
    for (uint32_t i = 0; i < proto->proto_count; i++) {
        nova_optimize_stats(proto->protos[i], level, &local_stats);
    }
    
    if (stats != NULL) {
        /* Accumulate into caller's stats */
        stats->peephole_rewrites   += local_stats.peephole_rewrites;
        stats->constants_folded    += local_stats.constants_folded;
        stats->jumps_shortened     += local_stats.jumps_shortened;
        stats->jumps_removed       += local_stats.jumps_removed;
        stats->tailcalls_detected  += local_stats.tailcalls_detected;
        stats->dead_instructions   += local_stats.dead_instructions;
        stats->returns_specialized += local_stats.returns_specialized;
        stats->nops_removed        += local_stats.nops_removed;
        stats->total_before        += local_stats.total_before;
        stats->total_after         += local_stats.total_after;
    }
}
```

### nova_opt_dump_stats
Print to stderr in a table format.

## Key Constants / Macros

```c
#define NOVAI_MAX_CHAIN_HOPS  16   /* Max jumps followed in chain collapse */
#define NOVAI_MAX_WORKLIST   4096  /* Max worklist entries for reachability */
```

## Instruction Encoding Helpers Needed

Need NOVA_SET_SBX macro or inline. Check if it exists:
- `NOVA_SET_SBX(i, sbx)` — modify sBx in-place
- If not, we rebuild: `code[pc] = NOVA_ENCODE_ASBX(op, a, new_sbx)`

From nova_opcode.h we have NOVA_ENCODE_ASBX. So rebuild is:
```c
NovaOpcode op = NOVA_GET_OPCODE(code[pc]);
uint8_t a = NOVA_GET_A(code[pc]);
code[pc] = NOVA_ENCODE_ASBX(op, a, new_sbx);
```

## Edge Cases

1. **Empty proto**: code_count == 0 → return immediately
2. **Single instruction**: (just RETURN0) → nothing to optimize
3. **No NOPs created**: squeeze is a no-op (remap is identity)
4. **Sub-protos**: recurse after optimizing parent
5. **Upvalue close in JMP**: JMP with A > 0 means "close upvalues >= R(A)".
   Don't eliminate these JMPs even if they jump to next instruction.
6. **EXTRAARG**: LOADKX is followed by EXTRAARG. Don't separate them.
   The peephole/squeeze must keep LOADKX+EXTRAARG adjacent.
7. **SETLIST batching**: SETLIST may be followed by EXTRAARG for large
   table initializers. Keep them adjacent.

## Types Needed (internal)

None beyond what's in the headers. All state is local to each pass
function — no persistent optimizer state struct needed.
