import { defaultKeymap, indentWithTab } from "@codemirror/commands";
import { LanguageSupport, indentService, indentUnit } from "@codemirror/language";
import { forceLinting } from "@codemirror/lint";
import { Compartment, EditorState } from "@codemirror/state";
import { keymap } from "@codemirror/view";
import { EditorView, basicSetup } from "codemirror";
import { Component, createEffect, createMemo, onCleanup, onMount } from "solid-js";
import { createAsmLinter } from "./AssemblerErrors";
import { breakpointGutter } from "./Breakpoint";
import { lineHighlightEffect, lineHighlightState } from "./LineHighlight";
import { riscvLanguage } from "./RiscVLanguage";
import { headerDecoration } from "./TestSuite";
import { ViewPlugin } from "@codemirror/view";
import { tags as t } from "@lezer/highlight"
import { HighlightStyle, syntaxHighlighting } from "@codemirror/language"
import { Extension } from "@codemirror/state";

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
                cmTheme.of([cssTheme, highlightingExtension]),
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

const cssTheme = EditorView.theme({
    "&": {
        color: "var(--color-text)",
        backgroundColor: "var(--color-base0)"
    },

    ".cm-content": {
        caretColor: "var(--color-editor-caret)"
    },
    ".cm-debugging.cm-activeLine": {
        backgroundColor: "var(--color-debugging)"
    },
    ".cm-cursor, .cm-dropCursor": { borderLeftColor: "var(--color-editor-caret)" },
    "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection": { backgroundColor: "var(--color-base2)" },
    ".cm-activeLine": { "background-color": "var(--color-editor-activeline)" },
    ".cm-content ::selection .cm-activeLine": { backgroundColor: "var(--color-base3)" },

    ".cm-panels": { backgroundColor: "var(--color-base1)", color: "var(--color-base4)" },
    ".cm-panels.cm-panels-top": { borderBottom: "1px solid var(--color-border)" },
    ".cm-panels.cm-panels-bottom": { borderTop: "1px solid var(--color-border)" },

    ".cm-searchMatch": {
        backgroundColor: "#72a1ff59",
        outline: "1px solid #457dff"
    },
    ".cm-searchMatch.cm-searchMatch-selected": {
        backgroundColor: "#6199ff2f"
    },

    ".cm-selectionMatch": { backgroundColor: "var(--color-editor-selection-match)" },

    "&.cm-focused .cm-matchingBracket, &.cm-focused .cm-nonmatchingBracket": {
        backgroundColor: "#bad0f847"
    },

    ".cm-gutters": {
        backgroundColor: "var(--color-base0)",
        color: "var(--color-base4)",
        border: "none"
    },

    ".cm-activeLineGutter": {
        backgroundColor: "var(--color-editor-activeline)"
    },

    ".cm-foldPlaceholder": {
        backgroundColor: "transparent",
        border: "none",
        color: "#ddd"
    },
    ".cm-textfield": {
        backgroundColor: "var(--color-base1a)",
        backgroundImage: "none",
        border: "none",
    },
    ".cm-button": {
        backgroundColor: "var(--color-base1a)",
        backgroundImage: "none",
        border: "none",
    },

    ".cm-search > label": {
        "display": "flex",
        "align-items": "center"
    },
    ".cm-search > br": {
        "display": "none",
    },
    ".cm-panel.cm-search input[type=checkbox]": {
        "-webkit-appearance": "none",
        "-moz-appearance": "none",
        "appearance": "none",
        "width": "20px",
        "margin": "5px",
        "height": "20px",
        "border": "none",
        "background-color": "var(--color-base1a)",
        "cursor": "pointer",
    },

    ".cm-panel.cm-search input[type=checkbox]:hover": {
        "background-color": "var(--color-base3)",
    },

    ".cm-panel.cm-search input[type=checkbox]:checked": {
        "background-color": "var(--color-base4)",
    },

    ".cm-panel.cm-search input[type=checkbox]:checked:hover": {
        "background-color": "var(--color-base4)",
    },

    ".cm-search > button:hover": {
        "background-color": "var(--color-base3)",
        "background-image": "none",
    },

    ".cm-search > button:active": {
        "background-color": "var(--color-base4)",
        "color": "var(--color-base0)",
        "background-image": "none",
    },

    ".cm-search > button:active:hover": {
        "background-color": "var(--color-base4)",
        "color": "var(--color-base0)",
        "background-image": "none",
    },

    ".cm-tooltip": {
        border: "none",
        backgroundColor: "var(--color-base3)"
    },
    ".cm-tooltip .cm-tooltip-arrow:before": {
        borderTopColor: "transparent",
        borderBottomColor: "transparent"
    },
    ".cm-tooltip .cm-tooltip-arrow:after": {
        borderTopColor: "var(--color-base3)",
        borderBottomColor: "var(--color-base3)"
    },
});

export const highlightStyle = HighlightStyle.define([
    {
        tag: t.keyword,
        class: "text-editor-insn"
    },
    {
        tag: [t.name, t.deleted, t.character, t.propertyName, t.macroName],
        class: "text-editor-reg"
    },
    {
        tag: [t.color, t.constant(t.name), t.standard(t.name), t.typeName, t.className, t.number, t.changed, t.annotation, t.modifier, t.self, t.namespace, t.atom, t.bool, t.special(t.variableName)],
        class: "text-editor-const"
    },
    {
        tag: [t.operator, t.operatorKeyword, t.url, t.escape, t.regexp, t.link, t.special(t.string)],
        class: "text-editor-directive"
    },
    {
        tag: [t.meta, t.comment],
        class: "text-editor-comment"
    },
    {
        tag: [t.processingInstruction, t.string, t.inserted],
        class: "text-editor-string"
    },
    {
        tag: t.strong,
        class: "font-bold"
    },
    {
        tag: t.emphasis,
        class: "italic"
    },
    {
        tag: t.strikethrough,
        class: "line-through"
    },
    {
        tag: t.link,
        class: "underline"
    },
    {
        tag: t.invalid,
        class: "text-base4"
    },
]);

const highlightingExtension: Extension = syntaxHighlighting(highlightStyle);
