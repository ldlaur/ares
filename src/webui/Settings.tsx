import {Component, createEffect, createSignal, onCleanup, Show} from "solid-js";
import {ThemeIcon} from "./EditorToolbar";
import {darkTheme, lightTheme, setDarkTheme, setLightTheme} from "./Theme";


export const Settings : Component<{ close: () => void }> = (props) => {
    let modalRef: HTMLDivElement | undefined;
    const [selection, setSelection] = createSignal(0);

    function ModifyTheme(value: string, light: boolean) {
        if (light) {
            setLightTheme(value);
            localStorage.setItem("lightTheme", value);
        } else {
            setDarkTheme(value);
            localStorage.setItem("darkTheme", value);
        }


        const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
        if ((light && (ThemeIcon() == "sunny" || (ThemeIcon() == "night_sight_auto" && !prefersDark))) || (!light && (ThemeIcon() == "dark_mode" || (ThemeIcon() == "night_sight_auto" && prefersDark)))) {
            document.documentElement.dataset.theme = value;
        }
    }


    createEffect(() => {
        const handleKeyDown = (e: KeyboardEvent) => {
            if (e.key === "Escape") props.close();
        };

        window.addEventListener("keydown", handleKeyDown);
        modalRef?.focus();

        onCleanup(() => window.removeEventListener("keydown", handleKeyDown));
    });
    return (
        <div
            class="fixed inset-0 z-50 flex items-center justify-center bg-black/70"
            onClick={props.close}
        >
            <div
                ref={modalRef}
                tabindex="-1"
                class="relative flex flex-col w-full max-w-[90vw] max-h-[90vh] border overflow-hidden theme-border outline-none"
                onClick={(e) => e.stopPropagation()}
                role="dialog"
                aria-modal="true"
                aria-labelledby="modal-title"
            >
                <header class="sticky top-0 z-10 flex items-center justify-between pl-4 theme-border border-b theme-bg">
                    <h2 id="modal-title" class="text-lg font-bold text-base4">
                        Settings
                    </h2>
                    <button
                        onClick={props.close}
                        aria-label="Close modal"
                        type="button"
                        class="theme-bg p-2 text-base4 cursor-pointer material-symbols-outlined"
                    >
                        {"close"}
                    </button>
                </header>

                <div class="flex flex-row px-4 h-full items-center theme-bg">
                    <div class="flex flex-col pt-4 pb-4 mr-2 pr-4 justify-evenly ">
                        <button
                            type="button"
                            aria-label="Settings"
                            class="theme-bg text-base4 cursor-pointer text-xl"
                            onClick={() => setSelection(0)}
                        >
                            Settings
                        </button>
                        <button
                            type="button"
                            aria-label="Settings"
                            class="theme-bg text-base4 cursor-pointer text-xl mt-4"
                            onClick={() => setSelection(1)}
                        >
                            About
                        </button>
                    </div>
                    <Show when={selection() == 0}>
                        <div class="flex flex-col w-full gap-3 h-full border-l-[1px] theme-border justify-center">
                            <div class="flex flex-row justify-between gap-1 mt-2 ml-[10%] mr-[10%]">
                                <p class="text-base4 text-lg font-bold self-center">Light Theme</p>
                                <select
                                    class="text-base4 w-[30%] theme-bg border-2 theme-border p-2 focus:ring-blue-400 focus:border-blue-400 shadow-xs"
                                    onChange={ e => ModifyTheme(e.target.value, true)}
                                    value={lightTheme()}
                                >
                                    <option value="ayu-light">Ayu</option>
                                    <option value="catpuccin-latte">Catpuccin Latte</option>
                                    <option value="github-light">Default</option>
                                    <option value="rose-pine-dawn">Rosé Pine Dawn</option>
                                </select>
                            </div>
                            <div class="flex flex-row justify-between gap-1 mb-2 ml-[10%] mr-[10%]">
                                <p class="text-base4 text-lg font-bold self-center">Dark Theme</p>
                                <select
                                    class="text-base4 w-[30%] theme-bg border-2 theme-border p-2 focus:ring-blue-400 focus:border-blue-400 shadow-xs"
                                    onChange={ e => ModifyTheme(e.target.value, false)}
                                    value={darkTheme()}
                                >
                                    <option value="ayu-dark">Ayu Dark</option>
                                    <option value="ayu-mirage">Ayu Mirage</option>
                                    <option value="catpuccin-frappe">Catpuccin Frappé</option>
                                    <option value="catpuccin-macchiato">Catpuccin Macchiato</option>
                                    <option value="catpuccin-mocha">Catpuccin Mocha</option>
                                    <option value="github-dark">Default</option>
                                    <option value="rose-pine">Rosé Pine</option>
                                    <option value="rose-pine-moon">Rosé Pine Moon</option>
                                </select>
                            </div>
                        </div>
                    </Show>
                    <Show when={selection() == 1}>
                        <div class="flex flex-col w-full h-full border-l-[1px] theme-border justify-center pl-[2%] pb-2 pt-2">
                            <a class="text-3xl font-bold underline text-base4 hover:text-blue-400" href="https://github.com/ldlaur/ares" >
                                ARES
                            </a>
                            <p class="text-base4 mt-2">
                                A RISC-V educational simulator designed to help computer architecture students to visualize memory and call stacks, and detect common calling convention mistakes.
                            </p>
                            <p class="text-base4">
                                It is inspired by RARS and MARS, and designed to be a spiritual web successor of them.
                            </p>
                            <p class="text-base4 mt-1">
                                If you have any suggestions for new features or have encountered bugs, don't be afraid to open a pull request or an issue on
                                <a href="https://github.com/ldlaur/ares" class=" pl-1 underline font-bold text-base4 hover:text-blue-400">GitHub</a>.
                            </p>
                            <p class="text-base4 mt-1">Thanks to all contributors and testers, and to everyone who has used it</p>
                        </div>
                    </Show>
                </div>
            </div>
        </div>
    );
};
