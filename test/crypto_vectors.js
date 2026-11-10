// Test cases for Chirp's hand-written crypto, made with Node's (OpenSSL's):
// random inputs of awkward sizes, and what OpenSSL makes of them. One case a
// line, fields in hex ("-" for empty), to test/out/crypto_vectors.txt.
// XChaCha20 isn't in OpenSSL: it's HChaCha20 (below, checked against the
// draft's vector by the C test too) and then OpenSSL's ChaCha20-Poly1305.
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

// `node crypto_vectors.js verify <file>`: OpenSSL checks the signatures Chirp
// made (lines "pub msg sig", hex), the other way round from the cases below
if (process.argv[2] === "verify") {
    let bad = 0, n = 0;
    for (const line of fs.readFileSync(process.argv[3], "utf8").split("\n")) {
        if (!line.trim()) continue;
        const [pub, msg, sig] = line.trim().split(" ").map((h) => Buffer.from(h === "-" ? "" : h, "hex"));
        const key = crypto.createPublicKey({ key: { kty: "EC", crv: "P-256", x: b64u(pub.subarray(1, 33)), y: b64u(pub.subarray(33)) }, format: "jwk" });
        n++;
        if (!crypto.verify("sha256", msg, { key, dsaEncoding: "der" }, sig)) bad++, console.log("OpenSSL rejects:", line.trim());
    }
    console.log(`OpenSSL checked ${n} of Chirp's signatures: ${bad ? bad + " BAD" : "all good"}`);
    process.exit(bad ? 1 : 0);
}

function b64u(b) { return b.toString("base64url"); }
function rawPub(keyObject) {
    const j = keyObject.export({ format: "jwk" });
    return Buffer.concat([Buffer.from([4]), Buffer.from(j.x, "base64url"), Buffer.from(j.y, "base64url")]);
}
function rawPriv(keyObject) { return Buffer.from(keyObject.export({ format: "jwk" }).d, "base64url"); }

const out = [];
const hex = (b) => (b.length ? b.toString("hex") : "-");
const rnd = (n) => crypto.randomBytes(n);
const sizes = [0, 1, 15, 16, 17, 31, 32, 33, 55, 56, 63, 64, 65, 100, 127, 128, 129, 255, 256, 1000, 3000];

for (const n of sizes) {
    const m = rnd(n);
    out.push(["sha256", hex(m), crypto.createHash("sha256").update(m).digest("hex")].join(" "));
    const key = rnd([0, 16, 32, 64, 65, 100][n % 6]);
    out.push(["hmac", hex(key), hex(m), crypto.createHmac("sha256", key).update(m).digest("hex")].join(" "));
    const salt = rnd(n % 40), info = rnd(n % 30), len = 1 + (n * 7) % 100;
    const okm = Buffer.from(crypto.hkdfSync("sha256", m, salt, info, len));
    out.push(["hkdf", hex(salt), hex(m), hex(info), len, hex(okm)].join(" "));
    for (const bits of [128, 256]) {
        const k = rnd(bits / 8), iv = rnd(12), aad = rnd(n % 37);
        const c = crypto.createCipheriv(`aes-${bits}-gcm`, k, iv);
        c.setAAD(aad);
        const ct = Buffer.concat([c.update(m), c.final()]);
        out.push(["gcm", hex(k), hex(iv), hex(aad), hex(m), hex(ct), hex(c.getAuthTag())].join(" "));
    }
    {
        const k = rnd(32), nonce = rnd(12), aad = rnd(n % 29);
        const c = crypto.createCipheriv("chacha20-poly1305", k, nonce, { authTagLength: 16 });
        c.setAAD(aad, { plaintextLength: m.length });
        const ct = Buffer.concat([c.update(m), c.final()]);
        out.push(["chachapoly", hex(k), hex(nonce), hex(aad), hex(m), hex(ct), hex(c.getAuthTag())].join(" "));
    }
    {
        const k = rnd(32), nonce = rnd(24), aad = rnd(n % 13);
        const sub = hchacha20(k, nonce.subarray(0, 16));
        const n12 = Buffer.concat([Buffer.alloc(4), nonce.subarray(16)]);
        const c = crypto.createCipheriv("chacha20-poly1305", sub, n12, { authTagLength: 16 });
        c.setAAD(aad, { plaintextLength: m.length });
        const ct = Buffer.concat([c.update(m), c.final()]);
        out.push(["xchachapoly", hex(k), hex(nonce), hex(aad), hex(m), hex(ct), hex(c.getAuthTag())].join(" "));
    }
}

