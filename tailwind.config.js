/** @type {import('tailwindcss').Config} */
export default {
  content: [
    './index.html',
    './src/**/*.{js,ts,jsx,tsx,css,md,mdx,html,json,scss}',
  ],
  theme: {
    extend: {
      colors: {
        base0: "var(--color-base0)",
        base1: "var(--color-base1)",
        base1a: "var(--color-base1a)",
        base2: "var(--color-base2)",
        base3: "var(--color-base3)",
        base4: "var(--color-base4)",
        surface: "var(--color-surface)",
        "highlight-low": "var(--color-highlight-low)",
        "highlight-med": "var(--color-highlight-med)",
        "highlight-high": "var(--color-highlight-high)",
      },
    },
  },
  plugins: [],
}

