# ARES
A minimal assembler, editor, simulator and debugger for RISC-V (RV32IM and RV32C support), meant to be a useful tool for computer architecture students.
This project was inspired by [RARS](https://github.com/TheThirdOne/rars).

You can try the RV32IM version online on [ares-sim.github.io](https://ares-sim.github.io), or the RV32C version online on [https://ares-sim.github.io/rvc/](https://ares-sim.github.io/rvc/)

![Screenshot of the ARES Web UI, debugging a recursive factorial program](images/webui.png)
## Features
This initial release introduces the following core features:
### Web UI version:
- **modern editing experience**:
  - whole-UI light and dark themes
  - CodeMirror 6-based editor with RV32IM and RV32C syntax highlighting
  - live assembler error reporting
- **debugging tools**:
  - register and memory visualization with animation to highlight writes
  - disassembly view for regular and supported compressed instructions
  - breakpoint management
  - step/next/continue debugging
  - reverse debugging (step back)
  - call stack inspector with parameters
- **calling convention checker** (CallSan):
  - detect uninitialized reads at program start, at function entry, and after function return
  - detect invalid stack movement and access
  - validate callee-saved register save and restore
  - detect mismatched call stack
- **assignment mode**:
  - embedded instructions
  - CodeRunner-style test suite with expected/actual

### Command-line utilities
- minimal, cross-platform C
- ELF binary and object file generation
- headless execution of the emulator

### Supported compressed instructions
ARES currently implements a subset of RV32C in the assembler, emulator, disassembler, and Web UI grammar:

```
c.lwsp c.swsp
c.lw c.sw
c.j c.jal
c.jr c.jalr
c.beqz c.bnez
c.li c.lui
c.addi c.addi16sp c.addi4spn
c.slli
c.srli c.srai
c.andi
c.mv c.add
c.and c.or c.xor c.sub
c.nop
c.ebreak
```

# Installation
## Command-line utilities
> [!NOTE]
The CLI version relies on ezld, so make sure you cloned recursively, or alternatively do `git submodule update --init --recursive`

A working C compiler and 
```
make
```

## Web UI
Clang with support for WASM (if you're using macOS, ensure you're not using AppleClang), node and npm
```
npm install
npm run dev # for a development live reload server
npm run build # to build in dist/
```
