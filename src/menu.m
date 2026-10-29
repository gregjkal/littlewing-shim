#include "menu.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cf.h"
#include "display.h"
#include "game.h"
#include "gamestart.h"
#include "menumgr.h"
#include "practice.h"
#include "recordings.h"
#include "replay.h"
#include "settings.h"
#include "sound.h"
#include "util.h"
#include "watch.h"

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

/* Writes the shared copy of info's recording to url, or says why not. A
   copy without a snapshot of the game's start takes one first, behind a
   panel, so its replay starts at the game on the recipient's Mac
   (gamestart.h); NO if that is cancelled. */
- (BOOL)write:(NSDictionary *)info to:(NSURL *)url {
    char err[1024];
    if (!replay_share([info[@"path"] fileSystemRepresentation], url.fileSystemRepresentation, err,
                      sizeof err)) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"The replay can't be shared";
        alert.informativeText = @(err);
        [alert runModal];
        return NO;
    }
    if (gamestart_in_file(url.fileSystemRepresentation))
        return YES;
    return [self takeSnapshotInto:url];
}

/* Runs a second copy of LittleWing on the shared copy at url to keep a
   snapshot of the game's start in it, with a panel that says so and can
   cancel it. YES once it has ended (with a snapshot or, if none could be
   taken, without: the copy still plays, from launch). */
- (BOOL)takeSnapshotInto:(NSURL *)url {
    char err[1024];
    NSString *self_path = NSBundle.mainBundle.executablePath;
    if (!self_path || !gamestart_take_start(self_path.fileSystemRepresentation, url.fileSystemRepresentation,
                                            err, sizeof err)) {
        log_msg("menu: can't take a snapshot for the shared replay: %s", self_path ? err : "no program");
        return YES;
    }
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 360, 116)
                                                styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    panel.title = @"Share Replay";
    NSTextField *label = [NSTextField
        wrappingLabelWithString:@"Getting the replay ready, so it starts at the game for whoever "
                                @"watches it. This takes a while for a game late in a session."];
    label.frame = NSMakeRect(20, 52, 320, 48);
    NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 22, 230, 20)];
    bar.style = NSProgressIndicatorStyleBar;
    bar.indeterminate = YES;
    [bar startAnimation:nil];
    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:NSApp action:@selector(abortModal)];
    cancel.frame = NSMakeRect(260, 16, 82, 32);
    cancel.keyEquivalent = @"\033"; /* Escape */
    for (NSView *v in @[ label, bar, cancel ])
        [panel.contentView addSubview:v];
    [panel center];
    __block bool kept = false;
    NSTimer *poll = [NSTimer timerWithTimeInterval:0.2
                                           repeats:YES
                                             block:^(NSTimer *t) {
                                               if (gamestart_take_poll(&kept)) {
                                                   [t invalidate];
                                                   [NSApp stopModalWithCode:NSModalResponseOK];
                                               }
                                             }];
    [[NSRunLoop currentRunLoop] addTimer:poll forMode:NSModalPanelRunLoopMode];
    NSModalResponse r = [NSApp runModalForWindow:panel];
    [poll invalidate];
    [panel orderOut:nil];
    if (r != NSModalResponseOK) {
        gamestart_take_stop();
        log_msg("menu: sharing a replay was cancelled while its snapshot was taken");
        return NO;
    }
    return YES;
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

/* The Replay menu's items' target, each tagged with its watch_action: they
   act as the viewer's keys do. An item's own key does nothing here: SDL has
   passed the key to the viewer already (watch_key) by the time AppKit
   matches it to the item, and acting again would step twice. (A delegate's
   menuHasKeyEquivalent: can't stop that, since AppKit still searches the
   items a menu has.) */
@interface LoonyReplayMenu : NSObject <NSMenuItemValidation>
@end

@implementation LoonyReplayMenu
- (void)act:(NSMenuItem *)item {
    NSEvent *e = NSApp.currentEvent;
    NSString *typed = e.type == NSEventTypeKeyDown ? e.charactersIgnoringModifiers : nil;
    if ([typed isEqualToString:@"\x7f"]) /* the Delete key types DEL, its equivalent is a backspace */
        typed = @"\b";
    if (typed && item.keyEquivalent.length &&
        [typed caseInsensitiveCompare:item.keyEquivalent] == NSOrderedSame)
        return;
    watch_act((watch_action)item.tag, false);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    return watch_can((watch_action)item.tag);
}
@end

