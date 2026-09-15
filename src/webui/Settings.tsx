import {Component, createSignal, For, Show} from "solid-js";
import {appendCustomTheme, cssVarNames, darkTheme, lightTheme, setTheme, themeList} from "./Theme";
import {Modal} from "./Modal";

export const Settings: Component<{ close: () => void }> = (props) => {
    const [selection, setSelection] = createSignal(0);
    const [themeError, setThemeError] = createSignal("");
    const [displayError, setDisplayError] = createSignal(false);
    let customThemeLight = false;

    function modifyTheme(value: string, light: boolean) {
        const suffix = light ? "light" : "dark";
        const style = document.getElementById("customTheme-" + suffix);
        if (value === "custom-" + suffix && style != null) {
            setTheme(value, light)
        } else if (value !== "custom-" + suffix) {
            setTheme(value, light);
        }
    }

    function readJSON(event: ProgressEvent<FileReader>) {
        if (event.target?.result != null) {
            let json;
            let err = false;
            try {
                json = JSON.parse(event.target.result as string);
            } catch (e) {
                err = true;
                setDisplayError(true);
                setThemeError("There was a problem parsing this file.");
            }
            if (!err) {
                let i = 0;
                while (i < cssVarNames.length && !err) {
                    if (cssVarNames[i] in json) {
                        i++
                    } else {
                        err = true;
                        setDisplayError(true);
                        setThemeError("Your custom theme didn't contain all required keys.");
                    }
                }
                if (!err) {
                    let css: string;
                    const suffix = customThemeLight ? "light" : "dark";
                    css = css = `:root[data-theme="custom-${suffix}"] {`;
                    i = 0;
                    while (i < cssVarNames.length && !err) {
                        console.log(json[cssVarNames[i]]);
                        if (json[cssVarNames[i]].trim().length > 0) {
                            css += "--color-" + cssVarNames[i] + ":" + json[cssVarNames[i]] + ";";
                        } else {
                            setDisplayError(true);
                            setThemeError("Your custom theme has some invalid colors.");
                            err = true;
                        }
                        i++;
                    }
                    console.log(err);
                    if (!err) {
                        css += "}";
                        appendCustomTheme(css, suffix);
                        localStorage.setItem("custom_theme_" + suffix, css);
                    }
                }
            }
        }
    }

    function uploadTheme(event: Event & {
        currentTarget: HTMLInputElement
        target: HTMLInputElement
    }, light: boolean) {
        setDisplayError(false);
        setThemeError("");
        let file = event.target.files![0];
        if (file.type == "application/json") {
            customThemeLight = light;
            const reader = new FileReader();
            reader.onload = readJSON;
            reader.readAsText(file);
        } else {
            setDisplayError(true);
            setThemeError(`Unsupported type.`);
        }
    }

    return (
        <Modal title={"settings"} close={props.close}>
            <div class="flex flex-row px-4 h-full items-center bg-base0">
                <div class="flex flex-col pt-4 pb-4 mr-2 pr-4 justify-evenly ">
                    <button
                        type="button"
                        aria-label="Settings"
                        class="bg-base0 theme-fg cursor-pointer text-xl"
                        onClick={() => setSelection(0)}
                        classList={{
                            "font-bold": selection() === 0,
                        }}
                    >
                        theming
                    </button>
                    <button
                        type="button"
                        aria-label="Settings"
                        class="bg-base0 theme-fg cursor-pointer text-xl mt-4"
                        onClick={() => setSelection(1)}
                        classList={{
                            "font-bold": selection() === 1,
                        }}

                    >
                        about
                    </button>
                </div>
                <Show when={selection() == 0}>
                    <div class="flex flex-col w-full gap-3 h-full justify-center">
                        <div class="flex flex-row justify-between gap-1 mt-2 ml-[2%] mr-[2%]">
                            <p class="theme-fg text-lg font-bold self-center">light theme</p>
                            <div class="pb-0.5 relative inline-block w-48">
                                <select
                                    class="font-semibold w-full fixwebkit text-left pr-6 pb-1 pl-2 border-b-2 theme-fg bg-base0 border-b-base2 focus:outline-none cursor-pointer"
                                    onChange={e => modifyTheme(e.target.value, true)}
                                    value={lightTheme()}
                                >
                                    <For each={themeList.light}>
                                        {theme => <option class="bg-base0 theme-fg" value={theme.name}>{theme.nameUser}</option>}
                                    </For>
                                </select>
                                <svg class="pointer-events-none absolute right-1 top-1/2 -translate-y-1/2 w-4 h-4 theme-fg"
                                     xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M480-344 240-584l56-56 184 184 184-184 56 56-240 240Z" /></svg>
                            </div>
                        </div>

                        <div class="flex flex-row justify-between gap-1 mb-2 ml-[2%] mr-[2%]">
                            <p class="theme-fg text-lg font-bold self-center">dark theme</p>
                            <div class="pb-0.5 relative inline-block w-48">
                                <select
                                    class="font-semibold w-full text-left fixwebkit pr-6 pb-1 pl-2 theme-fg bg-base0  focus:outline-none cursor-pointer"
                                    onChange={e => modifyTheme(e.target.value, false)}
                                    value={darkTheme()}
                                >
                                    <For each={themeList.dark}>
                                        {theme => <option class="bg-base0 theme-fg" value={theme.name}>{theme.nameUser}</option>}
                                    </For>
                                </select>
                                <svg class="pointer-events-none absolute right-1 top-1/2 -translate-y-1/2 w-4 h-4 theme-fg"
                                     xmlns="http://www.w3.org/2000/svg" height="24px" viewBox="0 -960 960 960" width="24px" fill="currentColor"><path d="M480-344 240-584l56-56 184 184 184-184 56 56-240 240Z" /></svg>
                                </div>
                        </div>
                        <div class="flex flex-row justify-between gap-1 ml-[2%] mr-[2%]">
                            <p class="theme-fg text-lg font-bold self-center">custom light theme</p>
                            <div class="flex items-center gap-3">
                                <label
                                    for="fileInputLight"
                                    class="font-semibold w-full text-left pb-1 pr-2 pl-2 theme-fg bg-base0 hover:bg-border focus:outline-none cursor-pointer"
                                >
                                    Upload
                                </label>
                                <input
                                    id="fileInputLight"
                                    type="file"
                                    class="sr-only"
                                    onChange={(e) => uploadTheme(e, true)}
                                />
                            </div>
                        </div>
                        <div class="flex flex-row justify-between gap-1 mb-2 ml-[2%] mr-[2%]">
                            <p class="theme-fg text-lg font-bold self-center">custom dark theme</p>
                            <div class="flex items-center gap-3">
                                <label
                                    for="fileInputDark"
                                    class="font-semibold w-full text-left pb-1 pr-2 pl-2 theme-fg bg-base0 hover:bg-border focus:outline-none cursor-pointer"
                                >
                                    Upload
                                </label>
                                <input
                                    id="fileInputDark"
                                    type="file"
                                    class="sr-only"
                                    onChange={(e) => uploadTheme(e, false)}
                                />
                            </div>
                        </div>
                        <div
                            class={(displayError() ? "flex" : "hidden") + " flex-row flex-wrap p-4 mb-4 gap-1 border-dashed border-2 border-regtable-special"}>
                            <span class="text-xl font-bold text-regtable-special">Error!</span>
                            <p class="theme-fg font-medium self-center pl-0.5">{themeError()}</p>
                            <p class="theme-fg font-medium self-center">For more information, see our guide</p>
                            <a href="https://github.com/ldlaur/ares/blob/master/docs/theme/CustomTheme.md"
                               class="underline font-bold theme-fg self-center">here.</a>
                        </div>
                    </div>
                </Show>
                <Show when={selection() == 1}>
                    <div class="flex flex-col w-full h-full justify-center ml-[2%] mr-[2%] pb-2 pt-2">
                        <a class="text-3xl font-bold underline theme-fg hover:text-highlight-low" href="https://github.com/ldlaur/ares" >
                            ARES
                        </a>
                        <p class="theme-fg mt-2">
                            A RISC-V (RV32IMC) educational simulator built to help computer architecture students visualize registers, memory, call stacks, and catch common calling convention mistakes.
                        </p>
                        <p class="theme-fg">
                            It's inspired by RARS and MARS, and aims to be their spiritual successor on the web.
                        </p>
                        <p class="theme-fg mt-1">
                            Found a bug or have a feature idea? Don't hesitate to open an issue or pull request on
                            <a href="https://github.com/ldlaur/ares" class=" pl-1 underline font-bold theme-fg">GitHub</a>.
                        </p>
                        <p class="theme-fg mt-1 pb-2">Thanks to all contributors, testers, and everyone who's used ARES.</p>
                        <p class="theme-fg mt-1">Rosé Pine theme based on the original at <a class="underline" href="https://rosepinetheme.com/">https://rosepinetheme.com/</a></p>
                        <p class="theme-fg mt-1">Catppuccin theme based on the original at <a class="underline" href="https://catppuccin.com/">https://catppuccin.com/</a></p>
                        <p class="theme-fg mt-1">Ayu theme based on the original at <a class="underline" href="https://ayutheme.com/">https://ayutheme.com/</a></p>
                    </div>
                </Show>
            </div>
        </Modal>
    );
};
