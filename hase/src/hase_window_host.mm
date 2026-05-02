#import <Cocoa/Cocoa.h>

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface HaSeLinuxWindow : NSObject
@property(nonatomic, copy) NSString *windowID;
@property(nonatomic, copy) NSString *title;
@property(nonatomic) NSInteger processID;
@property(nonatomic) NSInteger x;
@property(nonatomic) NSInteger y;
@property(nonatomic) NSInteger width;
@property(nonatomic) NSInteger height;
@end

@implementation HaSeLinuxWindow
@end

static NSString *ShellQuote(NSString *s) {
    NSMutableString *out = [NSMutableString stringWithString:@"'"];
    for (NSUInteger i = 0; i < [s length]; ++i) {
        unichar ch = [s characterAtIndex:i];
        if (ch == '\'') {
            [out appendString:@"'\\''"];
        } else {
            [out appendFormat:@"%C", ch];
        }
    }
    [out appendString:@"'"];
    return out;
}

static BOOL IsValidBottleName(NSString *s) {
    if ([s length] == 0 || [s length] >= 120) return NO;
    for (NSUInteger i = 0; i < [s length]; ++i) {
        unichar ch = [s characterAtIndex:i];
        if (!(isalnum((int)ch) || ch == '-' || ch == '_')) return NO;
    }
    return YES;
}

static BOOL IsValidWindowID(NSString *s) {
    if ([s length] == 0 || [s length] > 32) return NO;
    NSUInteger start = 0;
    if ([s hasPrefix:@"0x"] || [s hasPrefix:@"0X"]) {
        if ([s length] <= 2) return NO;
        start = 2;
    }
    for (NSUInteger i = start; i < [s length]; ++i) {
        unichar ch = [s characterAtIndex:i];
        if (!isxdigit((int)ch)) return NO;
    }
    return YES;
}

static NSString *VMNameForBottle(NSString *bottle) {
    return [NSString stringWithFormat:@"hase-%@", bottle];
}

static NSString *BottlePathForBottle(NSString *bottle) {
    const char *root = getenv("HASE_ROOT");
    NSString *rootPath = nil;
    if (root && *root) {
        rootPath = [NSString stringWithUTF8String:root];
    } else {
        rootPath = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/HaSe/bottles"];
    }
    return [rootPath stringByAppendingPathComponent:bottle];
}

static NSData *RunShellData(NSString *script, int *exitCode, NSString **errorText) {
    NSTask *task = [[NSTask alloc] init];
    NSPipe *stdoutPipe = [NSPipe pipe];
    NSPipe *stderrPipe = [NSPipe pipe];

    task.launchPath = @"/bin/sh";
    task.arguments = @[@"-lc", script];
    task.standardOutput = stdoutPipe;
    task.standardError = stderrPipe;

    @try {
        [task launch];
    } @catch (NSException *e) {
        if (exitCode) *exitCode = 127;
        if (errorText) *errorText = [NSString stringWithFormat:@"failed to launch shell: %@", e.reason];
        return [NSData data];
    }

    NSData *outData = [[stdoutPipe fileHandleForReading] readDataToEndOfFile];
    NSData *errData = [[stderrPipe fileHandleForReading] readDataToEndOfFile];
    [task waitUntilExit];

    if (exitCode) *exitCode = task.terminationStatus;
    if (errorText) {
        NSString *err = [[NSString alloc] initWithData:errData encoding:NSUTF8StringEncoding];
        *errorText = err ?: @"";
    }
    return outData ?: [NSData data];
}

static NSString *LimactlShellScript(NSString *vmName, NSString *guestScript) {
    NSString *path = @"export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH";
    return [NSString stringWithFormat:
        @"%@; limactl --tty=false shell --workdir=/mnt/hase %@ sh -lc %@",
        path, ShellQuote(vmName), ShellQuote(guestScript)];
}

static NSArray<HaSeLinuxWindow *> *FetchLinuxWindows(NSString *bottle, NSString **errorText) {
    // With Matchbox, we capture the entire root window instead of specific clients.
    // Return a dummy root window.
    HaSeLinuxWindow *w = [[HaSeLinuxWindow alloc] init];
    w.windowID = @"root";
    w.processID = 0;
    w.x = 0;
    w.y = 0;
    w.width = 1920;
    w.height = 1080;
    w.title = @"HaSe Hidden Session";
    return @[w];
}

