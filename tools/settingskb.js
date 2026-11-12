(function () {
  // settingskb (2026-11-12, ggsense 1.8.31 and later): two places only, nothing else changes.
  // 1. Settings: with defer 0 (x03-viewchange) the library's side menu stayed behind the Settings page (user photo
  //    IMG_1800). vs_open 1 gives SettingsOpened alone the old cycle again: xochitl's flash is withdrawn, the page is
  //    drawn by its own update, then ONE whole-screen in-place clean once nothing changed for vs_quiet s. Document
  //    close, SettingsClosed and every other view change keep defer 0.
  // 2. Keyboard: the "+" menu stayed visible inside the on-screen keyboard (IMG_1801). kb_open_full 1: when a keyboard
  //    OPENS, one whole-screen in-place clean about 0.5 s later, before the first key (never while typing, never with
  //    the pen near the screen; dropped at the first key). The clean when the keyboard closes stays as it is.
  // 3. (ggsense 1.8.32) Settings CLOSED too: xochitl's own flash at SettingsClosed left the whole Settings page as a
  //    ghost on the library (user photo IMG_1804). vs_open is a bit mask: 1 opened, 2 closed, 3 both (sent here).
  // Boot extra after x03:  deploy/persist_install.sh ... --extra tools/viewchange.js --extra tools/settingskb.js
  // Off again, now:  console.log("GGQ 9 set vs_open 0"); console.log("GGQ 9 set kb_open_full 0")
  // ggsense 1.8.31 takes any vs_open other than 0 as 1 (opened only); 1.8.30 and older do not know these keys.
  console.log("GGQ 63 set vs_open 3");
  console.log("GGQ 63 set vs_quiet 0.2");
  console.log("GGQ 64 set kb_open_full 1");
  return "settings open/close + keyboard open: one in-place clean each (vs_open 3, kb_open_full 1)";
})()
