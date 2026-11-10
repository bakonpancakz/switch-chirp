// What the voice gateway can check of Chirp's op 28 with no secrets, from the
// messages in fromlog.json (node fromlog.mjs <chirp.log> first): Chirp's epoch-0
// group rebuilt from public data (its key package's leaf, op 25's external
// sender, the channel id), then the commit's signature and proposal reference.
import { readFileSync } from "node:fs";
import { getCiphersuiteImpl, getCiphersuiteFromName, decodeMlsMessage, encodeExternalSender, decodeExternalSender } from "ts-mls";
import { treeHashRoot } from "./node_modules/ts-mls/dist/src/treeHash.js";
import { encodeGroupContext } from "./node_modules/ts-mls/dist/src/groupContext.js";
import { verifyWithLabel } from "./node_modules/ts-mls/dist/src/crypto/signature.js";

const cs = await getCiphersuiteImpl(getCiphersuiteFromName("MLS_128_DHKEMP256_AES128GCM_SHA256_P256"));
const d = JSON.parse(readFileSync(new URL("./fromlog.json", import.meta.url), "utf8"));
const b = (k) => new Uint8Array(Buffer.from(d[k], "hex"));
const varint = (n) => (n < 64 ? Uint8Array.of(n) : Uint8Array.of(0x40 | (n >> 8), n & 0xff));
const cat = (...a) => new Uint8Array(Buffer.concat(a.map((x) => Buffer.from(x))));
const hex = (u) => Buffer.from(u).toString("hex");

const kp = decodeMlsMessage(b("op26"), 0)[0].keyPackage;
const commitBytes = b("op28");
const [msg, commitLen] = decodeMlsMessage(commitBytes, 0);
const pm = msg.publicMessage;

// epoch 0 as the gateway must see it: one leaf (the key package's), no transcript, the external sender
const tree = [{ nodeType: "leaf", leaf: kp.leafNode }];
const treeHash = await treeHashRoot(tree, cs.hash);
const op25 = b("op25");
const gc = {
    version: "mls10", cipherSuite: "MLS_128_DHKEMP256_AES128GCM_SHA256_P256", groupId: pm.content.groupId, epoch: 0n,
    treeHash, confirmedTranscriptHash: new Uint8Array(), extensions: [{ extensionType: "external_senders", extensionData: cat(varint(op25.length), op25) }],
};
const gcBytes = encodeGroupContext(gc);
console.log("epoch-0 tree hash", hex(treeHash));

// FramedContentTBS: version, wire format, the FramedContent (as sent), the group context
const sig = pm.auth.signature;
const tail = (sig.length < 64 ? 1 : 2) + sig.length + 33 + 33;  // signature<V>, confirmation_tag<V>, membership_tag<V>
const content = commitBytes.slice(4, commitLen - tail);
const tbs = cat(Uint8Array.of(0, 1, 0, 1), content, gcBytes);
const ok = await verifyWithLabel(kp.leafNode.signaturePublicKey, "FramedContentTBS", tbs, sig, cs.signature);
console.log("commit signature, against the group the gateway can rebuild:", ok ? "VALID" : "INVALID");

// the proposal reference: RefHash("MLS 1.0 Proposal Reference", AuthenticatedContent {wire_format, content, auth})
const p = b("op27");
const vstart = p[1] >> 6 === 0 ? 2 : 3;
const [pmsg, plen] = decodeMlsMessage(p, vstart);
const ac = cat(Uint8Array.of(0, 1), p.slice(vstart + 4, vstart + plen));  // (the PublicMessage without its version and wire format; no membership tag: external)
const label = new TextEncoder().encode("MLS 1.0 Proposal Reference");
const refInput = cat(varint(label.length), label, varint(ac.length).length === 1 && ac.length < 64 ? varint(ac.length) : Uint8Array.of(0x40 | (ac.length >> 8), ac.length & 0xff), ac);
const ref = await cs.hash.digest(refInput);
console.log("proposal ref we computed  ", hex(ref));
console.log("proposal ref in the commit", hex(pm.content.commit.proposals[0].reference));
console.log("external sender ok:", !!decodeExternalSender(op25, 0));
