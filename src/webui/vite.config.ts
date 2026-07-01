import { defineConfig, Plugin } from "vite";
import solidPlugin from "vite-plugin-solid";
import clangPlugin from "./vite-plugin-clang.js";
import { lezer } from "@lezer/generator/rollup";
import tailwindcss from "tailwindcss";
import autoprefixer from "autoprefixer";

// prefetch the wasm file from index.html directly
// saving one round trip
function wasmPrefetchPlugin(): Plugin {
  let wasmFileName: string | undefined;

  return {
    name: "wasm-prefetch",
    generateBundle(_, bundle) {
      for (const [fileName] of Object.entries(bundle)) {
        if (fileName.endsWith(".wasm")) {
          wasmFileName = fileName;
        }
      }
    },
    transformIndexHtml() {
      if (!wasmFileName) return [];
      return [
        {
          tag: "script",
          attrs: {},
          children: `window.__wasmFetch = fetch('/${wasmFileName}');`,
          injectTo: "head-prepend",
        },
      ];
    },
  };
}

export default defineConfig({
  root: "src/webui",
  publicDir: "../../public",
  plugins: [
    wasmPrefetchPlugin(),
    solidPlugin(),
    clangPlugin(),
    lezer()
  ],
  server: {
    port: 3000,
  },
  optimizeDeps: {
    include: ["@lezer/generator"],
  },
  css: {
    postcss: {
      plugins: [tailwindcss(), autoprefixer()],
    },
  },
  build: {
    outDir: "../../dist",
    emptyOutDir: true,
    target: "firefox89",
  },
});
