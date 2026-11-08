// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The dictation history page — see history.h for why it is a view over the corpus.

#define KD_LOG_MODULE "history"

#include "history.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include "util/json.h"
#include "util/log.h"

namespace kd {
namespace {

// A session's events file is tens of kilobytes; the largest in a 900-session corpus is 59 KB.
// The bound is only there so a stray multi-gigabyte file in the corpus folder is skipped rather
// than read into memory.
constexpr uintmax_t kMaxEventsBytes = 64ull * 1024 * 1024;

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

// "20261108-005539-s175" -> "2026-11-08T00:55:39", for a session whose start line is missing.
std::string started_from_folder(std::string_view session) {
    if (session.size() < 15 || session[8] != '-') return {};
    for (size_t i = 0; i < 15; ++i) {
        if (i != 8 && (session[i] < '0' || session[i] > '9')) return {};
    }
    std::string s;
    s.append(session.substr(0, 4)).append("-").append(session.substr(4, 2)).append("-");
    s.append(session.substr(6, 2)).append("T").append(session.substr(9, 2)).append(":");
    s.append(session.substr(11, 2)).append(":").append(session.substr(13, 2));
    return s;
}

struct Ymd {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
};

bool parse_ymd(std::string_view started, Ymd& out) {
    if (started.size() < 10 || started[4] != '-' || started[7] != '-') return false;
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (std::sscanf(std::string(started.substr(0, 10)).c_str(), "%4d-%2u-%2u", &y, &m, &d) != 3) {
        return false;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    out = {y, m, d};
    return true;
}

constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",
                                   "June",    "July",     "August",    "September",
                                   "October", "November", "December"};
constexpr const char* kWeekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};

// "Sunday 8 November 2026".
std::string day_heading(std::string_view started) {
    Ymd v;
    if (!parse_ymd(started, v)) return "Undated";
    const std::chrono::year_month_day ymd{std::chrono::year{v.y}, std::chrono::month{v.m},
                                          std::chrono::day{v.d}};
    if (!ymd.ok()) return "Undated";
    const unsigned wd = std::chrono::weekday{std::chrono::sys_days{ymd}}.c_encoding();
    return std::string(kWeekdays[wd]) + " " + std::to_string(v.d) + " " + kMonths[v.m - 1] + " " +
           std::to_string(v.y);
}

// "8 Nov 2026".
std::string short_date(std::string_view started) {
    Ymd v;
    if (!parse_ymd(started, v)) return {};
    return std::to_string(v.d) + " " + std::string(kMonths[v.m - 1]).substr(0, 3) + " " +
           std::to_string(v.y);
}

std::string clock_of(std::string_view started) {
    return started.size() >= 16 ? std::string(started.substr(11, 5)) : std::string();
}

std::string duration_of(uint64_t ms) {
    const uint64_t s = (ms + 500) / 1000;
    if (s < 60) return std::to_string(s) + " s";
    return std::to_string(s / 60) + " min " + std::to_string(s % 60) + " s";
}

// "Code.exe" -> "Code".
std::string app_label(std::string_view target) {
    if (target.size() > 4) {
        std::string tail(target.substr(target.size() - 4));
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".exe") return std::string(target.substr(0, target.size() - 4));
    }
    return std::string(target);
}

std::string local_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

