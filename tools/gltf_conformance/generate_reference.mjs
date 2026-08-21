#!/usr/bin/env node
// tools/gltf_conformance/generate_reference.mjs -- Phase 5 Task 11 (#47),
// gate ruling T11: the ONE reference-render generation mechanism this
// ticket's own text calls for ("generation procedure scripted/documented +
// committed"). Drives a LOCALLY BUILT, pinned glTF-Sample-Renderer (see
// tools/fetch_gltf_sample_renderer.sh) through tools/gltf_conformance/
// harness.html's own window.rxCapture() via Playwright, at matched camera/
// environment/tonemap settings -- see that file's own header comment for
// the full methodology.
//
// NEVER AUTO-RUN: no CI workflow or ctest target invokes this script,
// mirroring tools/regen_references.sh's own "documented, deliberate,
// human-invoked maintenance step" convention exactly. The COMMITTED
// artifacts this script produces (tests/conformance/references/<Model>/
// reference.png + provenance.json) are what every real ctest run compares
// against; nothing regenerates them silently.
//
// PREREQUISITES:
//   tools/fetch_gltf_sample_renderer.sh   (builds toolchain/gltf-sample-renderer/dist/)
//   tools/fetch_assets.sh --conformance   (fetches the glTF conformance models)
//   npm ci    (in this directory -- installs the pinned Playwright)
//   npx playwright install chromium   (downloads the headless Chromium build)
//
// USAGE: node generate_reference.mjs [modelName ...]
//   With no arguments, regenerates every model in MODELS below. Pass one or
//   more model names to regenerate a subset (e.g. after a single asset's
//   own version bump).

import { chromium } from "playwright";
import { createServer } from "node:http";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(__dirname, "..", "..");

const RENDER_WIDTH = 512;
const RENDER_HEIGHT = 512;

// Mirrors tests/conformance/models.h's own MODEL REGISTRY table --
// intentionally duplicated rather than shared (this script and the C++
// harness are two independent language runtimes; see that header's own
// comment for the cross-reference note keeping the two lists in sync).
// `env: true` models are lit by the shared gate_test_env.hdr fixture
// (every model here -- IBL-only content, no KHR_lights_punctual, matching
// harness.html's own "no punctual lights" methodology note).
const MODELS = [
  {
    name: "MetalRoughSpheres",
    gltf: "assets/fetched/MetalRoughSpheres/glTF/MetalRoughSpheres.gltf",
  },
  {
    name: "MetalRoughSpheresNoTextures",
    gltf: "assets/fetched/MetalRoughSpheresNoTextures/glTF/MetalRoughSpheresNoTextures.gltf",
  },
  {
    name: "EmissiveStrengthTest",
    gltf: "assets/fetched/EmissiveStrengthTest/glTF/EmissiveStrengthTest.gltf",
  },
  {
    name: "CompareEmissiveStrength",
    gltf: "assets/fetched/CompareEmissiveStrength/glTF/CompareEmissiveStrength.gltf",
  },
  {
    name: "TextureTransformTest",
    gltf: "assets/fetched/TextureTransformTest/glTF/TextureTransformTest.gltf",
  },
  // AlphaBlendModeTest, not TextureTransformMultiTest -- see
  // tools/fetch_assets.sh's own comment for why: TextureTransformMultiTest
  // requires KHR_materials_clearcoat, unsupported by RendererX until
  // Stage 3, so its own reference render's clearcoat rows would fail this
  // gate for a reason outside this ticket's own scope.
  {
    name: "AlphaBlendModeTest",
    gltf: "assets/fetched/AlphaBlendModeTest/glTF/AlphaBlendModeTest.gltf",
  },
  // EnvironmentTest: fetch-on-demand ONLY, never committed (proprietary
  // Adobe Stock license on the model content itself -- gate ruling T11,
  // matrix-p5t11-conformance-harness.md's own per-model license-
  // verification finding). This script CAN still generate a reference for
  // it (local use only -- proves the harness end-to-end on a real
  // multi-environment IBL asset); tests/conformance never commits its
  // output, and the ctest gate skips gracefully when it is absent (same
  // shape as DamagedHelmet's own "not fetched -> skip" convention).
  {
    name: "EnvironmentTest",
    gltf: "assets/fetched/EnvironmentTest/glTF/EnvironmentTest.gltf",
    optional: true,
  },
];

const ENV_HDR = "samples/08_gltf_viewer/environments/gate_test_env.hdr";
const OUTPUT_ROOT = "tests/conformance/references";

