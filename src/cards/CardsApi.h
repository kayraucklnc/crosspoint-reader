#pragma once

class WebServer;

// JSON endpoints behind the web app's Cards panel (see CardStore.h).
//
//   GET  /api/cards                {screen: {width, height, band}, max, cards[]}
//   POST /api/cards                {id?, title, subtitle, payload}  add a QR card, or edit one
//   POST /api/cards/delete         {id}
//   POST /api/cards/order          {ids[]}  the full list in its new order
//   POST /api/cards/show           {id}     the device switches to this card
//   POST /api/cards/image          multipart file: a 1-bit BMP sized to the screen.
//                                  ?id= replaces that card's picture, else ?title= adds a card
//   GET  /api/cards/image?id=      the picture, for the editor's preview
// Card changes answer with the same body as GET /api/cards.
void registerCardsApi(WebServer& server);
