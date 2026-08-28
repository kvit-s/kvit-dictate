// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
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

    // The cadence above is end-of-session, which is the only one under which this may be on.
    s.normalizer.enabled = true;
    s.normalizer.url = "http://strixhalo.lan:10301/v1/text/normalize";
    s.normalizer.timeout_ms = 2500;
    s.normalizer.max_bytes = 3000;
    s.normalizer.styling = "formal";
    s.normalizer.structure = "lists";
    s.normalizer.context = "email";

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

    KD_CHECK_EQ(out.normalizer.enabled, in.normalizer.enabled);
    KD_CHECK_EQ(out.normalizer.url, in.normalizer.url);
    KD_CHECK_EQ(out.normalizer.timeout_ms, in.normalizer.timeout_ms);
    KD_CHECK_EQ(out.normalizer.max_bytes, in.normalizer.max_bytes);
    KD_CHECK_EQ(out.normalizer.styling, in.normalizer.styling);
    KD_CHECK_EQ(out.normalizer.structure, in.normalizer.structure);
    KD_CHECK_EQ(out.normalizer.context, in.normalizer.context);
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

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The normalizer block (normalizer.h)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(settings, the_normalizer_is_forced_off_unless_the_cadence_is_end_of_session) {
    // The same forcing `editing.enabled` gets, for a harder reason: a rewrite is not
    // prefix-stable, so under a cadence that has already typed the text, applying one would mean
    // retracting bytes from the user's document (plan.md §1.4).
    for (kd::Cadence c : {kd::Cadence::Immediate, kd::Cadence::Periodic}) {
        kd::Settings s;
        s.stability.cadence = c;
        s.normalizer.enabled = true;
        s.normalizer.url = "http://strixhalo.lan:10301/v1/text/normalize";
        s.normalise();
        KD_CHECK(!s.normalizer.enabled);
        // ...and forced, not rejected: a user who switches cadence back gets a working program,
        // not a settings file that refuses to load.
        KD_CHECK(s.validate().ok());
    }
}

KD_TEST(settings, the_normalizer_protects_the_terms_the_user_already_maintains) {
    // Derived in normalise(), exactly as session.context is. One list to edit, two things
    // protected — the recogniser's vocabulary and the rewriter's floor.
    kd::Settings s;
    s.stability.cadence = kd::Cadence::EndOfSession;
    s.context_terms = {"llama.cpp", "strixhalo"};
    s.normalise();
    KD_REQUIRE_EQ(s.normalizer.protect_terms.size(), size_t{2});
    KD_CHECK_EQ(s.normalizer.protect_terms[1], std::string("strixhalo"));
}

KD_TEST(settings, the_derived_term_list_is_not_persisted) {
    // A derived field written to the file is how two answers to one question come to exist. The
    // terms live in `context`; the normalizer block must not carry a stale copy.
    kd::Settings s;
    s.stability.cadence = kd::Cadence::EndOfSession;
    s.context_terms = {"llama.cpp"};
    s.normalise();
    const std::string text = kd::serialize_settings(s);
    KD_CHECK(text.find("protect_terms") == std::string::npos);
}

KD_TEST(settings, an_untrained_control_value_is_named_rather_than_ignored) {
    // The endpoint answers an untrained control with 400 rather than sending it to the model,
    // which invents text for one. The settings file is where that should be caught.
    kd::Result<kd::Settings> parsed = kd::parse_settings(R"({
        "cadence": { "mode": "end-of-session" },
        "normalizer": { "enabled": true,
                        "url": "http://strixhalo.lan:10301/v1/text/normalize",
                        "styling": "breezy" }
    })");
    KD_REQUIRE(!parsed.is_ok());
    KD_CHECK(parsed.status().message().find("styling") != std::string::npos);
}

