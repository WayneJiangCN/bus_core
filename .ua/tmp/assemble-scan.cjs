const fs = require("fs");
const path = require("path");

const projectRoot = process.argv[2];
const mode = process.argv[3];
const uaDir = path.join(projectRoot, ".ua");
const scanPath = path.join(uaDir, "tmp", "ua-scan-files.json");
const importInputPath = path.join(uaDir, "tmp", "ua-import-map-input.json");
const importOutputPath = path.join(uaDir, "tmp", "ua-import-map-output.json");
const finalPath = path.join(uaDir, "intermediate", "scan-result.json");
const scan = JSON.parse(fs.readFileSync(scanPath, "utf8"));

if (!scan.scriptCompleted || scan.totalFiles !== scan.files.length) {
  throw new Error("Deterministic scan output is incomplete or inconsistent");
}

if (mode === "prepare-imports") {
  fs.writeFileSync(
    importInputPath,
    JSON.stringify({ projectRoot, files: scan.files }, null, 2),
    "utf8",
  );
  process.exit(0);
}

if (mode !== "assemble") {
  throw new Error(`Unknown mode: ${mode}`);
}

const imports = JSON.parse(fs.readFileSync(importOutputPath, "utf8"));
if (!imports.scriptCompleted || !imports.importMap) {
  throw new Error("Import-map output is incomplete");
}

for (const file of scan.files) {
  if (!Object.prototype.hasOwnProperty.call(imports.importMap, file.path)) {
    throw new Error(`Import map is missing file: ${file.path}`);
  }
}

const result = {
  name: path.basename(projectRoot),
  description: "暂无可用的项目描述。",
  languages: Object.keys(scan.stats.byLanguage).sort(),
  frameworks: [],
  files: scan.files,
  totalFiles: scan.totalFiles,
  filteredByIgnore: scan.filteredByIgnore,
  estimatedComplexity: scan.estimatedComplexity,
  importMap: imports.importMap,
};

fs.writeFileSync(finalPath, JSON.stringify(result, null, 2), "utf8");
