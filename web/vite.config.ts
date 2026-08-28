import { defineConfig } from "vite";

// Using a relative base ("./") means the built index.html always references
// its JS/CSS with relative paths, so it works correctly whether it's hosted
// at the domain root or in a GitHub Pages subdirectory like /macroz/ -
// no need to pass --base on the command line at all.
export default defineConfig({
  base: "./",
});
