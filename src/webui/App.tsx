import {
	createComputed,
	createEffect,
	createRoot,
	onMount,
	Show,
	Signal,
	type Component,
} from "solid-js";
import { basicSetup, EditorView } from "codemirror";
import { Decoration, keymap, WidgetType } from "@codemirror/view";
import { Compartment, EditorState, Transaction } from "@codemirror/state";

import { lineHighlightEffect, lineHighlightState } from "./LineHighlight";
import { breakpointGutter } from "./Breakpoint";
import { createAsmLinter } from "./AssemblerErrors";
import { defaultKeymap, indentWithTab } from "@codemirror/commands"

import { parser } from "./riscv.grammar";
import { highlighting } from "./GrammarHighlight";
import { LRLanguage, LanguageSupport, indentService, indentUnit } from "@codemirror/language"
import { RegisterTable } from "./RegisterTable";
import { MemoryView } from "./MemoryView";
import { PaneResize } from "./PaneResize";
import { githubLight, githubDark, Theme, Colors, githubHighlightStyle } from './GithubTheme'
import { AsmErrState, continueStep, DebugState, ErrorState, fetchTestcases, getCurrentLine, IdleState, initialRegs, nextStep, quitDebug, RunningState, runNormal, runTestSuite, setWasmRuntime, singleStep, startStep, startStepTestSuite, StoppedState, testData, TestSuiteState, TestSuiteTableEntry, TEXT_BASE, wasmInterface, wasmRuntime, wasmTestsuite, wasmTestsuiteIdx } from "./EmulatorState";
import { highlightTree } from "@lezer/highlight";

let parserWithMetadata = parser.configure({
	props: [highlighting]
})

export const riscvLanguage = LRLanguage.define({
	parser: parserWithMetadata,
})

let currentTheme: Theme = getDefaultTheme();

export let view: EditorView;
let cmTheme: Compartment = new Compartment();
const lintCompartment = new Compartment();

function updateCss(colors: Colors): void {
	document.getElementById('themestyle').innerHTML = `
.theme-bg {
	background-color: ${colors.base0};
}
.cm-debugging {
	background-color: ${colors.bgorange};
}
.cm-tooltip-lint {
	color: ${colors.base5};
	background-color: ${colors.base0};
	font-family: monospace;
}
.cm-breakpoint-marker {
	background-color: ${colors.red};
}
.theme-bg-hover:hover {
	background-color: ${colors.base1};
}
.theme-bg-active:active {
	background-color: ${colors.base1};
}
.theme-gutter {
	background-color: ${colors.base0};
}
.theme-separator {
	background-color: ${colors.base1};
}
.theme-fg {
	color: ${colors.base4};
}
.theme-fg2 {
	color: ${colors.base3};
}
.theme-scrollbar-slim {
	scrollbar-width: thin;
	scrollbar-color: ${colors.base3} ${colors.base0};
}
.theme-scrollbar {
	scrollbar-color: ${colors.base3} ${colors.base0};
}
.theme-border {
	border-color: ${colors.base2};
}
.frame-highlight {
	background-color: ${colors.bggreen};
}
.theme-testsuccess {
	background-color: ${colors.testgreen};
}
.theme-testfail {
	background-color: ${colors.testred};
}

.cm-header-widget {
}

.theme-style0 { color: ${colors.purp}; }
.theme-style1 { color: ${colors.red}; }
.theme-style2 { color: ${colors.blue}; }
.theme-style3 { color: ${colors.orange}; }
.theme-style4 { color: ${colors.base4}; }
.theme-style5 { color: ${colors.orange}; }
.theme-style6 { color: ${colors.lightblue}; }
.theme-style7 { color: ${colors.base3}; }
.theme-style8 { font-weight: bold; }
.theme-style9 { font-style: italic; }
.theme-style10 { text-decoration: line-through; }
.theme-style11 { text-decoration: underline; }
.theme-style12 { color: ${colors.base3}; text-decoration: underline; }
.theme-style13 { color: ${colors.orange}; }
.theme-style14 { color: ${colors.green}; }
.theme-style15 { color: ${colors.base5}; }

.cm-header-widget {
	padding-bottom: 1rem;
}

.cm-header-widget > a {
	background-color: ${colors.base1};
	font-style: italic;
	font-weight: bold;
}

.cm-header-widget > div {
	background-color: ${colors.base1};
	display: inline-block;
	padding-left: 0.5em;
	padding-right: 0.5em;
	padding-top: 0.25em;
	padding-bottom: 0.25em;
}


@keyframes fadeHighlight {
	from {
		background-color: ${colors.bgorange};
	}
	to {
	}
}
.animate-fade-highlight {
	animation: fadeHighlight 1s forwards;
}
`;
}

