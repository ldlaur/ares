import { defaultKeymap, indentWithTab } from "@codemirror/commands";
import { LanguageSupport, indentService, indentUnit } from "@codemirror/language";
import { forceLinting } from "@codemirror/lint";
import { Compartment, EditorState } from "@codemirror/state";
import { keymap } from "@codemirror/view";
import { EditorView, basicSetup } from "codemirror";
import { Component, createEffect, createMemo, onCleanup, onMount } from "solid-js";
import { createAsmLinter } from "./AssemblerErrors";
import { breakpointGutter } from "./Breakpoint";
import { Theme } from "./GithubTheme";
import { lineHighlightEffect, lineHighlightState } from "./LineHighlight";
import { riscvLanguage } from "./RiscVLanguage";
import { headerDecoration } from "./TestSuite";
import { ViewPlugin } from "@codemirror/view";

// we cannot use naive setText for every keystroke, that would be too inefficient for larger files
// so we need to return a getText getter from Editor
// but there is a timing problem: components need getText on first render,
// but EditorView only exists after onMount.
// so, create a stable getter that returns "" until CM6 is mounted
export class EditorInterface {
    view?: EditorView;
    getText = (): string => {
        return this.view ? this.view.state.doc.toString() : "";
    }
    setText = (s: string): void => {
        if (!this.view) return;
        this.view.dispatch({
            changes: { from: 0, to: this.view.state.doc.length, insert: s },
        });
    }
}

// NOTE: origText/saveText is not authoritative for all text modifications (or you would negate the benefits of using a text buffer structure)
// origText is just the initial text at the start of the editor
// and storeText is a rate-limited callback that stores the text once in a while 
type EditorProps = {
    origText: string,
    asmLinterOn: boolean,
    editorBlocked: boolean,
    highlightedLine?: number
    diagnostics?: { line: number, message: string },
    theme: Theme,
    readonly editorInterfaceRef: EditorInterface,
    readonly storeText: (text: string) => void,
    readonly setBreakpoints: (lines: number[]) => void,
    readonly doBuild: (text: string) => boolean,
    readonly onHoveredLine: (line: number | null) => void,
};

export function hoveredLinePlugin(onHoveredLine: (line: number | null) => void) {
    return ViewPlugin.define(view => {
        let currentLine: number | null = null;

        const handleMouseMove = (e: MouseEvent) => {
            const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
            if (pos === null) {
                if (currentLine !== null) {
                    currentLine = null;
                    onHoveredLine(null);
                }
                return;
            }
            const line = view.state.doc.lineAt(pos).number;
            if (line !== currentLine) {
                currentLine = line;
                onHoveredLine(line);
            }
        };

        const handleMouseLeave = () => {
            if (currentLine !== null) {
                currentLine = null;
                onHoveredLine(null);
            }
        };

        view.dom.addEventListener("mousemove", handleMouseMove);
        view.dom.addEventListener("mouseleave", handleMouseLeave);

        return {
            destroy() {
                view.dom.removeEventListener("mousemove", handleMouseMove);
                view.dom.removeEventListener("mouseleave", handleMouseLeave);
            },
        };
    });
}

