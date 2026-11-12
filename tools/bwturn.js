(function () {
  // bwturn (2026-11-12): black-and-white pages turn the way colour pages do. The owner: "can black-and-white pages
  // get the colour-page behaviour too?", tried at runtime from 14:24 UTC on, then: "make it permanent".
  // ggsense 1.8.30 promotes a page turn to ONE full refresh (with the white tail) only when the page has enough
  // colour (tf_chroma 25, the build's default). tf_chroma 0 takes that condition away: every page that has anything
  // on it turns with the full refresh; a blank page still turns without a flash.
  // A boot extra (deploy/persist_install.sh --extra tools/bwturn.js): ggapply.sh sends it after the clock, at every
  // start of xochitl with ggsense. The setting lives in ggsense's memory only.
  // Back to the build's default, now:   console.log("GGQ 9 set tf_chroma 25")
  console.log("GGQ 61 set tf_chroma 0");
  return "bw turn on: tf_chroma 0";
})()
