#!/usr/bin/env python3
"""Generate runtime-free calendar locale data from CLDR supplemental JSON.
Usage: gen-calendar-data.py weekData.json calendarPreferenceData.json likelySubtags.json
CLDR 42 source: unicode-org/cldr-json/42.0.0/cldr-json/cldr-core/supplemental
"""
import json
import sys
from pathlib import Path
week = json.loads(Path(sys.argv[1]).read_text())['supplemental']['weekData']
preferences = json.loads(Path(sys.argv[2]).read_text())['supplemental']['calendarPreferenceData']
days = {name: i + 1 for i, name in enumerate(['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'])}
countries = sorted(set(week['firstDay']) | set(week['minDays']) | set(preferences))
out = ['/* Generated from CLDR 42. License: LICENSE.unicode. */',
       'static const struct cal_region { const char *region, *type; unsigned char first, minimal; } cal_regions[] = {']
for country in countries:
    first = days[week['firstDay'].get(country, week['firstDay']['001'])]
    minimal = int(week['minDays'].get(country, week['minDays']['001']))
    preferred = preferences.get(country, preferences['001'])[0]
    out.append('{' + json.dumps(country) + ',' + json.dumps(preferred) + ',%d,%d},' % (first, minimal))
out.append('};')
likely = json.loads(Path(sys.argv[3]).read_text())['supplemental']['likelySubtags']
out.append('static const struct cal_language_region { const char *language, *region; } cal_language_regions[] = {')
for language, tag in sorted(likely.items()):
    if '_' not in language and '-' not in language:
        out.append('{' + json.dumps(language) + ',' + json.dumps(tag.replace('-', '_').split('_')[-1]) + '},')
out.append('};')
Path(__file__).resolve().parents[1].joinpath('src/dvm/calendar_data.h').write_text('\n'.join(out) + '\n')
