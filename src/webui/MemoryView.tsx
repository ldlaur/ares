import { createVirtualizer } from "@tanstack/solid-virtual";
import { Component, createSignal, onMount, createEffect, For, Show, on, Index, createMemo, onCleanup } from "solid-js";
import { TabSelector } from "./TabSelector";
import { DATA_BASE, STACK_TOP, TEXT_BASE } from "./core/RiscV";
import { formatMemoryValue, getCellWidthChars } from "./DisplayFormat";
import { unitSize, displayFormat } from "./RegisterTable";
import { ShadowStack } from "./ShadowStack";

const MEMORY_WINDOW_SIZE = 65536;

function loadWrapper(load: (addr: number, size: number) => number, ptr: number, size: number) {
    let val = 0;
    for (let i = 0; i < size; i++) {
        val |= load(ptr + i, 1) << (i * 8);
    }
    return val >>> 0;
}

const AddressGutter: Component<{
    index: number,
    addr: number,
    charWidth: number,
    addrSelect: number,
    setAddrSelect: (s: number) => void,
}> = (props) => (
    <div
        class={"text-addrcolumn shrink-0 w-[10ch] tabular-nums " +
            (props.addrSelect === props.index ? "select-text " : "select-none ")}
        onMouseDown={(e) => { props.setAddrSelect(props.index); e.stopPropagation(); }}
    >
        {props.addr.toString(16).padStart(8, "0")}
    </div>
);

const DisasmView: Component<{
    version: () => any,
    pc: number,
    highlightAddr: number,
    highlightLen: number,
    charWidth: number,
    charHeight: number,
    addrSelect: () => number,
    setAddrSelect: (i: number) => void,
    disassemble: (pc: number) => string | null,
    load: (addr: number, size: number) => number,
    parentRef: HTMLDivElement | undefined,
}> = (props) => {
    // TODO: compute this C-side instead?
    const addresses = createMemo(() => {
        props.version(); // reactive hook for code change
        const addrs: number[] = [];
        let addr = TEXT_BASE;
        const end = TEXT_BASE + MEMORY_WINDOW_SIZE;
        while (addr < end) {
            addrs.push(addr);
            addr += (props.load(addr, 1) & 0x3) === 0x3 ? 4 : 2;
        }
        return addrs;
    });

    const virtualizer = createVirtualizer({
        get count() { return addresses().length; },
        getScrollElement: () => props.parentRef ?? null,
        estimateSize: () => props.charHeight,
        getItemKey: (index) => addresses()[index],
        overscan: 5,
    });

    const virtualRows = createMemo(() =>
        virtualizer.getVirtualIndexes().map(index => ({
            index,
            addr: addresses()[index],
            start: index * props.charHeight,
            size: props.charHeight,
        }))
    );

    createEffect(() => {
        if (props.pc > 0) {
            // binary search to get the line from PC
            const addrs = addresses();
            let lo = 0, hi = addrs.length - 1, idx = -1;
            while (lo <= hi) {
                const mid = (lo + hi) >> 1;
                if (addrs[mid] === props.pc) { idx = mid; break; }
                else if (addrs[mid] < props.pc) lo = mid + 1;
                else hi = mid - 1;
            }
            if (idx >= 0) {
                virtualizer.scrollToIndex(idx, { align: "center" });
            }
        }
    });

    return (
        <div style={{ height: `${virtualizer.getTotalSize()}px`, width: "100%", position: "relative" }}>
            <For each={virtualRows()}>
                {(virtRow) => {
                    const addr = addresses()[virtRow.index];
                    return (
                        <div
                            style={{ "white-space": "nowrap", position: "absolute", top: `${virtRow.start}px`, height: `${virtRow.size}px` }}
                            class={"flex flex-row items-center w-full " + (addr === props.pc ? "bg-debugging" : "")}
                        >
                            <AddressGutter
                                index={virtRow.index}
                                addr={addr}
                                charWidth={props.charWidth}
                                addrSelect={props.addrSelect()}
                                setAddrSelect={props.setAddrSelect}
                            />
                            {(() => {
                                // trigger reactivity when the code changes
                                props.version();
                                const inst = props.disassemble(addr);
                                const isBold = addr >= props.highlightAddr && addr < (props.highlightAddr + props.highlightLen);
                                return <div class={isBold ? "font-bold" : ""}>{inst}</div>;
                            })()}
                        </div>
                    );
                }}
            </For>
        </div>
    );
};

