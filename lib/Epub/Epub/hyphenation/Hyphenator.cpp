#include "Hyphenator.h"

#include <Utf8.h>

#include <algorithm>
#include <cassert>
#include <vector>

#include "HyphenationCommon.h"
#include "LanguageHyphenator.h"
#include "LanguageRegistry.h"

const LanguageHyphenator* Hyphenator::cachedHyphenator_ = nullptr;

namespace {

// Normalize ISO 639-2 (three-letter) codes to ISO 639-1 (two-letter) codes used by the
// hyphenation registry.  EPUBs may use either form in their dc:language metadata (e.g.
// "eng" instead of "en").  Both the bibliographic ("fre"/"ger") and terminological
// ("fra"/"deu") ISO 639-2 variants are mapped.
struct Iso639Mapping {
  const char* iso639_2;
  const char* iso639_1;
};
static constexpr Iso639Mapping kIso639Mappings[] = {{"eng", "en"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"},
                                                    {"ger", "de"}, {"rus", "ru"}, {"spa", "es"}, {"ita", "it"},
                                                    {"ukr", "uk"}, {"swe", "sv"}};

// Maps a BCP-47 or ISO 639-2 language tag to a language-specific hyphenator.
const LanguageHyphenator* hyphenatorForLanguage(const std::string& langTag) {
  if (langTag.empty()) return nullptr;

  // Extract primary subtag and normalize to lowercase (e.g., "en-US" -> "en", "ENG" -> "en").
  std::string primary;
  primary.reserve(langTag.size());
  for (char c : langTag) {
    if (c == '-' || c == '_') break;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    primary.push_back(c);
  }
  if (primary.empty()) return nullptr;

  // Normalize ISO 639-2 three-letter codes to two-letter equivalents.
  for (const auto& mapping : kIso639Mappings) {
    if (primary == mapping.iso639_2) {
      primary = mapping.iso639_1;
      break;
    }
  }

  return getLanguageHyphenatorForPrimaryTag(primary);
}

// Maps a codepoint index back to its byte offset inside the source word.
size_t byteOffsetForIndex(const std::vector<CodepointInfo>& cps, const size_t index) {
  return (index < cps.size()) ? cps[index].byteOffset : (cps.empty() ? 0 : cps.back().byteOffset);
}

// True for characters that may sit on either side of a compound hyphen break.
// Digits are included so forms like "99,9-процентным" / "COVID-19" can break at '-'.
// Digit–digit ranges ("10-12") are excluded by the caller.
bool isCompoundHyphenNeighbor(const uint32_t cp) { return isAlphabetic(cp) || isAsciiDigit(cp); }

// Builds a vector of break information from explicit hyphen markers in the given codepoints.
// Hyphens that join letter/digit compounds are valid breaks. Digit–digit ranges (e.g. "10-12")
// are not treated as hyphenation opportunities.
//
// Example: "US-Satellitensystems" (cps: U, S, -, S, a, t, ...)
//   -> finds '-' at index 2 with alphabetic neighbors 'S' and 'S'
//   -> returns one BreakInfo at the byte offset of 'S' (the char after '-'),
//      with requiresInsertedHyphen=false because '-' is already visible.
//
// Example: "99,9-процентным" (digit before '-', letter after)
//   -> break after "99,9-" (no inserted hyphen), plus Liang breaks inside "процентным"
//      once appendSegmentPatternBreaks runs on that branch.
//
// Example: "Satel\u00ADliten" (soft-hyphen between 'l' and 'l')
//   -> returns one BreakInfo with requiresInsertedHyphen=true (soft-hyphen
//      is invisible and needs a visible '-' when the break is used).
std::vector<Hyphenator::BreakInfo> buildExplicitBreakInfos(const std::vector<CodepointInfo>& cps) {
  std::vector<Hyphenator::BreakInfo> breaks;

  for (size_t i = 1; i + 1 < cps.size(); ++i) {
    const uint32_t cp = cps[i].value;
    const uint32_t left = cps[i - 1].value;
    const uint32_t right = cps[i + 1].value;
    if (!isExplicitHyphen(cp) || !isCompoundHyphenNeighbor(left) || !isCompoundHyphenNeighbor(right)) {
      continue;
    }
    // Keep numeric ranges like "10-12" / "1990-1991" unsplittable at the dash.
    if (isAsciiDigit(left) && isAsciiDigit(right)) {
      continue;
    }
    // Offset points to the next codepoint so rendering starts after the hyphen marker.
    breaks.push_back({cps[i + 1].byteOffset, isSoftHyphen(cp)});
  }

  return breaks;
}

bool isSegmentSeparator(const uint32_t cp) { return isExplicitHyphen(cp) || isApostrophe(cp); }

void appendSegmentPatternBreaks(const std::vector<CodepointInfo>& cps, const LanguageHyphenator& hyphenator,
                                const bool includeFallback, std::vector<Hyphenator::BreakInfo>& outBreaks) {
  size_t segStart = 0;

  for (size_t i = 0; i <= cps.size(); ++i) {
    const bool atEnd = i == cps.size();
    const bool atSeparator = !atEnd && isSegmentSeparator(cps[i].value);
    if (!atEnd && !atSeparator) {
      continue;
    }

    if (i > segStart) {
      std::vector<CodepointInfo> segment(cps.begin() + segStart, cps.begin() + i);
      auto segIndexes = hyphenator.breakIndexes(segment);

      if (includeFallback && segIndexes.empty()) {
        const size_t minPrefix = hyphenator.minPrefix();
        const size_t minSuffix = hyphenator.minSuffix();
        for (size_t idx = minPrefix; idx + minSuffix <= segment.size(); ++idx) {
          segIndexes.push_back(idx);
        }
      }

      for (const size_t idx : segIndexes) {
        assert(idx > 0 && idx < segment.size());
        if (idx == 0 || idx >= segment.size()) continue;
        const size_t cpIdx = segStart + idx;
        if (cpIdx < cps.size()) {
          outBreaks.push_back({cps[cpIdx].byteOffset, true});
        }
      }
    }

    segStart = i + 1;
  }
}

void appendApostropheContractionBreaks(const std::vector<CodepointInfo>& cps,
                                       std::vector<Hyphenator::BreakInfo>& outBreaks) {
  constexpr size_t kMinLeftSegmentLen = 3;
  constexpr size_t kMinRightSegmentLen = 3;
  size_t segmentStart = 0;

  for (size_t i = 0; i < cps.size(); ++i) {
    if (isSegmentSeparator(cps[i].value)) {
      if (isApostrophe(cps[i].value) && i > 0 && i + 1 < cps.size() && isAlphabetic(cps[i - 1].value) &&
          isAlphabetic(cps[i + 1].value)) {
        size_t leftPrefixLen = 0;
        for (size_t j = segmentStart; j < i; ++j) {
          if (isAlphabetic(cps[j].value)) {
            ++leftPrefixLen;
          }
        }

        size_t rightSuffixLen = 0;
        for (size_t j = i + 1; j < cps.size() && !isSegmentSeparator(cps[j].value); ++j) {
          if (isAlphabetic(cps[j].value)) {
            ++rightSuffixLen;
          }
        }

        // Avoid stranding short clitics like "l'"/"d'" or contraction tails like "'ve"/"'re"/"'ll".
        if (leftPrefixLen >= kMinLeftSegmentLen && rightSuffixLen >= kMinRightSegmentLen) {
          outBreaks.push_back({cps[i + 1].byteOffset, false});
        }
      }
      segmentStart = i + 1;
    }
  }
}

// Runs Liang (and optional every-N fallback) on each maximal alphabetic run.
// Used for mixed tokens like "9витеиташка" / "99,9процентным" where a whole-word Liang
// call rejects the token because of digits or other non-letters. Non-letter characters
// stay glued to neighboring letters in the rendered prefix (e.g. "9ви-"), and are not
// themselves treated as hyphenation points.
void appendAlphabeticRunPatternBreaks(const std::vector<CodepointInfo>& cps, const LanguageHyphenator* hyphenator,
                                      const bool includeFallback, std::vector<Hyphenator::BreakInfo>& outBreaks) {
  const size_t minPrefix = hyphenator ? hyphenator->minPrefix() : LiangWordConfig::kDefaultMinPrefix;
  const size_t minSuffix = hyphenator ? hyphenator->minSuffix() : LiangWordConfig::kDefaultMinSuffix;

  size_t i = 0;
  while (i < cps.size()) {
    if (!isAlphabetic(cps[i].value)) {
      ++i;
      continue;
    }

    const size_t runStart = i;
    while (i < cps.size() && isAlphabetic(cps[i].value)) {
      ++i;
    }

    std::vector<CodepointInfo> segment(cps.begin() + static_cast<std::ptrdiff_t>(runStart),
                                       cps.begin() + static_cast<std::ptrdiff_t>(i));
    std::vector<size_t> segIndexes;
    if (hyphenator) {
      segIndexes = hyphenator->breakIndexes(segment);
    }

    if (includeFallback && segIndexes.empty()) {
      for (size_t idx = minPrefix; idx + minSuffix <= segment.size(); ++idx) {
        segIndexes.push_back(idx);
      }
    }

    for (const size_t idx : segIndexes) {
      assert(idx > 0 && idx < segment.size());
      if (idx == 0 || idx >= segment.size()) continue;
      const size_t cpIdx = runStart + idx;
      if (cpIdx < cps.size()) {
        outBreaks.push_back({cps[cpIdx].byteOffset, true});
      }
    }
  }
}

void sortAndDedupeBreakInfos(std::vector<Hyphenator::BreakInfo>& infos) {
  std::sort(infos.begin(), infos.end(), [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
    if (a.byteOffset != b.byteOffset) {
      return a.byteOffset < b.byteOffset;
    }
    return a.requiresInsertedHyphen < b.requiresInsertedHyphen;
  });

  infos.erase(std::unique(infos.begin(), infos.end(),
                          [](const Hyphenator::BreakInfo& a, const Hyphenator::BreakInfo& b) {
                            return a.byteOffset == b.byteOffset;
                          }),
              infos.end());
}

std::vector<Hyphenator::BreakInfo> breaksFromCodepointIndexes(const std::vector<CodepointInfo>& cps,
                                                              const std::vector<size_t>& indexes) {
  std::vector<Hyphenator::BreakInfo> breaks;
  breaks.reserve(indexes.size());
  for (const size_t idx : indexes) {
    // CJK characters can break without inserting a visible hyphen.
    bool needsHyphen = true;
    if (idx < cps.size() && utf8IsCjkBreakable(cps[idx].value)) {
      needsHyphen = false;
    } else if (idx > 0 && utf8IsCjkBreakable(cps[idx - 1].value)) {
      needsHyphen = false;
    }
    breaks.push_back({byteOffsetForIndex(cps, idx), needsHyphen});
  }
  return breaks;
}

}  // namespace

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  if (word.empty()) {
    return {};
  }

