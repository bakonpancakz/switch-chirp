// ts-mls 1.6.4 reads the external_senders extension's data as one ExternalSender;
// RFC 9420 12.1.8.1 (and mlspp, which libdave and Discord's clients use) has it a
// vector of them: `ExternalSender external_senders<V>`. DAVE's has one in it. This
// makes ts-mls skip the vector's length, so it reads the first sender of the list
// the way Discord sends it. Idempotent; run after npm install.
import { readFileSync, writeFileSync } from "node:fs";

const files = ["clientState.js", "externalProposal.js", "publicMessage.js"];
const skip = (d) => `(${d}[0] >> 6 === 0 ? 1 : ${d}[0] >> 6 === 1 ? 2 : 4)`;
for (const f of files) {
    const path = new URL(`./node_modules/ts-mls/dist/src/${f}`, import.meta.url);
    let s = readFileSync(path, "utf8");
    const before = s;
    s = s.replace(/decodeExternalSender\(([\w.]+\.extensionData), 0\)/g, (_, d) => `decodeExternalSender(${d}, ${skip(d)})`);
    if (s !== before) writeFileSync(path, s);
    console.log(`${f}: ${s !== before ? "patched" : "already patched"}`);
}

// Its P-256 signature keys are compressed (33 bytes); mlspp's, the RFC's vectors' and
// Chirp's are uncompressed (65): its keys made uncompressed (it reads both)
{
    const path = new URL("./node_modules/ts-mls/dist/src/crypto/implementation/default/makeNobleSignatureImpl.js", import.meta.url);
    let s = readFileSync(path, "utf8");
    const before = s;
    s = s.replace("publicKey: p256.getPublicKey(signKey) }", "publicKey: p256.getPublicKey(signKey, false) }");
    if (s !== before) writeFileSync(path, s);
    console.log(`makeNobleSignatureImpl.js: ${s !== before ? "patched" : "already patched"}`);
}
