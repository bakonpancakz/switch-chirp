// The MLS working group's test vectors (test/vectors/mls, from
// github.com/mlswg/mls-implementations), flattened for the C tests: for each
// file, the cases for ciphersuite 2 (DAVE's), one "=== <file> <n>" line then
// "path value" lines (objects' keys joined with '.', arrays as their index;
// arrays of numbers on one line, null as -1). To test/out/mls_vectors.txt.
const fs = require("fs");
const path = require("path");

const files = ["crypto-basics", "tree-math", "key-schedule", "tree-validation", "passive-client-welcome", "passive-client-handling-commit"];
const out = [];

function flat(prefix, v) {
    if (v === null || typeof v !== "object") {
        out.push(`${prefix} ${v === null ? -1 : v === "" ? "-" : v}`);
    } else if (Array.isArray(v) && v.every((x) => x === null || typeof x === "number")) {
        out.push(`${prefix} ${v.map((x) => (x === null ? -1 : x)).join(" ")}`);
    } else {
        for (const [k, x] of Object.entries(v)) flat(prefix ? `${prefix}.${k}` : k, x);
    }
}

for (const f of files) {
    const cases = JSON.parse(fs.readFileSync(path.join(__dirname, "vectors", "mls", f + ".json"), "utf8"));
    cases.forEach((c, i) => {
        if (c.cipher_suite !== undefined && c.cipher_suite !== 2) return;
        out.push(`=== ${f} ${i}`);
        flat("", c);
    });
}

fs.mkdirSync(path.join(__dirname, "out"), { recursive: true });
fs.writeFileSync(path.join(__dirname, "out", "mls_vectors.txt"), out.join("\n") + "\n");
console.log(`${out.filter((l) => l.startsWith("===")).length} MLS cases`);
