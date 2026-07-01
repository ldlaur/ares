import {
	createSignal,
	onMount,
	Show,
	type Component,
} from "solid-js";

import { emulator, testData } from ".";
import { BacktraceView } from "./BacktraceView";
import { Editor, EditorInterface } from "./Editor";
import { doSave, EditorToolbar } from "./EditorToolbar";
import { MemoryView } from "./MemoryView";
import { PaneResize } from "./PaneResize";
import { RegisterTable } from "./RegisterTable";
import { currentTheme } from "./Theme";
import { TEXT_BASE } from "./core/RiscV";
import { buildForLinter, consoleText, continueExecution, nextStep, quitDebug, reverseStep, run, runTestSuite, setBreakpointLines, singleStep, startDebug, state, testSuiteIndex, testSuiteResults } from "./EmulatorStore";
import { TestSuiteViewer } from "./TestSuite";
import { IntegratedHelp } from "./IntegratedHelp";
import { Settings } from "./Settings";

// TODO: exporting those to access them in Theme.ts, but if i do 
// theming with constant CSS classes i shouldn't need this anyways
export const testsuiteName = (new URLSearchParams(window.location.search)).get('testsuite');
export const isMac = navigator.platform.toLowerCase().includes('mac');
export const prefixStr = isMac ? "Ctrl-Shift" : "Ctrl-Alt"
const localStorageKey = testsuiteName ? ("savedtext-" + testsuiteName) : "savedtext";
const origText = localStorage.getItem(localStorageKey) || "";
let editorInterface = new EditorInterface();
export const [integratedHelp, setIntegratedHelp] = createSignal<boolean>(false);
export const [showSettings, setShowSettings] = createSignal(false);

const App: Component = () => {
	onMount(() => {
		window.addEventListener('keydown', (event) => {
			// FIXME: this is deprecated but i'm not sure what is the correct successor
			const prefix = isMac ? (event.ctrlKey && event.shiftKey) : (event.ctrlKey && event.altKey);

			if ((event.ctrlKey || event.metaKey) && event.key.toUpperCase() == 'S' && !prefix) {
				event.preventDefault();
				doSave(editorInterface.getText());
			} else if (state.status == "debug" && prefix && event.key.toUpperCase() == 'S') {
				event.preventDefault();
				singleStep();
			}
			else if (state.status == "debug" && prefix && event.key.toUpperCase() == 'N') {
				event.preventDefault();
				nextStep();
			}
			else if (state.status == "debug" && prefix && event.key.toUpperCase() == 'C') {
				event.preventDefault();
				continueExecution();
			}
			else if (state.status == "debug" && prefix && event.key.toUpperCase() == 'Z') {
				event.preventDefault();
				reverseStep();
			}
			else if (state.status == "debug" && prefix && event.key.toUpperCase() == 'X') {
				event.preventDefault();
				quitDebug();
			}
			if (testData) {
				if (prefix && event.key.toUpperCase() == 'R') {
					event.preventDefault();
					runTestSuite(editorInterface.getText());
				}
			} else {
				if (prefix && event.key.toUpperCase() == 'R') {
					event.preventDefault();
					run(editorInterface.getText());
				}
				else if (prefix && event.key.toUpperCase() == 'D') {
					event.preventDefault();
					startDebug(editorInterface.getText());
				}
			}
		});
	});
	let [address, setAddress] = createSignal({ start: 0, len: 0 });
	return (
		<>
			<div class="fullsize flex flex-row overflow-hidden" inert={integratedHelp() || undefined}>
				<PaneResize firstSize={0.5} direction="horizontal" second={true}>
					{() =>
						<div class="flex flex-col h-full w-full">
							<EditorToolbar textGetter={editorInterface.getText} setText={editorInterface.setText} />
							<div class="flex-grow overflow-hidden">
								<PaneResize firstSize={0.65} direction="vertical"
									second={testSuiteResults().length ? true : null}>
									{() => <PaneResize firstSize={0.85} direction="vertical"
										second={(((state.status == "debug" || state.status == "error")) && state.shadowStack.length > 0) ? state : null}>
										{() => <Editor origText={origText} storeText={text => localStorage.setItem(localStorageKey, text)} asmLinterOn={state.status != "debug" && state.status != "error"}
											editorBlocked={state.status == "debug"}
											highlightedLine={(state.status == "debug" || state.status == "error") ? state.line : undefined}
											editorInterfaceRef={editorInterface} setBreakpoints={setBreakpointLines}
											diagnostics={state.status == "asmerr" ? state.error : undefined}
											doBuild={(s) => buildForLinter(s)}
											theme={currentTheme()}
											onHoveredLine={line => setAddress(line !== null ? emulator.getAddrFromLine(line) : { start: 0, len: 0 })}
										/>}
										{r => <BacktraceView shadowStack={r.shadowStack} />}
									</PaneResize>}
									{(td) => <TestSuiteViewer table={testSuiteResults()} currentDebuggingEntry={testSuiteIndex()} textGetter={editorInterface.getText} />}
								</PaneResize>
							</div>
						</div>
					}

					{() => <PaneResize firstSize={0.75} direction="vertical" second={true}>
						{() => <PaneResize firstSize={0.55} direction="horizontal" second={true}>
							{() => <MemoryView version={() => state.version}
								writeAddr={state.status == "debug" ? state.memWrittenAddr : 0}
								writeLen={state.status == "debug" ? state.memWrittenLen : 0}
								highlightAddr={address().start}
								highlightLen={address().len}
								sp={(state.status == "debug" || state.status == "error" || state.status == "stopped") ? state.regs[2] : 0}
								fp={(state.status == "debug" || state.status == "error" || state.status == "stopped") ? state.regs[8] : 0}
								pc={(state.status == "debug" || state.status == "error" || state.status == "stopped") ? state.pc : 0}
								load={(addr, size) => emulator.load(addr, size)}
								shadowStack={(state.status == "debug" || state.status == "error") ? state.shadowStack : []}
								disassemble={(pc) => emulator.disassemble(pc)}
							/>}
							{() => <RegisterTable pc={(state.status == "debug" || state.status == "error" || state.status == "stopped") ? state.pc : TEXT_BASE}
								regs={(state.status == "idle" || state.status == "asmerr") ? (new Array(32).fill(0)) : state.regs}
								regWritten={state.status == "debug" ? state.regWritten : 0} />}
						</PaneResize>}
						{() => (<div
							innerText={consoleText() ? consoleText() : "Console output will go here..."}
							class={"w-full h-full theme-mono ml-2 mt-1 text-md overflow-auto theme-scrollbar theme-bg theme-fg"}
						></div>)}
					</PaneResize>}
				</PaneResize>
			</div>
			<Show when={showSettings()}>
				<Settings close={() => setShowSettings(false)} />
			</Show>
			<Show when={integratedHelp()}>
				<IntegratedHelp close={() => setIntegratedHelp(false)} />
			</Show>
		</>
	);
};

export default App;