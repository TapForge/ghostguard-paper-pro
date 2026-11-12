(function () {
  // clock 14f (2026-11-12, the user: "the gap between the two flashes is too long"): clock 14 with FINE ticks between
  // 0.75 s and 1.5 s after the last screen update (every 50 ms instead of 250 ms), so the second pass of a full
  // STD turn (ggsense 1.8.26+, turn_full 2) goes out within 50 ms of the panel going idle instead of up to 250 ms
  // later. Ticks at 0.25, 0.5, 0.75 s and from 1.5 s to 4 s are as in clock 14; the pen rule is the same (no
  // tick in the first 1.5 s while the pen is near). Same object name, so clock14.js replaces it (the way back).
  // GhostGuard 14 clock for ggsense 1.8.6 (clock 13 + the 1.8.6 page turn: every turn that leaves a trace is
  // cleaned, right after the page's frame; sent before the triggers).
  // Only a clock: "GGQ 0 settle" every 250 ms for 4 s after every screen
  // update and "GGQ 0 hb" every 20 s, both with " pen" while the pen is near (pen near: no settle in the first
  // 1.5 s). No decisions here any more (clock 10b's 'look' timer and its "auto doc/closed" requests are gone):
  // ggsense 1.8 takes its triggers from xochitl's own log lines (ScreenDriver document close and gesture end,
  // page turns, on-screen keyboard close; see the header of ggsense-1.8.c) and the ticks only tell it when the
  // screen has been quiet. Options sent: defer 1 (view changes: Settings, dialogs), the residue model calibration
  // of clock 10b and the four 1.8 triggers (all off until this clock turns them on). The 1.8.1 keyboard-open clean is
  // sent OFF: ggsense 1.8.3 retired it and refuses to turn it on (1.8 ignores the unknown option).
  // Pure QtQuick objects only (Item, Timer, Connections); never a C++-backed element from an eval.
  // Idempotent: the new clock is created first; only then are older clocks (8, 9, 10, 11, 12, 13, 14) removed.
  // The page rule (ggsense 1.8.6, see its header): every page change is counted and due (page_gap 1); a page turn
  // cleans when the model sees ghost >= turn_px (6000: any turn that leaves a trace), 0.2 s after the page's frame
  // (page_quiet), so the flash starts while the page is still being drawn; a document gesture's redraw cleans only
  // from page_px (80000); the 6th change cleans anyway (page_every); a clean that falls due while the user leafs
  // (turns < 2 s apart) waits for 1.5 s without updates. Send this clock to ggsense 1.8.6 only (1.8.5 ignores
  // turn_px/page_quiet and would keep its 80000 threshold for turns; 1.8.4 ignores the whole rule).
  // Back to clock 10b (1.6 behaviour, 1.8 triggers off): tools/clock10b.js, then "GGQ 9 set sd_close 0" etc.
  var pen = null;
  try { pen = PenClosenessMonitor; } catch (e) { pen = null; }
  var src = [
    "import QtQuick",
    "Item {",
    "  id: c",
    "  objectName: 'gg14clock'",
    "  property var fb: null",
    "  property var pen: null",
    "  property int ms: 0",
    "  property int n: 0",
    "  function tag() { return (c.pen && c.pen.penWasClose) ? ' pen' : ''; }",
    "  function kick() { c.ms = 0; settle.interval = 250; settle.restart(); }",
    "  Connections {",
    "    target: c.fb",
    "    function onFramebufferUpdated(r) { c.kick(); }",
    "  }",
    "  Connections {",
    "    target: c.pen",
    "    ignoreUnknownSignals: true",
    "    function onPenWasCloseChanged() { if (c.pen && !c.pen.penWasClose) c.kick(); }",
    "  }",
    "  Timer {",
    "    id: settle",
    "    interval: 250",
    "    repeat: false",
    "    onTriggered: {",
    "      c.ms = c.ms + settle.interval;",
    "      var t = c.tag();",
    "      if (t === '' || c.ms >= 1500) { c.n = c.n + 1; console.log('GGQ 0 settle' + t); }",
    "      if (c.ms < 4000) { settle.interval = (c.ms >= 750 && c.ms < 1500) ? 50 : 250; settle.restart(); }",
    "    }",
    "  }",
    "  Timer { interval: 20000; repeat: true; running: true; onTriggered: console.log('GGQ 0 hb' + c.tag()) }",
    "}"
  ].join("\n");
  var o = Qt.createQmlObject(src, root, "gg14clock");
  o.pen = pen;
  o.fb = EPFramebuffer;
  var k = 0;
  for (var i = root.children.length - 1; i >= 0; i--) {
    var ch = root.children[i];
    var n = ch.objectName;
    if (ch !== o && (n === "gg8clock" || n === "gg9clock" || n === "gg10clock" || n === "gg11clock" || n === "gg12clock" ||
                     n === "gg13clock" || n === "gg14clock" || n === "gg72proto")) { ch.destroy(); k++; }
  }
  console.log("GGQ 0 hb");
  console.log("GGQ 80 set tr_quiet 0.5");
  console.log("GGQ 81 set defer 1");
  console.log("GGQ 82 set w_ui 1.0");
  console.log("GGQ 83 set w_content 1.2");
  console.log("GGQ 84 set w_std 1.2");
  console.log("GGQ 85 set quiet_s 2.0");
  console.log("GGQ 86 set auto_quiet 0.4");
  console.log("GGQ 87 set stock_gap 3");
  console.log("GGQ 88 set gesture_min 0.02");
  console.log("GGQ 89 set page_min 0.5");
  console.log("GGQ 75 set page_gap 1");
  console.log("GGQ 76 set page_px 80000");
  console.log("GGQ 77 set page_every 6");
  console.log("GGQ 78 set page_flip 2.0");
  console.log("GGQ 79 set page_settle 1.5");
  console.log("GGQ 74 set turn_px 6000");
  console.log("GGQ 73 set page_quiet 0.2");
  console.log("GGQ 90 set sd_close 1");
  console.log("GGQ 91 set gesture_clean 1");
  console.log("GGQ 92 set page_clean 1");
  console.log("GGQ 93 set kb_clean 1");
  console.log("GGQ 94 set kb_open_clean 0"); // ggsense 1.8.3: retired (IMG_1753; its band clean ran into the dialog's own update)
  o.kick();
  return "gg14 clock on (" + k + " old removed), defer 1, 1.8 triggers sd_close/gesture/page/kb on, page rule 1/6000/80000/6/0.2, " +
         "kb_open off, fine ticks 0.75-1.5 s, pen " +
         (pen ? "seen" : "not available");
})()