static NSData *CaptureWindowBMP(NSString *bottle, int *exitCode, NSString **errorText) {
    NSString *vmName = VMNameForBottle(bottle);
    NSString *guestScript = @"export DISPLAY=\"${HASE_DISPLAY:-:99}\"; "
                             "xwd -silent -root | xwdtopnm 2>/dev/null | ppmtobmp 2>/dev/null";
    return RunShellData(LimactlShellScript(vmName, guestScript), exitCode, errorText);
}

static BOOL HasBMPSignature(NSData *data) {
    static const unsigned char sig[2] = { 'B', 'M' };
    return [data length] >= sizeof sig && memcmp([data bytes], sig, sizeof sig) == 0;
}

static uint32_t BSwap32(uint32_t v) {
    return ((v & 0x000000ffU) << 24) |
           ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) |
           ((v & 0xff000000U) >> 24);
}

static uint16_t ReadPixel16(const uint8_t *p, BOOL msbFirst) {
    if (msbFirst) return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint32_t ReadPixel32(const uint8_t *p, BOOL msbFirst) {
    if (msbFirst) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint32_t ReadXWDU32(const uint8_t *p, BOOL swap) {
    uint32_t v = 0;
    memcpy(&v, p, sizeof v);
    return swap ? BSwap32(v) : v;
}

static uint8_t XWDChannel(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1U) == 0U && shift < 31) shift++;
    uint32_t compact = (pixel & mask) >> shift;
    uint32_t maxValue = mask >> shift;
    if (maxValue == 0) return 0;
    return (uint8_t)((compact * 255U + (maxValue / 2U)) / maxValue);
}

static NSImage *ImageFromXWDData(NSData *data, NSSize *imageSize) {
    const uint8_t *bytes = (const uint8_t *)[data bytes];
    NSUInteger length = [data length];
    if (length < 100) return nil;

    uint32_t rawVersion = 0;
    memcpy(&rawVersion, bytes + 4, sizeof rawVersion);
    BOOL swap = NO;
    if (rawVersion == 7U) {
        swap = NO;
    } else if (BSwap32(rawVersion) == 7U) {
        swap = YES;
    } else {
        return nil;
    }

    uint32_t fields[25];
    for (NSUInteger i = 0; i < 25; ++i) {
        fields[i] = ReadXWDU32(bytes + (i * 4), swap);
    }

    uint32_t headerSize = fields[0];
    uint32_t width = fields[4];
    uint32_t height = fields[5];
    uint32_t byteOrder = fields[8];
    uint32_t bitsPerPixel = fields[11];
    uint32_t bytesPerLine = fields[12];
    uint32_t redMask = fields[15];
    uint32_t greenMask = fields[16];
    uint32_t blueMask = fields[17];
    uint32_t ncolors = fields[19];
    NSUInteger pixelOffset = (NSUInteger)headerSize + ((NSUInteger)ncolors * 12U);

    if (width == 0 || height == 0 || width > 4096 || height > 4096) return nil;
    if (bitsPerPixel != 16 && bitsPerPixel != 24 && bitsPerPixel != 32) return nil;
    if (pixelOffset >= length) return nil;

    NSUInteger bytesPerPixel = (bitsPerPixel + 7U) / 8U;
    NSUInteger needed = pixelOffset + ((NSUInteger)height * (NSUInteger)bytesPerLine);
    if (needed > length || bytesPerLine < (NSUInteger)width * bytesPerPixel) return nil;

    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:(NSInteger)width
                      pixelsHigh:(NSInteger)height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:(NSInteger)width * 4
                    bitsPerPixel:32];
    if (!rep) return nil;

    uint8_t *dst = [rep bitmapData];
    BOOL msbFirst = (byteOrder != 0);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *srcRow = bytes + pixelOffset + ((NSUInteger)y * bytesPerLine);
        uint8_t *dstRow = dst + ((NSUInteger)y * width * 4U);
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *src = srcRow + ((NSUInteger)x * bytesPerPixel);
            uint32_t pixel = 0;
            if (bitsPerPixel == 16) {
                pixel = ReadPixel16(src, msbFirst);
            } else if (bitsPerPixel == 24) {
                pixel = msbFirst
                    ? (((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2])
                    : (((uint32_t)src[2] << 16) | ((uint32_t)src[1] << 8) | src[0]);
            } else {
                pixel = ReadPixel32(src, msbFirst);
            }
            uint8_t *d = dstRow + ((NSUInteger)x * 4U);
            d[0] = XWDChannel(pixel, redMask);
            d[1] = XWDChannel(pixel, greenMask);
            d[2] = XWDChannel(pixel, blueMask);
            d[3] = 255;
        }
    }

    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [img addRepresentation:rep];
    if (imageSize) *imageSize = NSMakeSize(width, height);
    return img;
}

