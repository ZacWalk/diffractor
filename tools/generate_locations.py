"""
Location data generator for Diffractor.

Downloads geonames data and generates location text files:
- location-countries.txt
- location-states.txt
- location-places.txt

Based on the original C# LocationImport utility.

Data sources:
- https://download.geonames.org/export/dump/
"""

import os
import re
import sys
import json
import math
import shutil
import zipfile
import unicodedata
import urllib.request
from pathlib import Path
from collections import defaultdict
from dataclasses import dataclass, field


# ============================================================================
# Configuration
# ============================================================================

GEONAMES_BASE_URL = "https://download.geonames.org/export/dump/"

# Files to download from geonames
GEONAMES_FILES = {
    "allCountries": f"{GEONAMES_BASE_URL}allCountries.zip",
    "admin1CodesASCII": f"{GEONAMES_BASE_URL}admin1CodesASCII.txt",
    "countryInfo": f"{GEONAMES_BASE_URL}countryInfo.txt",
    "alternateNamesV2": f"{GEONAMES_BASE_URL}alternateNamesV2.zip",
}

# URL for structured country alternate names
COUNTRIES_EXTRA_URL = "https://raw.githubusercontent.com/mledoze/countries/master/dist/countries.json"

# Minimum population for populated places to include globally
MIN_POPULATION = 1000

# Countries where neighborhoods, small localities, and known-population places
# provide useful extra autocomplete and reverse-geocoding coverage.
PRIORITY_COUNTRIES = {"US", "GB", "DE", "AU", "NZ", "NL", "CA"}

# Administrative seats are useful even when GeoNames has no population value.
ADMINISTRATIVE_FEATURE_CODES = {"PPLC", "PPLA", "PPLA2", "PPLA3", "PPLA4", "PPLA5", "PPLG"}

# Neighborhoods and small inhabited localities improve local reverse geocoding.
PRIORITY_LOCAL_FEATURE_CODES = {"PPLX", "PPLL"}

# Obsolete populated-place records are poor autocomplete and reverse-geocoding results.
EXCLUDED_FEATURE_CODES = {"PPLH", "PPLQ", "PPLW"}

# Maximum alternate names read per record before de-duplication.
MAX_ALT_NAMES = 16

# Extra (untagged) alternate names written per record. Alternate names are half the shipped
# file and every one of them is another way for a query to land on the wrong place, so the
# written budget is much smaller than the budget read.
MAX_EXTRA_NAMES = 6

# GeoNames writes '00' as the admin1 code meaning "no region"; admin1CodesASCII.txt carries no
# name for it, so it can only ever render as a region that does not exist.
UNKNOWN_ADMIN1_CODES = {"", "00"}

# Two records this close, holding the same name or the same population inside one country, are
# one real place recorded twice rather than two places a user could tell apart.
DUPLICATE_MERGE_KM = 10.0

# Console output interval
CONSOLE_OUTPUT_INTERVAL = 10000


# ============================================================================
# Display-language table (issue #119)
# ============================================================================
#
# Each language's index in this list is its STABLE bit position in a place record's
# language bitmap (the langmask column). For every place we emit one localized name per
# set bit, ordered by bit index, immediately after the default name column. The C++ loader
# (src/model_locations.cpp, location_language_codes) mirrors this list exactly and MUST stay
# in sync. NEVER reorder or remove entries - bit positions are baked into location-places.txt.
# Codes are geonames isolanguage values (ISO-639-1) and match Diffractor's .po language codes
# for the shipped UI languages. Room is left for future UI languages (32-bit budget).
LANGUAGE_BITS = [
    "en", "es", "de", "fr", "it", "pt", "nl", "ru", "uk", "pl", "cs", "tr", "ja", "ko", "zh", "ar",
    "he", "hi", "th", "vi", "id", "sv", "no", "da", "fi", "el", "hu", "ro", "bg", "sr", "hr", "ca",
]

assert len(LANGUAGE_BITS) <= 32, "language bitmap is a 32-bit integer"

LANG_TO_BIT = {code: i for i, code in enumerate(LANGUAGE_BITS)}

# Legacy / alternate geonames isolanguage codes mapped onto our canonical codes.
LANG_ALIASES = {
    "nb": "no",  # Norwegian Bokmal
    "nn": "no",  # Norwegian Nynorsk
    "iw": "he",  # legacy Hebrew
    "in": "id",  # legacy Indonesian
}