function contentTypeFor(filePath) {
  const ext = path.extname(filePath).toLowerCase();
  switch (ext) {
    case ".html":
      return "text/html; charset=utf-8";
    case ".js":
    case ".mjs":
      return "text/javascript; charset=utf-8";
    case ".json":
      return "application/json; charset=utf-8";
    case ".wasm":
      return "application/wasm";
    case ".png":
      return "image/png";
    case ".jpg":
    case ".jpeg":
      return "image/jpeg";
    case ".gltf":
      return "model/gltf+json";
    case ".bin":
      return "application/octet-stream";
    case ".hdr":
      return "application/octet-stream";
    default:
      return "application/octet-stream";
  }
}

// The built glTF-Sample-Renderer module resolves its own OWN worker/wasm
// sibling files (mikktspace_bg.wasm, libktx.wasm, draco_decoder.wasm, ...)
// relative to the HTML PAGE's own URL (document-base-relative), not
// relative to the ESM module's own import.meta.url -- reproduced directly
// this task (a 404 for "tools/gltf_conformance/libs/mikktspace_bg.wasm",
// requested from harness.html's own directory even though the actual file
// lives under toolchain/gltf-sample-renderer/dist/libs/). Rather than
// physically copying the whole dist/ tree next to harness.html (duplicate,
// staleness-prone), the static server below transparently redirects any
// request under tools/gltf_conformance/{libs,assets}/ to the real
// toolchain/gltf-sample-renderer/dist/{libs,assets}/ location.
// Order matters: "assets/images/" (the LUT PNGs' own pre-flattening
// source-relative path, still what the built module's own runtime code
// requests -- e.g. "assets/images/lut_sheen_E.png") must be tried BEFORE
// the plain "assets/" rule, since rollup-plugin-copy's own `dest: "dist/
// assets"` flattens that `images/` segment away when it copies them
// (rollup.config.js's own copy target list, pinned commit) -- the file
// physically lands at dist/assets/lut_sheen_E.png, not dist/assets/images/
// lut_sheen_E.png.
const RENDERER_DIST_REDIRECTS = ["libs", "assets/images", "assets"];

// A deliberately tiny static file server (repo root as document root) --
// Node's own built-in http/fs modules are already "a well-established
// library" for exactly this (CLAUDE.md's own "ready-made over hand-rolled"
// rule is about NOT reimplementing parsers/allocators/protocol handling;
// a ~25-line directory-relative static GET handler is not that class of
// problem, and pulling in a whole extra npm dependency for it would not
// be a meaningfully more "ready-made" solution). Serves ONLY what this
// script itself requests (localhost, ephemeral port, torn down at the end
// of main()) -- never a general-purpose or long-lived server.
function startStaticServer(documentRoot) {
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, "http://localhost");
      let relPath = decodeURIComponent(url.pathname).replace(/^\/+/, "");
      for (const sub of RENDERER_DIST_REDIRECTS) {
        const prefix = `tools/gltf_conformance/${sub}/`;
        if (relPath.startsWith(prefix)) {
          // "assets/images" redirects into the SAME dist/assets/ directory
          // "assets" does (see RENDERER_DIST_REDIRECTS' own comment) --
          // only the "libs"/"assets" leaf segment (not "assets/images")
          // is a real directory name under dist/.
          const destSub = sub === "assets/images" ? "assets" : sub;
          relPath = `toolchain/gltf-sample-renderer/dist/${destSub}/${relPath.slice(prefix.length)}`;
          break;
        }
      }
      const filePath = path.join(documentRoot, relPath);
      if (!filePath.startsWith(documentRoot)) {
        res.writeHead(403);
        res.end("forbidden");
        return;
      }
      const data = await readFile(filePath);
      res.writeHead(200, { "Content-Type": contentTypeFor(filePath) });
      res.end(data);
    } catch (err) {
      res.writeHead(404);
      res.end(`not found: ${err.message}`);
    }
  });
  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

