export const TEXT_BASE = 0x00400000;
export const TEXT_END = 0x10000000;
export const DATA_BASE = 0x10000000;
export const DATA_END = 0x70000000;
export const STACK_TOP = 0x7ffff000;
export const STACK_LEN = 4096;

export const REG_RA = 1;
export const REG_SP = 2;
export const REG_FP = 8;

export function toUnsigned(x: number): number {
  return x >>> 0;
}

export function convertNumber(x: number, decimal: boolean): string {
  if (!decimal) {
    return toUnsigned(x).toString(16).padStart(8, "0");
  }
  const u = toUnsigned(x);
  const isPointer =
    (u >= TEXT_BASE && u <= TEXT_END) ||
    (u >= STACK_TOP - STACK_LEN && u <= STACK_TOP) ||
    (u >= DATA_BASE && u <= DATA_END);
  return isPointer ? "0x" + u.toString(16).padStart(8, "0") : u.toString();
}

export function instructionSizeFromHalfword(halfword: number): number {
  return (halfword & 0b11) === 0b11 ? 4 : 2;
}

interface WasmExports {
  get_pc: (g: number) => number;
  get_reg: (g: number, idx: number) => number;
  set_reg: (g: number, idx: number, value: number) => void;
  get_csr: (g: number, idx: number) => number;
  set_csr: (g: number, idx: number, value: number) => void;
  get_privilege_level: (g: number,) => number;
  get_exited: (g: number) => number;
  get_exit_code: (g: number) => number;
  get_error_line: (g: number) => number;
  get_error: (g: number) => number;
  get_runtime_error_type: (g: number) => number;
  get_runtime_error_params: (g: number, idx: number) => number;
  get_mem_written_addr: (g: number) => number;
  get_mem_written_len: (g: number) => number;
  get_reg_written: (g: number) => number;
  get_got_breakpoint: (g: number) => number;
  get_callsan_written_by: (g: number, offset: number) => number;
  get_shadow_stack_len: (g: number) => number;
  get_shadow_stack_pc: (g: number, ent: number) => number;
  get_shadow_stack_sp: (g: number, ent: number) => number;
  get_shadow_stack_args: (g: number, ent: number, i: number) => number;

  // Core functions
  emulate: (g: number) => void;
  assemble: (g: number, offset: number, len: number, allow_externs: boolean) => void;
  pc_to_label: (g: number, pc: number) => void;
  get_addr_from_line: (g: number, address: number) => void;
  get_line_from_pc: (g: number) => number;
  emu_load: (g: number, addr: number, size: number) => number;
  emu_disassemble_addr: (g: number, addr: number) => number;

  g_get_addr_from_line_start: number;
  g_get_addr_from_line_end: number;
  g_emu_disassemble_buf: number;
  g_pc_to_label_txt: number;
  g_pc_to_label_len: number;
  g_state: number;

  __heap_base: number;
  g_heap_size: number;
}

const INSTRUCTION_LIMIT: number = 100 * 1000;


export class WasmInterface {
  private readonly memory: WebAssembly.Memory;
  private readonly wasmInstance: WebAssembly.Instance;
  private readonly exports: WasmExports;
  private readonly originalMemory: Uint8Array;
  public readonly getAddrFromLineStart: Uint32Array;
  public readonly getAddrFromLineEnd: Uint32Array;

  public successfulExecution: boolean = false;
  private currRunMemory: Uint8Array;

  public textBuffer: string = "";
  public hasError: boolean = false;
  public numOfExecutedInstructions: number = 0;

  public emu_load: (addr: number, size: number) => number;

  constructor(memory: WebAssembly.Memory, instance: WebAssembly.Instance) {
    this.memory = memory;
    this.wasmInstance = instance;
    this.exports = this.wasmInstance.exports as unknown as WasmExports;
    this.emu_load = (a, s) => this.exports.emu_load(this.exports.g_state, a, s);
    this.originalMemory = new Uint8Array(this.memory.buffer.slice(0));
    this.currRunMemory = new Uint8Array(this.memory.buffer.slice(0));
    this.getAddrFromLineStart = this.createU32(this.exports.g_get_addr_from_line_start);
    this.getAddrFromLineEnd = this.createU32(this.exports.g_get_addr_from_line_end);
  }

