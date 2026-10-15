#include "menu.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cf.h"
#include "game.h"
#include "replay.h"
#include "settings.h"
#include "sound.h"
#include "util.h"

@interface LoonySoundMenu : NSObject <NSMenuDelegate>
@property(nonatomic) int saved; /* the volume as last saved, or as loaded */
@end

@implementation LoonySoundMenu
- (void)slid:(NSSlider *)slider {
    sound_set_volume((int)lround(slider.doubleValue));
}

- (void)menuDidClose:(NSMenu *)menu {
    (void)menu;
    if (sound_volume() != self.saved) {
        self.saved = sound_volume();
        settings_set_int(SETTINGS_VOLUME, self.saved);
    }
}
@end

static LoonySoundMenu *controller; /* the menu's delegate and the slider's target */

static struct {
    bool on;
    char game_id[64], title[64], rec_dir[PATH_MAX];
} scores_config;

static void set_scores_config(const char *game_id, const char *title, const char *rec_dir) {
    scores_config.on = true;
    snprintf(scores_config.game_id, sizeof scores_config.game_id, "%s", game_id);
    snprintf(scores_config.title, sizeof scores_config.title, "%s", title);
    snprintf(scores_config.rec_dir, sizeof scores_config.rec_dir, "%s", rec_dir);
}

static void add_scores_menu(void);

void menu_add_high_scores(const char *game_id, const char *title, const char *rec_dir) {
    set_scores_config(game_id, title, rec_dir);
    @autoreleasepool {
        add_scores_menu(); /* the picker has already made the menu bar */
    }
}

/* The High Scores menu, rebuilt each time it opens, so a score set a
   moment ago is in it. A share item's representedObject holds the
   recording's path and the shared copy's file name. */
@interface LoonyHighScores : NSObject <NSMenuDelegate>
@end

static NSString *mac_roman(const char *s) {
    NSString *t = [[NSString alloc] initWithCString:s encoding:NSMacOSRomanStringEncoding];
    return t ?: @"";
}

/* "1   GREG       430,745,960": the name at a tab, the score at a right tab. */
static NSAttributedString *score_row(int rank, NSString *name, NSString *score) {
    NSMutableParagraphStyle *p = [[NSMutableParagraphStyle alloc] init];
    p.tabStops = @[
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:22 options:@{}],
        [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight location:230 options:@{}],
    ];
    NSFont *font = [NSFont monospacedDigitSystemFontOfSize:[NSFont systemFontSize]
                                                    weight:NSFontWeightRegular];
    NSString *text = [NSString stringWithFormat:@"%d\t%@\t%@", rank, name, score];
    return [[NSAttributedString alloc]
        initWithString:text
            attributes:@{NSParagraphStyleAttributeName : p, NSFontAttributeName : font}];
}

static NSMenuItem *disabled_item(NSString *title) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    item.enabled = NO;
    return item;
}

@implementation LoonyHighScores
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    game_high_score hs[GAME_HIGH_SCORES];
    int n = game_high_scores(cf_pref_string, cf_pref_int, hs);
    if (n == 0) {
        [menu addItem:disabled_item(@"No high scores yet")];
        return;
    }
    NSNumberFormatter *nf = [[NSNumberFormatter alloc] init];
    nf.numberStyle = NSNumberFormatterDecimalStyle;
    NSDateFormatter *df = [[NSDateFormatter alloc] init];
    df.dateStyle = NSDateFormatterMediumStyle;
    df.timeStyle = NSDateFormatterShortStyle;
    NSCharacterSet *unsafe = [NSCharacterSet characterSetWithCharactersInString:@"/:"];
    for (int i = 0; i < n; i++) {
        NSString *name = mac_roman(hs[i].name);
        NSString *score = [nf stringFromNumber:@(hs[i].score)];
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
        item.attributedTitle = score_row(i + 1, name.length ? name : @"(no name)", score);
        NSMenu *sub = [[NSMenu alloc] init];
        sub.autoenablesItems = NO;
        char path[PATH_MAX];
        if (hs[i].when) {
            NSDate *when = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)hs[i].when];
            [sub addItem:[NSMenuItem sectionHeaderWithTitle:[df stringFromDate:when]]];
        }
        if (!hs[i].when) {
            [sub addItem:disabled_item(@"No date for this score, so no replay")];
        } else if (!replay_find_recording(scores_config.rec_dir, scores_config.game_id, hs[i].when,
                                          path, sizeof path)) {
            [sub addItem:disabled_item(@"No replay was kept of this game")];
        } else {
            NSString *file = [NSString stringWithFormat:@"%s %@ %@", scores_config.title, score, name];
            file = [[file componentsSeparatedByCharactersInSet:unsafe] componentsJoinedByString:@"-"];
            NSDictionary *info = @{
                @"path" : @(path),
                @"file" : [file stringByAppendingPathExtension:@"lwreplay"],
            };
            NSMenuItem *share = [[NSMenuItem alloc] initWithTitle:@"Share Replay…"
                                                           action:@selector(share:)
                                                    keyEquivalent:@""];
            NSMenuItem *save = [[NSMenuItem alloc] initWithTitle:@"Save Replay…"
                                                          action:@selector(save:)
                                                   keyEquivalent:@""];
            for (NSMenuItem *m in @[ share, save ]) {
                m.target = self;
                m.representedObject = info;
                [sub addItem:m];
            }
        }
        item.submenu = sub;
        [menu addItem:item];
    }
}