  // Convert to codepoints and normalize word boundaries.
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);
  const auto* hyphenator = cachedHyphenator_;

  // Detect apostrophe-like separators early; used by both branches below.
  bool hasApostropheLikeSeparator = false;
  for (const auto& cp : cps) {
    if (isApostrophe(cp.value)) {
      hasApostropheLikeSeparator = true;
      break;
    }
  }

  // Explicit hyphen markers (soft or hard) take precedence over language breaks.
  // This includes digit–letter compounds ("99,9-процентным"); without recognizing those,
  // Liang would see digits/commas/hyphens and refuse the whole token, leaving no breaks.
  auto explicitBreakInfos = buildExplicitBreakInfos(cps);
  if (!explicitBreakInfos.empty()) {
    // When a word contains explicit hyphens we also run Liang patterns on each alphabetic
    // segment between them. Without this, "US-Satellitensystems" would only offer one split
    // point (after "US-"), making it impossible to break mid-"Satellitensystems" even when
    // "US-Satelliten-" would fit on the line.
    //
    // Example: "US-Satellitensystems"
    //   Segments: ["US", "Satellitensystems"]
    //   Explicit break: after "US-"           -> @3  (no inserted hyphen)
    //   Pattern breaks on "Satellitensystems" -> @5  Sa|tel  (+hyphen)
    //                                            @8  Satel|li  (+hyphen)
    //                                            @10 Satelli|ten  (+hyphen)
    //                                            @13 Satelliten|sys  (+hyphen)
    //                                            @16 Satellitensys|tems  (+hyphen)
    //   Result: 6 sorted break points; the line-breaker picks the widest prefix that fits.
    if (hyphenator) {
      appendSegmentPatternBreaks(cps, *hyphenator, /*includeFallback=*/false, explicitBreakInfos);
    }
    // Also add apostrophe contraction breaks when present (e.g. "l'état-major"
    // has both an explicit hyphen and an apostrophe that can independently break).
    if (hasApostropheLikeSeparator) {
      appendApostropheContractionBreaks(cps, explicitBreakInfos);
    }
    // Merge all break points into ascending byte-offset order.
    sortAndDedupeBreakInfos(explicitBreakInfos);
    return explicitBreakInfos;
  }

  // Apostrophe-like separators split compounds into alphabetic segments; run Liang on each segment.
  // This allows words like "all'improvviso" to hyphenate within "improvviso" instead of becoming
  // completely unsplittable due to the apostrophe punctuation. Apostrophe contraction breaks are
  // applied regardless of whether a language hyphenator is available.
  if (hasApostropheLikeSeparator) {
    std::vector<BreakInfo> segmentedBreaks;
    if (hyphenator) {
      appendSegmentPatternBreaks(cps, *hyphenator, includeFallback, segmentedBreaks);
    }
    appendApostropheContractionBreaks(cps, segmentedBreaks);
    sortAndDedupeBreakInfos(segmentedBreaks);
    return segmentedBreaks;
  }

  bool allLetters = !cps.empty();
  for (const auto& cp : cps) {
    if (!isAlphabetic(cp.value)) {
      allLetters = false;
      break;
    }
  }

  if (!allLetters) {
    // Mixed letter/non-letter token (e.g. "9витеиташка", "99,9процентным"): whole-word Liang
    // rejects non-letters, so hyphenate each alphabetic run on its own.
    std::vector<BreakInfo> runBreaks;
    appendAlphabeticRunPatternBreaks(cps, hyphenator, includeFallback, runBreaks);
    sortAndDedupeBreakInfos(runBreaks);
    return runBreaks;
  }

  // Pure alphabetic word: single Liang pass (and optional full-word fallback).
  std::vector<size_t> indexes;
  if (hyphenator) {
    indexes = hyphenator->breakIndexes(cps);
  }

  if (includeFallback && indexes.empty()) {
    const size_t minPrefix = hyphenator ? hyphenator->minPrefix() : LiangWordConfig::kDefaultMinPrefix;
    const size_t minSuffix = hyphenator ? hyphenator->minSuffix() : LiangWordConfig::kDefaultMinSuffix;
    for (size_t idx = minPrefix; idx + minSuffix <= cps.size(); ++idx) {
      indexes.push_back(idx);
    }
  }

  if (indexes.empty()) {
    return {};
  }

  return breaksFromCodepointIndexes(cps, indexes);
}

void Hyphenator::setPreferredLanguage(const std::string& lang) { cachedHyphenator_ = hyphenatorForLanguage(lang); }