const HexView: Component<{
    version: () => any,
    activeTab: () => ".text" | ".data" | "stack" | "frames" | "disasm",
    writeAddr: number,
    writeLen: number,
    highlightAddr: number,
    highlightLen: number,
    sp: number,
    fp: number,
    load: (addr: number, size: number) => number,
    charWidth: number,
    charHeight: number,
    addrSelect: () => number,
    setAddrSelect: (i: number) => void,
    chunksPerLine: () => number,
    lineCount: () => number,
    parentRef: HTMLDivElement | undefined,
    // not reactive, used to store data across mounts
    scrollPositions: Record<string, number>
}> = (props) => {
    const virtualizer = createVirtualizer({
        get count() { return props.lineCount(); },
        getScrollElement: () => props.parentRef ?? null,
        estimateSize: () => props.charHeight,
        overscan: 5,
    });

    const bytesPerLine = createMemo(() => unitSize() * props.chunksPerLine());
    const getStartAddr = () => {
        const tab = props.activeTab();
        if (tab === ".text") return TEXT_BASE;
        if (tab === ".data") return DATA_BASE;
        if (tab === "stack") return STACK_TOP - MEMORY_WINDOW_SIZE;
        return 0;
    };

    createEffect(on(props.activeTab, (next, prev) => {
        if (!props.parentRef) return;
        const bpl = bytesPerLine();
        if (prev) {
            props.scrollPositions[prev] = Math.round(props.parentRef.scrollTop / props.charHeight) * bpl;
        }
        const saved = props.scrollPositions[next];
        props.parentRef.scrollTop = Math.round(saved / bpl) * props.charHeight;
    }));

    createEffect(on(bytesPerLine, (next, prev) => {
        if (!props.parentRef) return;
        if (!prev) return;
        if (props.activeTab() === "stack") {
            // TODO: figure out the math to make this keep working while pinning the end of the stack
            // for now, resetting to the end is good enough
            props.parentRef.scrollTop = props.parentRef.scrollHeight - props.parentRef.clientHeight;
        } else {
            let addr = Math.round(props.parentRef.scrollTop / props.charHeight) * prev;
            props.parentRef.scrollTop = Math.round(addr / next) * props.charHeight;
        }
    }));

    const getStyle = (ptr: number) => {
        const isStack = props.activeTab() === "stack";
        const bytesPerUnit = unitSize();
        const selectMode = props.addrSelect() === -1 ? "select-text" : "select-none";
        const writeStartAligned = props.writeAddr & ~(bytesPerUnit - 1);
        const writeEndAligned = (props.writeAddr + props.writeLen + bytesPerUnit - 1) & ~(bytesPerUnit - 1);
        const highlightStartAligned = props.highlightAddr & ~(bytesPerUnit - 1);
        const highlightEndAligned = (props.highlightAddr + props.highlightLen + bytesPerUnit - 1) & ~(bytesPerUnit - 1);
        const isAnimated = ptr >= writeStartAligned && ptr < writeEndAligned;
        const isGray = isStack && (ptr < props.sp || (props.fp > props.sp && ptr > props.fp));
        const isSp = isStack && ptr >= props.sp && ptr < props.sp + 4;
        // need this specific check for isStack since fp is s0, and it can be used as a regular register
        const isFp = isStack && ptr >= props.fp && ptr < props.fp + 4;
        let style = selectMode;
        if (isGray) style = "theme-fg2";
        else if (isSp) style = "bg-sp-highlight";
        else if (isFp) style = "bg-fp-highlight";
        if (ptr >= highlightStartAligned && ptr < highlightEndAligned)
            style += " font-bold";
        if (isAnimated) style += " animate-fade-highlight";

        return style;
    }

    return (
        <div style={{ height: `${virtualizer.getTotalSize()}px`, width: "100%", position: "relative" }}>
            <For each={virtualizer.getVirtualItems()}>
                {(virtRow) => (
                    <div
                        style={{ position: "absolute", top: `${virtRow.start}px` }}
                        class="flex flex-row items-center w-full"
                        data-index={virtRow.index}
                    >
                        <AddressGutter
                            index={virtRow.index}
                            addr={getStartAddr() + virtRow.index * props.chunksPerLine() * 4}
                            charWidth={props.charWidth}
                            addrSelect={props.addrSelect()}
                            setAddrSelect={props.setAddrSelect}
                        />
                        <Index each={Array.from({ length: props.chunksPerLine() }, (_, i) => {
                            const bytesPerUnit = unitSize();
                            const startAddr = getStartAddr();
                            return Array.from({ length: 4 / bytesPerUnit }, (_, j) => {
                                const ptr = startAddr + (virtRow.index * props.chunksPerLine() + i) * 4 + j * bytesPerUnit;
                                return { ptr, bytesPerUnit, isLast: i === props.chunksPerLine() - 1 && j === 4 / bytesPerUnit - 1 };
                            });
                        }).flat()}>
                            {(cell) => {
                                const cellWidth = () => getCellWidthChars(cell().bytesPerUnit);
                                const str = () => {
                                    // trigger reactivity when the data changes
                                    props.version();
                                    let val = loadWrapper(props.load, cell().ptr, cell().bytesPerUnit);
                                    let style = getStyle(cell().ptr);
                                    // i need to add instead of override since getStyle also does bold highlighting
                                    if (unitSize() == 1 && displayFormat() == "ascii" && (val < 26 || val > 126)) style += " theme-fg2";
                                    return {
                                        "style": style,
                                        "val": cell().ptr >= getStartAddr() + MEMORY_WINDOW_SIZE ? "" : formatMemoryValue(val, cell().bytesPerUnit, displayFormat())
                                    };
                                };
                                return (
                                    <span
                                        class={str().style + " cursor-default tabular-nums whitespace-pre"}
                                        style={{
                                            "margin-right": `${cellWidth() + 1 - str().val.length}ch`,
                                            "display": "inline-block"
                                        }}
                                    >
                                        {str().val}
                                    </span>
                                );
                            }}
                        </Index>
                    </div>
                )}
            </For>
        </div>
    );
};

