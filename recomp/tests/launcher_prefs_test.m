// The launcher's reading and writing of the game's prefs.gas (the graphics options it sets for the game).
//   clang -fobjc-arc -framework AppKit -framework Metal -lz recomp/tests/launcher_prefs_test.m -o /tmp/launcher_prefs_test && /tmp/launcher_prefs_test
#include "../host/launcher.m"
#include <assert.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    @autoreleasepool {
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
        NSString *p = [dir stringByAppendingPathComponent:@"Dungeon Siege/prefs.gas"];
        /* a file as the game writes it: CRLF, tabs, a nested block after the values */
        NSString *game = @"[prefs]\r\n{\r\n\tblood = red;\r\n\tobject_detail_level = 1.000000;\r\n\ttexture_filtering = bilinear;\r\n"
                          "\tvideo_gamma = 0.650000;\r\n\tvideo_shadows = complex;\r\n\t[character]\r\n\t{\r\n\t\tcharacter_name = \"Ayla\";\r\n\t}\r\n}\r\n";
        [NSFileManager.defaultManager createDirectoryAtPath:p.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        [game writeToFile:p atomically:YES encoding:NSISOLatin1StringEncoding error:nil];
        CHECK([prefs_get(p, @"video_gamma") isEqualToString:@"0.650000"]);
        CHECK([prefs_get(p, @"video_shadows") isEqualToString:@"complex"]);
        CHECK(prefs_get(p, @"character_name") != nil);                 /* the nested block is read but never written */
        CHECK(prefs_get(p, @"video") == nil);                          /* whole keys only */
        prefs_set(p, @{@"video_gamma": @"1.200000", @"texture_filtering": @"trilinear"});
        NSString *t = [NSString stringWithContentsOfFile:p encoding:NSISOLatin1StringEncoding error:nil];
        CHECK([prefs_get(p, @"video_gamma") isEqualToString:@"1.200000"]);
        CHECK([prefs_get(p, @"texture_filtering") isEqualToString:@"trilinear"]);
        CHECK([t isEqualToString:[[game stringByReplacingOccurrencesOfString:@"0.650000" withString:@"1.200000"]
                                          stringByReplacingOccurrencesOfString:@"= bilinear" withString:@"= trilinear"]]);   /* nothing else touched */
        prefs_set(p, @{@"object_detail_level": @"0.500000"});
        CHECK([prefs_get(p, @"object_detail_level") isEqualToString:@"0.500000"]);
        /* a key the file doesn't have yet goes into the [prefs] block */
        prefs_set(p, @{@"video_shadows": @"none", @"new_key": @"x"});
        CHECK([prefs_get(p, @"new_key") isEqualToString:@"x"] && [prefs_get(p, @"video_shadows") isEqualToString:@"none"]);
        t = [NSString stringWithContentsOfFile:p encoding:NSISOLatin1StringEncoding error:nil];
        CHECK([t hasPrefix:@"[prefs]\r\n{\r\n\tnew_key = x;\r\n"]);
        /* no file yet (a new player): one is made with just these */
        NSString *q = [dir stringByAppendingPathComponent:@"Dungeon Siege LOA/prefs.gas"];
        prefs_set(q, @{@"video_gamma": @"1.500000"});
        CHECK([prefs_get(q, @"video_gamma") isEqualToString:@"1.500000"]);
        /* a value the launcher doesn't offer is kept, shown as Custom */
        DSRow *r = [DSRow new]; r.choices = gamma_choices();
        CHECK(pref_index(r, @"1.000000", 0) == 5 && pref_index(r, @"1.0", 0) == 5 && pref_index(r, nil, 3) == 3);
        NSInteger k = pref_index(r, @"0.650000", 5);
        CHECK(k == 11 && [r.choices[k][@"value"] isEqualToString:@"0.650000"] && [r.choices[k][@"label"] isEqualToString:@"Custom (0.65)"]);
        r.choices = game_shadow_choices(); CHECK(pref_index(r, @"complex_party", 0) == 2 && pref_index(r, @"none", 3) == 0);
        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
        printf(fails ? "launcher_prefs_test: %d failed\n" : "launcher_prefs_test: all passed\n", fails);
    }
    return fails != 0;
}
