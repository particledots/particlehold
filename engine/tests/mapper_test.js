// Runs the real particlehold Mapper script with a stand-in for Logic's Scripter, fed by the engine's real MIDI stream
// (from particlehold_ccdump). Also chains it with copies of the Bryn, Hillside, Ogdoad and Strange Drift Mappers
// (tests/fixtures), in every order (120), to show the five plugins can share a track. Adapted from Bryn's mapper test.
// Usage: PARTICLEHOLD_CCDUMP=<path> node mapper_test.js <particlehold-Mapper.js> <Bryn-Mapper.js> <Hillside-Mapper.js>
//        <Ogdoad-Mapper.js> <StrangeDrift-Mapper.js>

"use strict";
const fs = require("fs");
const vm = require("vm");
const { execFileSync } = require("child_process");

let checks = 0, failures = 0;
function expect(ok, what) { checks++; if (!ok) { failures++; console.log("FAIL: " + what); } }

const scriptText = fs.readFileSync(process.argv[2], "utf8");
const brynText = fs.readFileSync(process.argv[3], "utf8");
const hillsideText = fs.readFileSync(process.argv[4], "utf8");
const ogdoadText = fs.readFileSync(process.argv[5], "utf8");
const driftText = fs.readFileSync(process.argv[6], "utf8");
const dumpExe = process.env.PARTICLEHOLD_CCDUMP;

function readDump(seconds, channel, sendAs) {
    const lines = execFileSync(dumpExe, [String(seconds), String(channel), String(sendAs || 0)], { maxBuffer: 1 << 28 }).toString().trim().split("\n");
    const d = { events: [], notes: [], finals: [], pulsesOn: -1 };
    for (const line of lines) {
        const p = line.split(" ");
        if (p[0] === "final") d.finals[+p[1]] = +p[2];
        else if (p[0] === "pulses-on") d.pulsesOn = +p[1];
        else if (p[0] === "note") d.notes.push({ sample: +p[1], channel: +p[2], pitch: +p[3], velocity: +p[4] });
        else d.events.push({ sample: +p[0], channel: +p[1], cc: +p[2], value: +p[3] });
    }
    return d;
}
const CH = 13, MIDDLE = 8192 / 16383;
const dump = readDump(60, CH);
const events = dump.events, finals = dump.finals;
expect(events.length > 2000, "the engine produced a CC stream (" + events.length + " messages)");
const OUR_CC = [75, 76, 77, 78, 79];
expect(events.every(e => e.channel === CH && OUR_CC.includes(e.cc)), "the stream uses only CC 75-79 on channel 13");
expect(dump.notes.length === 0, "Send as CC (the default): no notes are sent");
expect(finals[0] === 8192 && finals[1] === 8192, "Signal and Noise end at 0 V (the middle, 8192) after the rest");
expect(dump.pulsesOn === 0, "the Clock is not left on at the end");

// The Clock in the real stream: 127 / 0 only, strictly alternating, ending at 0; every pulse at least 20 ms (960 samples).
const pulsesOf = n => events.filter(e => e.cc === n);
{
    const p = pulsesOf(79);
    expect(p.length >= 40 && p.every((e, i) => e.value === (i % 2 === 0 ? 127 : 0)), "Clock: CC 79 alternates 127 / 0 (" + p.length + " messages)");
    expect(p.length % 2 === 0, "Clock: ends at 0");
    let shortest = Infinity;
    for (let i = 1; i < p.length; i += 2) shortest = Math.min(shortest, p[i].sample - p[i - 1].sample);
    expect(shortest >= 960, "Clock: every pulse at least 20 ms (shortest " + shortest + " samples)");
}

// What the engine meant at each fine message (coarse * 128 + fine).
function decode(evs, coarseCc, fineCc) {
    let coarse = 0;
    const values = [];
    for (const e of evs) {
        if (e.cc === coarseCc) coarse = e.value;
        if (e.cc === fineCc) values.push((coarse * 128 + e.value) / 16383);
    }
    return values;
}
const truth = decode(events, 75, 76), noiseTruth = decode(events, 77, 78);
expect(truth.length > 200, "Signal moves (" + truth.length + " values)");
expect(noiseTruth.length > 3000, "Noise moves (" + noiseTruth.length + " values)");
expect(Math.max(...truth) > 0.9 && Math.min(...truth) < 0.1, "Signal covers the range");