/* Writes the shared copy of info's recording to url, or says why not. */
- (BOOL)write:(NSDictionary *)info to:(NSURL *)url {
    char err[1024];
    if (replay_share([info[@"path"] fileSystemRepresentation], url.fileSystemRepresentation, err,
                     sizeof err))
        return YES;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"The replay can't be shared";
    alert.informativeText = @(err);
    [alert runModal];
    return NO;
}

- (void)share:(NSMenuItem *)item {
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"LittleWing Replays"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    NSURL *url = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:item.representedObject[@"file"]]];
    if (![self write:item.representedObject to:url])
        return;
    NSView *view = (NSApp.keyWindow ?: NSApp.mainWindow).contentView;
    if (!view) { /* no window to show the share sheet on */
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
        return;
    }
    NSSharingServicePicker *picker = [[NSSharingServicePicker alloc] initWithItems:@[ url ]];
    NSRect middle = NSMakeRect(NSMidX(view.bounds), NSMidY(view.bounds), 1, 1);
    [picker showRelativeToRect:middle ofView:view preferredEdge:NSRectEdgeMinY];
}

- (void)save:(NSMenuItem *)item {
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = item.representedObject[@"file"];
    UTType *type = [UTType typeWithFilenameExtension:@"lwreplay"];
    if (type)
        panel.allowedContentTypes = @[ type ];
    if ([panel runModal] == NSModalResponseOK)
        [self write:item.representedObject to:panel.URL];
}
@end

static LoonyHighScores *scores; /* the High Scores menu's delegate and its items' target */
static NSMenuItem *sound_item;  /* the Sound menu's title in the bar, once installed */

/* The High Scores menu, after the Sound menu. */
static void add_scores_menu(void) {
    NSMenu *bar = NSApp.mainMenu;
    if (scores || !sound_item || !bar)
        return;
    scores = [[LoonyHighScores alloc] init];
    NSMenu *list = [[NSMenu alloc] initWithTitle:@"High Scores"];
    list.delegate = scores;
    list.autoenablesItems = NO;
    [list addItem:disabled_item(@"No high scores yet")]; /* until it opens */
    NSMenuItem *entry = [[NSMenuItem alloc] initWithTitle:@"High Scores" action:nil keyEquivalent:@""];
    entry.submenu = list;
    [bar insertItem:entry atIndex:[bar indexOfItem:sound_item] + 1];
    log_msg("menu: High Scores added");
}

/* Sparkle (Contents/Frameworks/Sparkle.framework, which a release bundles)
   is loaded when the app runs rather than linked, so builds without it need
   nothing. These are the parts of its API used here. */
@protocol LoonySparkleController
- (instancetype)initWithStartingUpdater:(BOOL)startUpdater
                        updaterDelegate:(id)updaterDelegate
                     userDriverDelegate:(id)userDriverDelegate;
- (void)checkForUpdates:(id)sender;
@end

@protocol LoonySparkleAppcastItem
@property(nonatomic, copy, readonly) NSString *displayVersionString;
@end

#define CHECK_FOR_UPDATES @"Check for Updates…"

/* Sparkle's user driver delegate. Sparkle shows an update its daily check
   finds at once only near launch; found later, mid-game, it would
   interrupt, so the Check for Updates item says there is one instead
   (Sparkle's gentle reminders), until the player looks at it. */
@interface LoonyUpdates : NSObject
@property(nonatomic, strong) id<LoonySparkleController> sparkle;
@property(nonatomic, strong) NSMenuItem *item;
@end

@implementation LoonyUpdates
- (BOOL)supportsGentleScheduledUpdateReminders {
    return YES;
}

- (BOOL)standardUserDriverShouldHandleShowingScheduledUpdate:(id)update
                                         andInImmediateFocus:(BOOL)immediateFocus {
    (void)update;
    return immediateFocus;
}