  createU8(off: number) {
    return new Uint8Array(this.memory.buffer, off);
  }
  createU32(off: number) {
    return new Uint32Array(this.memory.buffer, off);
  }


  public static async loadModule(buffer: any): Promise<WasmInterface> {
    const memory = new WebAssembly.Memory({ initial: 7 });
    let iface: WasmInterface | null;
    const { instance } = await WebAssembly.instantiate(buffer, {
      env: {
        memory: memory,
        putchar: (n: number) => {
          if (iface) iface.textBuffer += String.fromCharCode(n);
        },
        emu_exit: () => {
          if (iface) iface.successfulExecution = true;
        },
        panic: () => {
          alert("wasm panic");
        },
        gettime64: () => BigInt(new Date().getTime() * 10 * 1000),
      },
    });
    iface = new WasmInterface(memory, instance);
    return iface;
  }

  getPc(): number {
    return this.exports.get_pc(this.exports.g_state);
  }

  getReg(idx: number): number {
    return this.exports.get_reg(this.exports.g_state, idx);
  }

  setReg(idx: number, value: number): void {
    this.exports.set_reg(this.exports.g_state, idx, value);
  }

  getCsr(idx: number): number {
    return this.exports.get_csr(this.exports.g_state, idx);
  }

  setCsr(idx: number, value: number): void {
    this.exports.set_csr(this.exports.g_state, idx, value);
  }

  getPrivilegeLevel(): number {
    return this.exports.get_privilege_level(this.exports.g_state);
  }

  getExited(): boolean {
    return this.exports.get_exited(this.exports.g_state) !== 0;
  }

  getExitCode(): number {
    return this.exports.get_exit_code(this.exports.g_state);
  }

  getErrorLine(): number {
    return this.exports.get_error_line(this.exports.g_state);
  }

  getError(): number {
    return this.exports.get_error(this.exports.g_state);
  }

  getRuntimeErrorType(): number {
    return this.exports.get_runtime_error_type(this.exports.g_state);
  }

  getRuntimeErrorParam(idx: number): number {
    return this.exports.get_runtime_error_params(this.exports.g_state, idx);
  }

  getMemWrittenAddr(): number {
    return this.exports.get_mem_written_addr(this.exports.g_state);
  }

  getMemWrittenLen(): number {
    return this.exports.get_mem_written_len(this.exports.g_state);
  }

  getRegWritten(): number {
    return this.exports.get_reg_written(this.exports.g_state);
  }

  getGotBreakpoint(): boolean {
    return this.exports.get_got_breakpoint(this.exports.g_state) !== 0;
  }

  getShadowStackLen(): number {
    return this.exports.get_shadow_stack_len(this.exports.g_state);
  }

  getShadowStackPc(ent: number): number {
    return this.exports.get_shadow_stack_pc(this.exports.g_state, ent);
  }

  getShadowStackSp(ent: number): number {
    return this.exports.get_shadow_stack_sp(this.exports.g_state, ent);
  }

  getShadowStackArgs(ent: number, i: number): number {
    return this.exports.get_shadow_stack_args(this.exports.g_state, ent, i);
  }


  getCallsanWrittenBy(offset: number): number {
    return this.exports.get_callsan_written_by(this.exports.g_state, offset);
  }