static BOOL AppendInputCommand(NSString *bottle, NSString *script, NSString **errorText) {
    NSString *runtime = [BottlePathForBottle(bottle) stringByAppendingPathComponent:@"runtime"];
    NSString *queue = [runtime stringByAppendingPathComponent:@"input.queue"];
    NSString *line = [[script stringByReplacingOccurrencesOfString:@"\n" withString:@" "]
        stringByAppendingString:@"\n"];
    NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
    if (!data) return NO;

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:queue]) {
        [fm createFileAtPath:queue contents:nil attributes:nil];
    }
    NSFileHandle *handle = [NSFileHandle fileHandleForWritingAtPath:queue];
    if (!handle) {
        if (errorText) *errorText = @"could not open HaSe input queue";
        return NO;
    }
    @try {
        [handle seekToEndOfFile];
        [handle writeData:data];
        [handle closeFile];
        return YES;
    } @catch (NSException *e) {
        if (errorText) *errorText = e.reason ?: @"failed to write HaSe input queue";
        return NO;
    }
}

static CGFloat ClampCGFloat(CGFloat value, CGFloat minValue, CGFloat maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

static NSString *KeyNameForEvent(NSEvent *event) {
    NSString *chars = [event charactersIgnoringModifiers] ?: @"";
    if ([chars length] == 0) return nil;

    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 0x0003: return @"Return";
        case 0x0009: return @"Tab";
        case 0x000d: return @"Return";
        case 0x001b: return @"Escape";
        case 0x007f: return @"BackSpace";
        case NSDeleteFunctionKey: return @"Delete";
        case NSLeftArrowFunctionKey: return @"Left";
        case NSRightArrowFunctionKey: return @"Right";
        case NSUpArrowFunctionKey: return @"Up";
        case NSDownArrowFunctionKey: return @"Down";
        case NSHomeFunctionKey: return @"Home";
        case NSEndFunctionKey: return @"End";
        case NSPageUpFunctionKey: return @"Page_Up";
        case NSPageDownFunctionKey: return @"Page_Down";
        case NSF1FunctionKey: return @"F1";
        case NSF2FunctionKey: return @"F2";
        case NSF3FunctionKey: return @"F3";
        case NSF4FunctionKey: return @"F4";
        case NSF5FunctionKey: return @"F5";
        case NSF6FunctionKey: return @"F6";
        case NSF7FunctionKey: return @"F7";
        case NSF8FunctionKey: return @"F8";
        case NSF9FunctionKey: return @"F9";
        case NSF10FunctionKey: return @"F10";
        case NSF11FunctionKey: return @"F11";
        case NSF12FunctionKey: return @"F12";
        default: return nil;
    }
}

@class HaSeInputImageView;

@interface HaSeWindowHostController : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, copy) NSString *bottle;
@property(nonatomic, copy) NSString *explicitWindowID;
@property(nonatomic, strong) HaSeLinuxWindow *selectedWindow;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) HaSeInputImageView *imageView;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) dispatch_queue_t inputQueue;
@property(nonatomic) NSSize guestImageSize;
@property(nonatomic) NSTimeInterval lastMouseMoveSent;
@property(nonatomic) BOOL refreshInFlight;
@property(nonatomic) BOOL refreshQueued;
@property(nonatomic) BOOL sizedFromGuest;
@property(nonatomic) BOOL haveSeenWindow;
@property(nonatomic) NSInteger refreshCounter;
- (void)handleMouseEvent:(NSEvent *)event button:(NSInteger)button pressed:(BOOL)pressed;
- (void)handleMouseMoveEvent:(NSEvent *)event;
- (void)handleScrollEvent:(NSEvent *)event;
- (void)handleKeyEvent:(NSEvent *)event;
@end

