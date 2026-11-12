(function () {
  // viewchange (2026-11-12): one refresh AT a view change instead of "ghost first, hard flash later".
  // The owner: "when I close a file, a hard flash bursts on the main menu ... and before that burst there is a ghost
  // trace" (closing a document, opening and closing Settings).
  // With defer 1 (the clock's setting) ggsense takes xochitl's own full refresh away at a view change, the new view
  // is drawn over the old one (the ghost), and after 0.5 s of quiet the whole screen gets one in-place clean (the
  // flash, 0.6-0.9 s late). defer 0 leaves xochitl's own refresh where it is: the first frame of the new view goes
  // out as one full refresh (with the white tail), and no in-place clean follows it.
  // Page turns, the white tail, the gesture clean and the keyboard-close clean do not depend on it.
  // A full refresh that changes the content can keep a faint trace of the old view. For Settings it did, so
  // tools/settingskb.js (ggsense 1.8.31 and later) gives opening and closing Settings the in-place clean back.
  // A boot extra since 2026-11-12:
  //   deploy/persist_install.sh ... --extra tools/bwturn.js --extra tools/ggrefresh.js --extra tools/viewchange.js
  // (it must come after ggapply.sh's status check, which wants defer 1: every extra does).
  // Back to the old behaviour, now:   console.log("GGQ 9 set defer 1")
  console.log("GGQ 62 set defer 0");
  return "view change: stock refresh at the change (defer 0)";
})()