KD_TEST(settings, a_file_that_enables_the_normalizer_under_end_of_session_loads) {
    kd::Result<kd::Settings> parsed = kd::parse_settings(R"({
        "cadence": { "mode": "end-of-session" },
        "normalizer": { "enabled": true,
                        "url": "http://strixhalo.lan:10301/v1/text/normalize",
                        "timeout_ms": 2000 }
    })");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(parsed->normalizer.enabled);
    KD_CHECK_EQ(parsed->normalizer.timeout_ms, uint32_t{2000});
}

KD_TEST(settings, a_file_with_no_normalizer_block_leaves_it_off) {
    // plan.md §1.9: enabling this sends every dictated word to another machine. A first run, or
    // any file written before this feature existed, must not turn it on.
    kd::Result<kd::Settings> parsed = kd::parse_settings("{}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(!parsed->normalizer.enabled);
}

// ─── The server address (release-plan.md §2.2) ──────────────────────────────────────────────

KD_TEST(settings, a_file_with_no_server_block_has_no_server) {
    // docs/protocol.md §10.6: this is what a fresh install and every pre-existing settings file
    // look like, and it is a state rather than an error.
    kd::Result<kd::Settings> parsed = kd::parse_settings("{}");
    KD_REQUIRE(parsed.is_ok());
    KD_CHECK(!parsed->server.configured());
    KD_CHECK(parsed->server.url.empty());
    KD_CHECK(parsed->validate().is_ok());
}

KD_TEST(settings, server_round_trips) {
    kd::Settings s;
    s.server.url = "ws://strixhalo.lan:10402/kvit-dictate/v1";
    s.server.model = "parakeet-tdt";
    s.server.connect_timeout_ms = 9000;
    s.server.reconnect_initial_ms = 500;
    s.server.reconnect_max_ms = 60000;
    s.normalise();

    kd::Result<kd::Settings> back = kd::parse_settings(kd::serialize_settings(s));
    KD_REQUIRE(back.is_ok());
    KD_CHECK_EQ(back->server.url, s.server.url);
    KD_CHECK_EQ(back->server.model, s.server.model);
    KD_CHECK_EQ(back->server.connect_timeout_ms, uint32_t{9000});
    KD_CHECK_EQ(back->server.reconnect_initial_ms, uint32_t{500});
    KD_CHECK_EQ(back->server.reconnect_max_ms, uint32_t{60000});
    KD_CHECK(back->server.configured());
}

KD_TEST(settings, an_empty_url_is_written_rather_than_omitted) {
    // A present-but-blank key says "there is no server yet"; an absent one is indistinguishable
    // from a file written by an older build, and the first-run path has to tell those apart.
    const std::string text = kd::serialize_settings(kd::Settings{});
    KD_CHECK(text.find("\"server\"") != std::string::npos);
    KD_CHECK(text.find("\"url\"") != std::string::npos);
}

KD_TEST(settings, a_malformed_url_is_rejected_by_validate_not_at_connect_time) {
    // The alternative is a dialog that accepts the typo and a tray icon that goes red minutes
    // later, in a different part of the screen, with no obvious connection to what was typed.
    kd::Settings s;
    s.server.url = "strixhalo.lan:10402";  // no scheme
    KD_CHECK(!s.validate().is_ok());
    KD_CHECK(s.validate().message().find("server.url") != std::string::npos);

    s.server.url = "ws://box.lan@evil.example.com/";
    KD_CHECK(!s.validate().is_ok());
}

KD_TEST(settings, a_settings_file_carrying_a_bad_url_fails_to_load) {
    // parse_settings() runs validate() before it returns, so a URL that would never connect is
    // refused at load rather than surviving to the tray. The message names the field, because
    // the user has to find it in a file they may have edited by hand.
    kd::Result<kd::Settings> parsed = kd::parse_settings(R"({
        "server": { "url": "ws://host:0/" }
    })");
    KD_REQUIRE(!parsed.is_ok());
    KD_CHECK(parsed.status().message().find("server.url") != std::string::npos);
}

KD_TEST(settings, reconnect_bounds_must_be_ordered) {
    kd::Settings s;
    s.server.reconnect_initial_ms = 30000;
    s.server.reconnect_max_ms = 1000;
    KD_CHECK(!s.validate().is_ok());
}

