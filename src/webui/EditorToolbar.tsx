import { Component, createSignal, Show } from "solid-js";
import { prefixStr, setIntegratedHelp, setShowSettings, testsuiteName } from "./App";
import { currentTheme, doChangeTheme } from "./Theme";
import { continueExecution, nextStep, quitDebug, reverseStep, run, runTestSuite, singleStep, startDebug, state } from "./EmulatorStore";
import { githubLight } from "./GithubTheme";
export const [ThemeIcon, setThemeIcon] = createSignal(getDefaultIcon())

// to rebuild font.woff2, download https://fonts.googleapis.com/css2?family=Material+Symbols+Outlined:opsz,wght,FILL,GRAD@24,200,0,0&icon_names=arrow_forward,close,dark_mode,folder_open,help,night_sight_auto,play_circle,resume,save,settings,step_into,step_over,stop,sunny,undo

export const EditorToolbar: Component<{ textGetter: () => string, setText: (s: string) => void }> = (props) => {
    return (
        <div class="flex flex-col">
            <div class="flex-none flex border-b theme-border h-9 pr-1">
                <h1 class="select-none text-lg font-bold tracking-wide ml-2 mr-3 pl-2 uppercase"
                    style="line-height: 2.25rem; margin: 0;">
                    ARES
                </h1>
                <div class="flex-grow"></div>

                <div class="flex items-center gap-0.5">
                    <ToolbarBtn
                        class="theme-bg"
                        icon="help"
                        title="Show integrated help"
                        onClick={() => setIntegratedHelp(true)}
                    />

                    <ToolbarBtn
                        class="theme-bg"
                        icon={ThemeIcon()}
                        title="Change theme"
                        onClick={() => doChangeTheme(ThemeIcon())}
                    />

                    <ToolbarBtn
                        class="theme-bg"
                        icon="settings"
                        title="Settings"
                        onClick={() => setShowSettings(true)}
                    />

                    <div class="w-px h-5 theme-separator mx-1"></div>

                    <ToolbarBtn
                        class="theme-bg"
                        icon="save"
                        title="Save"
                        onClick={() => doSave(props.textGetter())} />

                    <ToolbarBtn
                        class="theme-bg"
                        icon="folder_open"
                        title="Open file"
                        onClick={() => doOpen(props.setText)} />

                    <div class="w-px h-5 theme-separator mx-1"></div>

                    <Show when={testsuiteName}>
                        <ToolbarBtn
                            class="theme-bg"
                            icon="play_circle"
                            title={`Run tests (${prefixStr}-R)`}
                            onClick={() => runTestSuite(props.textGetter())}
                        />
                    </Show>
                    <Show when={!testsuiteName}>
                        <ToolbarBtn
                            class="theme-bg"
                            icon="play_circle"
                            title={`Run (${prefixStr}-R)`}
                            onClick={() => run(props.textGetter())}
                        />
                        <ToolbarBtn
                            class="theme-bg"
                            icon="arrow_forward"
                            title={`Debug (${prefixStr}-D)`}
                            onClick={() => startDebug(props.textGetter())}
                        />
                    </Show>
                </div>
            </div>
            <Show when={state.status == "debug" ? state : null}>{debugRuntime => <>
                <div class="font-semibold text-sm pl-2 py-1 flex items-center gap-2 theme-bg-debugging pr-1">
                    <span>Debugging mode, exit it to edit text</span>
                    <div class="flex-grow"></div>
                    <ToolbarBtn
                        class="theme-bg-debugging"
                        icon="step_into"
                        title={`Step into (${prefixStr}-S)`}
                        onClick={() => singleStep()}
                    />
                    <ToolbarBtn
                        class="theme-bg-debugging"
                        icon="step_over"
                        title={`Step over/Next (${prefixStr}-N)`}
                        onClick={() => nextStep()}
                    />
                    <ToolbarBtn
                        class="theme-bg-debugging"
                        icon="resume"
                        title={`Continue (${prefixStr}-C)`}
                        onClick={() => continueExecution()}
                    />
                    <ToolbarBtn
                        class="theme-bg-debugging"
                        icon="undo"
                        title={`Reverse step (${prefixStr}-Z)`}
                        onClick={() => reverseStep()}
                    />
                    <ToolbarBtn
                        class="theme-bg-debugging"
                        icon="stop"
                        title={`Exit debugging (${prefixStr}-X)`}
                        onClick={() => quitDebug()}
                    />
                </div></>}
            </Show>
        </div>

    );
};

export function doSave(content: string) {
    const blob = new Blob([content], { type: "text/plain" });

    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = "main.s";

    link.click();

    URL.revokeObjectURL(link.href);
}


function openFile(): Promise<string> {
    return new Promise((resolve, reject) => {
        const input = document.createElement("input");
        input.type = "file";
        input.accept = ".s,.S,.asm,text/plain";

        input.onchange = async () => {
            const file = input.files?.[0];
            if (!file) {
                reject("No file selected");
                return;
            }

            const text = await file.text();
            resolve(text);
        };

        input.click();
    });
}

function doOpen(setText: (s: string) => void) {
    openFile().then(setText)
}

function getDefaultIcon(): string {
    const savedTheme = localStorage.getItem("theme");
    if (savedTheme && savedTheme == "System") {
        return "night_sight_auto";
    }
    return currentTheme() == githubLight ? "sunny" : "dark_mode";
}

const ToolbarBtn: Component<{ class: string, icon: string; title: string; onClick: () => void }> = (props) => (
    <button
        on:click={props.onClick}
        class={props.class + " cursor-pointer flex items-center justify-center w-7 h-7 material-symbols-outlined theme-bg-hover theme-bg-active"}
        style={{ "font-size": "26px" }}
        title={props.title}
    >
        {props.icon}
    </button>
);
