/* The map selector's maps (every platform): each map's free-skating mission,
 * mostly chapter 9's -- after the story (Hikage Street's 0925 is a flag battle
 * and 0926 never loads: chapter 8's 0825). Chapter 9 sets a rival gang's race
 * beside some maps, started by skating into its spot -- on 99th Street the
 * Immortals', right by the way to the garage: there, and on Dogenzaka Hill,
 * chapter 6's, which has none. The garage is entered as the game does,
 * through its character select (0997); the selector waits until that is
 * done before going anywhere else. A mission id is chapter << 16 |
 * (100 + number) for a map, chapter << 16 | number for a garage screen, as
 * mssnCCNN.bin. The names are the
 * game's own (its list at 0x1BF644 in default.xbe, in stage order Stg10..
 * Stg34), grouped by district. Asked for with nv_window_request_map(mission);
 * port/recomp_manual.c goes there at the next frame of play. */
#ifndef JSRF_MAPS_H
#define JSRF_MAPS_H

typedef struct { const char *name; const char *district; int mission; } JsrfMap;

static const JsrfMap k_jsrf_maps[] = {
    { "Garage",                              "Garage",      0x90061 },   /* mssn0997, the character select */
    { "Shibuya Terminal",                    "Shibuya-cho", 0x9006E },   /* mssn0910, Stg10 */
    { "Chuo Street",                         "Shibuya-cho", 0x90073 },   /* mssn0915, Stg11 */
    { "Dogenzaka Hill",                      "Shibuya-cho", 0x60078 },   /* mssn0620, Stg12 */
    { "Hikage Street",                       "Shibuya-cho", 0x8007D },   /* mssn0825, Stg13 */
    { "Rokkaku-dai Heights",                 "Kogane-cho",  0x90082 },   /* mssn0930, Stg20 */
    { "Tokyo Underground Sewage Facility",   "Kogane-cho",  0x90087 },   /* mssn0935, Stg21 */
    { "Kibogaoka Hill",                      "Kogane-cho",  0x9008C },   /* mssn0940, Stg22 */
    { "Bottom Point of the Sewage Facility", "Kogane-cho",  0x90091 },   /* mssn0945, Stg23 */
    { "Fortified Residential Zone",          "Kogane-cho",  0x90096 },   /* mssn0950, Stg24 */
    { "99th Street",                         "Benten-cho",  0x6009B },   /* mssn0655, Stg30 */
    { "Sky Dinosaurian Square",              "Benten-cho",  0x900A0 },   /* mssn0960, Stg31 */
    { "Rokkaku Expo Stadium",                "Benten-cho",  0x900A5 },   /* mssn0965, Stg32 */
    { "Highway Zero",                        "Benten-cho",  0x900AA },   /* mssn0970, Stg33 */
    { "Skyscraper District",                 "Benten-cho",  0x900AF },   /* mssn0975, Stg34 */
};
#define JSRF_MAP_COUNT ((int)(sizeof k_jsrf_maps / sizeof k_jsrf_maps[0]))

#endif
