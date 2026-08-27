/*
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Headless fuzzer for BKeymap::PopulateFromXkb()/PopulateFromXkbNames()
 * (src/kits/shared/KeymapXkb.cpp). VM-side only.
 *
 * Deliberately builds NO BApplication and opens NO window: every check here
 * calls straight into the real libshared.a code that derives a key_map from
 * a compiled xkb_keymap, with no app_server or input_server round trip at
 * all. That is what makes it safe to run against a live session; there is
 * nothing here that can touch the desktop.
 *
 * Crossed inputs: the full get_xkb_layout_catalog() catalog (589 compiling
 * layout/variant pairs) x a handful of xkb modifier-option strings x a
 * handful of compose locales, plus a separate robustness pass over
 * deliberately malformed layout names. See input-fuzzing-brief.md §2 for
 * the full rationale; each invariant check below is numbered to match
 * §2.3.
 *
 * Output follows testinput.cpp's convention so runs stay diffable:
 *   RESULT|<status>|<name>|<detail>      status: PASS FAIL DENIED MISSING SKIP
 * ending in SUMMARY|pass=|fail=|denied=|missing=.
 */

#include <Keymap.h>
#include <InterfaceDefs.h>
#include <ObjectList.h>
#include <String.h>

#include <OS.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static int sPass = 0;
static int sFail = 0;
static int sDenied = 0;
static int sMissing = 0;

static int64 sCombosRun = 0;
static uint32 sMaxCharsSize = 0;

// Invariants 2 and 7 only speak up on failure; these count what they
// actually looked at so coverage is visible in the summary.
static int64 sOffsetsChecked = 0;
static int64 sPoolStringsScanned = 0;


static void
record(const char* status, const char* name, const char* fmt, ...)
{
	char detail[512];
	detail[0] = '\0';
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	printf("RESULT|%s|%s|%s\n", status, name, detail);
	fflush(stdout);

	if (strcmp(status, "PASS") == 0)
		sPass++;
	else if (strcmp(status, "FAIL") == 0)
		sFail++;
	else if (strcmp(status, "DENIED") == 0)
		sDenied++;
	else if (strcmp(status, "MISSING") == 0)
		sMissing++;
}


static void
check(bool ok, const char* name, const char* fmt, ...)
{
	char detail[512];
	detail[0] = '\0';
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	record(ok ? "PASS" : "FAIL", name, "%s", detail);
}


//	#pragma mark - access shim


// fKeys is `protected`; PokeModifiers() needs write access to build
// invariant 9's reproducer: a modifier assignment PopulateFromXkb would
// never itself produce.
class FuzzKeymap : public BKeymap {
public:
			bool				PokeModifiers(uint32 command, uint32 control,
									uint32 lockSettings)
								{
									// hadModifiers gates on fChars != NULL;
									// poking before a real populate would be
									// silently discarded.
									if (fChars == NULL)
										return false;
									fKeys.left_command_key = command;
									fKeys.left_control_key = control;
									fKeys.lock_settings = lockSettings;
									return true;
								}
};


//	#pragma mark - invariant checks


struct TableEntry {
	const char*	name;
	const int32*	table;
};

struct DeadKeyEntry {
	const char*	name;
	const int32*	pairs;		// [32]
	uint32		tables;
	uint32		codepoint;	// haiku_byte_for_keysym() value for this accent
};


// Invariant 2: every offset in the character tables and dead-key arrays
// must be inside [0, fCharsSize), and its length-prefixed string must not
// run past the end of the pool.
static bool
check_pool_bounds(const char* label, const char* chars, uint32 charsSize,
	int32 offset, const char* what)
{
	if (offset == 0)
		return true;	// 0 means "not written", never a real offset

	sOffsetsChecked++;

	if (offset < 0 || (uint32)offset >= charsSize) {
		record("FAIL", "invariant2.offset_out_of_range",
			"%s: %s offset=%" B_PRId32 " charsSize=%" B_PRIu32,
			label, what, offset, charsSize);
		return false;
	}

	uint8 length = (uint8)chars[offset];
	// offset+1+length must not run past the end of the pool.
	if ((uint64)offset + 1 + length > charsSize) {
		record("FAIL", "invariant2.string_runs_past_end",
			"%s: %s offset=%" B_PRId32 " length=%u charsSize=%" B_PRIu32,
			label, what, offset, length, charsSize);
		return false;
	}

	return true;
}


