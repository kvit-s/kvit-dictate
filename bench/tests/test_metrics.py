# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Tests for bench/metrics.py.

Run from the repository root:

    C:/Python312/python.exe -m unittest discover -s bench/tests -t .

Every expected value below is hand-computed and the working is in the test, because a metrics
module whose tests were written by running the metrics module tests nothing. Where a figure is
tedious to derive by hand the derivation is spelled out in a comment.
"""

from __future__ import annotations

import os
import sys
import unittest

# bench/ is a directory of scripts, not a package (see bench/tests/__init__.py). Done here rather
# than relying on the package __init__ so this module works under every invocation style.
_BENCH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _BENCH_DIR not in sys.path:
    sys.path.insert(0, _BENCH_DIR)

import metrics as m  # noqa: E402


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Grapheme segmentation — must agree with src/util/utf8.h and the sidecar
# ══════════════════════════════════════════════════════════════════════════════════════════════

FAMILY = "\U0001F468\u200D\U0001F469\u200D\U0001F467"   # 👨‍👩‍👧, 5 code points, 1 cluster
FLAG_DE = "\U0001F1E9\U0001F1EA"                        # 🇩🇪, 2 code points, 1 cluster
E_ACUTE = "e\u0301"                                     # decomposed é, 2 code points, 1 cluster
KEYCAP = "#\uFE0F\u20E3"                                # #️⃣, 3 code points, 1 cluster


class TestGraphemes(unittest.TestCase):
    def test_never_splits_the_sequences_utf8_h_names(self):
        self.assertEqual(m.grapheme_clusters(E_ACUTE), [E_ACUTE])
        self.assertEqual(m.grapheme_clusters(FAMILY), [FAMILY])
        self.assertEqual(m.grapheme_clusters(FLAG_DE), [FLAG_DE])
        self.assertEqual(m.grapheme_clusters(KEYCAP), [KEYCAP])
        self.assertEqual(m.grapheme_clusters("a\r\nb"), ["a", "\r\n", "b"])

    def test_counts_clusters_not_code_points(self):
        text = "a" + E_ACUTE + FAMILY + FLAG_DE
        self.assertEqual(len(text), 1 + 2 + 5 + 2)   # code points
        self.assertEqual(m.count_graphemes(text), 4)  # clusters

    def test_two_regional_indicators_pair_but_three_do_not_all_merge(self):
        # GB12/GB13: pairs, left to right. Three RIs are one pair plus a lone one.
        self.assertEqual(len(m.grapheme_clusters(FLAG_DE + "\U0001F1E9")), 2)

    def test_boundaries_are_utf8_byte_offsets(self):
        # "é" decomposed is 'e' (1 byte) + U+0301 (2 bytes) = 3 bytes, one cluster.
        self.assertEqual(m.grapheme_boundaries(E_ACUTE), (0, 3))
        self.assertEqual(m.utf8_len(E_ACUTE), 3)
        self.assertEqual(m.grapheme_boundaries(""), (0,))

    def test_agrees_with_the_sidecar_implementation(self):
        """Cross-check against server/kvit_asr/protocol.py.

        This module carries a third copy of the grapheme rule set (C++, sidecar, bench) for the
        reason stated in metrics.py's docstring. This test is what keeps the copies honest, so a
        missing sidecar produces a skip *with a reason*, never a quiet pass.
        """
        repo_root = os.path.dirname(_BENCH_DIR)
        server_dir = os.path.join(repo_root, "server")
        if not os.path.isdir(server_dir):
            self.skipTest(f"sidecar not found at {server_dir}; cross-check not performed")
        if server_dir not in sys.path:
            sys.path.insert(0, server_dir)
        try:
            from kvit_asr import protocol as sidecar  # type: ignore
        except Exception as exc:  # pragma: no cover - environment-dependent
            self.skipTest(f"kvit_asr.protocol not importable ({exc!r}); cross-check not performed")

        samples = [
            "", "a", "hello world", E_ACUTE, FAMILY, FLAG_DE, KEYCAP,
            "a\r\nb", "llama.cpp", "naïve café", "a" + E_ACUTE + FAMILY + FLAG_DE + KEYCAP,
            "\U0001F44D\U0001F3FD",  # thumbs up + skin tone modifier
        ]
        for s in samples:
            with self.subTest(sample=repr(s)):
                self.assertEqual(m.grapheme_boundaries(s), sidecar.grapheme_boundaries(s))


# ══════════════════════════════════════════════════════════════════════════════════════════════
# WER / CER against hand-computed edit distances
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestEditDistance(unittest.TestCase):
    def test_identical(self):
        ops = m.word_error_ops("the quick brown fox", "the quick brown fox", normalised=False)
        self.assertEqual((ops.hits, ops.errors, ops.ref_len), (4, 0, 4))
        self.assertEqual(ops.rate, 0.0)

    def test_single_substitution(self):
        ops = m.word_error_ops("the quick brown fox", "the quick brown dog", normalised=False)
        self.assertEqual((ops.substitutions, ops.deletions, ops.insertions), (1, 0, 0))
        self.assertEqual(ops.rate, 0.25)

    def test_single_deletion(self):
        # hypothesis dropped "brown": one deletion out of a 4-word reference.
        ops = m.word_error_ops("the quick brown fox", "the quick fox", normalised=False)
        self.assertEqual((ops.substitutions, ops.deletions, ops.insertions), (0, 1, 0))
        self.assertEqual(ops.rate, 0.25)

    def test_single_insertion(self):
        ops = m.word_error_ops("the quick brown fox", "the very quick brown fox",
                               normalised=False)
        self.assertEqual((ops.substitutions, ops.deletions, ops.insertions), (0, 0, 1))
        self.assertEqual(ops.rate, 0.25)

    def test_mixed_operations_unambiguous(self):
        # ref: a b c d   hyp: x b c d e
        #      a->x sub | b,c,d hits | e inserted  => 2 errors / 4.
        # There is no equal-cost alternative decomposition here, so the op split is pinned.
        ops = m.word_error_ops("a b c d", "x b c d e", normalised=False)
        self.assertEqual((ops.substitutions, ops.deletions, ops.insertions), (1, 0, 1))
        self.assertEqual(ops.hits, 3)
        self.assertAlmostEqual(ops.rate, 0.5)

    def test_mixed_operations_with_an_ambiguous_alignment(self):
        # ref: a b c d e   hyp: a x c e f
        # Two optimal alignments of cost 3 exist: three substitutions (b->x, d->e, e->f), or
        # sub + delete + insert. The *total* is what WER reports and is alignment-independent;
        # the split between op types is not, so only the documented tie-break (diagonal first)
        # makes it reproducible. Assert the total, and the split the tie-break commits us to.
        ops = m.word_error_ops("a b c d e", "a x c e f", normalised=False)
        self.assertEqual(ops.errors, 3)
        self.assertAlmostEqual(ops.rate, 0.6)
        self.assertEqual((ops.substitutions, ops.deletions, ops.insertions), (3, 0, 0))
        self.assertEqual(ops.hits, 2)

    def test_empty_reference_has_error_count_but_no_rate(self):
        ops = m.word_error_ops("", "hello there", normalised=False)
        self.assertEqual((ops.ref_len, ops.hyp_len, ops.insertions), (0, 2, 2))
        self.assertIsNone(ops.rate)          # undefined, not 0.0 and not infinity
        self.assertEqual(ops.errors, 2)      # the count is still well defined

    def test_both_empty_is_a_clean_zero(self):
        ops = m.word_error_ops("", "", normalised=False)
        self.assertEqual(ops.rate, 0.0)

    def test_empty_hypothesis_is_all_deletions(self):
        ops = m.word_error_ops("one two three", "", normalised=False)
        self.assertEqual((ops.deletions, ops.errors, ops.ref_len), (3, 3, 3))
        self.assertEqual(ops.rate, 1.0)

    def test_aggregate_pools_counts_not_rates(self):
        # A 1-error/1-word clip and a 0-error/99-word clip. Mean of rates = 0.5; pooled = 0.01.
        a = m.EditOps(hits=0, substitutions=1, deletions=0, insertions=0, ref_len=1, hyp_len=1)
        b = m.EditOps(hits=99, substitutions=0, deletions=0, insertions=0, ref_len=99, hyp_len=99)
        pooled = m.aggregate_edit_ops([a, b])
        self.assertEqual(pooled.ref_len, 100)
        self.assertAlmostEqual(pooled.rate, 0.01)

    def test_cer_is_measured_on_grapheme_clusters(self):
        ops = m.char_error_ops("cat", "cot", normalised=False)
        self.assertEqual((ops.substitutions, ops.ref_len), (1, 3))
        # A decomposed é is one unit, so replacing it is one substitution, not two.
        ops = m.char_error_ops("caf" + E_ACUTE, "cafe", normalised=False)
        self.assertEqual((ops.ref_len, ops.errors), (4, 1))


# ══════════════════════════════════════════════════════════════════════════════════════════════
# The normaliser: what it fixes, and — the load-bearing half — what it must not fix
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestNormaliser(unittest.TestCase):
    def test_case_and_edge_punctuation(self):
        self.assertEqual(m.normalise_text("Hello, World!"), "hello world")
        self.assertEqual(m.normalise_text("  spaced   out  "), "spaced out")
        self.assertEqual(m.normalise_text('He said "yes" (twice).'), "he said yes twice")

    def test_internal_punctuation_is_preserved(self):
        self.assertEqual(m.normalise_text("llama.cpp"), "llama.cpp")
        self.assertEqual(m.normalise_text("--n-gpu-layers"), "--n-gpu-layers")
        self.assertEqual(m.normalise_text("sm_120"), "sm_120")
        self.assertEqual(m.normalise_text("C:\\Users\\sk\\AppData"), "c:\\users\\sk\\appdata")
        self.assertEqual(m.normalise_text("Win+H"), "win+h")
        # A trailing sentence stop is edge punctuation; the identifier's own dot is not.
        self.assertEqual(m.normalise_text("Use llama.cpp."), "use llama.cpp")

    def test_number_folding(self):
        self.assertEqual(m.normalise_text("thirty five"), "35")
        self.assertEqual(m.normalise_text("thirty-five"), "35")
        self.assertEqual(m.normalise_text("sixteen"), "16")
        self.assertEqual(m.normalise_text("1,000"), "1000")
        self.assertEqual(m.normalise_text("ninety"), "90")

    def test_spelled_punctuation_is_never_expanded(self):
        """The rule that makes this normaliser fit for purpose.

        If any of these ever start folding, "llama dot cpp" would score as a perfect match for
        "llama.cpp" and the benchmark would report the product's central failure as a success.
        """
        for word in ("dot", "dash", "slash", "underscore", "colon", "hash", "plus"):
            with self.subTest(word=word):
                self.assertIn(word, m.normalise_text(f"llama {word} cpp").split())
        self.assertEqual(m.normalise_text("llama dot cpp"), "llama dot cpp")


class TestRawVersusNormalised(unittest.TestCase):
    REF = "change the llama.cpp config"
    BAD = "change the llama dot cpp config"

    def test_the_technical_case_scores_badly_raw(self):
        # ref tokens: [change, the, llama.cpp, config]        (4)
        # hyp tokens: [change, the, llama, dot, cpp, config]  (6)
        # llama.cpp -> llama is a substitution, "dot" and "cpp" are insertions => 3 errors / 4.
        rates = m.error_rates(self.REF, self.BAD)
        self.assertEqual(rates.wer_raw.errors, 3)
        self.assertAlmostEqual(rates.wer_raw.rate, 0.75)

    def test_the_normaliser_does_not_rescue_it(self):
        """A normaliser that closed this gap would be broken for our purpose. Pin it."""
        rates = m.error_rates(self.REF, self.BAD)
        self.assertEqual(rates.wer_normalised.errors, rates.wer_raw.errors)
        self.assertEqual(rates.wer_normalised.rate, rates.wer_raw.rate)
        self.assertGreater(rates.wer_normalised.rate, 0.5)
        self.assertGreater(rates.cer_normalised.errors, 0)

    def test_the_normaliser_does_close_a_pure_formatting_gap(self):
        # The contrast case: if normalisation never helped it would not be worth having.
        rates = m.error_rates("Set the flag.", "set the flag")
        self.assertAlmostEqual(rates.wer_raw.rate, 2 / 3)   # "Set"->"set", "flag."->"flag"
        self.assertEqual(rates.wer_normalised.rate, 0.0)

    def test_report_carries_both_and_the_versions(self):
        d = m.error_rates(self.REF, self.BAD).to_dict()
        self.assertIn("wer_raw", d)
        self.assertIn("wer_normalised", d)
        self.assertIn("cer_raw", d)
        self.assertIn("cer_normalised", d)
        self.assertEqual(d["normaliser_version"], m.NORMALISER_VERSION)


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Technical-term F1
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestTermF1(unittest.TestCase):
    def setUp(self):
        self.matcher = m.TermMatcher()

    def test_perfect_match(self):
        text = "run llama.cpp with --n-gpu-layers 35 on the RTX 5090"
        rep = m.term_f1(text, text, matcher=self.matcher)
        self.assertEqual((rep.fp, rep.fn), (0, 0))
        self.assertGreaterEqual(rep.tp, 4)   # llama.cpp, --n-gpu-layers, RTX, 5090
        self.assertEqual(rep.f1, 1.0)

    def test_case_variant_still_matches(self):
        rep = m.term_f1("the wasapi capture path", "the WASAPI capture path",
                        matcher=self.matcher)
        self.assertEqual((rep.tp, rep.fp, rep.fn), (1, 0, 0))
        self.assertEqual(rep.f1, 1.0)

    def test_compound_identifier_matches_as_one_token(self):
        ref = "written under C:\\Users\\sk\\AppData today"
        rep = m.term_f1(ref, ref, matcher=self.matcher)
        canon = {d.canonical for d in rep.per_term}
        self.assertIn("C:\\Users\\sk\\AppData", canon)
        self.assertEqual(rep.fn, 0)

    def test_alias_counts_as_correct(self):
        # "UTF8" is a written-form equivalent of "UTF-8" and is listed as an alias.
        rep = m.term_f1("encoded as UTF-8 throughout", "encoded as UTF8 throughout",
                        matcher=self.matcher)
        self.assertEqual((rep.tp, rep.fn, rep.fp), (1, 0, 0))

    def test_wrong_form_is_a_miss_with_a_named_reason(self):
        ref = "run llama.cpp with --n-gpu-layers 35 on the RTX 5090"
        hyp = "run llama dot cpp with n gpu layers 35 on the RTX 5090"
        rep = m.term_f1(ref, hyp, matcher=self.matcher)

        detail = {d.canonical: d for d in rep.per_term}
        self.assertEqual(detail["llama.cpp"].tp, 0)
        self.assertEqual(detail["llama.cpp"].fn, 1)
        self.assertIn("llama dot cpp", detail["llama.cpp"].wrong_forms_seen)
        self.assertIn("n gpu layers", detail["--n-gpu-layers"].wrong_forms_seen)

        # RTX and 5090 survived; llama.cpp and --n-gpu-layers did not.
        self.assertEqual(rep.tp, 2)
        self.assertEqual(rep.fn, 2)
        self.assertEqual(rep.precision, 1.0)
        self.assertAlmostEqual(rep.recall, 0.5)
        self.assertAlmostEqual(rep.f1, 2 * 1.0 * 0.5 / 1.5)

    def test_hallucinated_term_is_a_false_positive(self):
        rep = m.term_f1("just some plain words", "just some plain words about llama.cpp",
                        matcher=self.matcher)
        self.assertEqual((rep.tp, rep.fn, rep.fp), (0, 0, 1))
        self.assertEqual(rep.precision, 0.0)

    def test_case_sensitive_term_is_compared_exactly(self):
        terms = (m.TechnicalTerm("MiB", case_sensitive=True),)
        matcher = m.TermMatcher(terms)
        self.assertEqual(matcher.find("size in MiB"), ["MiB"])
        self.assertEqual(matcher.find("size in mib"), [])

    def test_longest_form_wins(self):
        # "onnx runtime" must match onnxruntime, not ONNX followed by a stray word.
        self.assertEqual(self.matcher.find("uses onnx runtime here"), ["onnxruntime"])
        self.assertEqual(self.matcher.find("uses ONNX here"), ["ONNX"])

    def test_edge_punctuation_stripped_before_matching(self):
        self.assertEqual(self.matcher.find("we use llama.cpp, mostly"), ["llama.cpp"])
        self.assertEqual(self.matcher.find("(WASAPI)"), ["WASAPI"])

    def test_ruleset_version_and_rules_are_in_the_output(self):
        d = m.term_f1("llama.cpp", "llama.cpp", matcher=self.matcher).to_dict()
        self.assertEqual(d["ruleset_version"], m.TERM_RULESET_VERSION)
        self.assertTrue(d["rules"])


# ══════════════════════════════════════════════════════════════════════════════════════════════
# ICER — the gating number
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestIcer(unittest.TestCase):
    def test_a_clean_prefix_commit_trace_is_zero(self):
        commits = [
            m.CommitEvent("Can you change the ", t_ms=1200.0, utterance_id=1, seq=4),
            m.CommitEvent("llama.cpp ", t_ms=1800.0, utterance_id=1, seq=7),
        ]
        rep = m.icer(commits, "Can you change the llama.cpp configuration")
        self.assertTrue(rep.prefix_invariant_held)
        self.assertEqual(rep.errors, 0)
        self.assertEqual(rep.icer_per_1000, 0.0)
        self.assertIsNone(rep.divergence_grapheme_index)

    def test_zero_commits_does_not_divide_by_zero(self):
        rep = m.icer([], "a final that never reached the document")
        self.assertEqual(rep.committed_graphemes, 0)
        self.assertEqual(rep.icer_per_1000, 0.0)
        self.assertEqual(rep.icer_strict_per_1000, 0.0)
        self.assertTrue(rep.to_dict()["no_commits"])
        # And the fully-empty case.
        self.assertEqual(m.icer([], "").icer_per_1000, 0.0)

    def test_known_corruption(self):
        # committed: "hello wrld"        (10 graphemes)
        # final:     "hello world extra" (17 graphemes)
        # Longest common grapheme prefix is "hello w" = 7, so divergence index 7.
        # Alignment with the final's tail free: "hello world" vs "hello wrld" costs exactly one
        # deletion (the 'o'); " extra" was never committed and is not damage.
        #   headline ICER = 1 * 1000 / 10 = 100.0
        #   strict  ICER = (10 - 7) * 1000 / 10 = 300.0
        rep = m.icer([m.CommitEvent("hello wrld", t_ms=0.0)], "hello world extra")
        self.assertFalse(rep.prefix_invariant_held)
        self.assertEqual(rep.divergence_grapheme_index, 7)
        self.assertEqual(rep.divergence_byte_offset, 7)
        self.assertEqual((rep.substitutions, rep.deletions, rep.insertions), (0, 1, 0))
        self.assertEqual(rep.errors, 1)
        self.assertAlmostEqual(rep.icer_per_1000, 100.0)
        self.assertAlmostEqual(rep.icer_strict_per_1000, 300.0)

    def test_the_motivating_corruption(self):
        # The document says "llama dot cpp"; the final says "llama.cpp". Irrevocable.
        rep = m.icer([m.CommitEvent("the llama dot cpp ", t_ms=0.0)], "the llama.cpp config")
        self.assertFalse(rep.prefix_invariant_held)
        self.assertGreater(rep.icer_per_1000, 0.0)
        self.assertLessEqual(rep.icer_per_1000, rep.icer_strict_per_1000)
        self.assertEqual(rep.divergence_grapheme_index, 9)   # "the llama" then ' ' vs '.'

    def test_trace_order_matters_and_concatenation_is_the_document(self):
        commits = [m.CommitEvent("world", 2.0), m.CommitEvent("hello ", 1.0)]
        rep = m.icer(commits, "hello world")
        # The trace is consumed in the order given; a caller that hands over an unsorted trace
        # gets the document that order would have produced, not a repaired one.
        self.assertEqual(rep.committed_text, "worldhello ")
        self.assertFalse(rep.prefix_invariant_held)

    def test_counted_in_graphemes_not_code_points(self):
        rep = m.icer([m.CommitEvent("caf" + E_ACUTE, 0.0)], "caf" + E_ACUTE + " au lait")
        self.assertTrue(rep.prefix_invariant_held)
        self.assertEqual(rep.committed_graphemes, 4)          # c a f é
        # …but 6 UTF-8 bytes: "caf" is 3, 'e' is 1, U+0301 is 2. Byte length and grapheme count
        # are different questions and the wire uses the first while ICER uses the second.
        self.assertEqual(m.utf8_len("caf" + E_ACUTE), 6)

    def test_committing_a_base_letter_without_its_accent_is_a_whole_cluster_wrong(self):
        # "e" committed where the final says "é": the cluster differs, so the single committed
        # grapheme is wrong. This is why the commit engine must not split a combining sequence.
        rep = m.icer([m.CommitEvent("e", 0.0)], E_ACUTE + "x")
        self.assertFalse(rep.prefix_invariant_held)
        self.assertEqual(rep.divergence_grapheme_index, 0)
        self.assertEqual(rep.committed_graphemes, 1)
        self.assertAlmostEqual(rep.icer_per_1000, 1000.0)

    def test_to_dict_flags_transcript_text_and_gating(self):
        rep = m.icer([m.CommitEvent("hi", 0.0)], "hi there")
        with_text = rep.to_dict()
        self.assertTrue(with_text["contains_transcript_text"])
        self.assertTrue(with_text["gates"])
        without = rep.to_dict(include_text=False)
        self.assertNotIn("committed_text", without)
        self.assertNotIn("contains_transcript_text", without)


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Flicker — recorded, not gating
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestFlicker(unittest.TestCase):
    def test_pure_append_has_no_flicker(self):
        rep = m.flicker([
            m.HypothesisEvent("can", 10.0),
            m.HypothesisEvent("can you", 20.0),
            m.HypothesisEvent("can you change", 30.0),
        ])
        self.assertEqual((rep.revisions, rep.retracted_graphemes), (0, 0))
        self.assertEqual(rep.flicker_per_1000, 0.0)

    def test_retractions_are_counted_in_graphemes(self):
        # "can you change" -> "can you chain": common prefix "can you cha" (11), so 3 retracted.
        # "can you chain"  -> "can you change the": common prefix 11 again, so 2 retracted.
        rep = m.flicker([
            m.HypothesisEvent("can you change", 10.0),
            m.HypothesisEvent("can you chain", 20.0),
            m.HypothesisEvent("can you change the", 30.0),
        ])
        self.assertEqual(rep.revisions, 2)
        self.assertEqual(rep.retracted_graphemes, 5)
        self.assertEqual(rep.max_retraction_graphemes, 3)
        self.assertEqual(rep.final_graphemes, 18)
        self.assertAlmostEqual(rep.flicker_per_1000, 5 * 1000 / 18)

    def test_flicker_declares_that_it_does_not_gate(self):
        d = m.flicker([m.HypothesisEvent("a", 0.0)]).to_dict()
        self.assertFalse(d["gates"])
        self.assertIn("ICER gates", d["note"])

    def test_heavy_flicker_can_coexist_with_zero_icer(self):
        """The reason flicker must not gate, demonstrated rather than asserted in prose."""
        hyps = [
            m.HypothesisEvent("can you chain", 10.0),
            m.HypothesisEvent("can you change", 20.0),
            m.HypothesisEvent("can you change the", 30.0),
        ]
        self.assertGreater(m.flicker(hyps).retracted_graphemes, 0)
        # Nothing was committed until the prefix was stable, so the document is untouched.
        icer_rep = m.icer([m.CommitEvent("can you ", 40.0)], "can you change the")
        self.assertEqual(icer_rep.icer_per_1000, 0.0)


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Latency decomposition
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestLatency(unittest.TestCase):
    def _sample(self, **kw):
        base = dict(algorithmic_ms=50.0, queueing_ms=10.0, inference_ms=120.0,
                    endpoint_ms=800.0, ui_commit_ms=30.0)
        base.update(kw)
        return m.LatencySample(**base)

    def test_components_sum_to_the_end_to_end_figure(self):
        s = self._sample(measured_end_to_end_ms=1010.0)
        self.assertAlmostEqual(s.component_sum_ms, 1010.0)
        self.assertAlmostEqual(s.residual_ms, 0.0)
        self.assertTrue(s.is_consistent())

    def test_an_unaccounted_residual_is_reported_not_absorbed(self):
        s = self._sample(measured_end_to_end_ms=1035.0, label="warm-3")
        self.assertAlmostEqual(s.residual_ms, 25.0)
        self.assertFalse(s.is_consistent(1.0))
        self.assertTrue(s.is_consistent(30.0))
        rep = m.summarise_latency([s])["warm"]
        self.assertEqual(rep.inconsistent_samples, ("warm-3",))

    def test_all_five_stages_are_reported_separately(self):
        rep = m.summarise_latency([self._sample()])["warm"]
        self.assertEqual(tuple(rep.stage_spreads), m.LATENCY_STAGES)
        self.assertEqual(rep.stage_spreads["endpoint"].median, 800.0)
        self.assertEqual(rep.stage_spreads["inference"].median, 120.0)

    def test_median_and_spread_over_repeats(self):
        samples = [self._sample(inference_ms=v) for v in (100.0, 120.0, 140.0, 300.0)]
        rep = m.summarise_latency(samples)["warm"]
        inf = rep.stage_spreads["inference"]
        self.assertEqual(inf.n, 4)
        self.assertAlmostEqual(inf.median, 130.0)          # (120 + 140) / 2
        self.assertAlmostEqual(inf.minimum, 100.0)
        self.assertAlmostEqual(inf.maximum, 300.0)
        # Linear interpolation over 4 points: p25 at position 0.75 -> 100 + 20*0.75 = 115;
        # p75 at position 2.25 -> 140 + 160*0.25 = 180.
        self.assertAlmostEqual(inf.p25, 115.0)
        self.assertAlmostEqual(inf.p75, 180.0)
        self.assertAlmostEqual(inf.iqr, 65.0)
        self.assertAlmostEqual(inf.mad, 20.0)              # |100-130|,|120-130|,|140-130|,|300-130|
        self.assertIsNotNone(inf.stdev)

    def test_single_repeat_still_summarises(self):
        inf = m.summarise_latency([self._sample()])["warm"].stage_spreads["inference"]
        self.assertEqual(inf.n, 1)
        self.assertIsNone(inf.stdev)
        self.assertEqual(inf.iqr, 0.0)

    def test_cold_and_warm_are_never_pooled(self):
        samples = [self._sample(inference_ms=2000.0, cold=True),
                   self._sample(inference_ms=100.0),
                   self._sample(inference_ms=120.0)]
        out = m.summarise_latency(samples)
        self.assertEqual(out["cold"].n, 1)
        self.assertEqual(out["warm"].n, 2)
        self.assertEqual(out["cold"].stage_spreads["inference"].median, 2000.0)
        self.assertEqual(out["warm"].stage_spreads["inference"].median, 110.0)

    def test_report_dict_shows_the_sum_next_to_the_measurement(self):
        d = m.summarise_latency([self._sample(measured_end_to_end_ms=1010.0)])["warm"].to_dict()
        self.assertAlmostEqual(d["component_sum"]["median"], 1010.0)
        self.assertAlmostEqual(d["measured_end_to_end"]["median"], 1010.0)


class TestRtfAndResources(unittest.TestCase):
    def test_rtf(self):
        self.assertAlmostEqual(m.real_time_factor(10.0, 5.0), 0.5)
        self.assertAlmostEqual(m.real_time_factor(4.0, 8.0), 2.0)

    def test_rtf_rejects_a_non_positive_duration(self):
        with self.assertRaises(ValueError):
            m.real_time_factor(0.0, 1.0)

    def test_resource_report_round_trips(self):
        d = m.ResourceReport(peak_vram_mb=3400.0, cold_start_ms=8200.0,
                             warm_start_ms=140.0).to_dict()
        self.assertEqual(d["peak_vram_mb"], 3400.0)
        self.assertEqual(d["cold_start_ms"], 8200.0)
        self.assertEqual(d["warm_start_ms"], 140.0)


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Hallucination on silence-only and noise-only clips
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestHallucination(unittest.TestCase):
    def test_empty_reference_with_any_output_is_a_hallucination(self):
        self.assertTrue(m.is_hallucination("", "Thank you."))
        self.assertTrue(m.is_hallucination("", "."))
        self.assertFalse(m.is_hallucination("", ""))
        self.assertFalse(m.is_hallucination("", "   \n\t "))
        self.assertFalse(m.is_hallucination("hello", ""))

    def test_punctuation_only_output_still_counts(self):
        """Not normalised first: the normaliser strips edge punctuation, so a hypothesis of "."
        would normalise to "" and a real hallucination would score as clean silence."""
        rep = m.hallucination_report([m.HallucinationCase("noise-01", "noise", ".")])
        self.assertEqual(rep.hallucinated, 1)

    def test_rate_and_per_kind_breakdown(self):
        cases = [
            m.HallucinationCase("sil-01", "silence", "", duration_s=10.0),
            m.HallucinationCase("sil-02", "silence", " Thank you. ", duration_s=10.0),
            m.HallucinationCase("sil-03", "silence", "Bye", duration_s=10.0),
            m.HallucinationCase("noi-01", "noise", "", duration_s=10.0),
        ]
        rep = m.hallucination_report(cases)
        self.assertEqual((rep.total, rep.hallucinated), (4, 2))
        self.assertAlmostEqual(rep.rate, 0.5)
        self.assertEqual(rep.by_kind["silence"], (2, 3))
        self.assertEqual(rep.by_kind["noise"], (0, 1))

    def test_the_actual_text_is_reported_because_it_is_diagnostic(self):
        rep = m.hallucination_report([
            m.HallucinationCase("sil-02", "silence", " Thank you. "),
        ])
        d = rep.to_dict()
        self.assertEqual(d["texts"][0]["text"], " Thank you. ")
        self.assertTrue(d["contains_transcript_text"])
        # …and can be withheld when the result file is one of the committed aggregates.
        self.assertNotIn("texts", rep.to_dict(include_text=False))

    def test_empty_case_list(self):
        rep = m.hallucination_report([])
        self.assertEqual((rep.total, rep.hallucinated), (0, 0))
        self.assertEqual(rep.rate, 0.0)


# ══════════════════════════════════════════════════════════════════════════════════════════════
# Versioning and the gate metric list
# ══════════════════════════════════════════════════════════════════════════════════════════════


class TestVersioningAndGates(unittest.TestCase):
    def test_every_ruleset_is_versioned(self):
        v = m.metric_versions()
        self.assertEqual(set(v), {"metrics", "normaliser", "terms", "grapheme"})
        self.assertTrue(all(isinstance(x, str) and x for x in v.values()))

    def test_grapheme_version_does_not_claim_a_unicode_version_we_have_not_pinned(self):
        # Mirrors kd::utf8::kUnicodeVersion. docs/protocol.md §14 item 2 is still open.
        self.assertIn("unspecified", m.GRAPHEME_RULESET_VERSION)

    def test_flicker_is_not_among_the_gate_metrics(self):
        gates = m.select_gate_metrics()
        self.assertIn("icer_per_1000", gates)
        self.assertIn("term_f1", gates)
        self.assertNotIn("flicker_per_1000", gates)
        self.assertNotIn("flicker", gates)


if __name__ == "__main__":
    unittest.main()
