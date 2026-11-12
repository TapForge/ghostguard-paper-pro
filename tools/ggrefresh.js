(function () {
  // ggrefresh (2026-11-12): a refresh button inside the document toolbar, right below the undo/redo buttons.
  // The owner: "... let's put an icon button, a nice button that stands for refresh; pressing it fully refreshes the
  // page ... I want something that shows on every page", and, after seeing a first version next to the toolbar's
  // show/hide button: "could you put it into the toolbox on the left? right below the forward/back buttons."
  // A tap sends "GGQ 7 rclean 0 0 1620 2160": ONE full in-place refresh of what is on the screen (ggsense do_clean;
  // refused only while the tablet sleeps). At most one per 2.5 s.
  //   python3 -I tools/ggq.py tools/ggrefresh.js --sub GGRCMD=make|kill|state
  // Where it sits: the toolbar's tools are the children of the GridLayout "toolbarLayout"; the last editing tool is
  // the loader "editingToolLoader_redoButton", and below it the layout has an empty stretch down to the page-overview
  // button. The button is NOT put into the layout (a layout orders by child order, and nothing of xochitl's own may
  // be re-parented from here): it is a child of the layout's parent, the toolbar column, laid over the start of that
  // empty stretch, one tool cell (112x112, icon 48x48) large. Its place and its visibility follow the redo loader by
  // bindings, so it moves with the toolbar (left, right, top, bottom; in a row it sits after the redo button) and it
  // is hidden while the toolbar is closed, like the other tools.
  // Objects, pure QtQuick (Item, Rectangle, Image, MouseArea, Timer, Connections; never one of xochitl's own types
  // from an eval): the keeper "ggrefresh" under MainView and the button "ggrefreshbtn" in the toolbar column. The
  // keeper makes the button again when a document comes up without one (the button goes away with the document's
  // toolbar). Nothing of xochitl's own items is changed.
  // The icon is the interface's own "arrows_counter_clockwise", asked from its image provider the way the redo icon
  // above it is asked ("image://arkimageprovider/<path length>,<path><flag>,<w>,<h>,<colour>,<colour>"), with the
  // redo icon's flag; the same icon with flag 0 (known to load) and then a drawn ring stand in when that fails.
  // A xochitl restart removes all of it. Installed as a boot extra (deploy/persist_install.sh --extra
  // tools/ggrefresh.js), ggapply.sh makes it again after every start of xochitl with ggsense.
  // Property names: never one of Item's own (top, left, right, bottom, baseline ... are FINAL there).
  var cmd = "GGRCMD";
  var bsrc = [
    "import QtQuick",
    "Item {",
    "  id: b",
    "  objectName: 'ggrefreshbtn'",
    "  property var ref: null",
    "  property var lay: null",
    "  property var bar: null",
    "  property string icon: ''",
    "  property string alt: ''",
    "  property int istat: im.status",
    "  property real last: 0",
    "  property int taps: 0",
    "  property bool vert: !lay || lay.flow === undefined || lay.flow === 1",
    "  width: 112",
    "  height: 112",
    "  x: (ref && lay) ? lay.x + ref.x + (vert ? 0 : ref.width) : 0",
    "  y: (ref && lay) ? lay.y + ref.y + (vert ? ref.height : 0) : 0",
    "  opacity: lay ? lay.opacity : 1",
    "  visible: !!ref && !!lay && ref.visible && (!bar || bar.expanded !== false)",
    "  Image {",
    "    id: im",
    "    x: 32; y: 32; width: 48; height: 48; sourceSize.width: 48; sourceSize.height: 48",
    "    source: b.icon",
    "    onStatusChanged: { if (im.status === 3 && b.alt !== '' && b.icon !== b.alt) b.icon = b.alt; }",
    "  }",
    "  Item {",
    "    x: 32; y: 32; width: 48; height: 48",
    "    visible: b.icon !== '' && im.status === 3",
    "    Rectangle { x: 6; y: 6; width: 36; height: 36; radius: 18; color: 'white'; border.width: 4; border.color: 'black' }",
    "    Rectangle { x: 26; y: 2; width: 20; height: 16; color: 'white' }",
    "    Rectangle { x: 26; y: 6; width: 14; height: 4; color: 'black' }",
    "    Rectangle { x: 36; y: 6; width: 4; height: 14; color: 'black' }",
    "  }",
    "  MouseArea {",
    "    x: 0; y: 0; width: 112; height: 112",
    "    onClicked: {",
    "      var n = Date.now();",
    "      if (n - b.last < 2500) return;",
    "      b.last = n;",
    "      b.taps = b.taps + 1;",
    "      console.log('GGQ 7 rclean 0 0 1620 2160');",
    "    }",
    "  }",
    "}"
  ].join("\n");
  var ksrc = [
    "import QtQuick",
    "Item {",
    "  id: c",
    "  objectName: 'ggrefresh'",
    "  property bool on: true",
    "  property var ld: null",
    "  property var doc: null",
    "  property var btn: null",
    "  property string bsrc: ''",
    "  property int tries: 0",
    "  property int made: 0",
    "  property string note: ''",
    "  function find(o, name, depth, max) {",
    "    if (!o || depth > max) return null;",
    "    if (o.objectName === name) return o;",
    "    var k = o.children || [];",
    "    for (var i = 0; i < k.length; i++) { var r = c.find(k[i], name, depth + 1, max); if (r) return r; }",
    "    return null;",
    "  }",
    "  function provTail(o, depth) {",
    "    if (!o || depth > 5) return '';",
    "    var pre = 'image://arkimageprovider/';",
    "    var s = (o.source !== undefined && o.source !== null) ? String(o.source) : '';",
    "    if (s.indexOf(pre) === 0) {",
    "      var rest = s.substring(pre.length);",
    "      var comma = rest.indexOf(',');",
    "      var n = parseInt(rest.substring(0, comma));",
    "      if (comma > 0 && n > 0 && rest.length > comma + 1 + n) return rest.substring(comma + 1 + n);",
    "    }",
    "    var k = o.children || [];",
    "    for (var i = 0; i < k.length; i++) { var r = c.provTail(k[i], depth + 1); if (r !== '') return r; }",
    "    return '';",
    "  }",
    "  function iconSrc(flag) {",
    "    var want = ':/ark/icons/arrows_counter_clockwise';",
    "    return 'image://arkimageprovider/' + want.length + ',' + want + flag + ',48,48,ff000000,ff000000';",
    "  }",
    "  function ensure() {",
    "    if (!c.on) return 'off';",
    "    var d = c.ld ? c.ld.item : null;",
    "    if (!d) { c.doc = null; c.btn = null; c.tries = 0; return 'no document'; }",
    "    if (d === c.doc && c.btn) return 'kept';",
    "    if (d !== c.doc) { c.doc = d; c.btn = null; c.tries = 0; }",
    "    c.tries = c.tries + 1;",
    "    if (c.tries > 12 && c.tries % 12 !== 0) return 'waiting';",
    "    var redo = c.find(d, 'editingToolLoader_redoButton', 0, 8);",
    "    if (!redo) return 'no redo button yet';",
    "    var lay = redo.parent, col = lay ? lay.parent : null, bar = col ? col.parent : null;",
    "    if (!col) return 'no toolbar column';",
    "    var k = col.children;",
    "    for (var i = k.length - 1; i >= 0; i--) {",
    "      if (k[i].objectName === 'ggrefreshbtn') { k[i].objectName = 'ggrefreshbtn-old'; k[i].visible = false; k[i].destroy(); }",
    "    }",
    "    var t = '', flag = '0';",
    "    try { t = c.provTail(redo, 0); } catch (e) { t = ''; }",
    "    if (t !== '' && t.charAt(0) >= '0' && t.charAt(0) <= '9' && t.charAt(1) === ',') flag = t.charAt(0);",
    "    var nb = Qt.createQmlObject(c.bsrc, col, 'ggrefreshbtn');",
    "    nb.alt = c.iconSrc('0');",
    "    nb.icon = c.iconSrc(flag);",
    "    nb.bar = (bar && bar.expanded !== undefined) ? bar : null;",
    "    nb.lay = lay;",
    "    nb.ref = redo;",
    "    c.btn = nb;",
    "    c.made = c.made + 1;",
    "    c.tries = 0;",
    "    return 'made';",
    "  }",
    "  function guarded() {",
    "    try { return c.ensure(); } catch (x) { c.note = String(x); return 'error ' + x; }",
    "  }",
    "  Connections {",
    "    target: c.ld",
    "    ignoreUnknownSignals: true",
    "    function onItemChanged() { c.tries = 0; place.restart(); }",
    "  }",
    "  Timer {",
    "    id: place",
    "    interval: 300",
    "    repeat: false",
    "    onTriggered: { c.guarded(); if (c.on && c.ld && c.ld.item && !c.btn && c.tries < 12) place.restart(); }",
    "  }",
    "  Timer { interval: 5000; repeat: true; running: true; onTriggered: c.guarded() }",
    "}"
  ].join("\n");
  function find(o, name, depth, max) {
    if (!o || depth > max) return null;
    if (o.objectName === name) return o;
    var c = o.children || [];
    for (var i = 0; i < c.length; i++) { var r = find(c[i], name, depth + 1, max); if (r) return r; }
    return null;
  }
  // The places a button of this script (this version or an earlier one) can sit in.
  function hosts() {
    var d = null, a = [];
    try { d = documentView.item; } catch (e) { d = null; }
    if (d) {
      var redo = find(d, "editingToolLoader_redoButton", 0, 8);
      var col = (redo && redo.parent) ? redo.parent.parent : null;
      if (col) a.push({ name: "toolbar", item: col });
      var t = find(d, "hide-show-toolbar-button", 0, 8);
      if (t) a.push({ name: "toggle", item: t });
    }
    var h = find(root, "ContentHeader", 0, 6);
    if (h) a.push({ name: "library", item: h });
    return a;
  }
  function keepers() {
    var a = [], c = root.children || [];
    for (var i = 0; i < c.length; i++) if (c[i].objectName === "ggrefresh") a.push(c[i]);
    return a;
  }
  function buttons() {
    var a = [], hs = hosts();
    for (var j = 0; j < hs.length; j++) {
      var c = hs[j].item.children || [];
      for (var i = 0; i < c.length; i++) if (c[i].objectName === "ggrefreshbtn") a.push({ name: hs[j].name, item: c[i] });
    }
    return a;
  }
  function drop(b) { b.objectName = "ggrefreshbtn-old"; b.visible = false; b.destroy(); }
  function kill() {
    var k = keepers(), n = 0, m = 0, i;
    for (i = 0; i < k.length; i++) {
      var b1 = k[i].btn, b2 = k[i].lbtn;
      k[i].on = false;             // first: this keeper makes no further button
      k[i].objectName = "ggrefresh-old";
      if (b1) { drop(b1); m++; }
      if (b2) { drop(b2); m++; }
      k[i].destroy(); n++;
    }
    var s = buttons();
    for (i = 0; i < s.length; i++) { drop(s[i].item); m++; }
    return "keepers " + n + ", buttons " + m;
  }
  function at(o) { var p = o.mapToItem(null, 0, 0); return Math.round(p.x) + "," + Math.round(p.y); }
  function state() {
    var k = keepers(), s = buttons(), out = "keepers " + k.length + ", buttons " + s.length;
    if (k.length) out += ", made " + k[0].made + ", tries " + k[0].tries + (k[0].note ? ", note " + k[0].note : "");
    for (var i = 0; i < s.length; i++) {
      var b = s[i].item;
      out += " | button in the " + s[i].name + " @" + at(b) + " " + b.width + "x" + b.height + " visible " + b.visible +
             ", icon status " + b.istat + ", taps " + b.taps + ", src " + b.icon;
      if (b.ref) out += " | redo loader @" + at(b.ref) + " " + b.ref.width + "x" + b.ref.height + " visible " + b.ref.visible + " shown " + b.ref.shown;
      if (b.lay) out += " | layout @" + at(b.lay) + " " + b.lay.width + "x" + b.lay.height + " flow " + b.lay.flow + " opacity " + b.lay.opacity;
      if (b.bar) out += " | toolbar expanded " + b.bar.expanded + " position " + b.bar.position;
    }
    return out;
  }
  if (cmd === "kill") {
    var gone = kill();
    console.log("GGMARK ggrefresh kill: " + gone);
    return "ggrefresh removed: " + gone;
  }
  if (cmd === "state") return state();
  // make: asked by name, and also what the file does as it is, with no command filled in. The boot sends it that way
  // (deploy/boot/mkpayload.py --extra), and its result check is the text of the LAST return in this file: keep the
  // return below the last one.
  var was = kill();
  var o = Qt.createQmlObject(ksrc, root, "ggrefresh");
  o.bsrc = bsrc;
  o.ld = documentView;
  var r = o.guarded();
  console.log("GGMARK ggrefresh make: " + r);
  return "ggrefresh on (removed first: " + was + "): " + r + " | " + state();
})()