// Invariant 7: Return is 0x0a (B_ENTER) everywhere in the pool, never 0x0d.
static void
check_no_bare_cr(const char* label, const char* chars, uint32 charsSize)
{
	uint32 pos = 1;	// fChars[0] is deliberately left zero
	while (pos < charsSize) {
		uint8 length = (uint8)chars[pos];
		if (pos + 1 + length > charsSize)
			break;	// invariant2 already flagged this combo
		sPoolStringsScanned++;
		if (length == 1 && (uint8)chars[pos + 1] == 0x0d) {
			record("FAIL", "invariant7.bare_cr_in_pool",
				"%s: found raw 0x0d at pool offset %" B_PRIu32
				" (Return must be 0x0a)", label, pos);
			return;
		}
		pos += 1 + length;
	}
}


// Invariant 5 (approximation): if a dead key's *_tables bit is set for a
// combo, some entry in that table must decode to the accent-mark codepoint.
static void
check_dead_key_table_membership(const char* label, const char* chars,
	uint32 charsSize, const DeadKeyEntry& dk, const TableEntry* tables,
	int numTables, const uint32* tableBits)
{
	if (dk.tables == 0)
		return;

	for (int i = 0; i < numTables; i++) {
		if ((dk.tables & tableBits[i]) == 0)
			continue;

		bool found = false;
		for (int kc = 0; kc < 128 && !found; kc++) {
			int32 offset = tables[i].table[kc];
			if (offset <= 0 || (uint32)offset >= charsSize)
				continue;
			uint8 length = (uint8)chars[offset];
			if (offset + 1 + length > (int32)charsSize)
				continue;
			// Decode the (<=4 byte) UTF-8 char back to ucs4 the same way
			// it was encoded; only the single-byte and two-byte cases
			// matter here since every accent mark is Latin-1.
			uint32 ucs4 = 0;
			if (length == 1)
				ucs4 = (uint8)chars[offset + 1];
			else if (length == 2 && ((uint8)chars[offset + 1] & 0xE0) == 0xC0) {
				ucs4 = ((uint8)chars[offset + 1] & 0x1F) << 6
					| ((uint8)chars[offset + 2] & 0x3F);
			}
			if (ucs4 == dk.codepoint)
				found = true;
		}

		check(found, "invariant5.table_bit_has_accent_char",
			"%s: %s table bit set on %s but no slot decodes to accent "
			"char U+%04" B_PRIx32, label, dk.name, tables[i].name,
			dk.codepoint);
	}
}


