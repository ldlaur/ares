import { createEffect, createRoot, createSignal, onCleanup } from "solid-js";

export const themeList = {
	light: [
		{ name: "ayu-light", nameUser: "Ayu" },
		{ name: "catppuccin-latte", nameUser: "Catppuccin Latte" },
		{ name: "custom-light", nameUser: "Custom" },
		{ name: "github-light", nameUser: "Default" },
		{ name: "rose-pine-dawn", nameUser: "Rosé Pine Dawn" },
	], dark: [
		{ name: "ayu-dark", nameUser: "Ayu Dark" },
		{ name: "catppuccin-mocha", nameUser: "Catppuccin Mocha" },
		{ name: "custom-dark", nameUser: "Custom" },
		{ name: "github-dark", nameUser: "Default" },
		{ name: "rose-pine", nameUser: "Rosé Pine" },
		{ name: "rose-pine-moon", nameUser: "Rosé Pine Moon" },
	]
};

export const [lightTheme, setLightTheme] = createSignal(getTheme("light"));
export const [darkTheme, setDarkTheme] = createSignal(getTheme("dark"));
export const [themeMode, setThemeMode] = createSignal<"light" | "dark" | "system">((() => {
	let mode = localStorage.getItem("theme_mode");
	if (mode === "light" || mode === "dark" || mode === "system") return mode;
	return "system";
})());

export const cssVarNames = [
	'base0',
	'base1',
	'base1a',
	'base2',
	'base3',
	'base4',
	'text',
	'testsuite-red',
	'testsuite-green',
	'editor-selection-match',
	'button-hover',
	'button-active',
	'editor-activeline',
	'sp-highlight',
	'fp-highlight',
	'border',
	'regtable-special',
	'regtable-temp',
	'regtable-arg',
	'regtable-saved',
	'pseudoinst',
	'inst',
	'addrcolumn',
	'debugging',
	'editor-caret',
	'editor-insn',
	'editor-reg',
	'editor-const',
	'editor-string',
	'editor-directive',
	'editor-comment',
];

// dark -> light -> system
export function cycleTheme(): void {
	const curr = themeMode();
	if (curr === "dark") setThemeMode("light");
	else if (curr === "light") setThemeMode("system");
	else setThemeMode("dark");
}

function updateCssTheme() {
	const dark = themeMode() === "system"
		? window.matchMedia("(prefers-color-scheme: dark)").matches
		: themeMode() === "dark";
	document.documentElement.dataset.theme = dark ? getTheme("dark") : getTheme("light");
}

createRoot(() => {
  getCustomTheme("light");
	getCustomTheme("dark");

	const mql = window.matchMedia("(prefers-color-scheme: dark)");
	mql.addEventListener("change", updateCssTheme);
	onCleanup(() => mql.removeEventListener("change", updateCssTheme));

	createEffect(() => {
		localStorage.setItem("theme_light", lightTheme());
		localStorage.setItem("theme_dark", darkTheme());
		localStorage.setItem("theme_mode", themeMode());
		updateCssTheme();
	});
});

export function appendCustomTheme(content: string, suffix: string) {
	let style = document.getElementById("customTheme-" + suffix);
	if (style == null) {
		style = document.createElement("style");
		style.id = "customTheme-" + suffix;
		document.head.append(style);
	}
	style.textContent = content;
}

export function getCustomTheme(mode: "light" | "dark") {
	let custom = localStorage.getItem("custom_theme_" + mode);
	if (custom !== null && custom !== "") {
		appendCustomTheme(custom, mode);
	} else {
    const saved = localStorage.getItem("theme_" + mode);
    if (saved === "custom-" + mode) {
      localStorage.setItem("theme_" + mode, "github-" + mode);
      setTheme("github-" + mode, mode === "light" ? true : false);
    }
	}
}

export function setTheme(value: string, light: boolean) {
	if (light) setLightTheme(value);
	else setDarkTheme(value);
}


function getTheme(mode: "light" | "dark"): string {
	const key = mode === "light" ? "theme_light" : "theme_dark";
	const fallback = mode === "light" ? "github-light" : "github-dark";
	const saved = localStorage.getItem(key);
	const valid = themeList[mode].some(t => t.name === saved);
	return valid ? saved! : fallback;
}
