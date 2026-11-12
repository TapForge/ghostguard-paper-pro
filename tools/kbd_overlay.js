(function () {
  // D2 test (no restart): draw the on-screen keyboard with Overlay (PEN, wf1) instead of Animation (A2, wf7).
  // The keyboard window (VirtualKeyboardWindow, a BaseView) is created when a text field asks for it and is added
  // to the QQuickRootItem next to the other windows (#legacydevice/window/main, #gestures-global/window/gestures, ...).
  // A small watcher item ('ggkbdwatch', under root) listens to the root item's childrenChanged and switches the
  // ScreenModeItem with objectName "keyboard" (KeyboardPanel.qml:61-66) to 6 before its first frame where it can.
  // EPScreenMode (moc enum): Pen 0, Mono 1, Animation 2, UI 3, Content 4, Sleep 5, Overlay 6.
  // Undo: tools/kbd_anim.js (removes the watcher, mode back to 2). A xochitl restart also returns to stock.
  var top = root;
  while (top.parent) top = top.parent;
  var k = 0;
  for (var i = root.children.length - 1; i >= 0; i--) {
    if (root.children[i].objectName === "ggkbdwatch") { root.children[i].objectName = "ggkbdwatch-old"; root.children[i].destroy(); k++; }
  }
  var src = [
    "import QtQuick",
    "Item {",
    "  id: w",
    "  objectName: 'ggkbdwatch'",
    "  property var rootItem: null",
    "  property int want: 6",
    "  property int hits: 0",
    "  property int scans: 0",
    "  property var kb: null",
    "  function find(o, d) {",
    "    if (!o || d > 60) return null;",
    "    if (o.objectName === 'keyboard' && o.mode !== undefined) return o;",
    "    var c = o.children || [];",
    "    for (var i = 0; i < c.length; i++) { var r = w.find(c[i], d + 1); if (r) return r; }",
    "    return null;",
    "  }",
    "  function switchTo(o, why, where) {",
    "    w.kb = o;",
    "    if (o.mode !== w.want) {",
    "      var b = o.mode; o.mode = w.want; w.hits++;",
    "      console.log('GGMARK kbdwatch ' + why + ': ' + where + ' mode ' + b + ' -> ' + o.mode + ' ' + String(o).substring(0, 40));",
    "    }",
    "    return true;",
    "  }",
    "  function scan(why, all) {",
    "    if (w.objectName !== 'ggkbdwatch' || !w.rootItem) return true;",
    "    w.scans++;",
    "    var c = w.rootItem.children || [];",
    "    for (var i = 0; i < c.length; i++) {",
    "      var n = String(c[i].objectName || '');",
    "      if (!all && n.indexOf('legacydevice/window/main') === 0) continue;",
    "      var o = w.find(c[i], 0);",
    "      if (o) return w.switchTo(o, why, n);",
    "    }",
    "    return false;",
    "  }",
    "  function handleWindows() {",
    "    if (w.objectName !== 'ggkbdwatch') return;",
    "    if (!w.scan('added', false)) { retry.tries = 40; retry.start(); }",
    "  }",
    "  Timer { id: retry; interval: 25; repeat: true; property int tries: 0",
    "    onTriggered: { if (w.scan('retry', false)) { stop(); return; } if (--tries <= 0) stop(); } }",
    "  Timer { interval: 1000; repeat: true; running: true",
    "    onTriggered: { if (w.kb) { try { if (w.kb.mode !== w.want && !w.scan('reset', false)) w.kb = null; } catch (e) { w.kb = null; } } } }",
    "}"
  ].join("\n");
  var o = Qt.createQmlObject(src, root, "ggkbdwatch");
  o.rootItem = top;
  top.childrenChanged.connect(o, o.handleWindows);
  var now = o.scan("now", true);
  console.log("GGMARK kbd_overlay: watcher on (" + k + " old removed), keyboard " + (now ? "found and switched" : "not open yet"));
  return "kbd watcher on (" + k + " old removed), keyboard " + (now ? "found, mode " + o.kb.mode : "not open yet") +
         ", windows " + (top.children || []).length;
})()