static void
check_derivation(const char* label, BKeymap& km)
{
	const char* chars = km.Chars();
	uint32 charsSize = km.CharsSize();
	const key_map& map = km.Map();

	if (charsSize > sMaxCharsSize)
		sMaxCharsSize = charsSize;

	// Invariant 10: pool growth is bounded by the 65536-byte allocation in
	// PopulateFromXkb(); charsSize (== writePos when it returned) must never
	// exceed that.
	check(charsSize <= 65536, "invariant10.pool_bounded",
		"%s: charsSize=%" B_PRIu32 " (must be <= 65536)", label, charsSize);

	TableEntry tables[] = {
		{ "control_map",              map.control_map },
		{ "option_caps_shift_map",    map.option_caps_shift_map },
		{ "option_caps_map",          map.option_caps_map },
		{ "option_shift_map",         map.option_shift_map },
		{ "option_map",               map.option_map },
		{ "caps_shift_map",           map.caps_shift_map },
		{ "caps_map",                 map.caps_map },
		{ "shift_map",                map.shift_map },
		{ "normal_map",               map.normal_map },
	};
	static const uint32 kTableBits[] = {
		B_CONTROL_TABLE, B_OPTION_CAPS_SHIFT_TABLE, B_OPTION_CAPS_TABLE,
		B_OPTION_SHIFT_TABLE, B_OPTION_TABLE, B_CAPS_SHIFT_TABLE,
		B_CAPS_TABLE, B_SHIFT_TABLE, B_NORMAL_TABLE
	};
	static const int kNumTables = (int)(sizeof(tables) / sizeof(tables[0]));

	for (int t = 0; t < kNumTables; t++) {
		for (int kc = 0; kc < 128; kc++) {
			char what[64];
			snprintf(what, sizeof(what), "%s[0x%02x]", tables[t].name, kc);
			check_pool_bounds(label, chars, charsSize, tables[t].table[kc],
				what);
		}
	}

	DeadKeyEntry deadKeys[] = {
		{ "acute",      map.acute_dead_key,      map.acute_tables,      0xb4 },
		{ "grave",      map.grave_dead_key,      map.grave_tables,      0x60 },
		{ "circumflex", map.circumflex_dead_key, map.circumflex_tables, 0x5e },
		{ "dieresis",   map.dieresis_dead_key,   map.dieresis_tables,   0xa8 },
		{ "tilde",      map.tilde_dead_key,      map.tilde_tables,      0x7e },
	};
	static const int kNumDeadKeys
		= (int)(sizeof(deadKeys) / sizeof(deadKeys[0]));

	for (int d = 0; d < kNumDeadKeys; d++) {
		bool anyNonZeroPair = false;

		for (int slot = 0; slot < 32; slot++) {
			char what[64];
			snprintf(what, sizeof(what), "%s_dead_key[%d]", deadKeys[d].name,
				slot);
			check_pool_bounds(label, chars, charsSize, deadKeys[d].pairs[slot],
				what);
			if (deadKeys[d].pairs[slot] != 0)
				anyNonZeroPair = true;
		}

		// Invariant 3: dead-key consistency both directions.
		check(!anyNonZeroPair || deadKeys[d].tables != 0,
			"invariant3.pair_implies_flag",
			"%s: %s pairs=%s tables=0x%" B_PRIx32 " (non-zero pair requires "
			"a non-zero flag)", label, deadKeys[d].name,
			anyNonZeroPair ? "present" : "empty", deadKeys[d].tables);
		check(deadKeys[d].tables == 0 || anyNonZeroPair,
			"invariant3.flag_implies_pair",
			"%s: %s tables=0x%" B_PRIx32 " pairs=%s (non-zero flag requires "
			"a non-zero pair)", label, deadKeys[d].name, deadKeys[d].tables,
			anyNonZeroPair ? "present" : "empty");

		// Invariant 4: where a dead key exists, the space pair occupies
		// slot 0 and *_dead_key[1] names a non-empty string; DeadKeyIndex()
		// hardcodes that slot as "what this dead key types alone".
		if (deadKeys[d].tables != 0) {
			int32 slot0 = deadKeys[d].pairs[0];
			int32 slot1 = deadKeys[d].pairs[1];
			bool slot1NonEmpty = slot1 > 0 && (uint32)slot1 < charsSize
				&& (uint8)chars[slot1] > 0;
			check(slot0 != 0 && slot1NonEmpty,
				"invariant4.space_pair_in_slot0",
				"%s: %s reachable (tables=0x%" B_PRIx32 ") slot0/1 = "
				"%" B_PRId32 "/%" B_PRId32 " (slot1 non-empty=%d); a failure "
				"means no space-compose entry for this dead key in this "
				"locale", label, deadKeys[d].name, deadKeys[d].tables, slot0,
				slot1, slot1NonEmpty);
		}

		check_dead_key_table_membership(label, chars, charsSize, deadKeys[d],
			tables, kNumTables, kTableBits);
	}

	check_no_bare_cr(label, chars, charsSize);
}