// P-256: the public key of a private one, ECDH both ways, and OpenSSL's signatures
for (let i = 0; i < 40; i++) {
    const a = crypto.generateKeyPairSync("ec", { namedCurve: "prime256v1" });
    const b = crypto.generateKeyPairSync("ec", { namedCurve: "prime256v1" });
    out.push(["p256pub", hex(rawPriv(a.privateKey)), hex(rawPub(a.publicKey))].join(" "));
    const secret = crypto.diffieHellman({ privateKey: a.privateKey, publicKey: b.publicKey });
    out.push(["ecdh", hex(rawPriv(a.privateKey)), hex(rawPub(b.publicKey)), hex(secret)].join(" "));
    const msg = rnd(i * 13 % 200);
    const sig = crypto.sign("sha256", msg, { key: a.privateKey, dsaEncoding: "der" });
    out.push(["ecdsa", hex(rawPub(a.publicKey)), hex(msg), hex(sig)].join(" "));
}

// DAVE frames, sealed the way libdave does (its frame_processors.cpp, encryptor.cpp and mlspp's
// HashRatchet), with OpenSSL's AES-128-GCM: base secret -> per-generation key -> frame
function hkdfExpand(prk, info, len) {
    let t = Buffer.alloc(0), out = Buffer.alloc(0);
    for (let i = 1; out.length < len; i++) {
        t = crypto.createHmac("sha256", prk).update(Buffer.concat([t, info, Buffer.from([i])])).digest();
        out = Buffer.concat([out, t]);
    }
    return out.subarray(0, len);
}
function varint(n) { return n < 64 ? Buffer.from([n]) : Buffer.from([0x40 | (n >> 8), n & 0xff]); }
function deriveTreeSecret(secret, label, gen, len) {
    const l = Buffer.from("MLS 1.0 " + label), g = Buffer.alloc(4);
    g.writeUInt32BE(gen);
    const info = Buffer.concat([Buffer.from([len >> 8, len & 0xff]), varint(l.length), l, varint(4), g]);
    return hkdfExpand(secret, info, len);
}
function daveKey(base, gen) {
    let secret = base;
    for (let i = 0; i < gen; i++) secret = deriveTreeSecret(secret, "secret", i, 32);
    return deriveTreeSecret(secret, "key", gen, 16);
}
function leb128(n) {
    const b = [];
    do { let x = n & 0x7f; n = Math.floor(n / 128); b.push(n ? x | 0x80 : x); } while (n);
    return Buffer.from(b);
}
for (const counter of [1, 2, 127, 128, 300, 70000, (1 << 24) + 5, (2 << 24) + 1]) {
    const base = rnd(16), opus = rnd(20 + (counter % 140));
    const key = daveKey(base, counter >>> 24);
    const iv = Buffer.alloc(12);
    iv.writeUInt32LE(counter >>> 0, 8);
    const c = crypto.createCipheriv("aes-128-gcm", key, iv, { authTagLength: 8 });
    const ct = Buffer.concat([c.update(opus), c.final()]);
    const nonce = leb128(counter);
    const supp = 8 + nonce.length + 1 + 2;
    const frame = Buffer.concat([ct, c.getAuthTag(), nonce, Buffer.from([supp, 0xfa, 0xfa])]);
    out.push(["dave", hex(base), counter, hex(opus), hex(frame)].join(" "));
}

