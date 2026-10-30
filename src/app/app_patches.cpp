// Per-title adjustments, keyed by CFBundleIdentifier (and checked against the expected binary layout before any
// memory is touched). Keep these small: the general fixes belong in the frameworks.
#include "app.hpp"

#include <cstdlib>

namespace app {

IMP method_imp(const char* cls, const char* sel, bool class_method);   // app_runtime.cpp
bool override_method(const char* cls, const char* sel, bool class_method, IMP imp);

namespace {

// ---- Pocket God (com.boltcreative.pocketgod) ----
// The game is delisted, so its in-app store can no longer sell the skin packs it ships. Its only bonus pack is
// unlocked by a keychain secret the game writes itself when a score is reached ("BabaBooey"); the skin packs are
// unlocked by the same secret from its StoreKit purchase handler.
IMP g_pg_init_bonus = nullptr;

// -[BonusPackManager initBonusPackDefs] runs the original, then unlocks the static BonusPackDef exactly as
// -checkBonusPackUnlock:score: does (keychain secret + unlocked flag), without opening the unlock dialog.
void pg_init_bonus(id self, SEL s) {
    reinterpret_cast<void (*)(id, SEL)>(g_pg_init_bonus)(self, s);
    // BonusPackDef {6 ids, 2 ints, BCKeyChain* keychain @0x38, BOOL unlocked @0x40} at vmaddr 0x100602c88 (1.48.2)
    auto* def = reinterpret_cast<uint8_t*>(image_base() + 0x602c88);
    id pack_id = *reinterpret_cast<id*>(def);
    id keychain = *reinterpret_cast<id*>(def + 0x38);
    if (!is_string(pack_id) || !is_kind(keychain, "BCKeyChain")) { log("pocketgod: bonus pack layout not recognized, skipped"); return; }
    send<int>(keychain, "setSecret:", str("BabaBooey"));
    def[0x40] = 1;
    log("pocketgod: bonus pack %s unlocked", utf8(pack_id).c_str());
}

void patch_pocketgod() {
    std::string ver = utf8(dict_get(info_dictionary(), "CFBundleShortVersionString"));
    // skin packs: a local store that grants every product the game asks for (through its own purchase handler)
    setenv("LOADER_APP_OFFLINE_STORE", "own_all", 0);
    if (ver == "1.48.2") {
        g_pg_init_bonus = method_imp("BonusPackManager", "initBonusPackDefs", false);
        if (g_pg_init_bonus) override_method("BonusPackManager", "initBonusPackDefs", false, reinterpret_cast<IMP>(&pg_init_bonus));
    } else {
        log("pocketgod %s: bonus pack patch is for 1.48.2 only", ver.c_str());
    }
    // the skin pack store only asks for product data when its screen opens; ask once after launch so every pack
    // is owned from the start
    post_main([] {
        id mgr = send_cls(class_named("SkinPackManager"), "instance");
        if (mgr) send<void>(mgr, "requestProductData");
    }, 6.0);
    log("pocketgod %s: skin packs via offline store, bonus pack unlock", ver.c_str());
}

}  // namespace

void apply_game_patches() {
    if (getenv("LOADER_APP_NO_PATCHES")) return;
    std::string bid = utf8(dict_get(info_dictionary(), "CFBundleIdentifier"));
    if (bid == "com.boltcreative.pocketgod") patch_pocketgod();
}

}  // namespace app