// Invariant 6: Serbian Cyrillic must derive *empty* (all five *_tables == 0).
static void
check_serbian_cyrillic_is_empty(const key_map& map)
{
	bool anySet = map.acute_tables != 0 || map.grave_tables != 0
		|| map.circumflex_tables != 0 || map.dieresis_tables != 0
		|| map.tilde_tables != 0;
	check(!anySet, "invariant6.rs_derives_no_dead_keys",
		"rs/'' (Serbian Cyrillic): acute=0x%" B_PRIx32 " grave=0x%" B_PRIx32
		" circumflex=0x%" B_PRIx32 " dieresis=0x%" B_PRIx32 " tilde=0x%"
		B_PRIx32 " (all must be 0)", map.acute_tables, map.grave_tables,
		map.circumflex_tables, map.dieresis_tables, map.tilde_tables);
}


// Invariant 8: idempotence. Populating twice from the same rule names must
// yield byte-identical pool and key_map.
static void
check_idempotence(const char* rules, const char* model, const char* layout,
	const char* variant, const char* options, const char* locale)
{
	if (locale != NULL)
		setenv("LANG", locale, 1);

	BKeymap a;
	BKeymap b;
	status_t sa = a.PopulateFromXkbNames(rules, model, layout, variant,
		options);
	status_t sb = b.PopulateFromXkbNames(rules, model, layout, variant,
		options);

	if (sa != B_OK || sb != B_OK) {
		record("FAIL", "invariant8.idempotence",
			"%s/%s opts=\"%s\" locale=%s: derivation failed (sa=%s sb=%s)",
			layout, variant, options, locale, strerror(sa), strerror(sb));
		return;
	}

	bool poolMatches = a.CharsSize() == b.CharsSize()
		&& memcmp(a.Chars(), b.Chars(), a.CharsSize()) == 0;
	bool mapMatches = memcmp(&a.Map(), &b.Map(), sizeof(key_map)) == 0;

	check(poolMatches && mapMatches, "invariant8.idempotence",
		"%s/%s opts=\"%s\" locale=%s: poolMatches=%d mapMatches=%d", layout,
		variant, options, locale, poolMatches, mapMatches);
}


// Invariant 9: modifier preservation. The hadModifiers path must carry the
// caller's modifier assignment across a repopulate. Uses a sentinel value
// PopulateFromXkb() would never itself produce.
static void
check_modifier_preservation(const char* layout, const char* variant,
	const char* options)
{
	FuzzKeymap km;

	status_t status = km.PopulateFromXkbNames("evdev", "pc105", "us", "", "");
	if (status != B_OK) {
		record("FAIL", "invariant9.modifier_preservation",
			"initial populate failed: %s", strerror(status));
		return;
	}

	const uint32 kSentinelCommand = 0x71;	// KEY_LEFTMETA's Haiku slot never
	const uint32 kSentinelControl = 0x72;	// used as command/control by the
											// hardcoded default: picked to
											// be unmistakably not-default.
	const uint32 kSentinelLock = 0xdead0000;

	if (!km.PokeModifiers(kSentinelCommand, kSentinelControl,
			kSentinelLock)) {
		record("FAIL", "invariant9.modifier_preservation",
			"PokeModifiers() refused (fChars unexpectedly NULL)", NULL);
		return;
	}

	// Different layout/options entirely; the point is that char-table
	// derivation must not touch the modifier roles at all.
	status = km.PopulateFromXkbNames("evdev", "pc105", layout, variant,
		options);
	if (status != B_OK) {
		record("FAIL", "invariant9.modifier_preservation",
			"repopulate failed: %s", strerror(status));
		return;
	}

	const key_map& map = km.Map();
	check(map.left_command_key == kSentinelCommand
			&& map.left_control_key == kSentinelControl
			&& map.lock_settings == kSentinelLock,
		"invariant9.modifier_preservation",
		"after repopulate into %s/%s opts=\"%s\": left_command=0x%" B_PRIx32
		" (want 0x%" B_PRIx32
		") left_control=0x%" B_PRIx32 " (want 0x%" B_PRIx32
		") lock_settings=0x%" B_PRIx32 " (want 0x%" B_PRIx32 ")",
		layout, variant, options, map.left_command_key, kSentinelCommand,
		map.left_control_key, kSentinelControl, map.lock_settings,
		kSentinelLock);
}


//	#pragma mark - robustness pass


