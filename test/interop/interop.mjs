// Chirp's MLS (chirp_cli.exe: the sysmodule's mlsgroup.cpp) in groups with
// ts-mls, an MLS someone else wrote, the way DAVE runs them: the voice gateway
// is an external sender that proposes adds, the members commit them, new
// members come in by Welcome (with the tree inline), all PublicMessages.
//
//   A. a ts-mls member commits Chirp's add: ts-mls takes Chirp's KeyPackage, Chirp takes its Welcome
//   B. Chirp's own group, Chirp commits a ts-mls add: ts-mls takes Chirp's Welcome
//      (what happens when the bot is first in a call, and what got no answer on the Switch)
//   C. Chirp and ts-mls both in a group, Chirp commits a third's add: ts-mls takes Chirp's
//      commit as a member does (path, parent hashes, tags) and the third Chirp's Welcome
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import {
    ciphersuites, getCiphersuiteImpl, generateKeyPackage, defaultCapabilities, createGroup, proposeExternal,
    processMessage, createCommit, joinGroup, acceptAll, emptyPskIndex, encodeMlsMessage, decodeMlsMessage,
    encodeExternalSender, mlsExporter, getCiphersuiteFromName,
} from "ts-mls";
import { encodeWelcome, decodeWelcome } from "./node_modules/ts-mls/dist/src/welcome.js";

const SUITE = "MLS_128_DHKEMP256_AES128GCM_SHA256_P256";
const cs = await getCiphersuiteImpl(getCiphersuiteFromName(SUITE));

// --- Chirp, a line at a time ---
const cli = spawn(process.argv[2] || "./out/chirp_cli.exe", [], { stdio: ["pipe", "pipe", "inherit"] });
const lines = createInterface({ input: cli.stdout });
const waiting = [];
lines.on("line", (l) => waiting.shift()(l));
function chirp(cmd) {
    return new Promise((resolve) => {
        waiting.push(resolve);
        cli.stdin.write(cmd + "\n");
    });
}
const hex = (b) => (b && b.length ? Buffer.from(b).toString("hex") : "-");
const unhex = (s) => (s === "-" ? new Uint8Array() : new Uint8Array(Buffer.from(s, "hex")));

let passed = 0, failed = 0;
function check(what, ok, detail = "") {
    if (ok) passed++;
    else failed++;
    console.log(`  ${ok ? "ok  " : "FAIL"} ${what}${detail ? ": " + detail : ""}`);
}
async function attempt(what, f) {
    try {
        const r = await f();
        check(what, true);
        return r;
    } catch (e) {
        check(what, false, e && e.message ? e.message : String(e));
        return undefined;
    }
}