@interface HaSeInputImageView : NSImageView
@property(nonatomic, weak) HaSeWindowHostController *controller;
@end

@implementation HaSeInputImageView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}

- (void)mouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    [self.controller handleMouseEvent:event button:1 pressed:YES];
}

- (void)mouseUp:(NSEvent *)event {
    [self.controller handleMouseEvent:event button:1 pressed:NO];
}

- (void)rightMouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    [self.controller handleMouseEvent:event button:3 pressed:YES];
}

- (void)rightMouseUp:(NSEvent *)event {
    [self.controller handleMouseEvent:event button:3 pressed:NO];
}

- (void)otherMouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    [self.controller handleMouseEvent:event button:2 pressed:YES];
}

- (void)otherMouseUp:(NSEvent *)event {
    [self.controller handleMouseEvent:event button:2 pressed:NO];
}

- (void)mouseMoved:(NSEvent *)event {
    [self.controller handleMouseMoveEvent:event];
}

- (void)mouseDragged:(NSEvent *)event {
    [self.controller handleMouseMoveEvent:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
    [self.controller handleMouseMoveEvent:event];
}

- (void)otherMouseDragged:(NSEvent *)event {
    [self.controller handleMouseMoveEvent:event];
}

- (void)scrollWheel:(NSEvent *)event {
    [self.controller handleScrollEvent:event];
}

- (void)keyDown:(NSEvent *)event {
    [self.controller handleKeyEvent:event];
}

@end

@implementation HaSeWindowHostController

- (instancetype)initWithBottle:(NSString *)bottle explicitWindowID:(NSString *)windowID {
    self = [super init];
    if (!self) return nil;
    _bottle = [bottle copy];
    _explicitWindowID = [windowID copy];
    _inputQueue = dispatch_queue_create("dev.hase.window-host.input", DISPATCH_QUEUE_SERIAL);
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    [self createHostWindow:NSMakeSize(720, 420)];
    [self refreshNow];
    const char *intervalEnv = getenv("HASE_HOST_REFRESH_INTERVAL");
    NSTimeInterval interval = intervalEnv && *intervalEnv ? atof(intervalEnv) : (1.0 / 60.0);
    if (interval < 0.005) interval = 0.005;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:interval
                                                 repeats:YES
                                                   block:^(NSTimer *t) {
        (void)t;
        [self requestRefresh];
    }];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [self.timer invalidate];
    [NSApp terminate:nil];
}

- (void)createHostWindow:(NSSize)size {
    NSUInteger style = NSWindowStyleMaskTitled |
                       NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable |
                       NSWindowStyleMaskMiniaturizable;
    NSRect frame = NSMakeRect(0, 0, size.width, size.height);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = [NSString stringWithFormat:@"HaSe %@", self.bottle];
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;

    NSView *content = [[NSView alloc] initWithFrame:frame];
    content.autoresizesSubviews = YES;

    self.imageView = [[HaSeInputImageView alloc] initWithFrame:content.bounds];
    self.imageView.controller = self;
    self.imageView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.imageView.imageScaling = NSImageScaleAxesIndependently;
    self.imageView.wantsLayer = YES;
    self.imageView.layer.backgroundColor = [[NSColor blackColor] CGColor];
    [content addSubview:self.imageView];

    self.statusLabel = [[NSTextField alloc] initWithFrame:NSInsetRect(content.bounds, 24, 24)];
    self.statusLabel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.statusLabel.editable = NO;
    self.statusLabel.selectable = NO;
    self.statusLabel.bezeled = NO;
    self.statusLabel.drawsBackground = NO;
    self.statusLabel.alignment = NSTextAlignmentCenter;
    self.statusLabel.textColor = [NSColor secondaryLabelColor];
    self.statusLabel.font = [NSFont systemFontOfSize:14];
    self.statusLabel.stringValue = @"Waiting for a Linux window...";
    [content addSubview:self.statusLabel];

    self.window.contentView = content;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.imageView];
}