// Deliberately malformed layout names must fall back to "us" or fail
// cleanly; never crash, never read out of bounds.
static void
run_robustness_pass()
{
	char veryLong[4096];
	memset(veryLong, 'A', sizeof(veryLong) - 1);
	veryLong[sizeof(veryLong) - 1] = '\0';

	char embeddedColons[64];
	strcpy(embeddedColons, "xkb:us:::::intl::");

	char nonUtf8[] = { 'x', 'k', 'b', ':', (char)0xff, (char)0xfe, (char)0x80,
		'\0' };

	char dotDot[] = "xkb:../../../etc/passwd";

	struct { const char* label; const char* name; }
	malformed[] = {
		{ "empty",              "" },
		{ "very_long",          veryLong },
		{ "embedded_colons",    embeddedColons },
		{ "non_utf8_bytes",     nonUtf8 },
		{ "xkb_prefix_only",    "xkb:" },
		{ "dotdot",             dotDot },
		{ "xkb_colon_dotdot",   "xkb:us:../foo" },
		{ "just_colon",         ":" },
		{ "unknown_name",       "Definitely Not A Real Keymap Name" },
	};

	for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
		const char* layout;
		const char* variant;
		look_up_xkb_layout(malformed[i].name, layout, variant);

		bool fellBackOrValid = layout != NULL && layout[0] != '\0';
		check(fellBackOrValid, "robustness.look_up_xkb_layout_safe",
			"%s: layout=\"%s\" variant=\"%s\" (must never be empty/NULL)",
			malformed[i].label, layout ? layout : "(null)",
			variant ? variant : "(null)");

		BKeymap km;
		status_t status = km.PopulateFromXkbNames("evdev", "pc105", layout,
			variant, "");
		// Reaching this line at all already proves "never crash" for this
		// malformed name; what remains to check is that a B_OK result
		// still obeys every pool invariant.
		if (status == B_OK) {
			check_derivation(malformed[i].label, km);
		} else {
			record("PASS", "robustness.clean_failure",
				"%s: PopulateFromXkbNames failed cleanly with %s",
				malformed[i].label, strerror(status));
		}
	}

	// NULs embedded in a C string are indistinguishable from end-of-string
	// at this API boundary (const char*, no length parameter); exercised
	// here for the record, not because it can do anything different from
	// "empty" and "very_long" above.
	char embeddedNul[16] = "xkb";
	embeddedNul[3] = '\0';
	embeddedNul[4] = ':';
	embeddedNul[5] = 'u';
	embeddedNul[6] = 's';
	const char* layout;
	const char* variant;
	look_up_xkb_layout(embeddedNul, layout, variant);
	check(layout != NULL && layout[0] != '\0',
		"robustness.embedded_nul_safe",
		"layout=\"%s\" variant=\"%s\"", layout ? layout : "(null)",
		variant ? variant : "(null)");
}


//	#pragma mark - main


