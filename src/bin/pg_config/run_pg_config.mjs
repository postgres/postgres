#!/usr/bin/env node
import Module from "./pg_config.mjs";

// If the module has a `main()` (or similar) method, call it:
if (typeof Module.main === "function") {
  await Module.main();
} else if (typeof Module === "function") {
  // If it's a callable function/class, instantiate or run it
  await Module();
} else {
  console.error("⚠️ Module has no callable entry point");
}