export const MemoryView: Component<{
    version: () => any,
    writeAddr: number,
    writeLen: number,
    highlightAddr: number,
    highlightLen: number,
    pc: number,
    sp: number,
    fp: number,
    load: (addr: number, size: number) => number,
    shadowStack: any,
    disassemble: (pc: number) => string | null
}> = (props) => {
    let pr: HTMLDivElement | undefined = undefined;
    const [parentRef, setParentRef] = createSignal<HTMLDivElement | undefined>(undefined);
    let scrollPositions: Record<string, number> = { ".text": 0, ".data": 0, "stack": MEMORY_WINDOW_SIZE };
    let dummyChar: HTMLDivElement | undefined;

    const [containerWidth, setContainerWidth] = createSignal<number>(0);
    const [charWidth, setCharWidth] = createSignal<number>(0);
    const [charHeight, setCharHeight] = createSignal<number>(0);
    const [chunksPerLine, setChunksPerLine] = createSignal<number>(1);
    const [lineCount, setLineCount] = createSignal<number>(0);
    const [addrSelect, setAddrSelect] = createSignal<number>(-1);
    const [activeTab, setActiveTab] = createSignal<".text" | ".data" | "frames" | "stack" | "disasm">(".text");

    onMount(() => {
        if (dummyChar) setCharWidth(dummyChar.getBoundingClientRect().width);
        if (dummyChar) setCharHeight(dummyChar.getBoundingClientRect().height);
        const ro = new ResizeObserver((entries) => {
            for (const entry of entries) setContainerWidth(entry.contentRect.width);
        });
        if (pr) ro.observe(pr);
        setParentRef(pr)
        onCleanup(() => ro.disconnect());
    });

    createEffect(() => {
        const cw = charWidth();
        const containerW = containerWidth();
        const unit = unitSize();

        if (cw > 0 && containerW > 0 && activeTab() !== "disasm") {
            const addressGutterChars = 12; // strlen("00400000  ")
            const availablePx = containerW - addressGutterChars * cw;
            const unitWidthChars = getCellWidthChars(unit);
            const valuesPerChunk = 4 / unit;
            const chunkWidthChars = valuesPerChunk * unitWidthChars + valuesPerChunk;
            const chunkWidthPx = chunkWidthChars * cw;
            const count = Math.max(1, Math.floor(availablePx / chunkWidthPx));
            setChunksPerLine(count);
            setLineCount(Math.ceil(MEMORY_WINDOW_SIZE / (count * 4)));
        }
    });

    const showHex = () => activeTab() !== "frames" && activeTab() !== "disasm";

    return (
        <div class="h-full flex flex-col overflow-hidden" onMouseDown={() => setAddrSelect(-1)}>
            <TabSelector tab={activeTab()} setTab={setActiveTab} tabs={[".text", "disasm", ".data", "stack", "frames"]} />

            <div class="font-semibold theme-mono ml-2 theme-fg">
                <span class="text-addrcolumn inline-block" style={{ width: charWidth() * 10 + "px" }}>address</span>
                <span>{activeTab() === "disasm" ? "instructions" : "contents"}</span>
            </div>

            <div ref={pr} class="theme-mono text-lg overflow-y-auto overflow-x-auto theme-scrollbar ml-2">
                <div ref={dummyChar} class="invisible absolute">0</div>

                <Show when={activeTab() === "frames"}>
                    <ShadowStack
                        shadowStack={props.shadowStack}
                        memWrittenAddr={props.writeAddr}
                        memWrittenLen={props.writeLen}
                    />
                </Show>

                <Show when={activeTab() === "disasm"}>
                    <DisasmView
                        version={props.version}
                        pc={props.pc}
                        highlightAddr={props.highlightAddr}
                        highlightLen={props.highlightLen}
                        charWidth={charWidth()}
                        charHeight={charHeight()}
                        addrSelect={addrSelect}
                        setAddrSelect={setAddrSelect}
                        disassemble={props.disassemble}
                        parentRef={parentRef()}
                        load={props.load}
                    />
                </Show>

                <Show when={showHex()}>
                    <HexView
                        scrollPositions={scrollPositions}
                        version={props.version}
                        activeTab={activeTab}
                        writeAddr={props.writeAddr}
                        writeLen={props.writeLen}
                        highlightAddr={props.highlightAddr}
                        highlightLen={props.highlightLen}
                        sp={props.sp}
                        fp={props.fp}
                        load={props.load}
                        charHeight={charHeight()}
                        charWidth={charWidth()}
                        addrSelect={addrSelect}
                        setAddrSelect={setAddrSelect}
                        chunksPerLine={chunksPerLine}
                        lineCount={lineCount}
                        parentRef={parentRef()}
                    />
                </Show>
            </div>
        </div>
    );
};