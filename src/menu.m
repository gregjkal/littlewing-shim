#include "menu.h"

#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "settings.h"
#include "sound.h"

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
        NSInteger at = NSApp.windowsMenu ? [bar indexOfItemWithSubmenu:NSApp.windowsMenu] : -1;
        [bar insertItem:top atIndex:at < 0 ? bar.numberOfItems : at];
    }
}
