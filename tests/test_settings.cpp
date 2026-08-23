// Suite `settings` — parse_settings / serialize_settings / Settings::normalise / validate.
//
// src/settings.cpp is 2409 lines and had **no tests at all** (progress.md §6 item 5).
// docs/editing.md §12 says adding the `editing` block "should not be the fifth thing added to it
// untested; adding it is a good occasion to write the first round-trip tests that file has ever
// had." This is that occasion, so the coverage here is deliberately not confined to the new
// block: the round-trip property is what makes every *other* field safe to add, and it is the
// one property that has never been checked.
//
// Nothing here touches the filesystem. `load_settings`/`save_settings` are thin wrappers over
// `parse_settings`/`serialize_settings` plus an atomic write, and the interesting mistakes are
// all on this side of that boundary.

#include "settings.h"

#include "kd_test.h"

namespace {

// A settings object deliberately unlike the defaults in every field the round trip has to carry.
// A round-trip test against defaults proves only that the parser's fallbacks agree with the
// struct's initialisers, which is the one case that cannot break.
kd::Settings unusual() {
    kd::Settings s;
    s.model_id = "parakeet-tdt-0.6b-v3";

    s.hotkey.vk = kd::vk::kF13 + 4;  // F17 — a binding that needs no Start-menu hack
    s.hotkey.mods = kd::kModNone;

    s.session.endpoint_silence_ms = 1234;
    s.session.session_silence_ms = 45678;
    s.session.session_silence_never = false;
    s.session.activation_grace_ms = 321;

    s.stability.hypotheses_n = 3;
    s.stability.retract_clusters_m = 5;
    s.stability.hold_ms_t = 275;
    s.stability.honour_server_stable_bytes = false;
    s.stability.cadence = kd::Cadence::EndOfSession;

    s.inject.chars_per_batch = 17;
    s.inject.inter_batch_delay_us = 2500;
    s.inject.refuse_password_fields = true;
    s.inject.refuse_elevated_targets = false;

    s.editing.enabled = true;
    s.editing.mouse = false;
    s.editing.on_foreign_key = kd::ForeignKeyAction::EndSession;
    s.editing.commit_vk = kd::vk::kReturn;

    s.overlay_mode = kd::OverlayMode::FloatingPill;
    s.overlay_style.font_family = "Cascadia Code";
    s.overlay_style.font_size_dip = 17.5f;
    s.overlay_style.theme = kd::ThemeMode::Custom;
    s.overlay_style.color_pending = 0xFF102030;
    s.overlay_style.color_unstable = 0x80405060;
    s.overlay_style.max_width_dip = 640.0f;

    s.anchor.min_confidence = 0.37f;
    s.anchor.below_threshold = kd::anchor::Fallback::NoPreview;

    s.audio_device_id = "{0.0.1.00000000}.{deadbeef}";
    s.log_level = kd::log::Level::Debug;
    s.diagnostics_file_enabled = true;
    s.start_with_windows = true;
    s.context_terms = {"llama.cpp", "Qwen3", "WASAPI"};
    s.send_context = true;

    s.normalise();
    return s;
}

kd::Settings round_trip(const kd::Settings& in, bool& ok) {
    const std::string text = kd::serialize_settings(in);
    kd::Result<kd::Settings> parsed = kd::parse_settings(text);
    ok = parsed.is_ok();
    if (!ok) {
        KD_INFO(("parse_settings rejected our own output: " + parsed.status().message()).c_str());
        return kd::Settings{};
    }
    return parsed.value();
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The round trip
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(settings, serialize_then_parse_preserves_every_field) {
    const kd::Settings in = unusual();
    bool ok = false;
    const kd::Settings out = round_trip(in, ok);
    KD_REQUIRE(ok);

    KD_CHECK_EQ(out.model_id, in.model_id);
    KD_CHECK_EQ(out.hotkey.to_display_string(), in.hotkey.to_display_string());

    KD_CHECK_EQ(out.session.endpoint_silence_ms, in.session.endpoint_silence_ms);
    KD_CHECK_EQ(out.session.session_silence_ms, in.session.session_silence_ms);
    KD_CHECK_EQ(out.session.session_silence_never, in.session.session_silence_never);
    KD_CHECK_EQ(out.session.activation_grace_ms, in.session.activation_grace_ms);

    KD_CHECK_EQ(out.stability.hypotheses_n, in.stability.hypotheses_n);
    KD_CHECK_EQ(out.stability.retract_clusters_m, in.stability.retract_clusters_m);
    KD_CHECK_EQ(out.stability.hold_ms_t, in.stability.hold_ms_t);
    KD_CHECK_EQ(out.stability.honour_server_stable_bytes, in.stability.honour_server_stable_bytes);
    KD_CHECK_EQ(static_cast<int>(out.stability.cadence), static_cast<int>(in.stability.cadence));

    KD_CHECK_EQ(out.inject.chars_per_batch, in.inject.chars_per_batch);
    KD_CHECK_EQ(out.inject.inter_batch_delay_us, in.inject.inter_batch_delay_us);
    KD_CHECK_EQ(out.inject.refuse_password_fields, in.inject.refuse_password_fields);
    KD_CHECK_EQ(out.inject.refuse_elevated_targets, in.inject.refuse_elevated_targets);

    KD_CHECK_EQ(static_cast<int>(out.overlay_mode), static_cast<int>(in.overlay_mode));
    KD_CHECK_EQ(out.overlay_style.font_family, in.overlay_style.font_family);
    KD_CHECK_NEAR(out.overlay_style.font_size_dip, in.overlay_style.font_size_dip, 0.001);
    KD_CHECK_EQ(static_cast<int>(out.overlay_style.theme),
                static_cast<int>(in.overlay_style.theme));
    KD_CHECK_EQ(out.overlay_style.color_pending, in.overlay_style.color_pending);
    KD_CHECK_EQ(out.overlay_style.color_unstable, in.overlay_style.color_unstable);
    KD_CHECK_NEAR(out.overlay_style.max_width_dip, in.overlay_style.max_width_dip, 0.001);

    KD_CHECK_NEAR(out.anchor.min_confidence, in.anchor.min_confidence, 0.0001);
    KD_CHECK_EQ(static_cast<int>(out.anchor.below_threshold),
                static_cast<int>(in.anchor.below_threshold));

    KD_CHECK_EQ(out.audio_device_id, in.audio_device_id);
    KD_CHECK_EQ(static_cast<int>(out.log_level), static_cast<int>(in.log_level));
    KD_CHECK_EQ(out.diagnostics_file_enabled, in.diagnostics_file_enabled);
    KD_CHECK_EQ(out.start_with_windows, in.start_with_windows);
    KD_CHECK_EQ(out.context_terms.size(), in.context_terms.size());
    KD_CHECK_EQ(out.send_context, in.send_context);
}

KD_TEST(settings, the_round_trip_is_a_fixed_point) {
    // Serialising the *parsed* copy must produce byte-identical text. Without this, a field that
    // parses into something slightly different from what it was written from — a colour that
    // loses its alpha, an enum that falls back to a default — passes the field-by-field check
    // above only for the fields somebody remembered to list.
    const kd::Settings in = unusual();
    const std::string first = kd::serialize_settings(in);
    kd::Result<kd::Settings> parsed = kd::parse_settings(first);
    KD_REQUIRE(parsed.is_ok());
    const std::string second = kd::serialize_settings(parsed.value());
    KD_CHECK_EQ(first, second);
}

KD_TEST(settings, defaults_round_trip_and_validate) {
    kd::Settings s;
    s.normalise();
    KD_CHECK(s.validate().is_ok());
    bool ok = false;
    const kd::Settings out = round_trip(s, ok);
    KD_REQUIRE(ok);
    KD_CHECK(out.validate().is_ok());
    KD_CHECK_EQ(kd::serialize_settings(out), kd::serialize_settings(s));
}

KD_TEST(settings, an_empty_object_is_a_first_run_not_an_error) {
    // A missing key means "the user did not set it", which is the only reading that lets a new
    // field be added without invalidating every settings file already on disk.
    kd::Result<kd::Settings> parsed = kd::parse_settings("{}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK_EQ(parsed.value().overlay_style.font_family, std::string("Segoe UI"));
}

KD_TEST(settings, malformed_json_is_reported_not_silently_defaulted) {
    // settings.h: "silently discarding a user's configuration is indistinguishable from losing
    // it." Every one of these must come back as an error rather than as defaults.
    KD_CHECK(!kd::parse_settings("").is_ok());
    KD_CHECK(!kd::parse_settings("[]").is_ok());
    KD_CHECK(!kd::parse_settings("{\"overlay\": {\"color_pending\": \"#ZZ\"}}").is_ok());
    KD_CHECK(!kd::parse_settings("{\"overlay\": {\"mode\": \"sideways\"}}").is_ok());
    KD_CHECK(!kd::parse_settings("{\"session\": {\"endpoint_silence_ms\": 5}}").is_ok());

    // A *section* of the wrong type is a gap, found by writing this file and recorded rather
    // than fixed here: `Obj::member` resolves through json::Value::find, which answers nullptr
    // for anything that is not an object, so every key in the section reads as "the user did not
    // set it" and the whole block silently reverts to defaults. That is the outcome settings.h
    // calls "indistinguishable from losing it". Fixing it is a change to how every section is
    // parsed, not to the editing block docs/editing.md asked for, so it is reported instead.
    KD_INFO("known gap: a section of the wrong type (\"session\": 5) is silently defaulted");
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The editing block — docs/editing.md §12
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(settings, the_editing_block_round_trips) {
    kd::Settings s = unusual();
    s.editing.enabled = true;
    s.editing.mouse = false;
    s.editing.on_foreign_key = kd::ForeignKeyAction::EndSession;
    s.normalise();
    KD_REQUIRE(s.editing.enabled);  // the cadence is end-of-session, so it survives normalise()

    bool ok = false;
    const kd::Settings out = round_trip(s, ok);
    KD_REQUIRE(ok);
    KD_CHECK_EQ(out.editing.enabled, true);
    KD_CHECK_EQ(out.editing.mouse, false);
    KD_CHECK_EQ(static_cast<int>(out.editing.on_foreign_key),
                static_cast<int>(kd::ForeignKeyAction::EndSession));
    KD_CHECK_EQ(out.editing.commit_vk, kd::vk::kReturn);
}

KD_TEST(settings, editing_is_forced_off_unless_the_cadence_is_end_of_session) {
    // docs/editing.md §6.1 and §16's second bullet: this is the exact shape of the
    // `min_confidence` bug — a setting that parses, validates and round-trips, and does nothing,
    // because the code that would honour it is never reached. normalise() is where the answer is
    // decided, so that no downstream reader has to re-derive it and disagree.
    for (kd::Cadence c : {kd::Cadence::Immediate, kd::Cadence::Periodic}) {
        kd::Settings s;
        s.stability.cadence = c;
        s.editing.enabled = true;
        s.normalise();
        KD_CHECK(!s.editing.enabled);
    }

    kd::Settings s;
    s.stability.cadence = kd::Cadence::EndOfSession;
    s.editing.enabled = true;
    s.normalise();
    KD_CHECK(s.editing.enabled);
}

KD_TEST(settings, editing_forced_off_survives_a_round_trip) {
    // The serialised file records `false`, so re-reading it cannot resurrect the feature under a
    // cadence that has nothing for it to edit.
    kd::Settings s;
    s.stability.cadence = kd::Cadence::Immediate;
    s.editing.enabled = true;
    s.normalise();

    bool ok = false;
    const kd::Settings out = round_trip(s, ok);
    KD_REQUIRE(ok);
    KD_CHECK(!out.editing.enabled);
}

KD_TEST(settings, a_settings_file_that_names_a_cadence_can_still_turn_editing_on) {
    kd::Result<kd::Settings> parsed = kd::parse_settings(
        "{\"cadence\": {\"mode\": \"end-of-session\"},"
        " \"editing\": {\"enabled\": true, \"mouse\": true,"
        " \"on_foreign_key\": \"ignore\", \"commit_key\": \"enter\"}}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(parsed.value().editing.enabled);
    KD_CHECK_EQ(parsed.value().editing.commit_vk, kd::vk::kReturn);
    KD_CHECK_EQ(static_cast<int>(parsed.value().editing.on_foreign_key),
                static_cast<int>(kd::ForeignKeyAction::Ignore));
}

KD_TEST(settings, a_bad_editing_value_is_named_rather_than_ignored) {
    kd::Result<kd::Settings> a =
        kd::parse_settings("{\"editing\": {\"on_foreign_key\": \"swallow\"}}");
    KD_CHECK(!a.is_ok());
    KD_CHECK(a.status().message().find("on_foreign_key") != std::string::npos);

    kd::Result<kd::Settings> b = kd::parse_settings("{\"editing\": {\"commit_key\": \"Ctrl+S\"}}");
    KD_CHECK(!b.is_ok());
    KD_CHECK(b.status().message().find("commit_key") != std::string::npos);

    kd::Result<kd::Settings> c = kd::parse_settings("{\"editing\": {\"enabled\": \"yes\"}}");
    KD_CHECK(!c.is_ok());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  validate()
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(settings, speculative_unsafe_cannot_be_reached_from_a_file) {
    // plan.md §1.4: "named unsafe, excluded from defaults, and never shipped."
    //
    // Two independent defences, and the test says which is which because they fail differently.
    // validate() refuses a Settings that carries the flag — that is the one settings.h documents.
    // The parser is the stronger of the two: it has no reader for the key at all, so a file that
    // sets it produces a Settings with the flag still false and never reaches validate() to be
    // refused. Asserting an error here would therefore have been asserting the wrong thing.
    kd::Settings s;
    s.stability.speculative_unsafe = true;
    KD_CHECK(!s.validate().is_ok());

    kd::Result<kd::Settings> parsed =
        kd::parse_settings("{\"stability\": {\"speculative_unsafe\": true}}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(!parsed.value().stability.speculative_unsafe);
    KD_CHECK(kd::serialize_settings(parsed.value()).find("speculative_unsafe") ==
             std::string::npos);
}

KD_TEST(settings, a_session_timer_below_the_endpoint_timer_is_refused) {
    // The session would end before an utterance could ever finish. plan.md §1.6 keeps the two
    // timers unrelated; this is the one relationship worth enforcing.
    kd::Settings s;
    s.session.endpoint_silence_ms = 5000;
    s.session.session_silence_ms = 2000;
    s.session.session_silence_never = false;
    KD_CHECK(!s.validate().is_ok());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The pill's colour scheme, and the file that predates it
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(settings, a_file_with_no_theme_and_default_colours_follows_windows) {
    // The overwhelmingly common upgrade: a settings file written before `theme` existed, whose
    // colours are the ones this program shipped. That is not a choice anyone made, so it becomes
    // "follow the system" — which is the whole point of the feature.
    kd::Result<kd::Settings> parsed =
        kd::parse_settings("{\"overlay\": {\"mode\": \"inline\", \"font_family\": \"Segoe UI\"}}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(parsed.value().overlay_style.theme == kd::ThemeMode::System);
}

KD_TEST(settings, a_file_with_hand_picked_colours_and_no_theme_keeps_them) {
    // The migration that matters. Someone set a colour; a new key must not quietly repaint over
    // it. The file predates `theme`, so it is read as `custom` and paints exactly what it did.
    kd::Result<kd::Settings> parsed =
        kd::parse_settings("{\"overlay\": {\"color_background\": \"#FF001122\"}}");
    KD_REQUIRE(parsed.is_ok());
    const kd::Settings& s = parsed.value();
    KD_CHECK(s.overlay_style.theme == kd::ThemeMode::Custom);
    KD_CHECK_EQ(s.overlay_style.color_background, 0xFF001122u);
    // And what the pill will actually paint with, which is the point of keeping them: the
    // user's colour, whatever the desktop is doing.
    KD_CHECK_EQ(kd::resolve_overlay_style(s.overlay_style, true).color_background, 0xFF001122u);
}

KD_TEST(settings, a_file_that_names_its_theme_is_never_migrated) {
    // The other side of the same rule: once `theme` is in the file it is the answer, even when
    // the colours beside it are not the defaults. Without this, a user who chose "follow
    // Windows" *and* kept a custom palette to switch back to would be dragged into `custom` on
    // every load.
    kd::Result<kd::Settings> parsed = kd::parse_settings(
        "{\"overlay\": {\"theme\": \"system\", \"color_background\": \"#FF001122\"}}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(parsed.value().overlay_style.theme == kd::ThemeMode::System);
    KD_CHECK_EQ(parsed.value().overlay_style.color_background, 0xFF001122u);
}

KD_TEST(settings, every_theme_name_round_trips_and_an_unknown_one_is_refused) {
    struct Case {
        const char* text;
        kd::ThemeMode mode;
    };
    for (const Case& c : {Case{"system", kd::ThemeMode::System}, Case{"light", kd::ThemeMode::Light},
                          Case{"dark", kd::ThemeMode::Dark},
                          Case{"custom", kd::ThemeMode::Custom}}) {
        const std::string text =
            std::string("{\"overlay\": {\"theme\": \"") + c.text + "\"}}";
        kd::Result<kd::Settings> parsed = kd::parse_settings(text);
        KD_REQUIRE(parsed.is_ok());
        KD_CHECK(parsed.value().overlay_style.theme == c.mode);
        // And the spelling survives being written back out, which is what keeps a settings file
        // stable across a save the user did not ask for.
        KD_CHECK(kd::serialize_settings(parsed.value()).find(std::string("\"") + c.text + "\"") !=
                 std::string::npos);
    }

    // Named rather than silently defaulted, like every other enum in this file.
    kd::Result<kd::Settings> bad = kd::parse_settings("{\"overlay\": {\"theme\": \"midnight\"}}");
    KD_CHECK(!bad.is_ok());
    KD_CHECK(bad.status().message().find("theme") != std::string::npos);
}