constexpr const char* kStyle = R"(
:root { color-scheme: light dark; --fg:#1f2328; --muted:#656d76; --line:#d8dee4; --bg:#ffffff;
        --card:#f6f8fa; --accent:#0969da; }
@media (prefers-color-scheme: dark) {
  :root { --fg:#e6edf3; --muted:#8d96a0; --line:#30363d; --bg:#0d1117; --card:#161b22;
          --accent:#4493f8; } }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:15px/1.5 "Segoe UI", system-ui, sans-serif; }
header { position:sticky; top:0; background:var(--bg); border-bottom:1px solid var(--line);
         padding:14px 0 12px; z-index:1; }
.wrap { max-width:900px; margin:0 auto; padding:0 20px; }
h1 { font-size:20px; margin:0 0 2px; font-weight:600; }
.sub { color:var(--muted); font-size:13px; }
.bar { display:flex; gap:12px; align-items:center; margin-top:10px; }
#q { flex:1; font:inherit; padding:7px 10px; border:1px solid var(--line); border-radius:6px;
     background:var(--card); color:var(--fg); }
#count { color:var(--muted); font-size:13px; white-space:nowrap; }
h2 { font-size:14px; font-weight:600; color:var(--muted); margin:22px 0 8px; }
article { background:var(--card); border:1px solid var(--line); border-radius:8px;
          padding:10px 14px; margin:0 0 8px; }
.meta { display:flex; gap:10px; align-items:baseline; font-size:12.5px; color:var(--muted); }
.meta .app { color:var(--fg); font-weight:600; }
.meta .note { color:#bf8700; }
.meta .sp { flex:1; }
.text { white-space:pre-wrap; overflow-wrap:anywhere; margin-top:4px; }
button { font:inherit; font-size:12px; color:var(--accent); background:none; border:1px solid
         var(--line); border-radius:5px; padding:1px 8px; cursor:pointer; }
button:hover { border-color:var(--accent); }
.empty { color:var(--muted); margin:40px 0; text-align:center; }
)";

constexpr const char* kScript = R"(
const q = document.getElementById('q');
const count = document.getElementById('count');
const items = Array.from(document.querySelectorAll('article'));
const days = Array.from(document.querySelectorAll('section'));
const keys = items.map(a => (a.querySelector('.text').textContent + ' ' +
                             (a.dataset.app || '')).toLowerCase());
function filter() {
  const terms = q.value.toLowerCase().split(/\s+/).filter(Boolean);
  let shown = 0;
  items.forEach((a, i) => {
    const hit = terms.every(t => keys[i].includes(t));
    a.hidden = !hit;
    if (hit) shown++;
  });
  days.forEach(d => { d.hidden = !d.querySelector('article:not([hidden])'); });
  count.textContent = terms.length ? shown + ' of ' + items.length : '';
}
q.addEventListener('input', filter);
document.addEventListener('keydown', e => {
  if (e.key === '/' && document.activeElement !== q) { e.preventDefault(); q.focus(); }
  if (e.key === 'Escape' && document.activeElement === q) { q.value = ''; filter(); }
});
function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text);
  const t = document.createElement('textarea');
  t.value = text; document.body.appendChild(t); t.select();
  document.execCommand('copy'); t.remove();
  return Promise.resolve();
}
let player = null, playing = null;
function stop() {
  if (player) { player.pause(); player = null; }
  if (playing) { playing.textContent = 'Play'; playing = null; }
}
document.addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  const a = b.closest('article');
  if (b.classList.contains('copy')) {
    copyText(a.querySelector('.text').textContent).then(() => {
      b.textContent = 'Copied';
      setTimeout(() => { b.textContent = 'Copy'; }, 1200);
    });
  } else if (b.classList.contains('play')) {
    if (playing === b) { stop(); return; }
    stop();
    player = new Audio(encodeURIComponent(a.dataset.dir) + '/audio.wav');
    playing = b;
    b.textContent = 'Stop';
    player.addEventListener('ended', stop);
    player.addEventListener('error', () => { stop(); b.textContent = 'No audio'; });
    player.play().catch(() => { stop(); b.textContent = 'No audio'; });
  }
});
)";

}  // namespace

HistoryEntry parse_history_entry(std::string session, std::string_view events) {
    HistoryEntry e;
    e.session = std::move(session);
    std::string finals;
    int64_t last_ms = 0;

    json::Options opts;
    opts.max_bytes = static_cast<size_t>(kMaxEventsBytes);

    size_t pos = 0;
    while (pos < events.size()) {
        size_t nl = events.find('\n', pos);
        if (nl == std::string_view::npos) nl = events.size();
        std::string_view line = events.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        // Partials are most of every file and none of the history. The corpus writes compact
        // JSON (json::serialize), so the type appears verbatim and can be skipped unparsed.
        if (line.find("\"t\":\"partial\"") != std::string_view::npos) continue;

        json::Value v;
        json::Error err;
        if (!json::parse_object(line, v, err, opts)) continue;
        const std::string_view t = v.find("t") != nullptr ? v.find("t")->as_string() : "";
        if (const json::Value* ms = v.find("ms")) last_ms = ms->as_int(last_ms);

        if (t == "start") {
            if (const json::Value* w = v.find("wall")) e.started = std::string(w->as_string());
            if (const json::Value* tg = v.find("target")) e.target = std::string(tg->as_string());
        } else if (t == "final") {
            if (const json::Value* tx = v.find("text")) {
                const std::string_view s = tx->as_string();
                if (!s.empty()) {
                    if (!finals.empty()) finals += ' ';
                    finals += s;
                }
            }
        } else if (t == "end") {
            e.finished = true;
            if (const json::Value* d = v.find("delivered")) e.text = std::string(d->as_string());
            if (const json::Value* r = v.find("reason")) e.reason = std::string(r->as_string());
            if (const json::Value* a = v.find("audio_ms")) {
                e.audio_ms = static_cast<uint64_t>(std::max<int64_t>(0, a->as_int()));
            }
        }
    }

    if (!e.finished) {
        e.text = std::move(finals);
        e.reason = "unfinished";
        e.audio_ms = static_cast<uint64_t>(std::max<int64_t>(0, last_ms));
    }
    if (e.started.empty()) e.started = started_from_folder(e.session);
    return e;
}

