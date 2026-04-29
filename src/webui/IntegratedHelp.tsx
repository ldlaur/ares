import { Component, createEffect, For, onCleanup, Show } from "solid-js";


let string = `
add rd, rs1, rs2
rd = rs1 + rs2

addi rd, rs1, imm
rd = rs1 + imm

and rd, rs1, rs2
rd = rs1 & rs2

andi rd, rs1, imm
rd = rs1 & imm

auipc rd, imm
rd = pc + (imm << 12)

beq rs1, rs2, label
if (rs1 == rs2) pc = label

beqz rs1, label
(beq rs1, zero, label)
if (rs1 == 0) pc = label

bge rs1, rs2, label
if (rs1 >= rs2) (signed) pc = label

bgeu rs1, rs2, label
if (rs1 >= rs2) (unsigned) pc = label

bgez rs1, label
(bge rs1, zero, label)
if (rs1 >= 0) (signed) pc = label

bgt rs1, rs2, label
(blt rs2, rs1, label)
if (rs1 > rs2) (signed) pc = label

bgtu rs1, rs2, label
(bltu rs2, rs1, label)
if (rs1 > rs2) (unsigned) pc = label

bgtz rs1, label
(blt zero, rs1, label)
if (rs1 > 0) (signed) pc = label

ble rs1, rs2, label
(bge rs2, rs1, label)
if (rs1 <= rs2) (signed) pc = label

bleu rs1, rs2, label
(bgeu rs2, rs1, label)
if (rs1 <= rs2) (unsigned) pc = label

blez rs1, label
(bge zero, rs1, label)
if (rs1 <= 0) (signed) pc = label

blt rs1, rs2, label
if (rs1 < rs2) (signed) pc = label

bltu rs1, rs2, label
if (rs1 < rs2) (unsigned) pc = label

bltz rs1, label
(blt rs1, zero, label)
if (rs1 < 0) (signed) pc = label

bne rs1, rs2, label
if (rs1 != rs2) pc = label

bnez rs1, label
(bne rs1, zero, label)
if (rs1 != 0) pc = label

call label
(jal ra / auipc ra; jalr ra, ra)
ra = pc + 4; pc = label

div rd, rs1, rs2
rd = rs1 / rs2 (signed) (*)

divu rd, rs1, rs2
rd = rs1 / rs2 (unsigned) (*)

ebreak
breakpoint trap

ecall
system call trap

j label
(jal zero, label)
pc = label 

jal rd, label
rd = pc + 4; pc = label

jalr rd, rs1, imm
rd = pc + 4; pc = (rs1 + imm) & ~1 (**)

jr rs1
(jalr zero, rs1, 0)
pc = rs1 & ~1 (**)

la rd, label
(auipc; addi)
rd = label

lb rd, imm(rs1)
rd = *(int8_t*)(rs1 + imm)

lbu rd, imm(rs1)
rd = *(uint8_t*)(rs1 + imm)

lh rd, imm(rs1)
rd = *(int16_t*)(rs1 + imm)

lhu rd, imm(rs1)
rd = *(uint16_t*)(rs1 + imm)

li rd, imm
(lui rd, %hi(imm); addi rd, rd, %lo(imm))
rd = imm

lui rd, imm
rd = imm << 12

lw rd, imm(rs1)
rd = *(int32_t*)(rs1 + imm)

mul rd, rs1, rs2
rd = (rs1 * rs2)[31:0]

mulh rd, rs1, rs2
rd = (rs1 * rs2)[63:32] (signed * signed)

mulhsu rd, rs1, rs2
rd = (rs1 * rs2)[63:32] (signed * unsigned)

mulhu rd, rs1, rs2
rd = (rs1 * rs2)[63:32] (unsigned * unsigned)

mv rd, rs1
(addi rd, rs1, 0)
rd = rs1

neg rd, rs1
(sub rd, zero, rs1)
rd = -rs1

nop
(addi zero, zero, 0)
no operation

not rd, rs1
(xori rd, rs1, -1)
rd = ~rs1

or rd, rs1, rs2
rd = rs1 | rs2 (bitwise OR)

ori rd, rs1, imm
rd = rs1 | imm (bitwise OR)

rem rd, rs1, rs2
rd = rs1 % rs2 (signed) (*)

remu rd, rs1, rs2
rd = rs1 % rs2 (unsigned) (*)

ret
(jr ra)
pc = ra & ~1 (**)

sb rs2, imm(rs1)
*(uint8_t*)(rs1 + imm) = rs2[7:0]

seqz rd, rs1
(sltiu rd, rs1, 1)
rd = (rs1 == 0) ? 1 : 0

sext.b rd, rs1
(slli rd, rs1, 24; srai rd, rd, 24)
rd = (int32_t)((int8_t)rs1[7:0])

sext.h rd, rs1
(slli rd, rs1, 16; srai rd, rd, 16)
rd = (int32_t)((int16_t)rs1[15:0])

sgtz rd, rs1
(slt rd, zero, rs1)
rd = (rs1 > 0) ? 1 : 0

sh rs2, imm(rs1)
*(uint16_t*)(rs1 + imm) = rs2[15:0]

sll rd, rs1, rs2
rd = rs1 << rs2[4:0]

slli rd, rs1, imm
rd = rs1 << imm

slt rd, rs1, rs2
rd = (rs1 < rs2) ? 1 : 0 (signed)

slti rd, rs1, imm
rd = (rs1 < imm) ? 1 : 0 (signed)

sltiu rd, rs1, imm
rd = (rs1 < sext(imm)) ? 1 : 0 (unsigned, ***)

sltu rd, rs1, rs2
rd = (rs1 < rs2) ? 1 : 0 (unsigned)

sltz rd, rs1
(slt rd, rs1, zero)
rd = (rs1 < 0) ? 1 : 0

snez rd, rs1
(sltu rd, zero, rs1)
rd = (rs1 != 0) ? 1 : 0

sra rd, rs1, rs2
rd = rs1 >> rs2[4:0] (sign-extended)

srai rd, rs1, imm
rd = rs1 >> imm (sign-extended)

srl rd, rs1, rs2
rd = rs1 >> rs2[4:0] (zero-extended/unsigned)

srli rd, rs1, imm
rd = rs1 >> imm (zero-extended/unsigned)

sub rd, rs1, rs2
rd = rs1 - rs2

sw rs2, imm(rs1)
*(uint32_t*)(rs1 + imm) = rs2

xor rd, rs1, rs2
rd = rs1 ^ rs2 

xori rd, rs1, imm
rd = rs1 ^ imm

zext.b rd, rs1
(andi rd, rs1, 0xFF)
rd = rs1[7:0]

zext.h rd, rs1
(slli rd, rs1, 16; srli rd, rd, 16)
rd = rs1[15:0]
`;