declare global {
  interface Window {
    urlParams: URLSearchParams;
	testsuiteName: string;
  }
}
export const testsuiteName = window.testsuiteName;
const localStorageKey = window.testsuiteName ? ("savedtext-" + window.testsuiteName) : "savedtext";

window.addEventListener("DOMContentLoaded", () => {
	updateCss(currentTheme.colors);
});

function changeTheme(theme: Theme): void {
	// TODO: the theme reconfigure would be unnecessary if i just have all the styles in one place
	view.dispatch({ effects: cmTheme.reconfigure(theme.cmTheme) });
	updateCss(theme.colors);
}

function getDefaultTheme() {
	const savedTheme = localStorage.getItem("theme");
	if (savedTheme && savedTheme == "GithubDark") return githubDark;
	else if (savedTheme && savedTheme == "GithubLight") return githubLight;

	const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
	if (prefersDark) return githubDark;
	else return githubLight;
}

function doChangeTheme(): void {
	if (currentTheme == githubDark) {
		currentTheme = githubLight;
		changeTheme(githubLight);
		localStorage.setItem("theme", "GithubLight");
	}
	else if (currentTheme == githubLight) {
		currentTheme = githubDark;
		changeTheme(githubDark);
		localStorage.setItem("theme", "GithubDark");
	}
}

const isMac = navigator.platform.toLowerCase().includes('mac');
const prefixStr = isMac ? "Ctrl-Shift" : "Ctrl-Alt"

window.addEventListener('keydown', (event) => {
	// FIXME: this is deprecated but i'm not sure what is the correct successor
	const prefix = isMac ? (event.ctrlKey && event.shiftKey) : (event.ctrlKey && event.altKey);

	if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'S') {
		event.preventDefault();
		singleStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'N') {
		event.preventDefault();
		nextStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'C') {
		event.preventDefault();
		continueStep(wasmRuntime, setWasmRuntime);
	}
	else if (wasmRuntime.status == "debug" && prefix && event.key.toUpperCase() == 'X') {
		event.preventDefault();
		quitDebug(wasmRuntime, setWasmRuntime);
	}
	if (testData) {
		if (prefix && event.key.toUpperCase() == 'R') {
			event.preventDefault();
			runTestSuite(wasmRuntime, setWasmRuntime);
		}
	} else {
		if (prefix && event.key.toUpperCase() == 'R') {
			event.preventDefault();
			runNormal(wasmRuntime, setWasmRuntime);
		}
		else if (prefix && event.key.toUpperCase() == 'D') {
			event.preventDefault();
			startStep(wasmRuntime, setWasmRuntime);
		}
	}
});

