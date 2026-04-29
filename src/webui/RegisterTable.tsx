import { Component, createSignal } from "solid-js";
import { DisplayFormat, formatRegister, UnitSize } from "./DisplayFormat";

export const [displayFormat, setDisplayFormat] = createSignal<DisplayFormat>("hex");
export const [unitSize, setUnitSize] = createSignal<UnitSize>(4);

export const RegisterTable: Component<{ pc: number, regs: number[], regWritten: number }> = (props) => {
  // idx is the hardware register number
  const registersLayout = [
    { name: "ra",  idx: 1,  color: "theme-style1" }, 
    { name: "sp",  idx: 2,  color: "theme-style1" },
    { name: "fp",  idx: 8,  color: "theme-style1" },

    { name: "t0",  idx: 5,  color: "theme-style2" }, 
    { name: "t1",  idx: 6,  color: "theme-style2" }, 
    { name: "t2",  idx: 7,  color: "theme-style2" },
    { name: "t3",  idx: 28, color: "theme-style2" }, 
    { name: "t4",  idx: 29, color: "theme-style2" }, 
    { name: "t5",  idx: 30, color: "theme-style2" }, 
    { name: "t6",  idx: 31, color: "theme-style2" },
    
    { name: "a0",  idx: 10, color: "theme-style5" }, 
    { name: "a1",  idx: 11, color: "theme-style5" }, 
    { name: "a2",  idx: 12, color: "theme-style5" }, 
    { name: "a3",  idx: 13, color: "theme-style5" },
    { name: "a4",  idx: 14, color: "theme-style5" }, 
    { name: "a5",  idx: 15, color: "theme-style5" }, 
    { name: "a6",  idx: 16, color: "theme-style5" }, 
    { name: "a7",  idx: 17, color: "theme-style5" },

    { name: "s1",  idx: 9,  color: "theme-style7" }, 
    { name: "s2",  idx: 18, color: "theme-style7" }, 
    { name: "s3",  idx: 19, color: "theme-style7" }, 
    { name: "s4",  idx: 20, color: "theme-style7" },
    { name: "s5",  idx: 21, color: "theme-style7" }, 
    { name: "s6",  idx: 22, color: "theme-style7" }, 
    { name: "s7",  idx: 23, color: "theme-style7" }, 
    { name: "s8",  idx: 24, color: "theme-style7" },
    { name: "s9",  idx: 25, color: "theme-style7" }, 
    { name: "s10", idx: 26, color: "theme-style7" }, 
    { name: "s11", idx: 27, color: "theme-style7" },
    
    { name: "gp",  idx: 3,  color: "" },
    { name: "tp",  idx: 4,  color: "" },
  ];

  // setting ascii will force unit size to byte
  // the alternative would have been to only show ascii while unit size is byte
  // but that is less discoverable

  const handleFormatChange = (format: DisplayFormat) => {
    if (format === "ascii") {
      setUnitSize(1);
    }
    setDisplayFormat(format);
  };

  const handleUnitChange = (size: UnitSize) => {
    setUnitSize(size);
    if (size !== 1 && displayFormat() === "ascii") {
      setDisplayFormat("hex");
    }
  };
  // all units being ch makes so that the precise sum is 1ch (left pad) + 7ch (x27/a10) + 10ch (0xdeadbeef) + 1ch (right pad)
  // round to 20ch so it has some padding between regname and hex
  // now i have the precise size in a font-independent format, as long as it's monospace
  // webkit workaround for lack of select styling (particularly apparent in dark mode)
  // using google material icons keyboard_arrow_down as it matches Chromium's native
  return (
    <div class="overflow-hidden flex-grow h-full self-start flex-shrink flex flex-col">
      <div class="flex-none flex items-center justify-end theme-gutter border-b theme-border min-h-9">
        <div class="flex flex-wrap items-center gap-1">
          <div class="pb-0.5 relative inline-block">
            <select
              class="appearance-none font-semibold theme-fg theme-gutter px-2 pr-6 theme-border focus:outline-none cursor-pointer"
              title="Memory unit size"
              value={unitSize()}
              onChange={(e) => handleUnitChange(Number(e.currentTarget.value) as UnitSize)}
            >
              <option value={1}>byte</option>
              <option value={2}>half</option>
              <option value={4}>word</option>
            </select>
            <svg class="pointer-events-none absolute right-1 top-1/2 -translate-y-1/2 w-4 h-4 theme-fg"
              xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M480-344 240-584l56-56 184 184 184-184 56 56-240 240Z" /></svg>
          </div>
          <div class="pb-0.5 relative inline-block">
            <select
              class="appearance-none font-semibold theme-fg theme-gutter px-2 pr-6 theme-border focus:outline-none cursor-pointer"
              title="Number format"
              value={displayFormat()}
              onChange={(e) => handleFormatChange(e.currentTarget.value as DisplayFormat)}
            >
              <option value="hex">hex</option>
              <option value="unsigned">unsigned</option>
              <option value="signed">signed</option>
              <option value="ascii">ascii</option>
            </select>
            <svg class="pointer-events-none absolute right-1 top-1/2 -translate-y-1/2 w-4 h-4 theme-fg"
              xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M480-344 240-584l56-56 184 184 184-184 56 56-240 240Z" /></svg>
          </div>
        </div>
      </div>

      {/* Register grid */}
      <div class="overflow-auto flex-grow text-md theme-mono theme-scrollbar-slim theme-border whitespace-nowrap">
        <div class="columns-[20ch] gap-0 min-w-max theme-border-column-rule">
          <div class="justify-between flex flex-row box-content theme-border py-[0.5ch]">
            <div class="self-center pl-[1ch] font-bold">pc</div>
            <div class="self-center pr-[1ch]">{formatRegister(props.pc, "hex")}</div>
          </div>
          {/* using Index here would optimize it, but it gets messy with animations
            naively keeping it as is and making regWritten a signal would still cause everything to be recomputed
          */}
          {registersLayout.map((regDef) => {
            const val = props.regs[regDef.idx];
            const hwName = `x${regDef.idx}`;

            return (
              <div class="justify-between flex flex-row box-content py-[0.5ch]">
                <div class={`self-center pl-[1ch] font-bold ${regDef.color}`}>
                  {regDef.name}/{hwName}
                </div>
                <div class={"self-center mr-[1ch] " + (regDef.idx == props.regWritten ? "animate-fade-highlight" : "")}>
                  {formatRegister(val, displayFormat())}
                </div>
              </div>
            );
          })}
        </div>        
      </div>
    </div>
  );
};