- (void)setStatus:(NSString *)status visible:(BOOL)visible {
    self.statusLabel.stringValue = status ?: @"";
    self.statusLabel.hidden = !visible;
}

- (void)resizeForGuestImage:(NSSize)imageSize {
    if (imageSize.width < 1 || imageSize.height < 1) return;

    NSScreen *screen = self.window.screen ?: [NSScreen mainScreen];
    NSRect visibleFrame = screen.visibleFrame;
    CGFloat maxW = visibleFrame.size.width * 0.85;
    CGFloat maxH = visibleFrame.size.height * 0.85;
    CGFloat scale = MIN(1.0, MIN(maxW / imageSize.width, maxH / imageSize.height));
    NSSize contentSize = NSMakeSize(MAX(220.0, floor(imageSize.width * scale)),
                                    MAX(90.0, floor(imageSize.height * scale)));

    NSSize current = self.window.contentView.bounds.size;
    if (fabs(current.width - contentSize.width) < 1.0 &&
        fabs(current.height - contentSize.height) < 1.0) {
        return;
    }
    [self.window setContentSize:contentSize];
}

- (BOOL)linuxPointForEvent:(NSEvent *)event x:(NSInteger *)outX y:(NSInteger *)outY {
    if (!self.selectedWindow || [self.selectedWindow.windowID length] == 0) return NO;
    NSRect bounds = self.imageView.bounds;
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return NO;

    NSPoint local = [self.imageView convertPoint:event.locationInWindow fromView:nil];

    /* Calculate relative coordinates within the window (0,0 is top-left) */
    CGFloat unitX = ClampCGFloat(local.x / bounds.size.width, 0.0, 1.0);
    CGFloat unitY = ClampCGFloat((bounds.size.height - local.y) / bounds.size.height, 0.0, 1.0);

    CGFloat w = self.guestImageSize.width > 0 ? self.guestImageSize.width : self.selectedWindow.width;
    CGFloat h = self.guestImageSize.height > 0 ? self.guestImageSize.height : self.selectedWindow.height;

    if (outX) *outX = (NSInteger)llround(unitX * (w - 1.0));
    if (outY) *outY = (NSInteger)llround(unitY * (h - 1.0));
    return YES;
}

- (void)sendInputScript:(NSString *)script {
    NSString *bottle = self.bottle;
    NSString *guestScript = [NSString stringWithFormat:
        @"export DISPLAY=\"${HASE_DISPLAY:-:99}\"; %@",
        script];

    dispatch_async(self.inputQueue, ^{
        int rc = 0;
        NSString *errorText = nil;
        if (!AppendInputCommand(bottle, script, &errorText)) {
            RunShellData(LimactlShellScript(VMNameForBottle(bottle), guestScript), &rc, &errorText);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (rc != 0 && [errorText length] > 0) {
                [self setStatus:errorText visible:YES];
            }
            [self requestRefresh];
        });
    });
}

- (NSString *)focusScriptForWindowID:(NSString *)windowID {
    return @"";
}

- (void)handleMouseMoveEvent:(NSEvent *)event {
    if (event.timestamp - self.lastMouseMoveSent < 0.016) return;
    self.lastMouseMoveSent = event.timestamp;

    NSInteger x = 0, y = 0;
    if (![self linuxPointForEvent:event x:&x y:&y]) return;

    NSString *script = [NSString stringWithFormat:
        @"xdotool mousemove %ld %ld",
        (long)x, (long)y];
    [self sendInputScript:script];
}

- (void)handleMouseEvent:(NSEvent *)event button:(NSInteger)button pressed:(BOOL)pressed {
    NSInteger x = 0, y = 0;
    if (![self linuxPointForEvent:event x:&x y:&y]) return;

    NSString *verb = pressed ? @"mousedown" : @"mouseup";
    NSString *script = [NSString stringWithFormat:
        @"xdotool mousemove %ld %ld %@ %ld",
        (long)x, (long)y, verb, (long)button];
    [self sendInputScript:script];
}