static bool replay_wanted;          /* menu_add_replay was called */
static LoonyReplayMenu *replay_menu; /* its items' target, once added */

/* The key equivalent that shows b's key, with its modifiers in *mask: a
   shifted key shows what it types on this keyboard ("<" for Shift-comma),
   unless that is only its capital (Shift-J shows as Shift and J), and
   with Option too it's what Shift alone types (Shift-Option-comma shows as
   Option and "<"). "" for none. */
static NSString *key_equivalent(const watch_binding *b, NSEventModifierFlags *mask) {
    *mask = 0;
    if (b->mods & SDL_KMOD_CTRL)
        *mask |= NSEventModifierFlagControl;
    if (b->mods & SDL_KMOD_ALT)
        *mask |= NSEventModifierFlagOption;
    if (b->mods & SDL_KMOD_GUI)
        *mask |= NSEventModifierFlagCommand;
    unichar c;
    if (b->scancode == SDL_SCANCODE_RIGHT) {
        c = NSRightArrowFunctionKey;
    } else if (b->scancode == SDL_SCANCODE_LEFT) {
        c = NSLeftArrowFunctionKey;
    } else {
        SDL_Keycode base = SDL_GetKeyFromScancode((SDL_Scancode)b->scancode, SDL_KMOD_NONE, false);
        SDL_Keycode typed = SDL_GetKeyFromScancode((SDL_Scancode)b->scancode,
                                                   b->mods & SDL_KMOD_SHIFT, false);
        if (base == SDLK_UNKNOWN || (base & SDLK_SCANCODE_MASK) || base > 0xFFFF)
            return @"";
        c = (unichar)base;
        if (b->mods & SDL_KMOD_SHIFT) {
            bool capital = typed < 0x80 && base < 0x80 && toupper((int)base) == (int)typed;
            if (!capital && typed != base && typed != SDLK_UNKNOWN && typed <= 0xFFFF &&
                !(typed & SDLK_SCANCODE_MASK))
                c = (unichar)typed;
            else
                *mask |= NSEventModifierFlagShift;
        }
    }
    return [NSString stringWithCharacters:&c length:1];
}

/* The Replay menu, after the Sound menu: watch_bindings' labelled rows. */
static void add_replay_menu(void) {
    NSMenu *bar = NSApp.mainMenu;
    if (replay_menu || !sound_item || !bar)
        return;
    replay_menu = [[LoonyReplayMenu alloc] init];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Replay"];
    for (size_t i = 0; i < watch_nbindings; i++) {
        const watch_binding *b = &watch_bindings[i];
        if (!b->label)
            continue;
        if (b->gap)
            [menu addItem:[NSMenuItem separatorItem]];
        NSEventModifierFlags mask;
        NSString *key = key_equivalent(b, &mask);
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(b->label)
                                                      action:@selector(act:)
                                               keyEquivalent:key];
        item.keyEquivalentModifierMask = mask;
        item.tag = b->action;
        item.target = replay_menu;
        [menu addItem:item];
    }
    NSMenuItem *entry = [[NSMenuItem alloc] initWithTitle:@"Replay" action:nil keyEquivalent:@""];
    entry.submenu = menu;
    [bar insertItem:entry atIndex:[bar indexOfItem:sound_item] + 1];
    log_msg("menu: Replay added");
}

void menu_add_replay(void) {
    replay_wanted = true;
    @autoreleasepool {
        add_replay_menu();
    }
}

/* The game's own menus (menumgr.h), mirrored after the app menu. Each
   item's tag is its menu's ID and its number, and its menu is rebuilt from
   the game's as it opens, so it shows what is enabled and checked then.
   As in the Replay menu, an item's own key does nothing here: SDL has sent
   the key to the game already. */
@interface LoonyGameMenus : NSObject <NSMenuDelegate>
@end