// ---- A stand-in for Scripter ---------------------------------------------------------------------------------
function makeScripter(text, settings, callParameterChanged, passTo) {
    const out = { passed: [], targets: [] };
    class Event { send() { out.passed.push(this); if (passTo) passTo(this); } trace() {} }
    class ControlChange extends Event { constructor(o) { super(); Object.assign(this, { channel: 1, number: 0, value: 0 }, o); } }
    class NoteOn extends Event { constructor(o) { super(); Object.assign(this, { channel: 1, pitch: 60, velocity: 100 }, o); } }
    class NoteOff extends Event { constructor(o) { super(); Object.assign(this, { channel: 1, pitch: 60, velocity: 0 }, o); } }
    class PitchBend extends Event { constructor(o) { super(); Object.assign(this, { channel: 1, value: 0 }, o); } }
    class TargetEvent { constructor() { this.target = ""; this.value = 0; } send() { out.targets.push({ target: this.target, value: this.value }); } }
    const values = {};
    const sandbox = {
        Event, ControlChange, NoteOn, NoteOff, PitchBend, TargetEvent, Math,
        GetParameter: (name) => { if (!(name in values)) throw new Error("GetParameter unknown name " + name); return values[name]; },
        Trace: () => {},
    };
    vm.createContext(sandbox);
    vm.runInContext(text, sandbox);
    const params = vm.runInContext("PluginParameters", sandbox);
    params.forEach(p => { if (p.type !== "target") values[p.name] = p.defaultValue !== undefined ? p.defaultValue : 0; });
    for (const [name, v] of Object.entries(settings)) {
        if (!(name in values)) throw new Error("test uses unknown setting " + name);
        values[name] = v;
        if (callParameterChanged) sandbox.ParameterChanged(params.findIndex(p => p.name === name), v);
    }
    return { sandbox, params, out, values };
}
const near = (a, b) => Math.abs(a - b) < 1e-9;
const lane = (s, n) => s.out.targets.filter(t => t.target === "Lane " + n + " target").map(t => t.value);
const dedupe = a => a.filter((v, i) => i === 0 || v !== a[i - 1]);
const eq = (a, b) => a.length === b.length && a.every((v, i) => near(v, b[i]));
const gateWant = (p, min, max) => dedupe(p.map(e => e.value === 127 ? max : min));

// ---- 1. The script itself -----------------------------------------------------------------------------------------
{
    const { params } = makeScripter(scriptText, {}, false);
    const names = params.map(p => p.name);
    expect(params.length === 33, "33 settings (channel + 8 lanes x 4)");
    expect(new Set(names).size === names.length, "setting names are unique");
    expect(params.filter(p => p.type === "target").length === 8, "8 Learn targets");
    expect(params.every(p => ["lin", "menu", "target"].includes(p.type)), "only setting types Scripter knows");
    expect(params[0].name === "particlehold MIDI channel" && params[0].defaultValue === 13, "channel setting, 13 by default");
    const src = params.find(p => p.name === "Lane 1 source").valueStrings;
    expect(src.join() === "Off,Signal,Noise,Clock", "Source menu in its fixed order");
    expect(src.every(x => /^[ -~]+$/.test(x)) && /^[\x09\x0a\x0d -~]*$/.test(scriptText), "plain characters only (safe to paste)");
    expect(!/\.trace\(\)/.test(scriptText), "no trace printing in the mapper");
    for (const other of [brynText, hillsideText, ogdoadText, driftText])
        expect(makeScripter(other, {}, false).params[0].name !== names[0], "its channel setting has its own name");
}

// ---- 2. Nothing set up: particlehold's CC stops, everything else passes -----------------------------------------------
{
    const s = makeScripter(scriptText, {}, false);
    const { ControlChange, NoteOn, NoteOff, PitchBend } = s.sandbox;
    for (const e of events) s.sandbox.HandleMIDI(new ControlChange({ channel: CH, number: e.cc, value: e.value }));
    expect(s.out.passed.length === 0 && s.out.targets.length === 0, "the whole stream stops at the mapper, no knob moves");
    s.sandbox.HandleMIDI(new NoteOn({ pitch: 64 }));
    s.sandbox.HandleMIDI(new NoteOff({ pitch: 64 }));
    s.sandbox.HandleMIDI(new NoteOn({ channel: CH, pitch: 48 }));             // the Clock note plays the instrument
    s.sandbox.HandleMIDI(new PitchBend({ value: 1000 }));
    s.sandbox.HandleMIDI(new ControlChange({ channel: 1, number: 1, value: 90 }));
    s.sandbox.HandleMIDI(new ControlChange({ channel: 1, number: 75, value: 90 }));   // CC 75 on another channel
    s.sandbox.HandleMIDI(new ControlChange({ channel: 14, number: 80, value: 5 }));   // Bryn
    s.sandbox.HandleMIDI(new ControlChange({ channel: 16, number: 3, value: 5 }));    // Hillside
    s.sandbox.HandleMIDI(new ControlChange({ channel: 16, number: 20, value: 5 }));   // Ogdoad LFO 1
    s.sandbox.HandleMIDI(new ControlChange({ channel: 15, number: 106, value: 5 }));  // Strange Drift
    s.sandbox.HandleMIDI(new ControlChange({ channel: CH, number: 7, value: 100 }));  // volume on 13
    expect(s.out.passed.length === 11, "notes, pitch bend, other CC, Bryn's, Hillside's, Ogdoad's and Strange Drift's CC pass through (" + s.out.passed.length + ")");
    expect(s.out.passed[0].pitch === 64 && s.out.passed[0] instanceof NoteOn, "notes pass unchanged");
}