  build(source: string): { line: number; message: string } | null {
    this.successfulExecution = false;
    this.textBuffer = "";
    this.hasError = false;
    this.numOfExecutedInstructions = 0;
    this.createU8(0).set(this.originalMemory);

    const encoder = new TextEncoder();
    const strBytes = encoder.encode(source);
    const strLen = strBytes.length;
    const offset = this.exports.__heap_base;

    if (offset + strLen > this.memory.buffer.byteLength) {
      const pages = Math.ceil(
        (offset + strLen - this.memory.buffer.byteLength) / 65536,
      );
      this.memory.grow(pages);
    }

    this.createU8(offset).set(strBytes);
    this.createU32(this.exports.g_heap_size)[0] = (strLen + 7) & ~7;
    this.exports.assemble(this.exports.g_state, offset, strLen, false);

    const errorLine = this.getErrorLine();
    const errorPtr = this.getError();
    if (errorPtr) {
      const error = this.createU8(errorPtr);
      const errorLen = error.indexOf(0);
      const errorStr = new TextDecoder("utf8").decode(error.slice(0, errorLen));
      return { line: errorLine, message: errorStr };
    }
    this.currRunMemory = new Uint8Array(this.memory.buffer.slice(0));
    return null;
  }

  getStringFromPc(pc: number): string {
    this.exports.pc_to_label(this.exports.g_state, pc);
    const labelPtr = this.createU32(this.exports.g_pc_to_label_txt)[0];
    if (labelPtr) {
      const labelLen = this.createU32(this.exports.g_pc_to_label_len)[0];
      const label = this.createU8(labelPtr);
      const labelStr = new TextDecoder("utf8").decode(label.slice(0, labelLen));
      return labelStr;
    }
    return "0x" + pc.toString(16);
  }

  getAddrFromLine(line: number): { start: number, len: number } {
    this.exports.get_addr_from_line(this.exports.g_state, line);
    return {
      start: this.getAddrFromLineStart[0],
      len: this.getAddrFromLineEnd[0] - this.getAddrFromLineStart[0]
    };
  }

  getLineFromPc(): number {
    return this.exports.get_line_from_pc(this.exports.g_state);
  }

  disassemble(pc: number): string {
    const len = this.exports.emu_disassemble_addr(this.exports.g_state, pc);
    const arr = this.createU8(this.exports.g_emu_disassemble_buf);
    const str = new TextDecoder("utf8").decode(arr.slice(0, len));
    return str;
  }

  getRegisterName(idx: number): string {
    const regnames = [
      "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
      "fp/s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
      "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
      "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
    ];
    return regnames[idx];
  }

