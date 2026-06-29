// Shim: Arduino-ESP32 3.x renamed WiFiClientSecure → NetworkClientSecure.
// Our platform (Arduino-ESP32 2.x / IDF 4.x) still uses WiFiClientSecure.
// ESP32-audioI2S 3.x includes <NetworkClientSecure.h>; this file satisfies
// that include and provides the type alias so the rest of the library compiles.
#pragma once
#include <WiFiClientSecure.h>
using NetworkClientSecure = WiFiClientSecure;