# ISO 639-1 (our codes) -> ISO 639-3, used to read the language-tagged country name
# translations in the mledoze countries.json ("translations" is keyed by 639-3). Languages
# absent from the source simply keep the English/default country name.
LANG_ISO3 = {
    "en": "eng", "es": "spa", "de": "deu", "fr": "fra", "it": "ita", "pt": "por", "nl": "nld",
    "ru": "rus", "uk": "ukr", "pl": "pol", "cs": "ces", "tr": "tur", "ja": "jpn", "ko": "kor",
    "zh": "zho", "ar": "ara", "he": "heb", "hi": "hin", "th": "tha", "vi": "vie", "id": "ind",
    "sv": "swe", "no": "nor", "da": "dan", "fi": "fin", "el": "ell", "hu": "hun", "ro": "ron",
    "bg": "bul", "sr": "srp", "hr": "hrv", "ca": "cat",
}


# ============================================================================
# Qualification level and record flags (locations.md 2.1 / 2.2)
# ============================================================================
#
# The flags column is fixed width and is written immediately BEFORE the name column,
# because the name column is followed by a variable-length run of localized names. The
# C++ loader (src/model_locations.cpp, Cols::flags) mirrors this layout.
#
#   bits 0-1  qualification level: the smallest name form that uniquely identifies the
#             place to a user. 0 = name, 1 = name + country, 2 = name + region + country.
#             3 is reserved; a loader that sees it treats the record as level 2.
#   bit  2    extent feature: matched by bounding box, no meaningful centroid. Excluded
#             from find_closest / find_largest / radius search. Populated places are
#             never extent features; water bodies (location-waters.txt) are.
#   bits 3-31 reserved, MUST be written as zero and ignored on read.

LEVEL_NAME = 0
LEVEL_NAME_COUNTRY = 1
LEVEL_NAME_REGION_COUNTRY = 2

FLAG_LEVEL_MASK = 0x3
FLAG_EXTENT = 0x4

_FOLD_STRIP_RE = re.compile(r"[^0-9a-z]+")
_FOLD_SPACE_RE = re.compile(r"\s+")


def _strip_accents(name: str) -> str:
    decomposed = unicodedata.normalize("NFKD", name)
    return "".join(c for c in decomposed if not unicodedata.combining(c))


def normalize_place_name(name: str) -> str:
    """Fold a place name for collision detection: case-, accent- and punctuation-insensitive.

    Mirrors the folding search uses so that a name a user cannot distinguish by typing is
    treated as a collision here.
    """
    return _FOLD_STRIP_RE.sub("", _strip_accents(name).casefold())


