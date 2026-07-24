// ─── On-device unit tests (Unity) ────────────────────────────────────────────
// Run against the REAL firmware code on an ESP32 — this project's logic is
// welded to hardware headers (LovyanGFX / WiFi / SPIFFS / ArduinoJson), so
// host-side testing would need an impractical fake layer. The [env:test]
// environment sets test_build_src (compiles src/ into this binary) and defines
// RUN_UNIT_TESTS so main.cpp renames its setup()/loop() and this file's runner
// takes over.
//
//   ~/.platformio/penv/bin/pio test -e test          (board connected)
//   ~/.platformio/penv/bin/pio test -e test --without-uploading --without-testing   (compile only)

#include <Arduino.h>
#include <unity.h>

#include "settings.h"          // gridToLatLon, classifyMode, tzFindByIana, SPOT_MODE_*
#include "screen_dxspots.h"    // callCountry

// Declared in ui_common.h, which pulls in LovyanGFX; forward-declare it here so
// this test stays a light translation unit. Defined in src/ui.cpp.
void fmtPrice(char* buf, size_t bufLen, float price);

// ─── gridToLatLon: Maidenhead locator → centre lat/lon ───────────────────────

void test_grid_six_char(void) {
    float lat = 0, lon = 0;
    gridToLatLon("IO92js", lat, lon);       // the project's default QTH
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 52.77f, lat);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -1.21f, lon);
}

void test_grid_four_char(void) {
    float lat = 0, lon = 0;
    gridToLatLon("IO92", lat, lon);         // 4-char = square centre
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 52.5f, lat);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -1.0f, lon);
}

void test_grid_western_hemisphere(void) {
    float lat = 0, lon = 0;
    gridToLatLon("FN20", lat, lon);         // north-eastern USA
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 40.5f, lat);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -75.0f, lon);
}

void test_grid_too_short_is_zeroed(void) {
    float lat = 9, lon = 9;
    gridToLatLon("IO", lat, lon);           // fewer than 4 chars → 0,0
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lat);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lon);
}

// ─── classifyMode: mode string → SPOT_MODE_* bit ─────────────────────────────

void test_mode_cw(void) {
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_CW, classifyMode("CW"));
}

void test_mode_case_insensitive(void) {
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_CW, classifyMode("cw"));
}

void test_mode_ssb_family(void) {
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_SSB, classifyMode("SSB"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_SSB, classifyMode("LSB"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_SSB, classifyMode("USB"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_SSB, classifyMode("FM"));
}

void test_mode_digital(void) {
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_FT8,  classifyMode("FT8"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_FT4,  classifyMode("FT4"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_RTTY, classifyMode("RTTY"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_RTTY, classifyMode("PSK31"));
}

void test_mode_unknown_and_empty(void) {
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_OTHER, classifyMode("HELLO"));
    TEST_ASSERT_EQUAL_UINT8(SPOT_MODE_OTHER, classifyMode(""));
}

// ─── tzFindByIana: IANA zone name → TZ_LIST index ────────────────────────────

void test_tz_direct_match(void) {
    TEST_ASSERT_EQUAL_INT(0, tzFindByIana("Europe/London"));
}

void test_tz_alias_match(void) {
    // Europe/Belfast is an alias that shares London's rules.
    TEST_ASSERT_EQUAL_INT(0, tzFindByIana("Europe/Belfast"));
}

void test_tz_utc(void) {
    int idx = tzFindByIana("UTC");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, idx);
    TEST_ASSERT_EQUAL_STRING("UTC0", TZ_LIST[idx].posix);
}

void test_tz_unknown_and_null(void) {
    TEST_ASSERT_EQUAL_INT(-1, tzFindByIana("Nowhere/Void"));
    TEST_ASSERT_EQUAL_INT(-1, tzFindByIana(""));
    TEST_ASSERT_EQUAL_INT(-1, tzFindByIana(nullptr));
}

// ─── callCountry: callsign prefix → DXCC entity ──────────────────────────────

void test_country_one_char_prefix(void) {
    TEST_ASSERT_EQUAL_STRING("England", callCountry("G4ABC"));
    TEST_ASSERT_EQUAL_STRING("USA",     callCountry("W1AW"));
}

void test_country_two_char_prefix(void) {
    TEST_ASSERT_EQUAL_STRING("Scotland", callCountry("GM3XYZ"));
    TEST_ASSERT_EQUAL_STRING("Germany",  callCountry("DL1ABC"));
}

void test_country_three_char_prefix_wins(void) {
    // "EA8" (Canary Is.) must beat the shorter "EA" (Spain).
    TEST_ASSERT_EQUAL_STRING("Canary Is.", callCountry("EA8AB"));
}

void test_country_strips_portable_suffix(void) {
    TEST_ASSERT_EQUAL_STRING("Scotland", callCountry("GM3XYZ/P"));
}

void test_country_unknown_is_empty(void) {
    TEST_ASSERT_EQUAL_STRING("", callCountry("QQ9QQ"));
}

// ─── fmtPrice: thousands separators ──────────────────────────────────────────

static void assert_price(const char* expected, float value) {
    char buf[24];
    fmtPrice(buf, sizeof(buf), value);
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

void test_price_no_separator_needed(void) {
    assert_price("999.50", 999.5f);
    assert_price("0.00",   0.0f);
}

void test_price_grouping(void) {
    assert_price("1,234.56",     1234.56f);      // 4-digit int part
    assert_price("5,123.45",     5123.45f);      // regression: was ",5123.45"
    assert_price("12,345.67",    12345.67f);     // 5-digit int part
    assert_price("1,234,568.00", 1234568.0f);    // 7-digit int part, two commas
    assert_price("10,000.00",    10000.0f);
}

// ─── Unity runner ────────────────────────────────────────────────────────────

void setUp(void)    {}
void tearDown(void) {}

void setup() {
    delay(2000);   // let the USB-serial link settle before Unity prints
    UNITY_BEGIN();

    RUN_TEST(test_grid_six_char);
    RUN_TEST(test_grid_four_char);
    RUN_TEST(test_grid_western_hemisphere);
    RUN_TEST(test_grid_too_short_is_zeroed);

    RUN_TEST(test_mode_cw);
    RUN_TEST(test_mode_case_insensitive);
    RUN_TEST(test_mode_ssb_family);
    RUN_TEST(test_mode_digital);
    RUN_TEST(test_mode_unknown_and_empty);

    RUN_TEST(test_tz_direct_match);
    RUN_TEST(test_tz_alias_match);
    RUN_TEST(test_tz_utc);
    RUN_TEST(test_tz_unknown_and_null);

    RUN_TEST(test_country_one_char_prefix);
    RUN_TEST(test_country_two_char_prefix);
    RUN_TEST(test_country_three_char_prefix_wins);
    RUN_TEST(test_country_strips_portable_suffix);
    RUN_TEST(test_country_unknown_is_empty);

    RUN_TEST(test_price_no_separator_needed);
    RUN_TEST(test_price_grouping);

    UNITY_END();
}

void loop() {}
