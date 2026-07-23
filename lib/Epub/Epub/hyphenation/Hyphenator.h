#pragma once

#include <cstddef>
#include <string>
#include <vector>

class LanguageHyphenator;

class Hyphenator {
 public:
  struct BreakInfo {
    size_t byteOffset;            // Byte position inside the UTF-8 word where a break may occur.
    bool requiresInsertedHyphen;  // true = a visible '-' must be rendered at the break (pattern/fallback breaks).
                                  // false = break occurs at an existing visible separator boundary
                                  //         (explicit '-' or eligible apostrophe contraction boundary).
  };

  // Returns byte offsets where the word may be hyphenated.
  //
  // Break sources (in priority order):
  //   1. Explicit hyphens already present in the word (e.g. '-' or soft-hyphen U+00AD),
  //      including letter/digit compounds ("99,9-процентным", "COVID-19"). Digit–digit
  //      ranges ("10-12") are not break opportunities.
  //      When found, language patterns are additionally run on each alphabetic segment
  //      between separators so compound words can break within their parts.
  //      Example: "US-Satellitensystems" yields breaks after "US-" (no inserted hyphen)
  //               plus pattern breaks inside "Satellitensystems" (Sa|tel|li|ten|sys|tems).
  //      Example: "99,9-процентным" yields a break after "99,9-" plus pattern breaks
  //               inside "процентным" (e.g. 99,9-про|цент|ным).
  //   2. Apostrophe contractions between letters (e.g. all'improvviso).
  //      Liang patterns are run per alphabetic segment around apostrophes.
  //      A direct break at the apostrophe boundary is allowed only when the left
  //      segment has at least 3 letters and the right segment has at least 3 letters,
  //      avoiding short clitics (e.g. l', d') and contraction tails (e.g. 've, 're, 'll).
  //   3. Language-specific Liang patterns (e.g. German de_patterns).
  //      Pure alphabetic words: one pass over the whole token.
  //      Example: "Quadratkilometer" -> Qua|drat|ki|lo|me|ter.
  //      Mixed tokens with digits/punctuation but no hyphen (e.g. "9витеиташка",
  //      "99,9процентным"): patterns run on each maximal alphabetic run so the
  //      leading numbers stay attached to the first syllable ("9ви-|теиташка").
  //   4. Fallback every-N-chars splitting (only when includeFallback is true AND no
  //      pattern breaks were found). For mixed tokens, fallback is limited to
  //      alphabetic runs. Used as a last resort to prevent a single oversized
  //      word from overflowing the page width.
  static std::vector<BreakInfo> breakOffsets(const std::string& word, bool includeFallback);

  // Provide a publication-level language hint (e.g. "en", "en-US", "ru") used to select hyphenation rules.
  static void setPreferredLanguage(const std::string& lang);

 private:
  static const LanguageHyphenator* cachedHyphenator_;
};