@implementation LoonyGameMenus
- (void)choose:(NSMenuItem *)item {
    NSEvent *e = NSApp.currentEvent;
    if (e.type == NSEventTypeKeyDown && item.keyEquivalent.length &&
        [e.charactersIgnoringModifiers caseInsensitiveCompare:item.keyEquivalent] == NSOrderedSame)
        return;
    menumgr_choose((int16_t)(item.tag >> 16), (int)(item.tag & 0xFFFF));
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSMenu *bar = menu.supermenu;
    NSInteger at = [bar indexOfItemWithSubmenu:menu];
    if (at < 0)
        return;
    int16_t id = (int16_t)[bar itemAtIndex:at].tag;
    menumgr_item_info info;
    for (int i = 1; menumgr_item(id, i, &info); i++) {
        if (info.text[0] == '-') {
            [menu addItem:[NSMenuItem separatorItem]];
            continue;
        }
        NSString *key = info.key ? [NSString stringWithFormat:@"%c", tolower(info.key)] : @"";
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:mac_roman(info.text)
                                                      action:@selector(choose:)
                                               keyEquivalent:key];
        item.target = self;
        item.tag = (NSInteger)((uint32_t)(uint16_t)id << 16 | (uint32_t)i);
        item.enabled = info.enabled;
        item.state = info.checked ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:item];
    }
}
@end

static LoonyGameMenus *game_menus; /* their delegate and their items' target */
static NSMutableArray *game_items; /* their titles in the bar */
static NSString *game_bar;         /* the game's bar they show, as "id title\n..." */

void menu_sync_game(void) {
    NSMenu *bar = NSApp.mainMenu;
    if (!controller || !bar)
        return;
    @autoreleasepool {
        NSMutableString *now = [NSMutableString string];
        menumgr_menu_info m;
        for (int i = 0; menumgr_bar_menu(i, &m); i++)
            [now appendFormat:@"%d %@\n", m.id, mac_roman(m.title)];
        if ([now isEqualToString:game_bar ?: @""])
            return;
        game_bar = now;
        if (!game_menus) {
            game_menus = [[LoonyGameMenus alloc] init];
            game_items = [NSMutableArray array];
        }
        for (NSMenuItem *item in game_items)
            [bar removeItem:item];
        [game_items removeAllObjects];
        NSInteger at = 1; /* after the app menu */
        for (int i = 0; menumgr_bar_menu(i, &m); i++) {
            if (!strcmp(m.title, "\x14")) /* the Apple menu: the app menu stands for it */
                continue;
            NSString *title = mac_roman(m.title);
            NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
            menu.delegate = game_menus;
            menu.autoenablesItems = NO;
            NSMenuItem *entry = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            entry.tag = m.id;
            entry.submenu = menu;
            [bar insertItem:entry atIndex:at++];
            [game_items addObject:entry];
            [game_menus menuNeedsUpdate:menu]; /* its key equivalents, before it first opens */
        }
        log_msg("menu: the game's menus are %lu", (unsigned long)game_items.count);
    }
}

/* recordings_delete's trash: the Finder's, so a slip can be undone. */
static bool trash(const char *path, char *err, size_t errlen) {
    NSError *error = nil;
    NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path isDirectory:NO relativeToURL:nil];
    if ([[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nil error:&error])
        return true;
    snprintf(err, errlen, "%s", error.localizedDescription.UTF8String ?: "unknown error");
    return false;
}

/* The Replays menu, rebuilt each time it opens so a game recorded a moment
   ago is in it, and the File menu's Open Recent, rebuilt the same way: the
   menus' delegate and their items' target. An item's representedObject
   holds the recording's path and where to start, in ticks of play. */
@interface LoonyRecordings : NSObject <NSMenuDelegate>
@property(nonatomic, strong) NSMenu *recent; /* Open Recent; the other is Replays */
@end

static NSMenuItem *open_item(NSString *title, const char *path, uint32_t from, id target) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:@selector(open:) keyEquivalent:@""];
    item.target = target;
    item.representedObject = @{@"path" : @(path), @"from" : @(from)};
    return item;
}

@implementation LoonyRecordings
/* Neither menu has key equivalents. Saying so keeps AppKit from filling
   them, a rescan of the recordings folder, on every Command-key press. */
