// The DAVE messages a chirp.log dumped ("DAVE hex opNN+offset ..."), decoded by
// ts-mls: Chirp's key package next to the one Discord's client made (in op 27)
import { readFileSync, writeFileSync } from "node:fs";
import { decodeMlsMessage, decodeExternalSender } from "ts-mls";
import { decodeKeyPackage } from "./node_modules/ts-mls/dist/src/keyPackage.js";

const log = readFileSync(process.argv[2], "utf8").split(/\r?\n/);
// the last dump of each opcode
const dumps = {};
let cur = null;
for (const l of log) {
    let m = l.match(/DAVE hex (op\d+): (\d+) bytes/);
    if (m) {
        cur = m[1];
        dumps[cur] = "";
        continue;
    }
    m = l.match(/DAVE hex (op\d+)\+(\d+) ([0-9a-f]+)/);
    if (m && m[1] === cur) dumps[cur] += m[3];
}
const bytes = Object.fromEntries(Object.entries(dumps).map(([k, v]) => [k, new Uint8Array(Buffer.from(v, "hex"))]));
writeFileSync(new URL("./fromlog.json", import.meta.url), JSON.stringify(dumps, null, 1));
for (const [k, v] of Object.entries(bytes)) console.log(`${k}: ${v.length} bytes`);

const show = (o) =>
    JSON.stringify(o, (k, v) => (v instanceof Uint8Array ? `<${v.length}: ${Buffer.from(v).toString("hex").slice(0, 64)}>` : typeof v === "bigint" ? v.toString() : v), 1);

// (op 26 is the bare KeyPackage now; logs from before had an MLSMessage around it)
const wrapped = bytes.op26[2] === 0 && bytes.op26[3] === 5;
const ours = wrapped ? decodeMlsMessage(bytes.op26, 0)?.[0].keyPackage : decodeKeyPackage(bytes.op26, 0)?.[0];
console.log(`\n=== Chirp's key package (op 26, ${wrapped ? "in an MLSMessage" : "bare"}) ===`);
console.log(ours ? show(ours) : "ts-mls couldn't read it");

// op 27: [optype u8][MLSMessage proposals<V>]
const p = bytes.op27;
let at = 1;
const lenByte = p[at] >> 6;
const vlen = lenByte === 0 ? p[at] : lenByte === 1 ? ((p[at] & 0x3f) << 8) | p[at + 1] : ((p[at] & 0x3f) << 24) | (p[at + 1] << 16) | (p[at + 2] << 8) | p[at + 3];
at += lenByte === 0 ? 1 : lenByte === 1 ? 2 : 4;
console.log(`\n=== op 27: type ${p[0]}, ${vlen} bytes of proposals ===`);
const end = at + vlen;
while (at < end) {
    const m = decodeMlsMessage(p, at);
    if (!m) {
        console.log("ts-mls couldn't read a proposal at", at);
        break;
    }
    const pm = m[0].publicMessage;
    console.log(show({ groupId: pm.content.groupId, epoch: pm.content.epoch, sender: pm.content.sender, proposalType: pm.content.proposal.proposalType }));
    if (pm.content.proposal.proposalType === "add") {
        console.log("--- the key package in it (Discord's client) ---");
        console.log(show(pm.content.proposal.add.keyPackage));
    }
    at += m[1];
}
if (bytes.op25) {
    const e = decodeExternalSender(bytes.op25, 0);
    console.log("\n=== op 25 ===\n" + (e ? show(e[0]) : "ts-mls couldn't read it"));
}