const asterisks = `
(*)   division and modulo do not generate an exception for edge cases, instead:
      div(a, 0) = -1
      divu(a, 0) = 0xFFFFFFFF
      div(INT_MIN, -1) = INT_MIN
      rem(a, 0) = a
      rem(INT_MIN, -1) = 0
      remu(a, 0) = a
(**)  you cannot jump to an odd address, instead of throwing an exception the least significant bit is just zeroed by & ~1
(***) the immediate is sign extended, but the comparison is unsigned

`;

const parseInstructions = (data: string) => {
    return data.trim().split('\n\n').map(block => {
        const lines = block.split('\n');
        if (lines.length == 3) {
            return {
                mnemonic: lines[0].trim(),
                pseudo: lines[1].trim(),
                description: lines[2].trim()
            };
        } else {
            return {
                mnemonic: lines[0].trim(),
                pseudo: "",
                description: lines[1].trim()
            };
        }
    });
};

export const IntegratedHelp: Component<{ close: () => void }> = (props) => {
    const instructions = parseInstructions(string);
    let modalRef: HTMLDivElement | undefined;

    createEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (e.key === "Escape") props.close();
        };

        window.addEventListener("keydown", handleKeyDown);
        modalRef?.focus();

        onCleanup(() => window.removeEventListener("keydown", handleKeyDown));
    });

    return (
        <div
            class="fixed inset-0 z-50 flex items-center justify-center bg-black/70"
            onClick={props.close}
        >
            <div
                ref={modalRef}
                tabindex="-1"
                class="relative flex flex-col w-full max-w-[90vw] max-h-[90vh] theme-bg border overflow-hidden theme-border outline-none"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
                aria-labelledby="modal-title"
            >
                <header class="sticky top-0 z-10 flex items-center justify-between pl-4 theme-border border-b theme-bg">
                    <h2 id="modal-title" class="text-lg font-bold theme-fg">
                        instruction and pseudoinstruction reference
                    </h2>
                    <button
                        onClick={props.close}
                        aria-label="Close modal"
                        type="button"
                        class="theme-bg p-2 theme-fg cursor-pointer material-symbols-outlined theme-bg-hover theme-bg-active"
                    >
                        {"close"}
                    </button>
                </header>

                <div class="overflow-y-auto px-4 pb-4">
                    <table class="w-full border-collapse">
                        <tbody class="font-mono">
                            <For each={instructions}>
                                {(item) => (
                                    <tr class="group border-b theme-border theme-bg theme-bg-hover">
                                        <td class="whitespace-nowrap align-baseline py-2">
                                            <div class="items-baseline pr-3">
                                                <Show
                                                    when={item.pseudo !== ""}
                                                    fallback={<span class="font-bold theme-style2">{item.mnemonic}</span>}
                                                >
                                                    <div class="font-bold theme-style3">{item.mnemonic}</div>
                                                    <div class="text-xs theme-fg">{item.pseudo}</div>
                                                </Show>
                                            </div>
                                        </td>
                                        <td class="theme-fg italic text-sm align-baseline py-2">
                                            {item.description}
                                        </td>
                                    </tr>
                                )}
                            </For>
                        </tbody>
                    </table>
                    <pre class="pt-3 theme-fg whitespace-pre-wrap">{asterisks}</pre>
                </div>
            </div>
        </div>
    );
};