// --- DAVE's shapes ---
const be64 = (v) => {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setBigUint64(0, BigInt(v));
    return b;
};
function varint(n) {
    if (n < 64) return Uint8Array.of(n);
    if (n < 16384) return Uint8Array.of(0x40 | (n >> 8), n & 0xff);
    return Uint8Array.of(0x80 | (n >>> 24), (n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff);
}
const cat = (...a) => new Uint8Array(Buffer.concat(a.map((x) => Buffer.from(x))));
const basic = (id) => ({ credentialType: "basic", identity: be64(id) });
const LIFETIME = { notBefore: 0n, notAfter: 0xffffffffffffffffn };

const gateway = await cs.signature.keygen();
const gatewaySender = encodeExternalSender({ signaturePublicKey: gateway.publicKey, credential: basic(0n) });
// external_senders: a vector of one ExternalSender (op 25's body is that ExternalSender)
const extData = cat(varint(gatewaySender.length), gatewaySender);
const extTs = { extensionType: "external_senders", extensionData: extData };
// Chirp's create() takes the extension as dave.cpp writes it: type, then data<V>
const extChirp = cat(Uint8Array.of(0, 5), varint(extData.length), extData);

async function tsMember(id) {
    const caps = defaultCapabilities();
    return generateKeyPackage(basic(id), caps, LIFETIME, [], cs);
}
// what the gateway sends in op 27: a PublicMessage proposal from external sender 0
async function gatewayAdd(groupContext, keyPackage) {
    const m = await proposeExternal({ groupContext }, { proposalType: "add", add: { keyPackage } }, gateway.publicKey, gateway.signKey, cs);
    return m;
}
async function exporterMatches(what, state, who) {
    const label = "Discord Secure Frames v0";
    const ctx = be64(who);
    const ts = await mlsExporter(state.keySchedule.exporterSecret, label, ctx, 16, cs);
    const r = await chirp(`EXPORT ${hex(Buffer.from(label))} ${hex(ctx)} 16`);
    check(what, r === "OK " + hex(ts), r);
}

const CHANNEL = 1234567890123456789n, BOT = 1111111111111111111n, YOU = 2222222222222222222n, FRIEND = 3333333333333333333n;

// --- A: a ts-mls member adds Chirp ---
console.log("A. ts-mls commits Chirp's add; Chirp joins by Welcome");
{
    const kpLine = await chirp(`KP ${BOT}`);
    const kpMsg = decodeMlsMessage(unhex(kpLine.split(" ")[1]), 0);
    check("ts-mls reads Chirp's KeyPackage", kpMsg && kpMsg[0].wireformat === "mls_key_package");
    const you = await tsMember(YOU);
    let state = await createGroup(be64(CHANNEL), you.publicPackage, you.privatePackage, [extTs], cs);
    const prop = await attempt("the gateway proposes adding Chirp", () => gatewayAdd(state.groupContext, kpMsg[0].keyPackage));
    const res = await attempt("ts-mls takes the proposal (validates Chirp's KeyPackage)", () =>
        processMessage(prop, state, emptyPskIndex, acceptAll, cs));
    if (res) state = res.newState;
    const commit = await attempt("ts-mls commits it", () =>
        createCommit({ state, cipherSuite: cs }, { wireAsPublicMessage: true, ratchetTreeExtension: true }));
    if (commit) {
        const r = await chirp(`JOIN ${hex(encodeWelcome(commit.welcome))}`);
        check("Chirp joins by ts-mls's Welcome", r === "OK", r);
        await exporterMatches("both have the same DAVE key for Chirp", commit.newState, BOT);
    }
}

// --- B: Chirp's own group, Chirp commits an add ---
console.log("B. Chirp commits an add in its own group; ts-mls joins by Chirp's Welcome");
let youB;
{
    await chirp(`KP ${BOT}`);
    check("Chirp creates its group", (await chirp(`CREATE ${hex(be64(CHANNEL))} ${hex(extChirp)}`)) === "OK");
    youB = await tsMember(YOU);
    const gc = { version: "mls10", cipherSuite: SUITE, groupId: be64(CHANNEL), epoch: 0n, treeHash: new Uint8Array(32),
        confirmedTranscriptHash: new Uint8Array(), extensions: [extTs] };
    const prop = await gatewayAdd(gc, youB.publicPackage);
    const r1 = await chirp(`HANDLE ${hex(encodeMlsMessage(prop))}`);
    check("Chirp takes the gateway's proposal", r1 === "OK", r1);
    const c = (await chirp("COMMIT")).split(" ");
    check("Chirp commits", c[0] === "OK" && c[2] !== "-", c.join(" ").slice(0, 80));
    if (c[0] === "OK") {
        const commitMsg = decodeMlsMessage(unhex(c[1]), 0);
        check("ts-mls reads Chirp's commit", commitMsg && commitMsg[0].wireformat === "mls_public_message");
        const w = decodeWelcome(unhex(c[2]), 0);
        check("ts-mls reads Chirp's Welcome", !!w);
        // (Chirp applies its own commit once the gateway announces it)
        const r2 = await chirp(`HANDLE ${c[1]}`);
        check("Chirp applies its own commit", r2 === "OK", r2);
        const joined = w && (await attempt("ts-mls joins by Chirp's Welcome (tree, signatures, tags)", () =>
            joinGroup(w[0], youB.publicPackage, youB.privatePackage, emptyPskIndex, cs)));
        if (joined) {
            await exporterMatches("both have the same DAVE key for Chirp", joined, BOT);
            youB.state = joined;
        }
    }
}

// --- C: Chirp commits in a group with ts-mls in it ---
console.log("C. Chirp commits a third's add; ts-mls takes the commit as a member");
if (youB && youB.state) {
    let state = youB.state;
    const friend = await tsMember(FRIEND);
    const prop = await gatewayAdd(state.groupContext, friend.publicPackage);
    const r1 = await chirp(`HANDLE ${hex(encodeMlsMessage(prop))}`);
    check("Chirp takes the proposal", r1 === "OK", r1);
    const res = await attempt("ts-mls takes the proposal", () => processMessage(prop, state, emptyPskIndex, acceptAll, cs));
    if (res) state = res.newState;
    const c = (await chirp("COMMIT")).split(" ");
    check("Chirp commits", c[0] === "OK", c.join(" ").slice(0, 80));
    if (c[0] === "OK") {
        const m = decodeMlsMessage(unhex(c[1]), 0);
        const after = await attempt("ts-mls takes Chirp's commit (path, parent hashes, confirmation and membership tags)", () =>
            processMessage(m[0], state, emptyPskIndex, acceptAll, cs));
        const r2 = await chirp(`HANDLE ${c[1]}`);
        check("Chirp applies its own commit", r2 === "OK", r2);
        if (after) await exporterMatches("ts-mls and Chirp agree on the next epoch's key", after.newState, BOT);
        const w = decodeWelcome(unhex(c[2]), 0);
        const joined = w && (await attempt("a new ts-mls member joins by Chirp's Welcome", () =>
            joinGroup(w[0], friend.publicPackage, friend.privatePackage, emptyPskIndex, cs)));
        if (joined) await exporterMatches("the new member has the same key too", joined, BOT);
        // --- D: a remove, which needs a path ---
        console.log("D. Chirp commits the gateway's remove (with a path); both ts-mls members take it");
        if (after && joined) {
            let you = after.newState, fr = joined;
            const removeFriend = await proposeExternal({ groupContext: you.groupContext },
                { proposalType: "remove", remove: { removed: 2 } }, gateway.publicKey, gateway.signKey, cs);
            const r3 = await chirp(`HANDLE ${hex(encodeMlsMessage(removeFriend))}`);
            check("Chirp takes the remove", r3 === "OK", r3);
            const y = await attempt("ts-mls takes the remove", () => processMessage(removeFriend, you, emptyPskIndex, acceptAll, cs));
            if (y) you = y.newState;
            const d = (await chirp("COMMIT")).split(" ");
            check("Chirp commits it, with a path", d[0] === "OK" && decodeMlsMessage(unhex(d[1]), 0)[0].publicMessage.content.commit.path !== undefined,
                d.join(" ").slice(0, 80));
            if (d[0] === "OK") {
                const m2 = decodeMlsMessage(unhex(d[1]), 0)[0];
                const after2 = await attempt("ts-mls takes Chirp's commit with its path", () => processMessage(m2, you, emptyPskIndex, acceptAll, cs));
                const r4 = await chirp(`HANDLE ${d[1]}`);
                check("Chirp applies its own commit", r4 === "OK", r4);
                if (after2) await exporterMatches("ts-mls and Chirp agree after the path", after2.newState, BOT);
            }
        }
    }
} else {
    check("(C needs B's group)", false);
}

cli.stdin.end();
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
