// Notices (UI overhaul PR2): the mod's own messages as a short stack of notifications in the top right, newest on top, in
// place of lines in the chat; the kill feed sits in the same column, under them. What the stack holds, how long each one
// lives and how many show is match/hsm_notice.h; the words are match/hsm_notice_text.h. This maps the stack onto real UMG
// widgets in two places: the game's UI_HUD (the match and any session fight, where the chat is) and the main menu, over the
// multiplayer page. Notices never take a click. Game thread only.
#pragma once
#include "match/hsm_hud_scene.h"
#include "match/hsm_notice.h"
#include "match/hsm_notice_text.h"
#include "ue/engine.h"
#include <string>

namespace hsm::ui {

// A notice (its time is now). A key replaces the notice with the same key; NoticeClear takes one down early. Logged once
// per change, as `notices: [kind] TITLE: text`.
void Notify(NoticeKind kind, const std::string& title, const std::string& text, const std::string& key = std::string());
void Notify(const hsm::notices::Msg& m);
void Notify(hsm::notices::Id id);
void NoticeClear(const std::string& key);

// Draws the stack and the kill feed on the UI_HUD while model.visible is true. Called by ChatTick, every frame, with the
// model the chat gets.
void NoticesTick(const HudScene& model);
// The HUD is gone (no session, a level change, the match ended): the stack and the feed hide.
void NoticesDrop();
// Draws the stack over the main menu `menu` while its multiplayer page is open (`panelOpen`). Called by the menu's tick.
void NoticesMenuTick(ue::UObject* menu, bool panelOpen);

}  // namespace hsm::ui