KD_TEST(settings, out_of_range_server_numbers_are_a_load_error) {
    kd::Result<kd::Settings> parsed = kd::parse_settings(R"({
        "server": { "url": "ws://h:1/", "connect_timeout_ms": 1 }
    })");
    KD_CHECK(!parsed.is_ok());
}

// ─── server.mode (openai-translator.md §4.6) ────────────────────────────────────────────────
//
// The field exists because the two things a user might type into one box are indistinguishable
// by inspection and catastrophically different in effect: `parse_server_url` treats `http://`
// as a **WebSocket synonym** (docs §2.6.1), so a speech-service address stored without a mode
// would be handed to `IpcClient::connect` and upgraded. Everything below is about keeping that
// from happening to a settings file written before the translator existed.

KD_TEST(settings, a_file_without_a_mode_keeps_its_websocket_server) {
    // The migration that must not surprise anybody: a user who already pointed the app at a
    // Python server keeps that, with no dialog and nothing spawned.
    kd::Result<kd::Settings> s =
        kd::parse_settings(R"({"schema":1,"server":{"url":"ws://mybox.lan:10402/"}})");
    KD_REQUIRE(s.is_ok());
    KD_CHECK(s->server.mode == kd::ServerMode::Protocol);
    KD_CHECK_EQ(s->server.url, std::string("ws://mybox.lan:10402/"));

    kd::Result<kd::Settings> secure =
        kd::parse_settings(R"({"schema":1,"server":{"url":"wss://mybox.lan/asr"}})");
    KD_REQUIRE(secure.is_ok());
    KD_CHECK(secure->server.mode == kd::ServerMode::Protocol);
}

KD_TEST(settings, a_file_without_a_mode_and_without_a_url_is_a_first_run) {
    kd::Result<kd::Settings> s = kd::parse_settings(R"({"schema":1,"server":{"url":""}})");
    KD_REQUIRE(s.is_ok());
    KD_CHECK(s->server.mode == kd::ServerMode::Local);
    KD_CHECK(!s->server.configured());
}

KD_TEST(settings, an_http_url_without_a_mode_becomes_local_rather_than_being_upgraded) {
    // The ambiguous case, and the one this enum exists for. An older build would have treated
    // this as a WebSocket synonym; there is now somewhere better for it to go, and feeding it to
    // IpcClient::connect is the exact failure openai-translator.md was written about.
    kd::Result<kd::Settings> s =
        kd::parse_settings(R"({"schema":1,"server":{"url":"http://strixhalo.lan:10301"}})");
    KD_REQUIRE(s.is_ok());
    KD_CHECK(s->server.mode == kd::ServerMode::Local);
    KD_CHECK(s->validate().ok());
}

KD_TEST(settings, an_explicit_mode_is_never_second_guessed) {
    // Somebody who genuinely runs a protocol server behind an http:// URL — the synonym docs
    // §2.6.1 permits — must be able to say so and be believed.
    kd::Result<kd::Settings> s = kd::parse_settings(
        R"({"schema":1,"server":{"mode":"protocol","url":"http://mybox.lan:10402/"}})");
    KD_REQUIRE(s.is_ok());
    KD_CHECK(s->server.mode == kd::ServerMode::Protocol);
    KD_CHECK(s->validate().ok());
}

KD_TEST(settings, an_unknown_mode_is_named_rather_than_ignored) {
    kd::Result<kd::Settings> s =
        kd::parse_settings(R"({"schema":1,"server":{"mode":"cloud","url":""}})");
    KD_REQUIRE(!s.is_ok());
    KD_CHECK(s.status().message().find("mode") != std::string::npos);
    KD_CHECK(s.status().message().find("cloud") != std::string::npos);
}

KD_TEST(settings, the_mode_survives_a_round_trip_and_the_schema_did_not_move) {
    kd::Settings in;
    in.server.mode = kd::ServerMode::Local;
    in.server.url = "https://asr.internal:8443";
    in.server.live_preview = true;

    kd::Result<kd::Settings> back = kd::parse_settings(kd::serialize_settings(in));
    KD_REQUIRE(back.is_ok());
    KD_CHECK(back->server.mode == kd::ServerMode::Local);
    KD_CHECK_EQ(back->server.url, in.server.url);
    KD_CHECK(back->server.live_preview);
    // Deliberately not bumped: every file written without `mode` can be interpreted correctly
    // from the URL it already holds, and a bump would make those files unreadable for a field
    // that needs no migration dialog.
    KD_CHECK_EQ(back->schema, 1);
}

KD_TEST(settings, validation_uses_the_parser_the_mode_names) {
    // The heart of it. The same URL is valid in one mode and rejected in the other, and it must
    // be rejected *here*, in the dialog, rather than at connect time in the tray.
    kd::Settings local;
    local.server.mode = kd::ServerMode::Local;
    local.server.url = "ws://127.0.0.1:10402/";
    KD_CHECK(!local.validate().ok());

    kd::Settings protocol;
    protocol.server.mode = kd::ServerMode::Protocol;
    protocol.server.url = "ws://127.0.0.1:10402/";
    KD_CHECK(protocol.validate().ok());

    kd::Settings speech;
    speech.server.mode = kd::ServerMode::Local;
    speech.server.url = "http://strixhalo.lan:10301";
    KD_CHECK(speech.validate().ok());
}

KD_TEST(settings, the_speech_service_guard_says_what_is_wrong) {
    KD_CHECK(kd::validate_speech_service_url("http://strixhalo.lan:10301").empty());
    KD_CHECK(kd::validate_speech_service_url("https://asr.example.com/v1").empty());
    KD_CHECK(kd::validate_speech_service_url("").empty());  // the first-run state

    // The most likely mistake gets a sentence naming the other radio, not a syntax complaint.
    const std::string ws = kd::validate_speech_service_url("ws://127.0.0.1:10402/");
    KD_CHECK(!ws.empty());
    KD_CHECK(ws.find("I run my own kvit-dictate server") != std::string::npos);

    KD_CHECK(!kd::validate_speech_service_url("strixhalo.lan:10301").empty());
    // The characters that would have to be quoted onto `kvit-asrd --service`. Rejected in the
    // dialog rather than at spawn time, because the user is looking at the field now.
    KD_CHECK(!kd::validate_speech_service_url("http://host x").empty());
    KD_CHECK(!kd::validate_speech_service_url("http://host\"x").empty());
    KD_CHECK(!kd::validate_speech_service_url("http://host\nx").empty());
}

KD_TEST(settings, a_websocket_url_in_local_mode_is_reconciled_rather_than_refused) {
    // A settings object built programmatically — by the bench, or by a caller that set a URL and
    // forgot the mode — must still round-trip. `kvit-asrd` speaks HTTP and nothing else, so a
    // `ws://` address in Local mode is never meaningful and the URL is the half to believe.
    kd::Settings s;
    s.server.url = "ws://mybox.lan:10402/";  // mode left at its default, Local
    s.normalise();
    KD_CHECK(s.server.mode == kd::ServerMode::Protocol);
    KD_CHECK(s.validate().ok());

    kd::Result<kd::Settings> back = kd::parse_settings(kd::serialize_settings(s));
    KD_REQUIRE(back.is_ok());
    KD_CHECK(back->server.mode == kd::ServerMode::Protocol);
}

KD_TEST(settings, an_http_url_in_protocol_mode_is_left_alone) {
    // The reverse is not reconciled: docs §2.6.1 makes `http://` a legal WebSocket synonym, and
    // somebody who ticked "I run my own server" and typed one meant it.
    kd::Settings s;
    s.server.mode = kd::ServerMode::Protocol;
    s.server.url = "http://mybox.lan:10402/";
    s.normalise();
    KD_CHECK(s.server.mode == kd::ServerMode::Protocol);
    KD_CHECK(s.validate().ok());
}