// Voice packets as Discord's servers send them (discord.js's VoiceReceiver reads them so):
// header (+ CSRCs, + extension preamble) as AAD; extension body and payload (+ padding)
// encrypted; then the tag and the 4-byte counter
for (let i = 0; i < 24; i++) {
    const mode = i % 2 ? "xchacha" : "aes", key = rnd(32), counter = 1 + i * 1000;
    const cc = i % 3, ext = (i >> 1) % 2 === 1, padTo = (i >> 2) % 2 === 1 ? 1 + (i % 7) : 0;
    const extWords = ext ? 1 + (i % 3) : 0;
    const opus = rnd(30 + i * 5);
    const seq = (i * 977) & 0xffff, ts = (i * 960 * 7) >>> 0, ssrc = 0x10000 + i;
    const h = Buffer.alloc(12 + 4 * cc + (ext ? 4 : 0));
    h[0] = 0x80 | (padTo ? 0x20 : 0) | (ext ? 0x10 : 0) | cc;
    h[1] = 0x78;
    h.writeUInt16BE(seq, 2), h.writeUInt32BE(ts, 4), h.writeUInt32BE(ssrc, 8);
    for (let k = 0; k < cc; k++) h.writeUInt32BE(0xc0000000 + k, 12 + 4 * k);
    if (ext) h.writeUInt16BE(0xbede, 12 + 4 * cc), h.writeUInt16BE(extWords, 14 + 4 * cc);
    const pad = padTo ? Buffer.concat([Buffer.alloc(padTo - 1), Buffer.from([padTo])]) : Buffer.alloc(0);
    const pt = Buffer.concat([rnd(4 * extWords), opus, pad]);
    const n4 = Buffer.alloc(4);
    n4.writeUInt32BE(counter);
    let ct, tag;
    if (mode === "aes") {
        const c = crypto.createCipheriv("aes-256-gcm", key, Buffer.concat([n4, Buffer.alloc(8)]));
        c.setAAD(h);
        ct = Buffer.concat([c.update(pt), c.final()]);
        tag = c.getAuthTag();
    } else {
        const nonce = Buffer.concat([n4, Buffer.alloc(20)]);
        const sub = hchacha20(key, nonce.subarray(0, 16));
        const c = crypto.createCipheriv("chacha20-poly1305", sub, Buffer.concat([Buffer.alloc(4), nonce.subarray(16)]), { authTagLength: 16 });
        c.setAAD(h, { plaintextLength: pt.length });
        ct = Buffer.concat([c.update(pt), c.final()]);
        tag = c.getAuthTag();
    }
    const pkt = Buffer.concat([h, ct, tag, n4]);
    out.push(["rtp", mode, hex(key), seq, ts, ssrc, hex(opus), hex(pkt), cc === 0 && !ext && !padTo ? counter : 0].join(" "));
}

function hchacha20(key, nonce16) {
    const s = new Uint32Array(16);
    s.set([0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]);
    for (let i = 0; i < 8; i++) s[4 + i] = key.readUInt32LE(i * 4);
    for (let i = 0; i < 4; i++) s[12 + i] = nonce16.readUInt32LE(i * 4);
    const rol = (v, n) => ((v << n) | (v >>> (32 - n))) >>> 0;
    const qr = (a, b, c, d) => {
        s[a] += s[b]; s[d] = rol(s[d] ^ s[a], 16);
        s[c] += s[d]; s[b] = rol(s[b] ^ s[c], 12);
        s[a] += s[b]; s[d] = rol(s[d] ^ s[a], 8);
        s[c] += s[d]; s[b] = rol(s[b] ^ s[c], 7);
    };
    for (let i = 0; i < 10; i++) {
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
    }
    const o = Buffer.alloc(32);
    for (let i = 0; i < 4; i++) o.writeUInt32LE(s[i], i * 4), o.writeUInt32LE(s[12 + i], 16 + i * 4);
    return o;
}

const dir = path.join(__dirname, "out");
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, "crypto_vectors.txt"), out.join("\n") + "\n");
console.log(`${out.length} cases`);
