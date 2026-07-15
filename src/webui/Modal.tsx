import { Component, createSignal, For, onCleanup, onMount, Show } from "solid-js";
import { Portal } from "solid-js/web";

export const Modal: Component<{ title: string, close: () => void, children: any }> = (props) => {
    let modalRef: HTMLDivElement | undefined;
    let previouslyFocused: HTMLElement | null = null;

    function handleKeyDown(e: KeyboardEvent) {
        if (e.key === "Escape") {
            props.close();
            return;
        }
    }

    onMount(() => {
        previouslyFocused = document.activeElement as HTMLElement;
        window.addEventListener("keydown", handleKeyDown);
        modalRef?.focus();
        onCleanup(() => {
            window.removeEventListener("keydown", handleKeyDown);
            previouslyFocused?.focus();
        });
    });
    return (
        <Portal>
            {/* codemirror uses z-index 300 for the search box */}
            <div
                class="fixed inset-0 z-[500] flex items-center justify-center bg-black/70"
                onClick={props.close}
            >
                <div
                    ref={modalRef}
                    tabindex="-1"
                    class="relative flex flex-col w-full max-w-[90vw] max-h-[90vh] bg-base0 border overflow-hidden theme-scrollbar theme-border outline-none"
                    onClick={(e) => e.stopPropagation()}
                    role="dialog"
                    aria-modal="true"
                    aria-labelledby="modal-title"
                >
                    <header class="sticky top-0 z-10 flex items-center justify-between pl-4 theme-border border-b bg-base0">
                        <h2 id="modal-title" class="text-lg font-bold theme-fg">
                            {props.title}
                        </h2>
                        <button
                            onClick={props.close}
                            aria-label="Close modal"
                            type="button"
                            class="bg-base0 p-2 theme-fg cursor-pointer material-symbols-outlined"
                        >
                            {"close"}
                        </button>
                    </header>

                    {props.children}

                </div>
            </div>
        </Portal>
    );
};