def fold_for_search(name: str) -> str:
    """Fold a name the way the search index folds it (str::normalize_for_compare).

    Case and accents disappear and whitespace runs collapse, but punctuation is kept. Two
    names with the same fold are the same query, so keeping both costs bytes and index
    entries and buys no recall at all.
    """
    return _FOLD_SPACE_RE.sub(" ", _strip_accents(name).casefold()).strip()


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance in kilometres."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    d_phi = p2 - p1
    d_lambda = math.radians(lon2 - lon1)
    h = math.sin(d_phi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(d_lambda / 2) ** 2
    return 2 * 6371.0 * math.asin(min(1.0, math.sqrt(h)))


# GeoNames disambiguates colliding names with a trailing parenthetical, and some records carry
# a truncated one with no closing bracket. Either way the bracket is editorial apparatus, not
# part of the name, and locations.md 2.1 already qualifies a name with its region and country.
_TRAILING_PAREN_RE = re.compile(r"\s*\(([^()]*)\)?\s*$")


def split_parenthetical_name(name: str) -> tuple[str, list[str]]:
    """Return (display name, extra search names) for a name with a trailing '(...)' suffix.

    The full original spelling and the bracketed text are both returned as extra names, so
    stripping the bracket narrows what is displayed without narrowing what is findable.
    """
    head = name
    extras: list[str] = []

    # Two rounds covers `Name (A) (B)`; more than that is malformed rather than qualified.
    for _ in range(2):
        match = _TRAILING_PAREN_RE.search(head)
        if not match:
            break

        candidate = head[:match.start()].strip()
        if len(candidate) < 2:
            break

        inner = (match.group(1) or "").strip()
        if len(inner) >= 2:
            extras.append(inner)

        head = candidate

    if head == name:
        return name, []

    return head, [name] + extras


def assign_qualification_levels(records) -> dict[int, int]:
    """Assign locations.md 2.2 qualification levels over the records actually emitted.

    Records must expose ``name``, ``country_code``, ``state_code`` and ``population`` and
    accept a ``flags`` attribute. Levels are computed from DEFAULT names only, so a record
    qualifies identically in every UI language. Returns a level -> count histogram.

    The caller is responsible for having sorted the records first; assignment itself is
    order-independent, so regeneration from the same snapshot is byte-identical.
    """
    by_name: dict[str, list] = {}

    for record in records:
        by_name.setdefault(normalize_place_name(record.name), []).append(record)

    histogram = {LEVEL_NAME: 0, LEVEL_NAME_COUNTRY: 0, LEVEL_NAME_REGION_COUNTRY: 0}

    for collisions in by_name.values():
        if len(collisions) == 1:
            level = LEVEL_NAME
            collisions[0].flags = (collisions[0].flags & ~FLAG_LEVEL_MASK) | level
            histogram[level] += 1
            continue

        by_country: dict[str, list] = {}

        for record in collisions:
            by_country.setdefault(record.country_code, []).append(record)

        for same_country in by_country.values():
            level = LEVEL_NAME_COUNTRY if len(same_country) == 1 else LEVEL_NAME_REGION_COUNTRY

            for record in same_country:
                record.flags = (record.flags & ~FLAG_LEVEL_MASK) | level
                histogram[level] += 1

    return histogram


# ============================================================================
# Data Classes
# ============================================================================

@dataclass
class StateRecord:
    """Represents a state/province/admin1 region."""
    code: str
    name: str
    
    def __str__(self) -> str:
        return f"{self.code}\t{self.name}"


@dataclass
class CountryRecord:
    """Represents a country."""
    code: str
    name: str
    alts: list[str] = field(default_factory=list)
    # Localized country names keyed by LANGUAGE_BITS bit index (issue #119).
    localized: dict[int, str] = field(default_factory=dict)

    def __str__(self) -> str:
        # Localized names in bit order build the langmask; a localized name equal to the
        # default name is skipped (the loader falls back to the default for that language).
        default_key = self.name.casefold()
        mask = 0
        localized_cols: list[str] = []

        for bit in range(len(LANGUAGE_BITS)):
            nm = self.localized.get(bit)
            if nm and nm.casefold() != default_key:
                mask |= (1 << bit)
                localized_cols.append(nm)

        seen = {default_key}
        seen.update(c.casefold() for c in localized_cols)

        extra: list[str] = []
        for nm in self.alts:
            key = nm.casefold()
            if key not in seen:
                seen.add(key)
                extra.append(nm)

        cols = [self.code, self.name, str(mask)]
        cols.extend(localized_cols)
        cols.extend(extra)
        return "\t".join(cols)


@dataclass
class CityRecord:
    """Represents a city/place."""
    id: int
    name: str
    latitude: float
    longitude: float
    state_code: str
    country_code: str
    population: int
    alternate_names: list[str] = field(default_factory=list)
    # Localized display names keyed by LANGUAGE_BITS bit index (issue #119).
    localized: dict[int, str] = field(default_factory=dict)
    # Qualification level and record flags (locations.md 2.1); set by assign_qualification_levels.
    flags: int = 0

    def __str__(self) -> str:
        # Localized names in bit order build the langmask; a localized name that equals the
        # default name is skipped (the C++ loader falls back to the default name for that
        # language, giving the identical result without a wasted column). The comparison here
        # is exact, because `Munchen` and `München` are the same query but not the same label.
        default_key = self.name.casefold()
        mask = 0
        localized_cols: list[str] = []

        for bit in range(len(LANGUAGE_BITS)):
            nm = self.localized.get(bit)
            if nm and nm.casefold() != default_key:
                mask |= (1 << bit)
                localized_cols.append(nm)

        # Extra (untagged) alternate names exist only for search recall, so they are de-duplicated
        # by the search fold rather than exactly: an accent-only variant is already findable.
        seen = {fold_for_search(self.name)}
        seen.update(fold_for_search(c) for c in localized_cols)

        extra: list[str] = []
        for nm in self.alternate_names:
            key = fold_for_search(nm)
            if not key or key in seen:
                continue
            seen.add(key)
            extra.append(nm)
            if len(extra) >= MAX_EXTRA_NAMES:
                break

        cols = [
            str(self.id),
            f"{self.latitude:.5g}",
            f"{self.longitude:.5g}",
            self.state_code,
            self.country_code,
            str(self.population),
            str(mask),
            str(self.flags),
            self.name,
        ]
        cols.extend(localized_cols)
        cols.extend(extra)
        return "\t".join(cols)


# ============================================================================
# Download Functions
# ============================================================================

def download_file(url: str, dest_path: Path, desc: str = "") -> None:
    """Download a file with progress indication."""
    print(f"  Downloading {desc or dest_path.name}...")
    temp_path = dest_path.with_suffix(dest_path.suffix + ".download")
    
    def progress_hook(block_num, block_size, total_size):
        if total_size > 0:
            downloaded = block_num * block_size
            percent = min(100, downloaded * 100 // total_size)
            mb_downloaded = downloaded / (1024 * 1024)
            mb_total = total_size / (1024 * 1024)
            sys.stdout.write(f"\r    {percent}% ({mb_downloaded:.1f}/{mb_total:.1f} MB)")
            sys.stdout.flush()
    
    try:
        urllib.request.urlretrieve(url, temp_path, progress_hook)
        os.replace(temp_path, dest_path)
        print()  # New line after progress
    except Exception as e:
        temp_path.unlink(missing_ok=True)
        print(f"\n  Error downloading {url}: {e}")
        raise


def download_geonames_data(output_dir: Path) -> dict[str, Path]:
    """Download all required geonames data files."""
    print("Downloading geonames data...")
    
    output_dir.mkdir(parents=True, exist_ok=True)
    paths = {}
    
    for name, url in GEONAMES_FILES.items():
        if url.endswith(".zip"):
            zip_path = output_dir / f"{name}.zip"
            txt_path = output_dir / f"{name}.txt"
            
            download_file(url, zip_path, f"{name}.zip")
            
            print(f"  Extracting {name}.txt...")
            temp_path = txt_path.with_suffix(txt_path.suffix + ".extract")
            try:
                with zipfile.ZipFile(zip_path, 'r') as zf:
                    with zf.open(f"{name}.txt") as source, open(temp_path, 'wb') as dest:
                        shutil.copyfileobj(source, dest)
                os.replace(temp_path, txt_path)
            except Exception:
                temp_path.unlink(missing_ok=True)
                raise
            
            paths[name] = txt_path
        else:
            txt_path = output_dir / f"{name}.txt"
            download_file(url, txt_path, f"{name}.txt")
            paths[name] = txt_path
    
    # Download countries extra data
    countries_extra_path = output_dir / "countries_extra.json"
    download_file(COUNTRIES_EXTRA_URL, countries_extra_path, "countries_extra.json")
    paths["countries_extra"] = countries_extra_path
    
    return paths


# ============================================================================
# States Processing
# ============================================================================

def load_states(states_file: Path) -> list[StateRecord]:
    """Load states from admin1CodesASCII.txt."""
    print(f"Loading states from {states_file.name}...")
    
    states = []
    records_loaded = 0
    placeholders_skipped = 0
    
    with open(states_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split('\t')
            if len(parts) >= 2:
                code = parts[0]  # e.g., "US.CA"
                name = parts[1]

                # `XX.00` is GeoNames' "no region" placeholder, historically named
                # `<Country> (general)`. It is not a region a user was ever in.
                if code.split('.', 1)[-1] in UNKNOWN_ADMIN1_CODES:
                    placeholders_skipped += 1
                    continue

                states.append(StateRecord(code=code, name=name))
                records_loaded += 1
                
                if records_loaded % CONSOLE_OUTPUT_INTERVAL == 0:
                    print(f"\r  Loaded {records_loaded} states...", end='')
    
    print(f"\r  Loaded {records_loaded} states, completed.")
    if placeholders_skipped:
        print(f"  Skipped {placeholders_skipped} placeholder admin1 rows.")
    return states


def process_states(states_file: Path, output_file: Path) -> None:
    """Process states and write output file."""
    print(f"Using states file: {states_file}")
    print(f"Producing states file: {output_file}")
    
    states = load_states(states_file)
    
    # Sort by code
    print("  Sorting states...")
    states.sort(key=lambda s: s.code)
    print("  Sorting states, completed.")
    
    # Write output
    records_written = 0
    with open(output_file, 'w', encoding='utf-8', newline='') as f:
        for state in states:
            f.write(str(state))
            f.write('\n')
            records_written += 1
            
            if records_written % CONSOLE_OUTPUT_INTERVAL == 0:
                print(f"\r  Wrote {records_written} states...", end='')
    
    print(f"\r  Wrote {records_written} states, completed.")


# ============================================================================
# Countries Processing
# ============================================================================

def load_countries(countries_file: Path, countries_extra_file: Path) -> list[CountryRecord]:
    """Load countries from countryInfo.txt and add alternate names from extra file."""
    print(f"Loading countries from {countries_file.name}...")
    
    countries = {}
    records_loaded = 0
    
    # Load main country info
    with open(countries_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split('\t')
            if len(parts) >= 5:
                code = parts[0]  # ISO code
                name = parts[4]  # Country name
                
                countries[code] = CountryRecord(code=code, name=name)
                records_loaded += 1
                
                if records_loaded % CONSOLE_OUTPUT_INTERVAL == 0:
                    print(f"\r  Loaded {records_loaded} countries...", end='')
    
    print(f"\r  Loaded {records_loaded} countries.")
    
    # Load alternate names from structured extra data. CSV cannot safely preserve
    # aliases which themselves contain commas.
    print(f"  Loading alternate names from {countries_extra_file.name}...")
    max_alts = 0

    with open(countries_extra_file, 'r', encoding='utf-8') as f:
        extra_countries = json.load(f)

    for extra_country in extra_countries:
        code = extra_country.get("cca2", "")
        if code not in countries:
            continue

        alt_parts = list(extra_country.get("altSpellings", []))
        if code == "US":
            alt_parts.extend(["United States", "United States of America", "America", "the States",
                              "US", "U.S.", "USA", "U.S.A."])

        seen = {code.casefold(), countries[code].name.casefold()}
        unique_alts = []
        for alt in alt_parts:
            alt = alt.strip()
            key = alt.casefold()
            if alt and '\t' not in alt and '\r' not in alt and '\n' not in alt and key not in seen:
                seen.add(key)
                unique_alts.append(alt)
            if len(unique_alts) >= MAX_ALT_NAMES:
                break

        countries[code].alts = unique_alts
        max_alts = max(max_alts, len(unique_alts))

        # Localized country names (issue #119). name.common is the English name; the
        # "translations" object is keyed by ISO 639-3 with a localized "common" name.
        translations = extra_country.get("translations", {}) or {}
        english_common = (extra_country.get("name", {}) or {}).get("common", "")
        localized: dict[int, str] = {}

        for bit, iso1 in enumerate(LANGUAGE_BITS):
            if iso1 == "en":
                nm = english_common
            else:
                iso3 = LANG_ISO3.get(iso1)
                entry = translations.get(iso3) if iso3 else None
                nm = (entry or {}).get("common", "") if isinstance(entry, dict) else ""

            nm = nm.strip() if nm else ""
            if nm and '\t' not in nm and '\r' not in nm and '\n' not in nm:
                localized[bit] = nm

        countries[code].localized = localized

    print(f"  Maximum alternate names: {max_alts}")
    
    return list(countries.values())


def process_countries(countries_file: Path, countries_extra_file: Path, output_file: Path) -> None:
    """Process countries and write output file."""
    print(f"Using countries file: {countries_file}")
    print(f"Producing countries file: {output_file}")
    
    countries = load_countries(countries_file, countries_extra_file)
    
    # Sort by code
    print("  Sorting countries...")
    countries.sort(key=lambda c: c.code)
    print("  Sorting countries, completed.")
    
    # Write output
    records_written = 0
    with open(output_file, 'w', encoding='utf-8', newline='') as f:
        for country in countries:
            f.write(str(country))
            f.write('\n')
            records_written += 1
            
            if records_written % CONSOLE_OUTPUT_INTERVAL == 0:
                print(f"\r  Writing {records_written} countries...", end='')
    
    print(f"\r  Writing {records_written} countries, completed.")


# ============================================================================
# Cities Processing
# ============================================================================

def compact_alternate_names(actual_name: str, other_names: str) -> list[str]:
    """Compact alternate names, removing duplicates and limiting count."""
    if not other_names:
        return []
    
    candidates = other_names.split(',')
    results = []
    seen = {actual_name.casefold()}
    
    for name in candidates:
        name = name.strip()
        if not name:
            continue
        key = name.casefold()
        if '\t' in name or '\r' in name or '\n' in name or key in seen:
            continue
        seen.add(key)
        if len(results) >= MAX_ALT_NAMES:
            break
        results.append(name)
    
    return results


def should_include_place(country: str, feature_code: str, population: int) -> bool:
    """Return whether a populated-place record provides enough user value to retain.

    A section or locality (PPLX/PPLL) is admitted on population like any other place, and
    additionally without a population inside PRIORITY_COUNTRIES. Gating it by feature code first
    was measured against the snapshot and rejected: only 67 populated sections worldwide outrank
    the nearest real town, and those are genuine larger subdivisions such as the Bermuda parishes,
    while 3,675 carry a true sub-population that labels a photo better than the enclosing city.
    """
    if feature_code in EXCLUDED_FEATURE_CODES:
        return False
    if population >= MIN_POPULATION or feature_code in ADMINISTRATIVE_FEATURE_CODES:
        return True
    if country not in PRIORITY_COUNTRIES:
        return False
    return feature_code in PRIORITY_LOCAL_FEATURE_CODES or (feature_code == "PPL" and population > 0)


def load_cities(cities_file: Path) -> list[CityRecord]:
    """Load cities from allCountries.txt (or cities1000.txt)."""
    print(f"Loading cities from {cities_file.name}...")
    print("  (This may take several minutes for allCountries.txt...)")
    
    cities = []
    records_loaded = 0
    max_alt_names = 0
    names_unbracketed = 0
    admin1_blanked = 0
    
    # Column indices for geonames allCountries.txt
    # geonameid, name, asciiname, alternatenames, latitude, longitude, 
    # feature class, feature code, country code, cc2, admin1 code, 
    # admin2 code, admin3 code, admin4 code, population, elevation, 
    # dem, timezone, modification date
    COL_ID = 0
    COL_NAME = 1
    COL_ALT_NAMES = 3
    COL_LATITUDE = 4
    COL_LONGITUDE = 5
    COL_FEATURE_CLASS = 6
    COL_FEATURE_CODE = 7
    COL_COUNTRY_CODE = 8
    COL_ADMIN1_CODE = 10
    COL_POPULATION = 14
    
    with open(cities_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            
            parts = line.split('\t')
            if len(parts) < 15:
                continue
            
            try:
                feature_class = parts[COL_FEATURE_CLASS]
                feature_code = parts[COL_FEATURE_CODE]
                country = parts[COL_COUNTRY_CODE]
                pop = int(parts[COL_POPULATION]) if parts[COL_POPULATION] else 0
                
                # Only include populated places (feature class P)
                if feature_class != 'P':
                    continue
                
                if not should_include_place(country, feature_code, pop):
                    continue

                name = parts[COL_NAME]
                lname = name.lower()

                # Skip certain patterns
                if '(former)' in lname or 'diocese' in lname:
                    continue

                name, bracket_names = split_parenthetical_name(name)
                if bracket_names:
                    names_unbracketed += 1

                alt_names = bracket_names + compact_alternate_names(name, parts[COL_ALT_NAMES])

                if len(alt_names) > max_alt_names:
                    max_alt_names = len(alt_names)

                # An unknown admin1 code can only render as a region that does not exist.
                admin1 = parts[COL_ADMIN1_CODE]
                if admin1 in UNKNOWN_ADMIN1_CODES:
                    if admin1:
                        admin1_blanked += 1
                    admin1 = ""

                city = CityRecord(
                    id=int(parts[COL_ID]),
                    name=name,
                    latitude=float(parts[COL_LATITUDE]),
                    longitude=float(parts[COL_LONGITUDE]),
                    state_code=admin1,
                    country_code=country,
                    population=pop,
                    alternate_names=alt_names
                )
                
                cities.append(city)
                records_loaded += 1
                
                if records_loaded % CONSOLE_OUTPUT_INTERVAL == 0:
                    print(f"\r  Loaded {records_loaded} cities...", end='')
            
            except (ValueError, IndexError) as e:
                continue  # Skip malformed lines
    
    print(f"\r  Loaded {records_loaded} cities, completed.")
    print(f"  Maximum alternate names: {max_alt_names}")
    print(f"  Names with a bracketed suffix moved to alternates: {names_unbracketed}")
    print(f"  Unknown admin1 codes blanked: {admin1_blanked}")

    return cities


def load_alternate_names(alt_names_file: Path, city_ids: set[int]) -> dict[int, dict[int, str]]:
    """Load localized alternate names (issue #119) for the given geonameids.

    Returns a mapping geonameid -> {language bit index -> best localized name}. Only
    languages in LANGUAGE_BITS are kept; colloquial and historic variants are ignored.
    Preferred names win over plain names, which win over short (abbreviated) names.
    """
    print(f"Loading localized names from {alt_names_file.name}...")
    print("  (This may take a while for alternateNamesV2.txt...)")

    # geonameid -> {bit -> (rank, name)}
    best: dict[int, dict[int, tuple[int, str]]] = {}
    records_loaded = 0

    with open(alt_names_file, 'r', encoding='utf-8') as f:
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) < 4:
                continue

            try:
                geonameid = int(parts[1])
            except ValueError:
                continue

            if geonameid not in city_ids:
                continue

            iso = parts[2]
            # Fold regional/script subtags (zh-Hant, pt-BR, en-GB, ...) onto the base
            # language subtag so those localized names still map to their language bit.
            code = iso.split('-', 1)[0].lower()
            code = LANG_ALIASES.get(code, code)
            bit = LANG_TO_BIT.get(code)
            if bit is None:
                continue

            is_preferred = len(parts) > 4 and parts[4] == '1'
            is_short = len(parts) > 5 and parts[5] == '1'
            is_colloquial = len(parts) > 6 and parts[6] == '1'
            is_historic = len(parts) > 7 and parts[7] == '1'

            if is_colloquial or is_historic:
                continue

            name = parts[3].strip()
            if not name or '\t' in name or '\r' in name or '\n' in name:
                continue

            rank = 3 if is_preferred else (1 if is_short else 2)

            record = best.setdefault(geonameid, {})
            current = record.get(bit)
            if current is None or rank > current[0]:
                record[bit] = (rank, name)

            records_loaded += 1
            if records_loaded % (CONSOLE_OUTPUT_INTERVAL * 10) == 0:
                print(f"\r  Matched {records_loaded} localized names...", end='')

    print(f"\r  Matched {records_loaded} localized names, completed.")

    return {gid: {bit: nm for bit, (_, nm) in langs.items()} for gid, langs in best.items()}


# ============================================================================
# Duplicate collapsing
# ============================================================================
#
# GeoNames records one real place more than once often enough to matter: the same name twice a
# few hundred metres apart, or two names for one town carrying the identical population figure
# (`Bedok New Town` and `Ulu Bedok`, both 276,990). Every such pair is a second identity a user
# cannot tell from the first -- a duplicate group header, a duplicate completion, and a
# find_largest answer that depends on which row happened to win.


def _merge_duplicate_names(survivor: CityRecord, dropped: CityRecord) -> None:
    """Fold a duplicate's searchable names into the record that replaces it."""
    survivor.alternate_names = ([dropped.name] + survivor.alternate_names +
                                dropped.alternate_names)

    for bit, nm in dropped.localized.items():
        survivor.localized.setdefault(bit, nm)


def names_are_variants(a: str, b: str) -> bool:
    """Return whether two names are spellings of one name rather than two different names.

    An identical population is strong evidence of a duplicate but not proof: two neighbouring
    towns can share a census figure, and merging those would put one town's photos under the
    other town's name. So the names must agree on a long prefix or one must contain the other,
    which keeps `Al Araad`/`Al'Arad` and `Rio Turbio`/`Yacimientos Rio Turbio` while rejecting
    `Haedo`/`San Antonio de Padua`. A merely shared word is not enough -- `Saint-Edouard` and
    `Saint-Jacques-le-Mineur` are two municipalities, not two spellings.
    """
    fa, fb = normalize_place_name(a), normalize_place_name(b)
    if not fa or not fb:
        return False

    short, long = (fa, fb) if len(fa) <= len(fb) else (fb, fa)

    if len(short) >= 5 and short in long:
        return True

    common = 0
    for ca, cb in zip(fa, fb):
        if ca != cb:
            break
        common += 1

    return common >= 0.7 * len(short)


def _collapse_group(records: list[CityRecord], merge_km: float,
                    accept=None) -> list[tuple[CityRecord, CityRecord]]:
    """Return (survivor, duplicate) pairs within one candidate group.

    Records must arrive in priority order; a record is dropped when an already-kept record of
    the same group lies within merge_km and `accept` allows the pair. A 0.25 degree bucket is
    at least 27 km tall, so one ring of neighbours always covers the merge distance.
    """
    cells: dict[tuple[int, int], list[CityRecord]] = defaultdict(list)
    pairs: list[tuple[CityRecord, CityRecord]] = []

    for record in records:
        cx = int(math.floor(record.latitude * 4))
        cy = int(math.floor(record.longitude * 4))
        survivor = None

        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for kept in cells.get((cx + dx, cy + dy), ()):
                    if haversine_km(record.latitude, record.longitude,
                                    kept.latitude, kept.longitude) > merge_km:
                        continue
                    if accept and not accept(kept, record):
                        continue
                    survivor = kept
                    break
                if survivor:
                    break
            if survivor:
                break

        if survivor is None:
            cells[(cx, cy)].append(record)
        else:
            pairs.append((survivor, record))

    return pairs


def _console_safe(text: str) -> str:
    """Place names are UTF-8 but the console rarely is, so never let a diagnostic abort a run."""
    encoding = sys.stdout.encoding or "utf-8"
    return text.encode(encoding, "replace").decode(encoding, "replace")


def _collapse_pass(cities: list[CityRecord], key_fn, order_key, label: str,
                   accept=None) -> list[CityRecord]:
    groups: dict[object, list[CityRecord]] = defaultdict(list)

    for city in cities:
        key = key_fn(city)
        if key is not None:
            groups[key].append(city)

    dropped_ids: set[int] = set()
    examples: list[str] = []

    for group in groups.values():
        if len(group) < 2:
            continue

        group.sort(key=order_key)

        for survivor, duplicate in _collapse_group(group, DUPLICATE_MERGE_KM, accept):
            _merge_duplicate_names(survivor, duplicate)
            dropped_ids.add(duplicate.id)
            if len(examples) < 8:
                examples.append(f"{duplicate.name} -> {survivor.name} ({survivor.country_code})")

    print(f"  {label}: collapsed {len(dropped_ids)} records.")
    for example in examples:
        print(f"    {_console_safe(example)}")

    return [c for c in cities if c.id not in dropped_ids]


def _written_coordinate(record: CityRecord) -> tuple[str, str]:
    """The coordinate as the file actually stores it, which is what lookups can distinguish."""
    return (f"{record.latitude:.5g}", f"{record.longitude:.5g}")


def _shadows_at_same_point(survivor: CityRecord, dropped: CityRecord) -> bool:
    """Whether the survivor so plainly dominates that the dropped record is never a better answer.

    Two records written at one coordinate cannot be told apart by a lookup, so whichever the
    k-d tree happens to reach first wins -- Munich's `Altstadt` district beat `Munich` itself
    purely because collapsing shifted the tree. Deciding it by size is only honest where one
    record clearly contains the other, so a district is folded into its city while two villages
    of comparable size are left alone rather than one being renamed to the other.
    """
    return dropped.population == 0 or survivor.population >= 4 * dropped.population


def collapse_duplicate_places(cities: list[CityRecord]) -> list[CityRecord]:
    """Collapse records that describe one real place, keeping every spelling as an alternate."""
    print("  Collapsing duplicate places...")

    cities = _collapse_pass(
        cities,
        lambda c: (fold_for_search(c.name), c.country_code),
        lambda c: (-c.population, c.id),
        "same name and country")

    # An identical non-trivial population inside one country and one town's width, carried by two
    # spellings of one name, is GeoNames copying a single census figure onto the same settlement.
    cities = _collapse_pass(
        cities,
        lambda c: (c.country_code, c.population) if c.population >= MIN_POPULATION else None,
        lambda c: c.id,
        "same population and related name",
        lambda a, b: names_are_variants(a.name, b.name))

    cities = _collapse_pass(
        cities,
        lambda c: (*_written_coordinate(c), c.country_code),
        lambda c: (-c.population, c.id),
        "one point shared by a place and its district",
        _shadows_at_same_point)

    return cities


def process_cities(cities_file: Path, alt_names_file: Path, output_file: Path) -> None:
    """Process cities and write output file."""
    print(f"Using cities file: {cities_file}")
    print(f"Producing cities file: {output_file}")
    
    cities = load_cities(cities_file)

    # Attach localized display names (issue #119) keyed by geonameid.
    city_ids = {c.id for c in cities}
    localized = load_alternate_names(alt_names_file, city_ids)
    attached = 0
    for city in cities:
        names = localized.get(city.id)
        if names:
            city.localized = names
            attached += 1
    print(f"  Attached localized names to {attached} of {len(cities)} places.")

    cities = collapse_duplicate_places(cities)
    print(f"  {len(cities)} places remain after collapsing duplicates.")

    # Sort by name, state, country, lat, lon
    print("  Sorting cities...")
    cities.sort(key=lambda c: (c.name, c.state_code, c.country_code, c.latitude, c.longitude))
    print("  Sorting cities, completed.")

    # Qualification levels (locations.md 2.2) over the records actually emitted.
    print("  Assigning qualification levels...")
    histogram = assign_qualification_levels(cities)
    print(f"  Levels: name={histogram[LEVEL_NAME]}, "
          f"name+country={histogram[LEVEL_NAME_COUNTRY]}, "
          f"name+region+country={histogram[LEVEL_NAME_REGION_COUNTRY]}.")
    
    # Write output
    records_written = 0
    with open(output_file, 'w', encoding='utf-8', newline='') as f:
        for city in cities:
            f.write(str(city))
            f.write('\n')
            records_written += 1
            
            if records_written % CONSOLE_OUTPUT_INTERVAL == 0:
                print(f"\r  Wrote {records_written} cities...", end='')
    
    print(f"\r  Wrote {records_written} cities, completed.")


# ============================================================================
# Main
# ============================================================================

def main():
    """Main entry point."""
    print("=" * 60)
    print("Diffractor Location Generator")
    print("=" * 60)
    print()
    
    # Determine directories - use project root structure
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    data_dir = project_root / "intermediate" / "geonames"
    output_dir = project_root / "exe"
    
    print(f"Project root: {project_root}")
    print(f"Download directory: {data_dir}")
    print(f"Output directory: {output_dir}")
    
    # Download data
    print()
    paths = download_geonames_data(data_dir)
    print()
    
    # Verify output directory exists
    if not output_dir.exists():
        print(f"Error: Output directory does not exist: {output_dir}")
        sys.exit(1)
    
    # Process states
    print()
    print("-" * 60)
    process_states(
        paths["admin1CodesASCII"],
        output_dir / "location-states.txt"
    )
    
    # Process countries
    print()
    print("-" * 60)
    process_countries(
        paths["countryInfo"],
        paths["countries_extra"],
        output_dir / "location-countries.txt"
    )
    
    # Process cities
    print()
    print("-" * 60)
    process_cities(
        paths["allCountries"],
        paths["alternateNamesV2"],
        output_dir / "location-places.txt"
    )
    
    print()
    print("=" * 60)
    print(f"Done! Location files updated in: {output_dir}")
    print("  - location-countries.txt")
    print("  - location-states.txt")
    print("  - location-places.txt")
    print("=" * 60)


if __name__ == "__main__":
    main()
