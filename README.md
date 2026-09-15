# ARES
A minimal assembler, editor, simulator and debugger for RISC-V (RV32IMC), meant to be a useful tool for computer architecture students. This project was inspired by RARS.
You can use it on [ares-sim.github.io](https://ares-sim.github.io)

![Screenshot of the ARES Web UI, debugging a recursive factorial program](images/webui.png)
## Features
This initial release introduces the following core features:
### Web UI version:
- **modern editing experience**:
  - whole-UI light and dark themes
  - [Custom themes](https://github.com/ldlaur/ares/blob/master/docs/theme/CustomTheme.md)
  - CodeMirror 6-based editor with RV32IMC syntax highlighting
  - live assembler error reporting
- **debugging tools**:
  - register and memory visualization with animation to highlight writes
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

# Installation
## Command-line utilities
> [!NOTE]
The test version relies on unity, so make sure you cloned recursively, or alternatively do `git submodule update --init --recursive`

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