const Navbar: Component = () => {
	return (
		<nav class="flex-none theme-gutter">
			<div class="mx-auto px-2">
				<div class="flex items-center h-10">
					<div class="flex-shrink-0">
						<h1 class="text-xl font-bold theme-fg">ARES</h1>
					</div>
					<div class="flex-shrink-0 mx-auto"></div>
					<Show when={wasmRuntime.status == "debug" ? wasmRuntime : null}>{debugRuntime => <>
						<button
							on:click={() => singleStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Step into (${prefixStr}-S)`}
						>
							step_into
						</button>
						<button
							on:click={() => nextStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Step over/Next (${prefixStr}-N)`}
						>
							step_over
						</button>
						<button
							on:click={() => continueStep(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Continue (${prefixStr}-C)`}
						>
							resume
						</button>
						<button
							on:click={() => quitDebug(debugRuntime(), setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Exit debugging (${prefixStr}-X)`}
						>
							stop
						</button>
						<div class="cursor-pointer flex-shrink-0 mx-auto"></div></>
					}
					</Show>
					<button
						on:click={doChangeTheme}
						class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
						title="Change theme"
					>
						dark_mode
					</button>
					<Show when={testsuiteName}>
						<button
							on:click={() => runTestSuite(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Run tests (${prefixStr}-R)`}
						>
							play_circle
						</button>
					</Show>
					<Show when={!testsuiteName}>
						<button
							on:click={() => runNormal(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Run (${prefixStr}-R)`}
						>
							play_circle
						</button>
						<button
							on:click={() => startStep(wasmRuntime, setWasmRuntime)}
							class="cursor-pointer flex-0-shrink flex material-symbols-outlined theme-fg theme-bg-hover theme-bg-active"
							title={`Debug (${prefixStr}-D)`}
						>
							arrow_forward
						</button>
					</Show>
				</div>
			</div>
		</nav>
	);
};

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

function toHex(arg: number): string {
	return "0x" + arg.toString(16).padStart(8, "0");
}

const BacktraceCall: Component<{ name: string, args: number[], sp: number }> = (props) => {
	return <div class="flex">
		<div class="font-bold pr-1">{props.name}</div>
		<div class="flex flex-row flex-wrap">
			<div class="theme-fg2">args=</div>
			<div class="pr-1">{toHex(props.args[0])}</div>
			<div class="pr-1">{toHex(props.args[1])}</div>
			<div class="pr-1">{toHex(props.args[2])}</div>
			<div class="pr-1">{toHex(props.args[3])}</div>
			<div class="pr-1">{toHex(props.args[4])}</div>
			<div class="pr-1">{toHex(props.args[5])}</div>
			<div class="pr-1">{toHex(props.args[6])}</div>
			<div class="pr-1">{toHex(props.args[7])}</div>
			<div class="theme-fg2">sp=</div>
			<div class="pr-1">{toHex(props.sp)}</div>
		</div>
	</div>
};


const BacktraceView = (state: DebugState | ErrorState) => {
	return <div class="w-full h-full font-mono text-sm overflow-auto theme-scrollbar-slim flex flex-col">
		{[...state.shadowStack].reverse().map(ent => <BacktraceCall name={ent.name} args={ent.args} sp={ent.sp} />)}
	</div>;
};

const Editor: Component = () => {
	let editor: HTMLDivElement | undefined;
	// enable and disable linter based on debugMode() and hasError()
	createComputed(() => {
		const disable = wasmRuntime.status == "debug" || wasmRuntime.status == "error";
		if (view == undefined) return;
		if (disable) {
			view.dispatch({
				effects: lintCompartment.reconfigure([])
			});
		} else {
			view.dispatch({
				effects: lintCompartment.reconfigure(createAsmLinter())
			});
		}
	})
	createComputed(() => {
		let lineno = 0;
		if (wasmRuntime.status == "debug" || wasmRuntime.status == "error") {
			lineno = getCurrentLine(wasmRuntime);
		}
		if (!view) return;
		view.dispatch({
			effects: lineHighlightEffect.of(lineno), // disable the line highlight, as line numbering starts from 1 
		});
	})

	onMount(async () => {
		await fetchTestcases();

		const theme = EditorView.theme({
			"&.cm-editor": { height: "100%" },
			".cm-scroller": { overflow: "auto" },
		});
		const savedText = localStorage.getItem(localStorageKey) || "";
		const state = EditorState.create({
			doc: savedText,
			extensions: [
				tabKeymap,
				new LanguageSupport(riscvLanguage, [dummyIndent]),
				lintCompartment.of(createAsmLinter()),
				breakpointGutter, // must be first so it's the first gutter
				basicSetup,
				theme,
				EditorView.editorAttributes.of({ style: "font-size: 1.4em" }),
				cmTheme.of(currentTheme.cmTheme),
				[lineHighlightState],
				indentUnit.of("    "),
				keymap.of([...defaultKeymap, indentWithTab]),
				headerDecoration(),
				EditorView.lineWrapping
			],
		});
		view = new EditorView({ state, parent: editor });

		setInterval(() => {
			localStorage.setItem(localStorageKey, view.state.doc.toString());
		}, 1000);
	});

	return <main
		class="w-full h-full overflow-hidden theme-scrollbar" style={{ contain: "strict" }}
		ref={editor} />;
}

let consoleText = (_runtime: IdleState | RunningState | DebugState | ErrorState | StoppedState | AsmErrState | TestSuiteState) => {
	if (_runtime.status == "idle" || _runtime.status == "testsuite") return "";
	return _runtime.consoleText;
}

const TestSuiteViewer = (table: TestSuiteTableEntry[], currentDebuggingEntry: number) => {
	return (
		<div class="theme-scrollbar theme-bg theme-fg overflow-x-auto overflow-y-auto w-full h-full">
			<table class="table w-full max-w-full h-full min-w-full border-collapse rounded-lg ">
				<thead class=" ">
					<tr class="  text-left theme-fg border-b theme-border">
						<th class="w-[8ch] px-2 py-1 font-semibold theme-fg">status</th>
						<th class="w-[14ch] px-2 py-1 font-semibold theme-fg">input</th>
						<th class="w-[8ch] px-2 py-1 font-semibold theme-fg whitespace-nowrap">expected</th>
						<th class="w-[14ch] px-2 py-1 font-semibold theme-fg whitespace-nowrap">yours</th>
					</tr>
				</thead>
				<tbody class=" ">
					{table.map((testcase, index) => {
						const passed = testcase.output === testcase.userOutput;
						const errorType = testcase.runErr ? "crashed" : "mismatched";
						return (
							<tr
								class={`  border-b theme-border ${passed ? 'theme-testsuccess' : 'theme-testfail'}`}
							>
								<td class="px-2">
									{passed ?
										<div class="flex flex-col">
											<span class="text-sm">success</span>
											<button class="text-left text-sm hover:font-semibold " on:click={() => startStepTestSuite(wasmRuntime, setWasmRuntime, index)}>
												{currentDebuggingEntry === index ? "debugging" : "debug it"}
											</button>
										</div> :
										<div class="flex flex-col">
											<span class="text-sm">{errorType}</span>
											<button class="text-left text-sm underline hover:font-semibold " on:click={() => startStepTestSuite(wasmRuntime, setWasmRuntime, index)}>
												{currentDebuggingEntry === index ? "debugging" : "debug it"}
											</button>
										</div>}
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class="text-xs font-mono rounded whitespace-pre-wrap break-words max-w-full block">
										{testcase.input}
									</code>
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class="px-1 py-1.5 rounded text-xs font-mono whitespace-pre-wrap break-words max-w-full block">
										{testcase.output}
									</code>
								</td>
								<td class="px-1 py-1.5 text-sm">
									<code class={`px-1 py-1.5 rounded text-xs font-mono whitespace-pre-wrap break-words max-w-full block `}>
										{testcase.userOutput}
									</code>
								</td>
							</tr>
						);
					})}
				</tbody>
			</table>
		</div>
	);
};


function createHighlightedText(code: string, syntaxHighlight: boolean) {
	let block = document.createElement("div")
	block.className = "cm-header-code"

	let pos = 0
	if (syntaxHighlight) {
		let tree = riscvLanguage.parser.parse(code)
		highlightTree(tree, githubHighlightStyle, (from, to, classes) => {
			if (from > pos) block.appendChild(document.createTextNode(code.slice(pos, from)))
			let span = document.createElement("span")
			span.className = classes
			span.textContent = code.slice(from, to)
			block.appendChild(span)

			pos = to
		})
	}
	if (pos < code.length) block.appendChild(document.createTextNode(code.slice(pos)))
	return block
}

function parseFormat(fmt: string, container): void {
	const parts = fmt.split(/(```[\s\S]*?```)/);

	for (const part of parts) {
		if (part.startsWith('```') && part.endsWith('```')) {
			let codeContent = part.slice(3, -3);
			let highlight = false;
			if (codeContent.startsWith("riscv")) {
				highlight = true;
				codeContent = codeContent.slice(5);
			}
			if (codeContent.startsWith('\r\n')) {
				codeContent = codeContent.slice(2);
			} else if (codeContent.startsWith('\n')) {
				codeContent = codeContent.slice(1);
			}

			const highlightedElement = createHighlightedText(codeContent, highlight);
			container.appendChild(highlightedElement);
		} else {
			parseInlineCode(part, container);
		}
	}
}

function parseInlineCode(text: string, container: HTMLElement): void {
	// Split by single backticks to handle inline code
	const parts = text.split(/(`[^`]+`)/);

	for (const part of parts) {
		if (part.startsWith('`') && part.endsWith('`')) {
			const codeContent = part.slice(1, -1);
			const anchor = document.createElement('a');
			anchor.textContent = codeContent;
			container.appendChild(anchor);
		} else if (part) {
			const textNode = document.createTextNode(part);
			container.appendChild(textNode);
		}
	}
}

class HeaderWidget extends WidgetType {
	constructor() {
		super();
	}

	toDOM(view: EditorView) {
		const container = document.createElement("div");
		container.className = "cm-header-widget";
		let tdata = testData;
		let assignment = tdata ? tdata.assignment : "";
		parseFormat(assignment, container);
		return container;
	}

	ignoreEvent() {
		return true;
	}
}




function headerDecoration() {
	return EditorView.decorations.of(Decoration.set([
		Decoration.widget({
			widget: new HeaderWidget(),
			side: -1,
			block: true
		}).range(0)
	]))
}


const App: Component = () => {
	return (
		<div class="fullsize flex flex-col justify-between overflow-hidden">
			<Navbar />
			<div class="grow flex overflow-hidden">
				<PaneResize firstSize={0.5} direction="horizontal" second={true}>
					{() =>
						<PaneResize firstSize={0.65} direction="vertical"
							second={wasmTestsuite().length > 0}>
							{() => <PaneResize firstSize={0.85} direction="vertical"
								second={((wasmRuntime && (wasmRuntime.status == "debug" || wasmRuntime.status == "error")) && wasmRuntime.shadowStack.length > 0) ? wasmRuntime : null}>
								{() => <Editor />}
								{wasmRuntime => BacktraceView(wasmRuntime)}
							</PaneResize>}
							{() => TestSuiteViewer(wasmTestsuite(), wasmTestsuiteIdx())}
						</PaneResize>
					}

					{() => <PaneResize firstSize={0.75} direction="vertical" second={true}>
						{() => <PaneResize firstSize={0.55} direction="horizontal" second={true}>
							{() => <RegisterTable pc={(wasmRuntime.status == "idle" || wasmRuntime.status == "asmerr" || wasmRuntime.status == "testsuite") ? TEXT_BASE : wasmRuntime.pc}
								regs={(wasmRuntime.status == "idle" || wasmRuntime.status == "asmerr" || wasmRuntime.status == "testsuite") ? initialRegs : wasmRuntime.regs}
								regWritten={wasmInterface.regWritten ? wasmInterface.regWritten[0] : 0} />}
							{() => <MemoryView version={() => wasmRuntime.version}
								writeAddr={wasmInterface.memWrittenAddr ? wasmInterface.memWrittenAddr[0] : 0}
								writeLen={wasmInterface.memWrittenLen ? wasmInterface.memWrittenLen[0] : 0}
								sp={wasmInterface.regsArr ? wasmInterface.regsArr[2 - 1] : 0}
								load={wasmInterface.emu_load}
							/>}
						</PaneResize>}
						{() => (<div
							innerText={consoleText(wasmRuntime) ? consoleText(wasmRuntime) : "Console output will go here..."}
							class={"w-full h-full font-mono text-md overflow-auto theme-scrollbar theme-bg " + (consoleText(wasmRuntime) ? "theme-fg" : "theme-fg2")}
						></div>)}
					</PaneResize>}
				</PaneResize>
			</div>
		</div>
	);
};

export default App;
