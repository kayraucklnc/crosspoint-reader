#pragma once

class WebServer;

// JSON endpoints behind the web app's Income, Trains, Wallpapers and Wi-Fi
// panels. Secrets are write-only: the Stripe key is never returned, only a hint.
//
//   GET  /api/income             {hasKey, keyHint}
//   POST /api/income/key         {key}          validate + store, drops the income cache
//   POST /api/income/key/delete
//   POST /api/income/test        {charges, balance}  HTTP status of one call each
//   GET  /api/trains             {stations[], origin, towards, home, favorites[]}
//   POST /api/trains             same fields (station indices)
//   GET  /api/wallpaper          {mode, fixed, files[]}
//   POST /api/wallpaper          {mode?, fixed?}
//   POST /api/wifi/order         {ssids[]}      saved-network priority
//   GET  /api/timezones          [{name, offset}]
//   GET  /api/device             {battery, charging?}
void registerWebAppApi(WebServer& server);
