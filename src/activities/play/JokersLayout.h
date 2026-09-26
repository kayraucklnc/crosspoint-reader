#pragma once

// Noir Jokers screen geometry for the 528x792 portrait panel, top to bottom.
namespace jokers::layout {

constexpr int SIDE = 16;
constexpr int HEADER_H = 64;         // black title band
constexpr int SCORE_BAND_END = 112;  // round score and progress bar
constexpr int JOKER_Y = 118;
constexpr int JOKER_STEP = 90;
constexpr int INFO_Y = 236;  // selected hand or joker details
constexpr int HAND_Y = 322;  // two rows of four cards
constexpr int HAND_ROW_GAP = 20;
constexpr int HAND_STEP = 126;
constexpr int LIFT = 16;  // selected cards rise this far
constexpr int ACTIONS_Y = 680;
constexpr int ACTIONS_H = 44;
constexpr int FOOTER_Y = 728;
constexpr int RING = 3;
constexpr int PILL_H = 26;
constexpr int REVEAL_NAME_Y = 268;
constexpr int REVEAL_CARDS_Y = 316;
// The theme's X3 side-button hints sit at y 155..235 on both edges.
constexpr int SIDE_HINT_Y = 150;
constexpr int SIDE_HINT_H = 90;
constexpr int SIDE_HINT_W = 38;

}  // namespace jokers::layout