- (void)handleScrollEvent:(NSEvent *)event {
    if (!self.selectedWindow) return;
    CGFloat dy = event.scrollingDeltaY;
    CGFloat dx = event.scrollingDeltaX;
    NSInteger button = 0;
    NSInteger steps = 0;

    if (fabs(dy) >= fabs(dx) && fabs(dy) > 0.01) {
        button = dy > 0 ? 4 : 5;
        steps = MAX(1, MIN(6, (NSInteger)ceil(fabs(dy) / 8.0)));
    } else if (fabs(dx) > 0.01) {
        button = dx > 0 ? 6 : 7;
        steps = MAX(1, MIN(6, (NSInteger)ceil(fabs(dx) / 8.0)));
    } else {
        return;
    }

    NSMutableString *clicks = [NSMutableString string];
    for (NSInteger i = 0; i < steps; ++i) {
        [clicks appendFormat:@" xdotool click %ld;", (long)button];
    }
    NSString *script = [NSString stringWithFormat:@"%@ %@", [self focusScriptForWindowID:self.selectedWindow.windowID], clicks];
    [self sendInputScript:script];
}

- (void)handleKeyEvent:(NSEvent *)event {
    if (!self.selectedWindow) return;
    if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) return;

    NSString *windowID = self.selectedWindow.windowID;
    NSString *keyName = KeyNameForEvent(event);
    NSString *script = nil;
    if (keyName) {
        script = [NSString stringWithFormat:@"%@ xdotool key --clearmodifiers %@",
                  [self focusScriptForWindowID:windowID], keyName];
    } else {
        NSString *chars = [event characters] ?: @"";
        if ([chars length] == 0) return;
        script = [NSString stringWithFormat:@"%@ xdotool type --clearmodifiers --delay 0 %@",
                  [self focusScriptForWindowID:windowID], ShellQuote(chars)];
    }
    [self sendInputScript:script];
}

- (void)requestRefresh {
    if (self.refreshInFlight) {
        self.refreshQueued = YES;
        return;
    }
    [self refreshNow];
}

- (void)refreshNow {
    if (self.refreshInFlight) {
        self.refreshQueued = YES;
        return;
    }
    self.refreshInFlight = YES;
    self.refreshQueued = NO;

    NSString *bottle = self.bottle;
    NSString *explicitID = self.explicitWindowID;
    /* Re-use cached window when possible; only re-list every 10 refreshes or
       when we have no window yet. */
    HaSeLinuxWindow *cachedWindow = self.selectedWindow;
    self.refreshCounter++;
    BOOL needsList = (!cachedWindow || self.refreshCounter % 10 == 0 ||
                      [explicitID length] > 0);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
        HaSeLinuxWindow *target = cachedWindow;

        if (needsList) {
            NSString *listError = nil;
            NSArray<HaSeLinuxWindow *> *windows = FetchLinuxWindows(bottle, &listError);
            BOOL explicitMissing = NO;
            if ([explicitID length] > 0) {
                target = nil;
                for (HaSeLinuxWindow *w in windows) {
                    if ([w.windowID caseInsensitiveCompare:explicitID] == NSOrderedSame) {
                        target = w;
                        break;
                    }
                }
                if (!target) {
                    explicitMissing = YES;
                    target = [[HaSeLinuxWindow alloc] init];
                    target.windowID = explicitID;
                    target.title = explicitID;
                }
            } else {
                target = [windows count] > 0 ? windows[0] : nil;
            }

            if (!target || (explicitMissing && self.haveSeenWindow)) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [self finishRefresh];
                    if (explicitMissing && self.haveSeenWindow) {
                        [self.window close];
                    } else {
                        [self setStatus:listError ?: @"No Linux windows found on display :99." visible:YES];
                    }
                });
                return;
            }
        }

        if (!target) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self finishRefresh];
                [self setStatus:@"No Linux windows found on display :99." visible:YES];
            });
            return;
        }

        int rc = 0;
        NSString *captureError = nil;
        NSImage *img = nil;
        NSSize decodedSize = NSZeroSize;

        /* Try to read from shared filesystem first (zero-SSH) */
        NSString *bottlePath = BottlePathForBottle(bottle);
        NSString *xwdPath = [bottlePath stringByAppendingPathComponent:@"runtime/frame.xwd"];
        NSString *bmpPath = [bottlePath stringByAppendingPathComponent:@"runtime/frame.bmp"];

        NSData *xwd = [NSData dataWithContentsOfFile:xwdPath options:NSDataReadingMappedIfSafe error:nil];
        img = ImageFromXWDData(xwd, &decodedSize);
        if (!img) {
            NSData *bmp = [NSData dataWithContentsOfFile:bmpPath options:NSDataReadingMappedIfSafe error:nil];
            if (HasBMPSignature(bmp)) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:bmp];
                img = [[NSImage alloc] initWithData:bmp];
                if (rep) decodedSize = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
            }
        }

        if (!img) {
            /* Fallback to SSH capture */
            NSData *bmp = CaptureWindowBMP(bottle, &rc, &captureError);
            if (HasBMPSignature(bmp)) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:bmp];
                img = [[NSImage alloc] initWithData:bmp];
                if (rep) decodedSize = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
            }
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [self finishRefresh];
            self.selectedWindow = target;
            self.window.title = [NSString stringWithFormat:@"HaSe %@ - %@",
                                 self.bottle,
                                 [target.title length] ? target.title : target.windowID];

            if (rc != 0 || !img) {
                NSString *err = [captureError length] ? captureError : @"Waiting for Steam to initialize...";
                [self setStatus:err visible:YES];
                return;
            }

            if (decodedSize.width > 0 && decodedSize.height > 0) {
                img.size = decodedSize;
                self.guestImageSize = img.size;
                if (!self.sizedFromGuest) {
                    [self resizeForGuestImage:img.size];
                    self.sizedFromGuest = YES;
                }
            }
            self.imageView.image = img;
            self.haveSeenWindow = YES;
            [self setStatus:@"" visible:NO];
        });
        }
    });
}

