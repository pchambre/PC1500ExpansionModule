/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* time_zone.c -- see time_zone.h. The rule syntax is POSIX's TZ (with
 * RFC 8536 sec.3.3's extension: transition hours -167..167), as tzdata's
 * TZif footers write it, e.g. "PST8PDT,M3.2.0,M11.1.0" or
 * "<+0330>-3:30". */
#include "time_zone.h"

#include <string.h>

#define DAY 86400

/* ---- names ---- */

/* The table's order: upper case, '_' as ' '. */
static unsigned char fold(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c == '_' ? ' ' : (unsigned char)c;
}

static int compare(const char *name, uint8_t len, const char *entry) {
    for (uint8_t i = 0; i < len; i++, entry++) {
        if (*entry == 0) return 1;
        int d = (int)fold(name[i]) - (int)fold(*entry);
        if (d) return d;
    }
    return *entry ? -1 : 0;
}

bool tz_find(const char *name, uint8_t len, char *canonical, tz_zone_t *zone) {
    unsigned lo = 0, hi = kTzTableCount;
    char rule[TZ_NAME_MAX + 1];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = compare(name, len, kTzTable[mid].name);
        if (c == 0) {
            strcpy(canonical, kTzTable[mid].name);
            zone->shifts = &kTzShifts[kTzTable[mid].shifts];
            zone->nshifts = kTzTable[mid].nshifts;
            return tz_parse(tz_table_rule(mid), &zone->rule);
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    if (len == 0 || len > TZ_NAME_MAX) return false;
    memcpy(rule, name, len);
    rule[len] = 0;
    if (!tz_parse(rule, &zone->rule)) return false;
    strcpy(canonical, rule);
    zone->shifts = 0;
    zone->nshifts = 0;
    return true;
}

/* ---- rules ---- */

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool number(const char **p, int32_t max, int32_t *out) {
    int32_t n = 0;
    if (!digit(**p)) return false;
    while (digit(**p)) {
        n = n * 10 + (**p - '0');
        if (n > max) return false;
        (*p)++;
    }
    *out = n;
    return true;
}

/* A zone abbreviation: 3+ letters, or <...> (letters, digits, + and -). */
static bool abbreviation(const char **p) {
    const char *s = *p;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') s++;
        if (*s != '>' || s - *p < 4) return false;
        *p = s + 1;
        return true;
    }
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
    if (s - *p < 3) return false;
    *p = s;
    return true;
}

/* [+|-]hh[:mm[:ss]], up to `max_hours` */
static bool hms(const char **p, int32_t max_hours, int32_t *out) {
    int32_t sign = 1, h, m = 0, s = 0;
    if (**p == '+' || **p == '-') sign = *(*p)++ == '-' ? -1 : 1;
    if (!number(p, max_hours, &h)) return false;
    if (**p == ':') {
        (*p)++;
        if (!number(p, 59, &m)) return false;
        if (**p == ':') {
            (*p)++;
            if (!number(p, 59, &s)) return false;
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

static bool change(const char **p, struct tz_change *c) {
    int32_t n, w, d;
    if (**p == 'M') {
        (*p)++;
        if (!number(p, 12, &n) || n < 1 || *(*p)++ != '.' || !number(p, 5, &w) || w < 1 || *(*p)++ != '.' ||
            !number(p, 6, &d))
            return false;
        c->kind = 'M';
        c->month = (uint8_t)n;
        c->week = (uint8_t)w;
        c->day = (uint16_t)d;
    } else if (**p == 'J') {
        (*p)++;
        if (!number(p, 365, &n) || n < 1) return false;
        c->kind = 'J';
        c->day = (uint16_t)n;
    } else {
        if (!number(p, 365, &n)) return false;
        c->kind = 'D';
        c->day = (uint16_t)n;
    }
    c->time = 2 * 3600;
    if (**p == '/') {
        (*p)++;
        if (!hms(p, 167, &c->time)) return false;
    }
    return true;
}

bool tz_parse(const char *posix, tz_rule_t *out) {
    const char *p = posix;
    int32_t west;
    memset(out, 0, sizeof *out);
    if (!abbreviation(&p) || !hms(&p, 24, &west)) return false;
    out->std_offset = -west;
    if (*p == 0) return true;
    if (!abbreviation(&p)) return false;
    out->has_dst = true;
    out->dst_offset = out->std_offset + 3600;
    if (*p && *p != ',') {
        if (!hms(&p, 24, &west)) return false;
        out->dst_offset = -west;
    }
    if (*p == 0) { /* POSIX leaves the dates to the implementation: the US's, as glibc */
        out->start.kind = out->end.kind = 'M';
        out->start.month = 3;
        out->start.week = 2;
        out->end.month = 11;
        out->end.week = 1;
        out->start.time = out->end.time = 2 * 3600;
        return true;
    }
    if (*p++ != ',' || !change(&p, &out->start) || *p++ != ',' || !change(&p, &out->end)) return false;
    return *p == 0;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static unsigned weekday(int64_t days) { return (unsigned)(((days % 7) + 11) % 7); } /* 1970-01-01: Thursday */

/* The change's date in year `y`, as days since 1970 */
static int64_t change_day(const struct tz_change *c, int64_t y) {
    int64_t jan1 = days_from_civil(y, 1, 1);
    if (c->kind == 'J') return jan1 + c->day - 1 + (leap(y) && c->day >= 60);
    if (c->kind == 'D') return jan1 + c->day;
    static const uint8_t kLength[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t first = days_from_civil(y, c->month, 1);
    unsigned length = kLength[c->month - 1] + (c->month == 2 && leap(y));
    unsigned day = 1 + (c->day + 7 - weekday(first)) % 7 + (c->week - 1) * 7u;
    if (day > length) day -= 7; /* week 5: the last */
    return first + day - 1;
}

static int32_t rule_offset_at(const tz_rule_t *rule, int64_t utc) {
    int64_t y;
    unsigned m, d;
    if (!rule->has_dst) return rule->std_offset;
    civil_from_days(floor_div(utc + rule->std_offset, DAY), &y, &m, &d);
    /* each change is in the local time in effect just before it */
    int64_t start = change_day(&rule->start, y) * DAY + rule->start.time - rule->std_offset;
    int64_t end = change_day(&rule->end, y) * DAY + rule->end.time - rule->dst_offset;
    bool dst = start < end ? (utc >= start && utc < end) : !(utc >= end && utc < start);
    return dst ? rule->dst_offset : rule->std_offset;
}

int32_t tz_offset_at(const tz_zone_t *zone, int64_t utc) {
    uint16_t n = zone->nshifts;
    if (n == 0 || utc < zone->shifts[0].at || utc >= zone->shifts[n - 1].at)
        return rule_offset_at(&zone->rule, utc);
    while (zone->shifts[n - 1].at > utc) n--;
    return zone->shifts[n - 1].offset;
}

static uint8_t bcd(unsigned n) { return (uint8_t)(((n / 10) << 4) | (n % 10)); }

void tz_clock_bytes(int64_t local, uint8_t out[5]) {
    int64_t days = floor_div(local, DAY), y;
    unsigned secs = (unsigned)(local - days * DAY), m, d;
    civil_from_days(days, &y, &m, &d);
    out[0] = (uint8_t)((m << 4) | weekday(days));
    out[1] = bcd(d);
    out[2] = bcd(secs / 3600);
    out[3] = bcd(secs / 60 % 60);
    out[4] = bcd(secs % 60);
}
