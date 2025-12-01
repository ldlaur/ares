# ARES
A minimal assembler, editor, simulator and debugger for RISC-V (RV32IM), meant to be a useful tool for computer architecture students.
This project was inspired by [RARS](https://github.com/TheThirdOne/rars), but it is not affiliated with it in any way.

You can try it now online on [ares-sim.github.io](https://ares-sim.github.io).

![Screenshot of the ARES Web UI, debugging a recursive factorial program](images/webui.png)
## Features
This initial release introduces the following core features:
### Web UI version:
- **modern editing experience**:
  - whole-UI light and dark themes
  - CodeMirror 6-based editor with RV32IM syntax highlighting
  - live error reporting
- **debugging tools**:
  - register and memory visualization with live updates
  - call stack inspector
  - breakpoint management
  - step/next/continue debugging

### Command-line utilities
- minimal, cross-platform C
- ELF binary and object file generation
- headless execution of the emulator

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
npm run dev # for a developement live reload server
npm run build # to build in dist/
```