- (void)standardUserDriverWillHandleShowingUpdate:(BOOL)handleShowingUpdate
                                        forUpdate:(id<LoonySparkleAppcastItem>)update
                                            state:(id)state {
    (void)state;
    if (handleShowingUpdate)
        return;
    self.item.title = [NSString stringWithFormat:@"Update to LittleWing %@…", update.displayVersionString];
    log_msg("updates: LittleWing %s is available", update.displayVersionString.UTF8String);
}

- (void)standardUserDriverDidReceiveUserAttentionForUpdate:(id)update {
    (void)update;
    self.item.title = CHECK_FOR_UPDATES;
}

- (void)standardUserDriverWillFinishUpdateSession {
    self.item.title = CHECK_FOR_UPDATES;
}
@end

static LoonyUpdates *updates; /* Sparkle's controller and its delegate, once started */

/* Starts Sparkle, which checks for updates once a day, and puts Check for
   Updates... after About in the app menu. Only an app whose Info.plist names
   a feed (SUFeedURL, which a release's has) and that bundles Sparkle. */
static void add_updates(NSMenu *bar) {
    NSBundle *app = NSBundle.mainBundle;
    if (![app objectForInfoDictionaryKey:@"SUFeedURL"])
        return;
    NSString *path = [app.privateFrameworksPath stringByAppendingPathComponent:@"Sparkle.framework"];
    NSBundle *sparkle = [NSBundle bundleWithPath:path];
    NSError *err = nil;
    if (!sparkle || ![sparkle loadAndReturnError:&err]) {
        log_msg("updates: can't load %s: %s", path.UTF8String,
                err ? err.localizedDescription.UTF8String : "it isn't there");
        return;
    }
    Class controller_class = NSClassFromString(@"SPUStandardUpdaterController");
    if (!controller_class) {
        log_msg("updates: Sparkle has no SPUStandardUpdaterController");
        return;
    }
    updates = [[LoonyUpdates alloc] init];
    updates.sparkle = [(id<LoonySparkleController>)[controller_class alloc] initWithStartingUpdater:YES
                                                                                   updaterDelegate:nil
                                                                                userDriverDelegate:updates];
    NSMenu *app_menu = bar.numberOfItems ? [bar itemAtIndex:0].submenu : nil;
    if (!app_menu)
        return;
    NSInteger at = 0;
    for (NSInteger i = 0; i < app_menu.numberOfItems; i++)
        if ([app_menu itemAtIndex:i].action == @selector(orderFrontStandardAboutPanel:))
            at = i + 1;
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:CHECK_FOR_UPDATES
                                                  action:@selector(checkForUpdates:)
                                           keyEquivalent:@""];
    item.target = updates.sparkle; /* which disables it while it's busy */
    updates.item = item;
    [app_menu insertItem:item atIndex:at];
    log_msg("menu: Check for Updates added");
}

static NSImageView *icon(NSString *symbol, NSRect frame) {
    NSImageView *v = [[NSImageView alloc] initWithFrame:frame];
    v.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
    v.contentTintColor = NSColor.secondaryLabelColor;
    return v;
}

void menu_install(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (controller || !driver || strcmp(driver, "cocoa") != 0)
        return;
    @autoreleasepool {
        NSMenu *bar = NSApp.mainMenu;
        if (!bar)
            return;
        controller = [[LoonySoundMenu alloc] init];
        controller.saved = sound_volume();

        NSView *row = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 240, 28)];
        [row addSubview:icon(@"speaker.fill", NSMakeRect(14, 6, 16, 16))];
        NSSlider *slider = [NSSlider sliderWithValue:sound_volume()
                                            minValue:0
                                            maxValue:100
                                              target:controller
                                              action:@selector(slid:)];
        slider.frame = NSMakeRect(36, 4, 168, 20);
        slider.continuous = YES;
        slider.accessibilityLabel = @"Volume";
        [row addSubview:slider];
        [row addSubview:icon(@"speaker.wave.3.fill", NSMakeRect(210, 6, 20, 16))];

        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Sound"];
        menu.delegate = controller;
        [menu addItem:[NSMenuItem sectionHeaderWithTitle:@"Volume"]];
        NSMenuItem *item = [[NSMenuItem alloc] init];
        item.view = row;
        [menu addItem:item];

        NSMenuItem *top = [[NSMenuItem alloc] initWithTitle:@"Sound" action:nil keyEquivalent:@""];
        top.submenu = menu;
        sound_item = top;
        NSInteger at = NSApp.windowsMenu ? [bar indexOfItemWithSubmenu:NSApp.windowsMenu] : -1;
        if (at < 0)
            at = bar.numberOfItems;
        [bar insertItem:top atIndex:at];
        add_updates(bar);
        if (scores_config.on)
            add_scores_menu();
    }
}
