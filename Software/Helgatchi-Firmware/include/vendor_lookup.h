#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Vendor lookup — resolve OUIs and BT SIG mfg IDs to org names from the raw
// IEEE / BT SIG registries. No short-name curation: lookups return the
// upstream organization name verbatim ("Apple, Inc.", "TASER International,
// Inc.", "Espressif Inc.").
//
// Tables live in flash (~870 KB) with a deduplicated name pool. Forward
// lookup is bsearch; rule-load-time substring matching iterates via the
// vendor_*_at() accessors.
//
// Editing the source data: scripts/refresh_vendor_sources.py to pull the
// latest registries, then any build regenerates include/vendor_tables_data.h.
// ---------------------------------------------------------------------------

// Forward lookups — bsearch over the sorted tables. Returns the org-name
// string pointer (into flash) or nullptr if no match.
const char* vendor_oui_lookup(uint32_t oui_prefix);
const char* vendor_mfg_lookup(uint16_t mfg_id);

// Convenience: pass the first 3 bytes of a MAC, get the vendor name.
const char* vendor_for_mac(const uint8_t mac[6]);

// Table sizes. Stable across builds (within a refresh window).
size_t vendor_oui_count();
size_t vendor_mfg_count();

// Iteration accessors over the full tables. `idx` is 0..count-1; out-of-range
// returns false and leaves the out params untouched. (RulesService no longer
// uses these — oui_org/mfg_org resolve via the forward lookups at match time.)
bool vendor_oui_at(size_t idx, uint32_t* prefix_out, const char** name_out);
bool vendor_mfg_at(size_t idx, uint16_t* mfg_id_out, const char** name_out);