export const Editor: Component<EditorProps> = props => {
    let editor: HTMLDivElement | undefined;
    let view: EditorView;
    let cmTheme: Compartment = new Compartment();
    let readOnlyCompartment: Compartment = new Compartment();
    let lintCompartment = new Compartment();
    // enable and disable linter based on debugMode() and hasError()
    const getDiagnostics = createMemo(() => props.diagnostics);
    const asmLinter = createAsmLinter(props.doBuild, getDiagnostics);
    onMount(() => {
        const theme = EditorView.theme({
            "&.cm-editor": { height: "100%" },
            ".cm-scroller": { overflow: "auto" },
        });
        let saveTimeoutId: number | undefined;
        const orig = props.origText;
        const state = EditorState.create({
            doc: orig,
            extensions: [
                [hoveredLinePlugin(props.onHoveredLine)],
                tabKeymap,
                new LanguageSupport(riscvLanguage, [dummyIndent]),
                lintCompartment.of(asmLinter),
                readOnlyCompartment.of(EditorState.readOnly.of(false)),
                breakpointGutter(props.setBreakpoints), // must be first so it's the first gutter
                basicSetup,
                theme,
                EditorView.editorAttributes.of({ style: "font-size: 1.4em" }),
                cmTheme.of(props.theme.cmTheme), // TODO: if i use constant CSS class names i dont need to let the rest of the code know the theme
                [lineHighlightState],
                indentUnit.of("    "),
                keymap.of([...defaultKeymap, indentWithTab]),
                headerDecoration(),
                EditorView.lineWrapping,
                EditorView.updateListener.of((update) => {
                    if (update.docChanged) {
                        window.clearTimeout(saveTimeoutId);
                        saveTimeoutId = window.setTimeout(() => {
                            props.storeText(view.state.doc.toString());
                        }, 1000);
                    }
                }),
            ],
        });
        view = new EditorView({ state, parent: editor });

        createEffect(() => {
            view.dispatch({
                effects: lintCompartment.reconfigure(
                    props.asmLinterOn ? asmLinter : []
                ),
            });
            // force an immediate relint if the state changes to immediately catch
            // errors that happened while linter was off (ie during debugging)
            if (props.asmLinterOn) forceLinting(view);
        });

        createEffect(() => {
            view.dispatch({ effects: cmTheme.reconfigure(props.theme.cmTheme) });
        });

        createEffect(() => {
            view.dispatch({
                effects: readOnlyCompartment.reconfigure(
                    EditorState.readOnly.of(props.editorBlocked)
                ),
            });
        });

        createEffect(() => {
            const _ = props.diagnostics;
            forceLinting(view);
        });

        createEffect(() => {
            let line = props.highlightedLine ?? 0;
            // note that line numbers start at 1!
            // 0 is an invalid line in CM
            view.dispatch({
                effects: lineHighlightEffect.of(line),
            });
            // scroll the editor to make the highlighted line visible
            if (line > 0 && line <= view.state.doc.lines) {
                const lineInfo = view.state.doc.line(line);
                view.dispatch({
                    effects: EditorView.scrollIntoView(lineInfo.from, { y: "center" }),
                });
            }
        })
        props.editorInterfaceRef.view = view;

        onCleanup(() => {
            window.clearTimeout(saveTimeoutId);
            props.storeText(view.state.doc.toString());
            view.destroy();
        });
    });

    return <main
        class="w-full h-full overflow-hidden theme-scrollbar"
        ref={editor} />

}

const dummyIndent = indentService.of((context, pos) => {
    if (pos < 0 || pos > context.state.doc.length) return null;
    let line = context.lineAt(pos);
    if (line.from === 0) return 0;
    let prevLine = context.lineAt(line.from - 1);
    let match = /^\s*/.exec(prevLine.text);
    if (!match) return 0;
    let cnt = 0;
    for (let i = 0; i < match[0].length; i++) {
        if (match[0][i] == '\t') cnt = cnt + 4 - cnt % 4;
        else cnt += 1;
    }
    return cnt;
});

const tabKeymap = keymap.of([{
    key: "Tab",
    run(view) {
        const { state, dispatch } = view;
        const { from, to } = state.selection.main;
        // insert tab instead of indenting if it's a single line selection
        // messy code for indenting the start of the line with spaces, but keep tabs for the tabulation inside the line
        let lineIsEmpty = true;
        let str = state.doc.toString();
        for (let i = state.doc.lineAt(from).from; i < from; i++) {
            if (str[i] != '\t' && str[i] != ' ' && str[i] != '\n') {
                lineIsEmpty = false;
                break;
            }
        }
        if (!lineIsEmpty && (from == to || state.doc.lineAt(from).number == state.doc.lineAt(to).number)) {
            dispatch(state.update(state.replaceSelection("\t"), {
                scrollIntoView: true,
                userEvent: "input"
            }));
            return true;
        }
        return false;
    }
}]);