- (BOOL)menuHasKeyEquivalent:(NSMenu *)menu
                    forEvent:(NSEvent *)event
                      target:(id _Nullable *)target
                      action:(SEL _Nullable *)action {
    (void)menu;
    (void)event;
    (void)target;
    (void)action;
    return NO;
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    if (menu == self.recent) {
        [self fillRecent:menu];
        return;
    }
    char dir[PATH_MAX] = "", label[256];
    const recording *list = NULL;
    size_t n = recordings_dir(dir, sizeof dir) ? recordings_list(dir, &list) : 0;
    bool games = false; /* of more than one game: each says which */
    for (size_t i = 1; i < n; i++)
        games = games || strcmp(list[i].game_id, list[0].game_id) != 0;
    if (!n)
        [menu addItem:disabled_item(@"No Recordings")];
    for (size_t i = 0; i < n; i++) {
        NSString *title = @(recordings_label(&list[i], games, label, sizeof label));
        [menu addItem:open_item(title, list[i].path, 0, self)];
        for (size_t m = 0; m < list[i].nmarks; m++) {
            if (!recordings_mark_shown(&list[i], m))
                continue;
            title = @(recordings_mark_label(list[i].marks[m], label, sizeof label));
            NSMenuItem *at = open_item(title, list[i].path, list[i].marks[m], self);
            at.indentationLevel = 1;
            [menu addItem:at];
            title = @(recordings_practice_label(list[i].marks[m], label, sizeof label));
            NSMenuItem *practice = open_item(title, list[i].path, list[i].marks[m], self);
            practice.action = @selector(practice:);
            practice.indentationLevel = 1;
            [menu addItem:practice];
        }
        if (!recordings_deletable(list[i].path))
            continue;
        NSMenuItem *del = [[NSMenuItem alloc] initWithTitle:@"Delete"
                                                     action:@selector(delete:)
                                              keyEquivalent:@""];
        del.target = self;
        del.representedObject = @(list[i].path);
        del.indentationLevel = 1;
        [menu addItem:del];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *show = [[NSMenuItem alloc] initWithTitle:@"Show Recordings in Finder"
                                                  action:@selector(showFolder:)
                                           keyEquivalent:@""];
    show.target = self;
    show.representedObject = @(dir);
    show.enabled = dir[0] != '\0';
    [menu addItem:show];
}

/* The recordings watched lately that are still there, then Clear Menu. */
- (void)fillRecent:(NSMenu *)menu {
    NSDocumentController *docs = [NSDocumentController sharedDocumentController];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSInteger shown = 0;
    for (NSURL *url in docs.recentDocumentURLs) {
        if (!url.fileURL || ![fm fileExistsAtPath:url.path])
            continue;
        NSMenuItem *item = open_item(url.lastPathComponent, url.fileSystemRepresentation, 0, self);
        NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:url.path];
        icon.size = NSMakeSize(16, 16);
        item.image = icon;
        [menu addItem:item];
        shown++;
    }
    if (shown)
        [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *clear = [[NSMenuItem alloc] initWithTitle:@"Clear Menu"
                                                   action:@selector(clearRecentDocuments:)
                                            keyEquivalent:@""];
    clear.target = docs; /* which disables it when there is nothing to clear */
    [menu addItem:clear];
}

- (void)open:(NSMenuItem *)item {
    NSDictionary *info = item.representedObject;
    display_request_open([info[@"path"] fileSystemRepresentation], [info[@"from"] unsignedIntValue]);
}

- (void)practice:(NSMenuItem *)item {
    NSDictionary *info = item.representedObject;
    display_request_practice([info[@"path"] fileSystemRepresentation], [info[@"from"] unsignedIntValue]);
}

- (void)practiceBundled:(NSMenuItem *)item {
    char path[PATH_MAX];
    uint32_t ticks;
    if (practice_bundled_open((size_t)item.tag, path, sizeof path, &ticks))
        display_request_practice(path, ticks);
    else
        display_flash("Can't open the practice");
}

- (void)delete:(NSMenuItem *)item {
    char err[512];
    const char *path = [item.representedObject fileSystemRepresentation];
    if (!recordings_delete(path, trash, err, sizeof err)) {
        log_msg("menu: can't delete %s: %s", path, err);
        char msg[600];
        snprintf(msg, sizeof msg, "Can't delete the recording: %s", err);
        display_flash(msg);
    }
}

- (void)showFolder:(NSMenuItem *)item {
    NSString *dir = item.representedObject;
    if (!make_dirs(dir.fileSystemRepresentation)) {
        log_msg("menu: can't create %s", dir.fileSystemRepresentation);
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}
@end

static bool replays_wanted;              /* menu_add_replays was called, and not taken back */
static LoonyRecordings *recordings;      /* the Replays and Open Recent menus' delegate, once added */
static NSMenuItem *replays_item, *file_item, *practice_item; /* their titles in the bar */
static char recent_path[PATH_MAX];       /* menu_note_recent's, until menu_install */

/* The Replays menu, before the Window menu (after Sound, High Scores and
   Replay), Practice after it, and the File menu, after the app menu. */
static void add_replays_menu(void) {
    NSMenu *bar = NSApp.mainMenu;
    if (recordings || !sound_item || !bar)
        return;
    recordings = [[LoonyRecordings alloc] init];
    NSMenu *list = [[NSMenu alloc] initWithTitle:@"Replays"];
    list.delegate = recordings;
    list.autoenablesItems = NO;
    [list addItem:disabled_item(@"No Recordings")]; /* until it opens */
    replays_item = [[NSMenuItem alloc] initWithTitle:@"Replays" action:nil keyEquivalent:@""];
    replays_item.submenu = list;
    NSInteger at = NSApp.windowsMenu ? [bar indexOfItemWithSubmenu:NSApp.windowsMenu] : -1;
    [bar insertItem:replays_item atIndex:at < 0 ? bar.numberOfItems : at];

    NSMenu *practices = [[NSMenu alloc] initWithTitle:@"Practice"];
    practices.autoenablesItems = NO;
    for (size_t i = 0; i < practice_bundled_count(); i++) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@(practice_bundled_title(i))
                                                      action:@selector(practiceBundled:)
                                               keyEquivalent:@""];
        item.target = recordings;
        item.tag = (NSInteger)i;
        [practices addItem:item];
    }
    practice_item = [[NSMenuItem alloc] initWithTitle:@"Practice" action:nil keyEquivalent:@""];
    practice_item.submenu = practices;
    [bar insertItem:practice_item atIndex:[bar indexOfItem:replays_item] + 1];

    NSMenu *recent = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    recent.delegate = recordings;
    recordings.recent = recent;
    [recent addItem:disabled_item(@"Clear Menu")]; /* until it opens */
    NSMenuItem *open_recent = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:nil keyEquivalent:@""];
    open_recent.submenu = recent;
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [file addItem:open_recent];
    file_item = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    file_item.submenu = file;
    [bar insertItem:file_item atIndex:bar.numberOfItems ? 1 : 0];
    log_msg("menu: Replays, Practice and File added");
}