// ---- 3. Lanes set up, real stream ----------------------------------------------------------------------------------
for (const viaParameterChanged of [true, false]) {
    const how = viaParameterChanged ? " (settings changed in the panel)" : " (settings restored with a project)";
    const settings = {
        "Lane 1 source": 1,
        "Lane 2 source": 1, "Lane 2 min %": 30, "Lane 2 max %": 70,
        "Lane 3 source": 1, "Lane 3 min %": 100, "Lane 3 max %": 0,
        "Lane 4 source": 0,
        "Lane 5 source": 2,
        "Lane 6 source": 3, "Lane 6 min %": 10, "Lane 6 max %": 90,
    };
    const s = makeScripter(scriptText, settings, viaParameterChanged);
    const { ControlChange, NoteOn } = s.sandbox;
    let notesIn = 0;
    const t0 = process.hrtime.bigint();
    events.forEach((e, k) => {
        s.sandbox.HandleMIDI(new ControlChange({ channel: CH, number: e.cc, value: e.value }));
        if (k % 300 === 0) { s.sandbox.HandleMIDI(new NoteOn({ pitch: 40 + (k % 30) })); notesIn++; }
    });
    const usPerEvent = Number(process.hrtime.bigint() - t0) / 1000 / events.length;
    const l1 = lane(s, 1), l2 = lane(s, 2), l3 = lane(s, 3), l4 = lane(s, 4), l5 = lane(s, 5), l6 = lane(s, 6);
    expect(eq(l1, dedupe(truth)), "lane 1 follows Signal value for value" + how);
    expect(eq(l2, dedupe(truth.map(v => 0.3 + 0.4 * v))), "lane 2: Signal at 30..70%" + how);
    expect(eq(l3, dedupe(truth.map(v => 1 - v))), "lane 3: Signal upside down" + how);
    expect(l4.length === 0, "a lane set to Off never moves" + how);
    expect(eq(l5, dedupe(noiseTruth)), "lane 5 follows Noise value for value" + how);
    expect(eq(l6, gateWant(pulsesOf(79), 0.1, 0.9)) && l6.length >= 40, "lane 6 follows the Clock (10..90%)" + how);
    expect(near(l1[l1.length - 1], MIDDLE) && near(l2[l2.length - 1], 0.3 + 0.4 * MIDDLE) && near(l5[l5.length - 1], MIDDLE) && near(l6[l6.length - 1], 0.1),
           "lanes end at 0 V (the middle of Min..Max) and the Clock lane at Min" + how);
    const distinct = new Set(l1.map(v => Math.round(v * 16383))).size;
    expect(distinct > 150, "14-bit detail reaches the target (" + distinct + " distinct values)" + how);
    const notCoarse = l5.filter(v => Math.round(v * 16383) % 128 !== 0).length > l5.length / 2;
    expect(notCoarse, "values arrive in 14-bit steps, not 128 coarse steps" + how);
    expect(s.out.passed.length === notesIn, "notes still pass with lanes in use" + how);
    console.log("mapper: " + usPerEvent.toFixed(2) + " microseconds per CC message in Node" + how);
}