  run(): void {
    this.exports.emulate(this.exports.g_state);
    this.numOfExecutedInstructions++;
    if (this.numOfExecutedInstructions > INSTRUCTION_LIMIT) {
      this.textBuffer += `ERROR: instruction limit ${INSTRUCTION_LIMIT} reached\n`;
      this.hasError = true;
    } else if (this.getRuntimeErrorType() != 0) {
      const errorType = this.getRuntimeErrorType();
      const pcString = `PC=0x${this.getPc().toString(16)}`;
      const runtimeParam1 = this.getRuntimeErrorParam(0);
      const runtimeParam2 = this.getRuntimeErrorParam(1);
      let regname = "";
      let oldVal = "";
      let newVal = "";
      let str = "";
      switch (errorType) {
        case 1:
          this.textBuffer += `ERROR: Program counter moved outside valid code (${pcString})\n`;
          if (this.getShadowStackLen() == 0)
            this.textBuffer += "Hint: The program may be missing an exit syscall\n";
          else
            this.textBuffer += "Hint: This may be caused by a bad jump address or a missing return instruction\n";
          break;
        case 2:
          str = convertNumber(runtimeParam1, false);
          this.textBuffer += `ERROR: cannot load from address 0x${str} at ${pcString}\n`;
          break;
        case 3:
          str = convertNumber(runtimeParam1, false);
          this.textBuffer += `ERROR: cannot store to address 0x${str} at ${pcString}\n`;
          break;
        case 4:
          this.textBuffer += `ERROR: unhandled instruction at ${pcString}\n`;
          break;
        case 5:
          regname = this.getRegisterName(runtimeParam1);
          this.textBuffer += `CallSan: ${pcString}\nAttempted to read from uninitialized register ${regname}. Check the calling convention!\n`;
          break;
        case 6:
          regname = this.getRegisterName(runtimeParam1);
          oldVal = convertNumber(runtimeParam2, false);
          newVal = convertNumber(this.getReg(runtimeParam1), false);
          this.textBuffer += `CallSan: callee-saved register ${regname} was modified but not restored.\n`;
          this.textBuffer += `Value at entry: ${oldVal}, at exit: ${newVal}\n`;
          this.textBuffer += "In the RISC-V ABI, s0-s11 must be preserved by the callee.\n";
          this.textBuffer += "The caller expects them to have the same value before and after the function call.\n";
          this.textBuffer += "Hint: you can use the stack to save and restore them.\n";
          break;
        case 7:
          oldVal = convertNumber(runtimeParam2, false);
          newVal = convertNumber(this.getReg(REG_SP), false);
          this.textBuffer += `CallSan: ${pcString}\nRegister sp has different value at the beginning and end of the function.\nPrev: ${oldVal}\nCurr: ${newVal}\nCheck the calling convention!\n`;
          break;
        case 8:
          oldVal = convertNumber(runtimeParam2, false);
          newVal = convertNumber(this.getReg(REG_RA), false);
          this.textBuffer += `CallSan: return address register was modified but not restored.\n`;
          this.textBuffer += `Value at entry: ${oldVal}, at exit: ${newVal}\n`;
          this.textBuffer += `Function calls overwrite ra, so in a nested function call, the inner call overwrites the return address of the outer function, preventing return to its caller.\n`;
          this.textBuffer += `Hint: in non-leaf functions, ra must be saved in the prologue and restored in the epilogue.\n`;
          break;
        case 9:
          this.textBuffer += `CallSan: ${pcString}\nReturn without matching call!\n`;
          break;
        case 10:
          str = convertNumber(runtimeParam1, false);
          this.textBuffer += `CallSan: read from uninitialized stack slot.\n`;
          this.textBuffer += `Attempted to read from stack address 0x${str}, which has not been written to since the stack pointer was moved.\n`;
          this.textBuffer += `This results in loading garbage data into the register.\n`;
          this.textBuffer += `Hint: the prologue should save registers to the stack and the epilogue should restore them, using the same offset.\n`;
          break;
        case 11:
          str = convertNumber(runtimeParam1, false);
          this.textBuffer += `ERROR: protection error\n`;
          break;
        case 12:
          str = convertNumber(runtimeParam1, false);
          this.textBuffer += `ERROR: Environment call ${runtimeParam1.toString()} is not supported\n`;
          break;
        case 13:
          regname = this.getRegisterName(runtimeParam1);
          this.textBuffer += `CallSan: ${regname} is caller-saved: value is not preserved across a call.\n`;
          this.textBuffer += "In the RISC-V ABI, a0-a7 and t0-t6 may be overwritten by the called function.\n";
          this.textBuffer += "Hint: S registers are preserved across a call\n";
          break;
        case 14:
          this.textBuffer += `CallSan: Stack pointer resides outside the stack region bounds.\n`;
          break;
        default:
          this.textBuffer += `ERROR${errorType}: ${pcString} ${runtimeParam1.toString(16)}\n`;
          break;
      }
      this.hasError = true;
    }
  }

  executeNInstructions(n: number): void {
    this.resetToInitialState();
    for (let i = 0; i < n; i++) {
      this.exports.emulate(this.exports.g_state);
      this.numOfExecutedInstructions++;
      if (this.getRuntimeErrorType() != 0 || this.hasError || this.successfulExecution) {
        break;
      }
    }
  }

  reverseStep(): void {
    if (this.numOfExecutedInstructions <= 0) return;
    this.textBuffer = "";
    const targetInstructions = this.numOfExecutedInstructions - 1;
    this.executeNInstructions(targetInstructions);
  }

  resetToInitialState(): void {
    this.successfulExecution = false;
    this.textBuffer = "";
    this.hasError = false;
    this.numOfExecutedInstructions = 0;
    this.createU8(0).set(this.currRunMemory);
  }
}
