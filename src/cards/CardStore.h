#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Pocket Cards: full-screen cards the device shows on demand (a QR code for
// any text or link with a name under it, or a picture such as a boarding
// pass). Edited from a phone through the web app; kept on the SD card in
// /.crosspoint/cards/ (cards.json plus one <id>.bmp per picture card).
//
// The web API (loop task) and the Cards screen's render task share the store,
// so every access goes through the mutex and readers take copies.
namespace cards {

enum class CardType : uint8_t { Qr, Image };

struct Card {
  std::string id;  // 8 hex chars; names the picture file
  CardType type = CardType::Qr;
  std::string title;     // shown under a QR code (picture cards bake theirs in)
  std::string subtitle;  // second, smaller line under the title
  std::string payload;   // QR content: a link, text, vCard, Wi-Fi config...
};

constexpr size_t MAX_CARDS = 24;
constexpr size_t MAX_PAYLOAD = 1200;
constexpr size_t MAX_TITLE = 80;
// Top strip kept clear of card content for the lock indicator and page count.
constexpr int STATUS_BAND = 22;

class CardStore {
 public:
  static CardStore& instance();

  // Reads cards.json; a missing or broken file leaves an empty list.
  void load();
  // load() on first use only (the web app can be opened from File Transfer too).
  void ensureLoaded();

  size_t count() const;
  bool cardAt(size_t index, Card& out) const;
  int indexOf(const std::string& id) const;
  std::vector<Card> all() const;

  // Each returns false (store unchanged) on invalid input or a failed write.
  bool add(Card card, std::string& idOut);
  bool update(const Card& card);
  bool remove(const std::string& id);
  bool reorder(const std::vector<std::string>& ids);

  // Bumped on every change, so the screen knows to redraw.
  uint32_t revision() const;

  // A card the phone asked the device to show; -1 when none is pending.
  void requestShow(int index);
  int takeShowRequest();

  // Portrait panel size, recorded by the Cards screen for the phone's image
  // conversion (pictures are uploaded already sized to it).
  void setScreenSize(int width, int height);
  int screenWidth() const;
  int screenHeight() const;

  static std::string imagePath(const std::string& id);
  static bool validId(const std::string& id);

 private:
  CardStore() = default;
  bool saveLocked() const;
  std::string newIdLocked() const;

  mutable std::mutex mutex;
  std::vector<Card> cardList;
  uint32_t rev = 0;
  bool loaded = false;
  int pendingShow = -1;
  int width = 0;
  int height = 0;
};

}  // namespace cards

#define CARD_STORE cards::CardStore::instance()