async function main() {
  const requested = process.argv.slice(2);
  const models = requested.length > 0 ? MODELS.filter((m) => requested.includes(m.name)) : MODELS;
  if (models.length === 0) {
    console.error(`no matching model(s) in: ${requested.join(", ")}`);
    process.exit(1);
  }

  const rendererDist = path.join(REPO_ROOT, "toolchain/gltf-sample-renderer/dist/gltf-viewer.module.js");
  if (!existsSync(rendererDist)) {
    console.error(
      `${rendererDist} not found -- run tools/fetch_gltf_sample_renderer.sh first`
    );
    process.exit(1);
  }

  const server = await startStaticServer(REPO_ROOT);
  const port = server.address().port;
  console.log(`[generate_reference] static server on http://127.0.0.1:${port} (document root: ${REPO_ROOT})`);

  const browser = await chromium.launch();
  try {
    for (const model of models) {
      const gltfAbsPath = path.join(REPO_ROOT, model.gltf);
      if (!existsSync(gltfAbsPath)) {
        if (model.optional) {
          console.log(`[generate_reference] ${model.name}: not fetched locally (optional) -- skipping`);
          continue;
        }
        console.error(`[generate_reference] ${model.name}: ${model.gltf} not found -- run tools/fetch_assets.sh first`);
        process.exitCode = 1;
        continue;
      }

      // A FRESH page (and its own WebGL2 context) per model -- reusing one
      // page/GltfView across every model in sequence was observed this task
      // to eventually wedge the headless Chromium GPU process indefinitely
      // (reproduced directly: the 6th sequential capture on a shared page
      // hung >15 minutes with 100% CPU in the gpu-process, never observed
      // when each model instead got its own page). Not investigated further
      // (a SwiftShader/software-WebGL resource-accumulation issue across
      // many createState()/loadGltf() calls on one context is the likely
      // cause) since a fresh page per model is both the robust fix AND
      // already this script's own natural unit of isolation (one glTF
      // conformance model's own capture has no reason to share GPU state
      // with another's).
      const page = await browser.newPage({ viewport: { width: RENDER_WIDTH, height: RENDER_HEIGHT } });
      page.on("console", (msg) => console.log(`[page:${msg.type()}] ${msg.text()}`));
      page.on("pageerror", (err) => console.error(`[page:error] ${err}`));
      page.on("requestfailed", (req) => console.error(`[page:requestfailed] ${req.url()} -- ${req.failure()?.errorText}`));
      page.on("response", (res) => {
        if (!res.ok()) {
          console.error(`[page:response] ${res.status()} ${res.url()}`);
        }
      });

      await page.goto(`http://127.0.0.1:${port}/tools/gltf_conformance/harness.html`, {
        waitUntil: "load",
      });
      await page.waitForFunction("window.rxHarnessReady === true", { timeout: 15000 });

      const modelUrl = `http://127.0.0.1:${port}/${model.gltf}`;
      const envUrl = `http://127.0.0.1:${port}/${ENV_HDR}`;
      console.log(`[generate_reference] ${model.name}: capturing (model=${modelUrl} env=${envUrl})`);

      const result = await page.evaluate(
        async ({ modelUrl, envUrl, width, height }) => window.rxCapture(modelUrl, envUrl, width, height),
        { modelUrl, envUrl, width: RENDER_WIDTH, height: RENDER_HEIGHT }
      );

      const base64 = result.dataUrl.replace(/^data:image\/png;base64,/, "");
      const pngBytes = Buffer.from(base64, "base64");

      const outDir = path.join(REPO_ROOT, OUTPUT_ROOT, model.name);
      await mkdir(outDir, { recursive: true });
      await writeFile(path.join(outDir, "reference.png"), pngBytes);

      const provenance = {
        model: model.name,
        sourceGltf: model.gltf,
        generator: "tools/gltf_conformance/generate_reference.mjs",
        rendererRepo: "https://github.com/KhronosGroup/glTF-Sample-Renderer",
        rendererCommit: "863b981fb755359063e370ff7b6e956bda0716e2",
        rendererLicense: "Apache-2.0",
        generatedAt: new Date().toISOString(),
        renderWidth: RENDER_WIDTH,
        renderHeight: RENDER_HEIGHT,
        environment: {
          path: ENV_HDR,
          intensity: 1.0,
          rotationDegrees: 0.0,
        },
        renderingParameters: {
          toneMap: "None (Linear mapping, clamped at 1.0)",
          exposure: 1.0,
          useIBL: true,
          usePunctual: true,
          renderEnvironmentMap: false,
          clearColor: [0.22, 0.25, 0.29, 1.0],
        },
        camera: result.camera,
        sceneExtents: result.sceneExtents,
      };
      await writeFile(path.join(outDir, "provenance.json"), `${JSON.stringify(provenance, null, 2)}\n`);
      console.log(`[generate_reference] ${model.name}: wrote ${outDir}/reference.png + provenance.json`);
      await page.close();
    }
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