// ---- 4. Send as Note and Both: the Clock note reaches the instrument; the CC (Both) still moves the lanes ------------
{
    const nd = readDump(20, CH, 1);
    expect(nd.events.every(e => [75, 76, 77, 78].includes(e.cc)), "Note: no Clock CC is sent");
    expect(nd.notes.length > 30 && nd.notes.every(n => n.channel === CH && n.pitch === 48), "Note: Clock note 48 (C2) on channel 13 (" + nd.notes.length + ")");
    let open = false, balanced = true;
    for (const n of nd.notes) {
        if (n.velocity > 0) { balanced = balanced && !open && n.velocity === 100; open = true; }
        else { balanced = balanced && open; open = false; }
    }
    expect(balanced && !open && nd.pulsesOn === 0, "Note: every Clock note is released once, nothing left on");
    const s = makeScripter(scriptText, { "Lane 1 source": 3 }, true);
    const { NoteOn, NoteOff } = s.sandbox;
    for (const n of nd.notes) s.sandbox.HandleMIDI(n.velocity > 0 ? new NoteOn({ channel: CH, pitch: n.pitch, velocity: n.velocity }) : new NoteOff({ channel: CH, pitch: n.pitch }));
    expect(s.out.passed.length === nd.notes.length && lane(s, 1).length === 0, "Note: the Clock notes pass the mapper to the instrument unchanged; the Clock lane stays put");

    const bd = readDump(20, CH, 2);
    const bCc = bd.events.filter(e => e.cc === 79), bNotes = bd.notes.filter(n => n.pitch === 48);
    expect(bCc.length > 10 && bCc.length === bNotes.length && bCc.every((e, i) => e.sample === bNotes[i].sample && (e.value === 127) === (bNotes[i].velocity > 0)),
           "Both: the Clock as CC and as a note, at the same samples");
    const b = makeScripter(scriptText, { "Lane 1 source": 3 }, true);
    for (const e of bd.events) b.sandbox.HandleMIDI(new b.sandbox.ControlChange({ channel: CH, number: e.cc, value: e.value }));
    for (const n of bd.notes) b.sandbox.HandleMIDI(new b.sandbox.NoteOn({ channel: CH, pitch: n.pitch, velocity: n.velocity }));
    const l1 = lane(b, 1);
    expect(l1.length === bCc.length && near(l1[l1.length - 1], 0) && b.out.passed.length === bd.notes.length, "Both: the lane follows the CC, the notes pass");
}

// ---- 5. A different channel ------------------------------------------------------------------------------------------
{
    const d = readDump(10, 7);
    expect(d.events.every(e => e.channel === 7) && d.events.length > 100, "out channel 7: the engine sends on channel 7");
    const s = makeScripter(scriptText, { "particlehold MIDI channel": 7, "Lane 1 source": 1 }, true);
    const { ControlChange } = s.sandbox;
    for (const e of events.slice(0, 400)) s.sandbox.HandleMIDI(new ControlChange({ channel: CH, number: e.cc, value: e.value }));
    expect(s.out.targets.length === 0 && s.out.passed.length === 400, "channel 7 set: channel 13 CC is left alone");
    for (const e of d.events) s.sandbox.HandleMIDI(new ControlChange({ channel: e.channel, number: e.cc, value: e.value }));
    expect(s.out.targets.length > 10 && s.out.passed.length === 400, "channel 7 set: channel 7 CC is mapped and stopped");
}