- (void)finishRefresh {
    self.refreshInFlight = NO;
    if (self.refreshQueued) {
        self.refreshQueued = NO;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self refreshNow];
        });
    }
}

@end

static void PrintUsage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  hase_window_host <bottle> [window-id]\n"
        "  hase_window_host --list <bottle>\n"
        "\n"
        "Shows a Linux X11 window from the hidden HaSe Lima VM as a native macOS window.\n"
        "Start the bottle first with: hasectl start <bottle>\n");
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            PrintUsage(argc < 2 ? stderr : stdout);
            return argc < 2 ? 2 : 0;
        }

        BOOL listOnly = NO;
        NSString *bottle = nil;
        NSString *windowID = nil;

        if (!strcmp(argv[1], "--list")) {
            if (argc != 3) {
                PrintUsage(stderr);
                return 2;
            }
            listOnly = YES;
            bottle = [NSString stringWithUTF8String:argv[2]];
        } else {
            if (argc < 2 || argc > 3) {
                PrintUsage(stderr);
                return 2;
            }
            bottle = [NSString stringWithUTF8String:argv[1]];
            if (argc == 3) windowID = [NSString stringWithUTF8String:argv[2]];
        }

        if (!IsValidBottleName(bottle)) {
            fprintf(stderr, "invalid bottle name\n");
            return 2;
        }
        if ([windowID length] > 0 && !IsValidWindowID(windowID)) {
            fprintf(stderr, "invalid window id\n");
            return 2;
        }

        if (listOnly) {
            NSString *errorText = nil;
            NSArray<HaSeLinuxWindow *> *windows = FetchLinuxWindows(bottle, &errorText);
            if ([windows count] == 0 && [errorText length] > 0) {
                fprintf(stderr, "%s\n", [errorText UTF8String]);
                return 1;
            }
            printf("linux_window_id\tprocess_id\tx\ty\twidth\theight\ttitle\n");
            for (HaSeLinuxWindow *w in windows) {
                printf("%s\t%ld\t%ld\t%ld\t%ld\t%ld\t%s\n",
                       [w.windowID UTF8String],
                       (long)w.processID,
                       (long)w.x,
                       (long)w.y,
                       (long)w.width,
                       (long)w.height,
                       [[w.title stringByReplacingOccurrencesOfString:@"\t" withString:@" "] UTF8String]);
            }
            return 0;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        HaSeWindowHostController *controller =
            [[HaSeWindowHostController alloc] initWithBottle:bottle explicitWindowID:windowID];
        app.delegate = controller;
        [app run];
    }
    return 0;
}
