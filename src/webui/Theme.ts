import { createEffect, createRoot, createSignal } from "solid-js";
import { Colors, githubDark, githubLight } from "./GithubTheme";
import { setThemeIcon, ThemeIcon } from "./EditorToolbar";

export const [lightTheme, setLightTheme] = createSignal(getDefaultLightTheme());
export const [darkTheme, setDarkTheme] = createSignal(getDefaultDarkTheme());
export let [currentTheme, setCurrentTheme] = createSignal(getDefaultTheme());

function updateCss(colors: Colors): void {
	document.getElementById('themestyle')!.innerHTML = `
.theme-bg {
	background-color: var(--color-base0);
}
.cm-debugging {
	background-color: ${colors.bgorange};
}
.theme-bg-debugging {
	background-color: ${colors.bgorange0};
}
.theme-mono {
    font-family: SFMono-Regular, Consolas, Liberation Mono, Menlo, monospace;
}
.cm-editor .cm-content  {
    font-family: SFMono-Regular, Consolas, Liberation Mono, Menlo, monospace;
}
.cm-gutterElement   {
    font-family: SFMono-Regular, Consolas, Liberation Mono, Menlo, monospace;
}
.cm-tooltip-lint {
	color: ${colors.base5};
	background-color: var(--color-base0);
    font-family: "Consolas", "Lucida Console", "Courier New", monospace;
}
.cm-breakpoint-marker {
	background-color: ${colors.red};
}
.theme-bg-hover:hover {
	background-color: var(--color-base1); 
}
.theme-bg-active:active {
	background-color: var(--color-base2); 
}
.theme-gutter {
	background-color: var(--color-base0);
}
.theme-separator {
	background-color: var(--color-base2);
}
.theme-fg {
	color: var(--color-base4);
}
.theme-fg-invert {
	color: var(--color-base0);
}
.theme-fg2 {
	color: ${colors.base3};
}
.theme-scrollbar-slim {
	scrollbar-width: thin;
	scrollbar-color: ${colors.base3} var(--color-base0);
}
.theme-scrollbar {
	scrollbar-color: ${colors.base3} var(--color-base0);
}
.theme-border {
	border-color: var(--color-base2);
}
.theme-border-column-rule {
	column-rule: 1px solid var(--color-base2);
}
.theme-tab {
	background-color: var(--color-base1a);
}
.theme-border-strong {
	border-color: ${colors.base3};
}
.sp-highlight {
	background-color: ${colors.bggreen};
}
.fp-highlight {
	background-color: ${colors.bgpurp};
}
.theme-testsuccess {
	background-color: ${colors.testgreen};
}
.theme-testfail {
	background-color: ${colors.testred};
}

.cm-header-widget {
	padding-bottom: 0;
}

.theme-style0 { color: ${colors.purp}; }
.theme-style1 { color: ${colors.red}; }
.theme-style2 { color: ${colors.blue}; }
.theme-style3 { color: ${colors.orange}; }
.theme-style4 { color: var(--color-base4); }
.theme-style5 { color: ${colors.orange}; }
.theme-style6 { color: ${colors.lightblue}; }
.theme-style7 { color: ${colors.comment}; }
.theme-style8 { font-weight: bold; }
.theme-style9 { font-style: italic; }
.theme-style10 { text-decoration: line-through; }
.theme-style11 { text-decoration: underline; }
.theme-style12 { color: ${colors.base3}; text-decoration: underline; }
.theme-style13 { color: ${colors.orange}; }
.theme-style14 { color: ${colors.green}; }
.theme-style15 { color: ${colors.base5}; }

.cm-header-widget > a {
	background-color: var(--color-base1);
	font-style: italic;
	font-weight: bold;
}

.cm-header-widget > div {
	background-color: var(--color-base1);
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
	animation: fadeHighlight 2.5s forwards;
}
`;
}

createRoot(() => createEffect(() => {
	const theme = currentTheme();
	updateCss(theme.colors);
}));


function getDefaultTheme() {
	const preference = localStorage.getItem("light");
	if (preference && preference == "true") {
		document.documentElement.dataset.theme = lightTheme();
		return githubLight;
	}
	else if (preference && preference == "false") {
		document.documentElement.dataset.theme = darkTheme();
		return githubDark;
	}

	const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
	if (prefersDark) {
		document.documentElement.dataset.theme = darkTheme();
		return githubDark;
	}
	else {
		document.documentElement.dataset.theme = lightTheme();
		return githubLight;
	}
}

export function doChangeTheme(Icon: string): void {
	if (Icon == "dark_mode") {
		setCurrentTheme(githubLight);
		setThemeIcon("sunny");
		document.documentElement.dataset.theme = lightTheme();
		localStorage.setItem("light", "true");
	}
	else if (Icon == "sunny") {
		localStorage.setItem("light", "System");
		setThemeIcon("night_sight_auto");
		const darkMode = window.matchMedia("(prefers-color-scheme: dark)").matches;
		if (darkMode) {
			setCurrentTheme(githubDark);
			document.documentElement.dataset.theme = darkTheme();
		} else {
			setCurrentTheme(githubLight);
			document.documentElement.dataset.theme = lightTheme();
		}
	} else {
		setCurrentTheme(githubDark);
		setThemeIcon("dark_mode");
		document.documentElement.dataset.theme = darkTheme();
		localStorage.setItem("light", "false");
	}

}

window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", (event) => {
	const preference = event.matches ? "dark" : "light";
	if (preference == "dark" && ThemeIcon() == "night_sight_auto") {
		setCurrentTheme(githubDark);
	} else if (ThemeIcon() == "night_sight_auto") {
		setCurrentTheme(githubLight);
	}
});

function getDefaultLightTheme() {
	const savedTheme = localStorage.getItem("lightTheme");
	if (savedTheme) {
		return savedTheme;
	}
	return "github-light";
}

function getDefaultDarkTheme() {
	const savedTheme = localStorage.getItem("darkTheme");
	if (savedTheme) {
		return savedTheme;
	}
	return "github-dark";
}