// ---- 6. Sharing a track with Bryn, Hillside, Ogdoad and Strange Drift: the five mappers chained, in all 120 orders ------
//         Also with particlehold on channel 16 (the same as Hillside and Ogdoad): the numbers never overlap.
for (const phChannel of [CH, 16]) {
    // particlehold's real stream, with stand-in Bryn (channel 14: Function = CC 80 + 81, EOC = CC 90), Hillside (channel
    // 16: Envelope = CC 3 + 35, Gate A = CC 85), Ogdoad (channel 16: LFO 1 = CC 20 + 52, trig = CC 102) and Strange
    // Drift (channel 15: Ripple X = CC 106 + 113) streams mixed in.
    const mixed = [];
    let k = 0;
    for (const e of events) {
        mixed.push({ channel: phChannel, number: e.cc, value: e.value });
        if (k++ % 3 === 0) {
            const v = (k * 37) % 16384, w = (k * 91) % 16384, h = (k * 53) % 16384, f = (k * 71) % 16384;
            mixed.push({ channel: 14, number: 80, value: f >> 7 }, { channel: 14, number: 81, value: f & 127 });
            mixed.push({ channel: 16, number: 20, value: v >> 7 }, { channel: 16, number: 52, value: v & 127 });
            mixed.push({ channel: 15, number: 106, value: w >> 7 }, { channel: 15, number: 113, value: w & 127 });
            mixed.push({ channel: 16, number: 3, value: h >> 7 }, { channel: 16, number: 35, value: h & 127 });
            if (k % 301 === 1) mixed.push({ channel: 16, number: 102, value: 127 }, { channel: 16, number: 102, value: 0 });
            if (k % 401 === 1) mixed.push({ channel: 16, number: 85, value: 127 }, { channel: 16, number: 85, value: 0 });
            if (k % 251 === 1) mixed.push({ channel: 14, number: 90, value: 127 }, { channel: 14, number: 90, value: 0 });
        }
    }
    const lastOf = (ch, fine) => { for (let i = mixed.length - 1; i >= 0; i--) if (mixed[i].channel === ch && mixed[i].number === fine) return (mixed[i - 1].value * 128 + mixed[i].value) / 16383; };
    const lastBr = lastOf(14, 81), lastOg = lastOf(16, 52), lastSd = lastOf(15, 113), lastHs = lastOf(16, 35);
    const carry = (to, ev) => new to.sandbox[ev.constructor.name](Object.assign({}, ev));
    const kinds = {
        P: { text: scriptText,   settings: { "particlehold MIDI channel": phChannel, "Lane 1 source": 1, "Lane 2 source": 3, "Lane 3 source": 2 } },
        B: { text: brynText,     settings: { "Lane 1 source": 1, "Lane 2 source": 4 } },
        H: { text: hillsideText, settings: { "Lane 1 source": 1, "Lane 2 source": 3 } },
        O: { text: ogdoadText,   settings: { "Lane 1 source": 1, "Lane 2 source": 17 } },
        S: { text: driftText,    settings: { "Lane 1 source": 1 } },
    };
    const perms = a => a.length <= 1 ? [a] : a.flatMap((x, i) => perms(a.slice(0, i).concat(a.slice(i + 1))).map(p => [x].concat(p)));
    const names = { P: "particlehold", B: "Bryn", H: "Hillside", O: "Ogdoad", S: "Strange Drift" };
    let orders = 0, good = 0;
    for (const order of perms(["P", "B", "H", "O", "S"])) {
        const instrument = [];
        const made = {};
        let next = ev => instrument.push(ev);
        for (const letter of order.slice().reverse()) {
            const to = next;
            const m = makeScripter(kinds[letter].text, kinds[letter].settings, true, to);
            made[letter] = m;
            next = ev => m.sandbox.HandleMIDI(carry(m, ev));
        }
        const first = made[order[0]];
        for (const m of mixed) first.sandbox.HandleMIDI(new first.sandbox.ControlChange(m));
        first.sandbox.HandleMIDI(new first.sandbox.NoteOn({ pitch: 50 }));
        const p1 = lane(made.P, 1), p2 = lane(made.P, 2), p3 = lane(made.P, 3), b1 = lane(made.B, 1), b2 = lane(made.B, 2);
        const h1 = lane(made.H, 1), h2 = lane(made.H, 2), o1 = lane(made.O, 1), o2 = lane(made.O, 2), s1 = lane(made.S, 1);
        const tag = " (order " + order.map(c => names[c]).join(", ") + " Mapper; particlehold on channel " + phChannel + ")";
        const ok = [
            [eq(p1, dedupe(truth)), "particlehold Signal lane follows Signal exactly"],
            [eq(p2, gateWant(pulsesOf(79), 0, 1)), "particlehold Clock lane follows the Clock"],
            [eq(p3, dedupe(noiseTruth)), "particlehold Noise lane follows Noise exactly"],
            [near(b1[b1.length - 1], lastBr) && b1.length > 100 && b2.length > 4, "Bryn lanes follow Bryn's stream"],
            [near(h1[h1.length - 1], lastHs) && h1.length > 100 && h2.length > 4, "Hillside lanes follow Hillside's stream"],
            [near(o1[o1.length - 1], lastOg) && o1.length > 100 && o2.length > 4, "Ogdoad lanes follow Ogdoad's stream"],
            [near(s1[s1.length - 1], lastSd) && s1.length > 100, "Strange Drift lane follows Strange Drift's stream"],
            [instrument.length === 1 && instrument[0].pitch === 50, "only the note reaches the instrument: each plugin's CC stops at its own mapper"],
        ];
        let all = true;
        for (const [cond, what] of ok) { expect(cond, what + tag); all = all && cond; }
        orders++; good += all ? 1 : 0;
    }
    console.log("sharing a track: " + good + " of " + orders + " mapper orders correct (particlehold on channel " + phChannel + ")");
}

if (failures === 0) console.log("mapper tests: " + checks + " checks passed");
else console.log("mapper tests: " + failures + " of " + checks + " checks FAILED");
process.exit(failures === 0 ? 0 : 1);