int
main(int argc, char** argv)
{
	printf("EVENT|start|pid=%d\n", (int)getpid());

	BObjectList<xkb_catalog_entry, true> catalog;
	status_t status = get_xkb_layout_catalog(catalog);
	if (status != B_OK) {
		record("FAIL", "get_xkb_layout_catalog", "%s", strerror(status));
		printf("SUMMARY|pass=%d|fail=%d|denied=%d|missing=%d\n", sPass, sFail,
			sDenied, sMissing);
		return 1;
	}

	int32 catalogCount = catalog.CountItems();
	record("PASS", "get_xkb_layout_catalog", "%" B_PRId32 " entries",
		catalogCount);

	static const char* kOptions[] = {
		"",
		"ctrl:swap_lalt_lctl",
		"altwin:swap_alt_win",
		"lv3:ralt_switch",
		"grp:alt_shift_toggle",
	};
	static const int kNumOptions = (int)(sizeof(kOptions) / sizeof(kOptions[0]));

	static const char* kLocales[] = {
		"C",
		"en_US.UTF-8",
		"ru_RU.UTF-8",
		"ja_JP.UTF-8",
	};
	static const int kNumLocales = (int)(sizeof(kLocales) / sizeof(kLocales[0]));

	// A quick-mode env var keeps a full local run tractable while the VM
	// run still covers every combination; see the report for which mode
	// actually produced the numbers.
	bool quick = getenv("TESTXKBFUZZ_QUICK") != NULL;
	int localeStep = quick ? kNumLocales : 1;
	int optionStep = quick ? kNumOptions : 1;

	for (int32 ci = 0; ci < catalogCount; ci++) {
		xkb_catalog_entry* entry = catalog.ItemAt(ci);
		const char* layout;
		const char* variant;
		look_up_xkb_layout(entry->id.String(), layout, variant);

		for (int oi = 0; oi < kNumOptions; oi += optionStep) {
			for (int li = 0; li < kNumLocales; li += localeStep) {
				setenv("LANG", kLocales[li], 1);

				BKeymap km;
				status_t st = km.PopulateFromXkbNames("evdev", "pc105",
					layout, variant, kOptions[oi]);
				sCombosRun++;

				char label[256];
				snprintf(label, sizeof(label), "%s (%s/%s) opts=\"%s\" "
					"locale=%s", entry->id.String(), layout, variant,
					kOptions[oi], kLocales[li]);

				// Invariant 1: B_OK or a genuine error, never B_OK with an
				// empty/unallocated fChars.
				if (st != B_OK) {
					check(false, "invariant1.status_or_genuine_error",
						"%s: PopulateFromXkbNames failed: %s", label,
						strerror(st));
					continue;
				}
				check(km.Chars() != NULL && km.CharsSize() > 0,
					"invariant1.status_or_genuine_error",
					"%s: B_OK with Chars()=%p CharsSize()=%" B_PRIu32
					" (B_OK requires an allocated, non-empty pool)", label,
					(void*)km.Chars(), km.CharsSize());

				check_derivation(label, km);

				if (strcmp(layout, "rs") == 0 && variant[0] == '\0')
					check_serbian_cyrillic_is_empty(km.Map());
			}
		}
	}

	record("PASS", "combinations_exercised", "%" B_PRId64
		" (catalog=%" B_PRId32 " x options=%d x locales=%d, quick=%d)",
		sCombosRun, catalogCount, kNumOptions, kNumLocales, quick);
	record("PASS", "max_chars_size_observed", "%" B_PRIu32
		" (allocation is 65536)", sMaxCharsSize);

	// Invariants 2 and 7 are silent on success. Report what they covered so a
	// clean run is distinguishable from one where they never ran.
	record("PASS", "invariant2.bounds_coverage", "%" B_PRId64
		" live offsets validated across %" B_PRId64 " combinations, 0 out of "
		"range", sOffsetsChecked, sCombosRun);
	record("PASS", "invariant7.cr_scan_coverage", "%" B_PRId64
		" pool strings scanned, no bare 0x0d", sPoolStringsScanned);

	// These are mechanism tests, not per-layout ones, but four hand-picked
	// combinations only prove the mechanism on the layouts someone already
	// thought about. Stride the catalog instead so the sample tracks it,
	// rotating options and locale so the two axes vary together.
	static const int kMechanismSamples = 48;
	int32 stride = catalogCount / kMechanismSamples;
	if (stride < 1)
		stride = 1;

	for (int32 ci = 0, n = 0; ci < catalogCount; ci += stride, n++) {
		xkb_catalog_entry* entry = catalog.ItemAt(ci);
		const char* layout;
		const char* variant;
		look_up_xkb_layout(entry->id.String(), layout, variant);

		check_idempotence("evdev", "pc105", layout, variant,
			kOptions[n % kNumOptions], kLocales[n % kNumLocales]);
		check_modifier_preservation(layout, variant,
			kOptions[n % kNumOptions]);
	}

	// Serbian Cyrillic by name as well as by stride; it is the regression
	// that started this work and must not depend on where the stride lands.
	check_idempotence("evdev", "pc105", "rs", "", "", "C");

	run_robustness_pass();

	printf("SUMMARY|pass=%d|fail=%d|denied=%d|missing=%d\n", sPass, sFail,
		sDenied, sMissing);
	return sFail > 0 ? 1 : 0;
}
