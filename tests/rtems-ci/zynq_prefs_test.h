#pragma once

// The RTEMS preferences store, on a filesystem.
//
// What is checked here is the store itself: that a value written comes back,
// that a key never written does not, that reset() clears both memory and disk,
// and -- the one worth the most -- that a damaged file is rejected whole rather
// than half-loaded.
//
// What is NOT checked here is surviving a reboot, because there is no
// persistent medium on this board yet: the filesystem is RAM. That half of
// rtems-esphome#16's exit criterion waits on #42, and saying so is better than
// a test that mounts a RAM disk and calls the result persistence.

#include "esphome/components/rtems/preferences.h"
#include "esphome/core/preferences.h"
#include "esphome/core/log.h"

#include <cstdio>

namespace rtems_prefs_test {

static const char *const TAG = "prefs_test";

static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-50s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static void run(const char *path) {
  auto *prefs = esphome::rtems::get_preferences();

  ESP_LOGI(TAG, "--- preferences test ---");

  // --- a value survives a write and a re-read of the file -----------------
  {
    const uint32_t written = 0xa5c3f00d;
    check("save", prefs->save(1, reinterpret_cast<const uint8_t *>(&written), sizeof(written)));
    check("sync writes the store", prefs->sync());

    // Before reading anything back: that the file exists at all. Without this
    // the read-backs below pass out of the in-memory map on a store that never
    // wrote a byte, which is exactly the defect this lane was built and failed
    // to catch the first time.
    {
      FILE *f = ::fopen(path, "rb");
      check("and the store is on disk", f != nullptr);
      if (f != nullptr) {
        ::fclose(f);
      }
    }

    // Re-read the file.  load_store() replaces the in-memory state with what
    // the file says, whatever it says, so this shows the value came off disk
    // rather than out of the map -- which is the part a reboot would otherwise
    // be needed to exercise.
    prefs->load_store();

    uint32_t read_back = 0;
    check("the value loads back from the file",
          prefs->load(1, reinterpret_cast<uint8_t *>(&read_back), sizeof(read_back)));
    check("and it is the value that was written", read_back == written);
  }

  // --- a key that was never written --------------------------------------
  {
    uint32_t scratch = 0;
    check("a key that was never written does not load",
          !prefs->load(99, reinterpret_cast<uint8_t *>(&scratch), sizeof(scratch)));
  }

  // --- a length that does not match ---------------------------------------
  {
    uint8_t two[2] = {};
    check("loading a key at the wrong length fails", !prefs->load(1, two, sizeof(two)));
  }

  // --- several keys, and one rewritten ------------------------------------
  {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {9};
    prefs->save(10, a, sizeof(a));
    prefs->save(11, b, sizeof(b));
    const uint8_t a2[] = {4, 5, 6};
    prefs->save(10, a2, sizeof(a2));
    check("sync with several keys", prefs->sync());

    prefs->load_store();

    uint8_t got[3] = {};
    check("the rewritten key loads", prefs->load(10, got, sizeof(got)));
    check("with the value written last", got[0] == 4 && got[1] == 5 && got[2] == 6);
    uint8_t one = 0;
    check("and the other key is still there", prefs->load(11, &one, 1) && one == 9);
  }

  // --- a damaged store ----------------------------------------------------
  // The checks that matter most. A store that half-loads is worse than one
  // that starts empty, because the caller cannot tell which keys survived.
  //
  // Two corruptions, not one, because the store has two independent defences
  // and a single flipped byte only exercises whichever it happens to land in.
  // A byte in a record's length field is caught by the length sanity check
  // before the checksum is ever computed -- so a test that flips only that
  // byte passes with the checksum comparison deleted, which is how the first
  // version of this test was found to be proving nothing.
  {
    // The layout, so the offsets below are chosen and not guessed: a 16-byte
    // StoreHeader, then per record an 8-byte {key, len} followed by len bytes.
    // Keys are written in ascending order, so with 1, 10 and 11 saved above,
    // offset 20 is key 1's length and offset 24 is the first byte of its value.
    const size_t OFFSET_OF_A_LENGTH = 20;
    const size_t OFFSET_OF_A_VALUE = 24;

    auto rewrite_store = [&]() {
      // A fresh store to damage, since the previous round destroys it.
      const uint8_t a[] = {4, 5, 6};
      const uint8_t b[] = {9};
      const uint32_t v = 0xa5c3f00d;
      prefs->save(1, reinterpret_cast<const uint8_t *>(&v), sizeof(v));
      prefs->save(10, a, sizeof(a));
      prefs->save(11, b, sizeof(b));
      return prefs->sync();
    };

    auto corrupt_at = [&](const char *what, size_t offset) {
      uint8_t buf[512];
      size_t n = 0;
      FILE *f = ::fopen(path, "rb");
      check("the store file exists on disk", f != nullptr);
      if (f == nullptr) {
        return;
      }
      n = ::fread(buf, 1, sizeof(buf), f);
      ::fclose(f);
      check("and it is long enough to hold the byte to damage", n > offset);
      if (n <= offset) {
        return;
      }

      buf[offset] ^= 0xff;
      f = ::fopen(path, "wb");
      check("the damaged store can be written back", f != nullptr);
      if (f != nullptr) {
        ::fwrite(buf, 1, n, f);
        ::fclose(f);
      }

      prefs->load_store();

      uint8_t got[4] = {};
      check(what, !prefs->load(1, got, sizeof(uint32_t)) && !prefs->load(10, got, 3) && !prefs->load(11, got, 1));
    };

    check("a store to damage", rewrite_store());
    corrupt_at("a damaged record length loads nothing at all", OFFSET_OF_A_LENGTH);

    check("a second store to damage", rewrite_store());
    // Only the checksum can catch this one: every length and count still
    // agrees, so the file parses cleanly and gives back a wrong value.
    corrupt_at("a damaged value loads nothing at all", OFFSET_OF_A_VALUE);
  }

  // --- reset --------------------------------------------------------------
  {
    const uint8_t v = 7;
    prefs->save(20, &v, 1);
    prefs->sync();
    check("reset", prefs->reset());

    uint8_t got = 0;
    check("nothing loads after a reset", !prefs->load(20, &got, 1));
    check("and the file is gone", ::fopen(path, "rb") == nullptr);
  }

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER prefs ok");
  }
}

}  // namespace rtems_prefs_test
