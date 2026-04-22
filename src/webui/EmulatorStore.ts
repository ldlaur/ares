import { createStore, reconcile } from "solid-js/store";
import { createSignal } from "solid-js";
import { emulator } from ".";
import { EmulatorState, TestCaseResult } from "./core/EmulatorState";


export const [state, setState] = createStore<EmulatorState>({
    status: "idle",
    version: 0,
});

export const [testSuiteResults, setTestSuiteResults] = createSignal<
    TestCaseResult[]
>([]);
export const [testSuiteIndex, setTestSuiteIndex] = createSignal<number>(-1);

export function run(source: string): void {
    setState(reconcile(emulator.run(source)));
}

export function runTestSuite(source: string): void {
    const results = emulator.runTestSuite(source);
    setTestSuiteResults(results.results);
    setState(reconcile(results.state));
}

export function startDebug(source: string): void {
    setState(reconcile(emulator.startDebug(source)));
}

export function startDebugTestCase(source: string, index: number): void {
    setTestSuiteIndex(index);
    setState(reconcile(emulator.startDebugTestCase(source, index)));
}

export function singleStep(): void {
    if (state.status !== "debug") return;
    setState(reconcile(emulator.singleStep()));
}

export function nextStep(): void {
    if (state.status !== "debug") return;
    setState(reconcile(emulator.nextStep()));
}

export function continueExecution(): void {
    if (state.status !== "debug") return;
    setState(reconcile(emulator.continueExecution()));
}

export function reverseStep(): void {
    if (state.status !== "debug") return;
    setState(reconcile(emulator.reverseStep()));
}

export function quitDebug(): void {
    setState(reconcile(emulator.quitDebug()));
}

export function setBreakpointLines(lines: number[]): void {
    emulator.setBreakpointLines(lines);
}

export function buildForLinter(source: string): boolean {
    if (
        state.status != "idle" &&
        state.status != "stopped" &&
        state.status != "asmerr"
    )
        return false;
    let newState = emulator.buildForLinter(source);
    if (newState !== null) setState(reconcile(newState));
    return true;
}

export function consoleText(): string {
    if (state.status == "idle") return "";
    if (state.status == "asmerr")
        return `Error on line ${state.error.line}: ${state.error.message}`;
    if (state.status == "stopped") {
        const needsNewline =
            state.consoleText.length > 0 &&
            state.consoleText[state.consoleText.length - 1] !== "\n";
        return (
            state.consoleText +
            (needsNewline ?
                "\nExecuted successfully."
            :   "Executed successfully.")
        );
    }
    return state.consoleText;
}
