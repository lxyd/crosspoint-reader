#pragma once

#include "FootnoteEntry.h"

#include <cstdio>
#include <cstring>

// Compact keys for long EPUB fragment identifiers (#anchor / id attributes).
// Uses FNV-1a 64-bit, matching ZipFile::fnvHash64 and BookMetadataCache::fnvHash64.
class AnchorHash {
 public:
  static constexpr char PREFIX[] = "fnv:";
  static constexpr size_t PREFIX_LEN = 4;
  static constexpr size_t HEX_LEN = 16;
  static constexpr size_t KEY_LEN = PREFIX_LEN + HEX_LEN;

  // Plain fragments longer than this are stored as fnv:<hex> so path#key fits in FOOTNOTE_HREF_LEN.
  static constexpr size_t MAX_PLAIN_FRAGMENT_LEN = FOOTNOTE_HREF_LEN - 32 - 1 - KEY_LEN;

  static uint64_t fnv1a64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  static bool shouldHashFragment(const size_t fragmentLen) { return fragmentLen > MAX_PLAIN_FRAGMENT_LEN; }

  static void writeKey(char* dest, const size_t destSize, const char* fragment, const size_t fragmentLen) {
    if (destSize < KEY_LEN + 1) {
      if (destSize > 0) {
        dest[0] = '\0';
      }
      return;
    }
    const uint64_t hash = fnv1a64(fragment, fragmentLen);
    snprintf(dest, destSize, "%s%016llx", PREFIX, static_cast<unsigned long long>(hash));
  }

  // Normalize a fragment or id attribute for anchor-map storage and lookup.
  static void storageKey(char* dest, const size_t destSize, const char* fragment) {
    if (!fragment || fragment[0] == '\0') {
      if (destSize > 0) {
        dest[0] = '\0';
      }
      return;
    }
    const size_t len = strlen(fragment);
    if (!shouldHashFragment(len)) {
      strncpy(dest, fragment, destSize - 1);
      dest[destSize - 1] = '\0';
      return;
    }
    writeKey(dest, destSize, fragment, len);
  }

  // Store href in dest, replacing a long #fragment with #fnv:<hex> when needed.
  static void copyCompactHref(char* dest, const size_t destSize, const char* href) {
    if (!dest || destSize == 0) {
      return;
    }
    if (!href || href[0] == '\0') {
      dest[0] = '\0';
      return;
    }

    const size_t hrefLen = strlen(href);
    const char* hashPos = strchr(href, '#');

    if (!hashPos) {
      strncpy(dest, href, destSize - 1);
      dest[destSize - 1] = '\0';
      return;
    }

    const char* fragment = hashPos + 1;
    const size_t fragmentLen = strlen(fragment);
    const bool hashFragment = hrefLen >= destSize || shouldHashFragment(fragmentLen);

    if (!hashFragment) {
      strncpy(dest, href, destSize - 1);
      dest[destSize - 1] = '\0';
      return;
    }

    char fragmentKey[KEY_LEN + 1];
    writeKey(fragmentKey, sizeof(fragmentKey), fragment, fragmentLen);

    if (hashPos == href) {
      snprintf(dest, destSize, "#%s", fragmentKey);
    } else {
      snprintf(dest, destSize, "%.*s#%s", static_cast<int>(hashPos - href), href, fragmentKey);
    }
    dest[destSize - 1] = '\0';
  }
};