std::vector<HistoryEntry> read_history(const std::filesystem::path& root) {
    std::vector<HistoryEntry> out;
    std::error_code ec;
    for (const auto& d : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!d.is_directory(ec)) continue;
        const std::filesystem::path events = d.path() / "events.jsonl";
        const uintmax_t size = std::filesystem::file_size(events, ec);
        if (ec || size > kMaxEventsBytes) {
            ec.clear();
            continue;
        }
        std::ifstream in(events, std::ios::binary);
        if (!in) continue;
        std::string text(static_cast<size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(size));
        text.resize(static_cast<size_t>(in.gcount()));
        out.push_back(parse_history_entry(d.path().filename().string(), text));
    }
    // Folder names begin with the local start time, YYYYMMDD-HHMMSS, so name order is time order
    // and does not depend on file times a copy or a backup tool may have rewritten.
    std::sort(out.begin(), out.end(),
              [](const HistoryEntry& a, const HistoryEntry& b) { return a.session > b.session; });
    return out;
}

std::string render_history_html(const std::vector<HistoryEntry>& entries,
                                std::string_view generated) {
    std::vector<const HistoryEntry*> listed;
    for (const HistoryEntry& e : entries) {
        if (!e.text.empty()) listed.push_back(&e);
    }
    const size_t silent = entries.size() - listed.size();

    std::ostringstream o;
    o << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<title>Dictation history</title>\n<style>" << kStyle << "</style></head><body>\n"
      << "<header><div class=\"wrap\"><h1>Dictation history</h1><div class=\"sub\">"
      << listed.size() << (listed.size() == 1 ? " dictation" : " dictations");
    if (!listed.empty()) {
        const std::string newest = short_date(listed.front()->started);
        const std::string oldest = short_date(listed.back()->started);
        if (!oldest.empty() && !newest.empty()) {
            o << ", " << html_escape(oldest);
            if (oldest != newest) o << " &ndash; " << html_escape(newest);
        }
    }
    if (silent > 0) {
        o << " &middot; " << silent << (silent == 1 ? " session" : " sessions")
          << " with nothing said not listed";
    }
    o << " &middot; snapshot at " << html_escape(clock_of(generated))
      << "; choose Dictation history again to refresh</div>\n"
      << "<div class=\"bar\"><input id=\"q\" type=\"search\" autofocus "
      << "placeholder=\"Search the text or the app (press / to come back here)\">"
      << "<span id=\"count\"></span></div></div></header>\n<main class=\"wrap\">\n";

    if (listed.empty()) {
        o << "<p class=\"empty\">Nothing has been dictated into the corpus yet.</p>\n";
    }

    std::string day;
    bool open = false;
    for (const HistoryEntry* e : listed) {
        const std::string this_day = e->started.substr(0, std::min<size_t>(10, e->started.size()));
        if (!open || this_day != day) {
            if (open) o << "</section>\n";
            o << "<section><h2>" << html_escape(day_heading(e->started)) << "</h2>\n";
            day = this_day;
            open = true;
        }
        const std::string app = app_label(e->target);
        o << "<article data-dir=\"" << html_escape(e->session) << "\" data-app=\""
          << html_escape(app) << "\"><div class=\"meta\"><span>" << html_escape(clock_of(e->started))
          << "</span>";
        if (!app.empty()) o << "<span class=\"app\">" << html_escape(app) << "</span>";
        o << "<span>" << html_escape(duration_of(e->audio_ms)) << "</span>";
        if (!e->finished) {
            o << "<span class=\"note\">not submitted &mdash; the session never finished</span>";
        }
        o << "<span class=\"sp\"></span><button class=\"play\">Play</button>"
          << "<button class=\"copy\">Copy</button></div>"
          << "<div class=\"text\">" << html_escape(e->text) << "</div></article>\n";
    }
    if (open) o << "</section>\n";
    o << "</main>\n<script>" << kScript << "</script>\n</body></html>\n";
    return o.str();
}

Result<std::filesystem::path> write_history_page(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return Status{StatusCode::NotFound, "the corpus folder " + root.string() + " does not exist"};
    }
    const std::vector<HistoryEntry> entries = read_history(root);
    const std::string page = render_history_html(entries, local_now());

    const std::filesystem::path final_path = root / "history.html";
    const std::filesystem::path tmp_path = root / "history.html.tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return Status{StatusCode::Unavailable, "could not write " + tmp_path.string()};
        out.write(page.data(), static_cast<std::streamsize>(page.size()));
        if (!out) return Status{StatusCode::Unavailable, "could not write " + tmp_path.string()};
    }
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        return Status{StatusCode::Unavailable,
                      "could not replace " + final_path.string() + ": " + ec.message()};
    }
    KD_LOG_INFO("wrote the dictation history: %zu sessions, %zu bytes, to %s", entries.size(),
                page.size(), final_path.string().c_str());
    return final_path;
}

}  // namespace kd