void menu_add_replays(void) {
    replays_wanted = true;
    @autoreleasepool {
        add_replays_menu();
    }
}

void menu_remove_replays(void) {
    replays_wanted = false;
    @autoreleasepool {
        NSMenu *bar = NSApp.mainMenu;
        if (!recordings || !bar)
            return;
        [bar removeItem:replays_item];
        [bar removeItem:file_item];
        [bar removeItem:practice_item];
        replays_item = file_item = practice_item = nil;
        recordings = nil;
        log_msg("menu: Replays, Practice and File taken away");
    }
}

static void note_recent(const char *path) {
    NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path isDirectory:NO relativeToURL:nil];
    [[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url.absoluteURL];
}

void menu_note_recent(const char *path) {
    if (!path || !*path)
        return;
    if (!controller) { /* the menu bar isn't there yet: menu_install notes it */
        snprintf(recent_path, sizeof recent_path, "%s", path);
        return;
    }
    @autoreleasepool {
        note_recent(path);
    }
}

void menu_reveal(const char *const *paths, size_t n) {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!n || !driver || strcmp(driver, "cocoa") != 0)
        return;
    @autoreleasepool {
        NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:n];
        for (size_t i = 0; i < n; i++)
            [urls addObject:[NSURL fileURLWithFileSystemRepresentation:paths[i]
                                                           isDirectory:NO
                                                         relativeToURL:nil]];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:urls];
    }
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
   finds at once only near launch or after the Mac has sat idle; found
   while the player plays, it would interrupt, so the Check for Updates item
   says there is one instead (Sparkle's gentle reminders), until the player
   looks at it. */
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
        if (replay_wanted)
            add_replay_menu();
        if (replays_wanted)
            add_replays_menu();
        if (recent_path[0]) {
            note_recent(recent_path);
            recent_path[0] = '\0';
        }
    }
}
