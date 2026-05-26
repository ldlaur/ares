import { createEffect, createRoot, createSignal } from "solid-js";
import { Colors, githubDark, githubLight } from "./GithubTheme";
import { setThemeIcon, ThemeIcon } from "./EditorToolbar";

export let [currentTheme, setCurrentTheme] = createSignal(getDefaultTheme());

function updateCss(colors: Colors): void {
	document.getElementById('themestyle')!.innerHTML = `
.theme-bg {
	background-color: ${colors.base0};
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
	background-color: ${colors.base0};
    font-family: "Consolas", "Lucida Console", "Courier New", monospace;
}
.cm-breakpoint-marker {
	background-color: ${colors.red};
}
.theme-bg-hover:hover {
	background-color: ${colors.base1}; 
}
.theme-bg-active:active {
	background-color: ${colors.base2}; 
}
.theme-gutter {
	background-color: ${colors.base0};
}
.theme-separator {
	background-color: ${colors.base2};
}
.theme-fg {
	color: ${colors.base4};
}
.theme-fg-invert {
	color: ${colors.base0};
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
.theme-border-column-rule {
	column-rule: 1px solid ${colors.base2};
}
.theme-tab {
	background-color: ${colors.base1a};
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
.theme-style4 { color: ${colors.base4}; }
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
	animation: fadeHighlight 2.5s forwards;
}
`;
}

createRoot(() => createEffect(() => {
	const theme = currentTheme();
	updateCss(theme.colors);
}));


function getDefaultTheme() {
	const savedTheme = localStorage.getItem("theme");
	if (savedTheme && savedTheme == "GithubDark") return githubDark;
	else if (savedTheme && savedTheme == "GithubLight") return githubLight;

	const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
	if (prefersDark) return githubDark;
	else return githubLight;
}

export function doChangeTheme(Icon: string): void {
	if (Icon == "dark_mode") {
		setCurrentTheme(githubLight);
		setThemeIcon("sunny");
		localStorage.setItem("theme", "GithubLight");
	}
	else if (Icon == "sunny") {
		localStorage.setItem("theme", "System");
		setThemeIcon("night_sight_auto");
		const darkMode = window.matchMedia("(prefers-color-scheme: dark)").matches;
		if (darkMode) {
			setCurrentTheme(githubDark);
		} else {
			setCurrentTheme(githubLight);
		}
	} else {
		setCurrentTheme(githubDark);
		localStorage.setItem("theme", "GithubDark");
		setThemeIcon("dark_mode");
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