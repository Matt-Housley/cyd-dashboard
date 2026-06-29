// Shim: Arduino-ESP32 3.x renamed WiFiClient → NetworkClient.
// Our platform (Arduino-ESP32 2.x / IDF 4.x) still uses WiFiClient.
// ESP32-audioI2S 3.x includes <NetworkClient.h>; this file satisfies that
// include and provides the type alias so the rest of the library compiles.
#pragma once
#include <WiFiClient.h>
using NetworkClient = WiFiClient;
