#import <Cocoa/Cocoa.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
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
    NSString *vmName = VMNameForBottle(bottle);
    NSString *guestScript = @"/mnt/hase/runtime/start-session.sh >/dev/null && /mnt/hase/runtime/window-snapshot.sh";
    NSString *hostScript = LimactlShellScript(vmName, guestScript);

    int rc = 0;
    NSString *stderrText = nil;
    NSData *data = RunShellData(hostScript, &rc, &stderrText);
    if (rc != 0) {
        if (errorText) {
            *errorText = [NSString stringWithFormat:@"window query failed for %@: %@", vmName, stderrText ?: @""];
        }
        return @[];
    }

    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!text) {
        if (errorText) *errorText = @"window query returned non-text data";
        return @[];
    }

    NSMutableArray<HaSeLinuxWindow *> *windows = [NSMutableArray array];
    NSArray<NSString *> *lines = [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines) {
        if ([line length] == 0 || [line hasPrefix:@"linux_window_id"]) continue;
        NSArray<NSString *> *fields = [line componentsSeparatedByString:@"\t"];
        if ([fields count] < 7) continue;

        HaSeLinuxWindow *w = [[HaSeLinuxWindow alloc] init];
        w.windowID = fields[0];
        w.processID = [fields[1] integerValue];
        w.x = [fields[2] integerValue];
        w.y = [fields[3] integerValue];
        w.width = [fields[4] integerValue];
        w.height = [fields[5] integerValue];
        w.title = [[fields subarrayWithRange:NSMakeRange(6, [fields count] - 6)] componentsJoinedByString:@"\t"];
        if ([w.windowID length] > 0 && w.width > 0 && w.height > 0) {
            [windows addObject:w];
        }
    }
    return windows;
}

static NSData *CaptureWindowPNG(NSString *bottle, NSString *windowID, int *exitCode, NSString **errorText) {
    NSString *vmName = VMNameForBottle(bottle);
    NSString *guestScript = [NSString stringWithFormat:
        @"WINDOW_ID=%@; "
         "if [ -x /mnt/hase/runtime/capture-window-png.sh ]; then "
         "  exec /mnt/hase/runtime/capture-window-png.sh \"$WINDOW_ID\"; "
         "fi; "
         "export DISPLAY=\"${HASE_DISPLAY:-:99}\"; "
         "for tool in xwd xwdtopnm pnmtopng; do "
         "  if ! command -v \"$tool\" >/dev/null 2>&1; then "
         "    echo \"$tool is not installed; install x11-apps and netpbm in the HaSe VM\" >&2; "
         "    exit 2; "
         "  fi; "
         "done; "
         "DISPLAY=\"${DISPLAY}\" xwd -silent -id \"$WINDOW_ID\" | xwdtopnm 2>/dev/null | pnmtopng",
        ShellQuote(windowID)];
    return RunShellData(LimactlShellScript(vmName, guestScript), exitCode, errorText);
}

static BOOL HasPNGSignature(NSData *data) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    return [data length] >= sizeof sig && memcmp([data bytes], sig, sizeof sig) == 0;
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
@property(nonatomic) BOOL sizedFromGuest;
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
    self.timer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                 repeats:YES
                                                   block:^(NSTimer *t) {
        (void)t;
        [self refreshNow];
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
    CGFloat imageWidth = self.guestImageSize.width > 0 ? self.guestImageSize.width : self.selectedWindow.width;
    CGFloat imageHeight = self.guestImageSize.height > 0 ? self.guestImageSize.height : self.selectedWindow.height;
    if (imageWidth <= 0 || imageHeight <= 0) return NO;

    CGFloat unitX = ClampCGFloat(local.x / bounds.size.width, 0.0, 1.0);
    CGFloat unitY = ClampCGFloat((bounds.size.height - local.y) / bounds.size.height, 0.0, 1.0);
    NSInteger linuxX = self.selectedWindow.x + (NSInteger)llround(unitX * (imageWidth - 1.0));
    NSInteger linuxY = self.selectedWindow.y + (NSInteger)llround(unitY * (imageHeight - 1.0));
    if (outX) *outX = linuxX;
    if (outY) *outY = linuxY;
    return YES;
}

- (void)sendInputScript:(NSString *)script {
    NSString *bottle = self.bottle;
    NSString *guestScript = [NSString stringWithFormat:
        @"/mnt/hase/runtime/start-session.sh >/dev/null; "
         "export DISPLAY=\"${HASE_DISPLAY:-:99}\"; "
         "if ! command -v xdotool >/dev/null 2>&1; then "
         "  echo 'xdotool is not installed in the HaSe VM' >&2; exit 2; "
         "fi; %@",
        script];

    dispatch_async(self.inputQueue, ^{
        int rc = 0;
        NSString *errorText = nil;
        RunShellData(LimactlShellScript(VMNameForBottle(bottle), guestScript), &rc, &errorText);
        if (rc != 0 && [errorText length] > 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self setStatus:errorText visible:YES];
            });
        }
    });
}

- (NSString *)focusScriptForWindowID:(NSString *)windowID {
    return [NSString stringWithFormat:
        @"WID=%@; "
         "xdotool windowactivate --sync \"$WID\" >/dev/null 2>&1 || "
         "xdotool windowfocus \"$WID\" >/dev/null 2>&1 || true; ",
        ShellQuote(windowID)];
}

- (void)handleMouseMoveEvent:(NSEvent *)event {
    if (event.timestamp - self.lastMouseMoveSent < 0.08) return;
    self.lastMouseMoveSent = event.timestamp;

    NSInteger x = 0, y = 0;
    if (![self linuxPointForEvent:event x:&x y:&y]) return;

    NSString *windowID = self.selectedWindow.windowID;
    NSString *script = [NSString stringWithFormat:
        @"%@ xdotool mousemove --sync %ld %ld",
        [self focusScriptForWindowID:windowID], (long)x, (long)y];
    [self sendInputScript:script];
}

- (void)handleMouseEvent:(NSEvent *)event button:(NSInteger)button pressed:(BOOL)pressed {
    NSInteger x = 0, y = 0;
    if (![self linuxPointForEvent:event x:&x y:&y]) return;

    NSString *windowID = self.selectedWindow.windowID;
    NSString *verb = pressed ? @"mousedown" : @"mouseup";
    NSString *script = [NSString stringWithFormat:
        @"%@ xdotool mousemove --sync %ld %ld %@ %ld",
        [self focusScriptForWindowID:windowID], (long)x, (long)y, verb, (long)button];
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

- (void)refreshNow {
    if (self.refreshInFlight) return;
    self.refreshInFlight = YES;

    NSString *bottle = self.bottle;
    NSString *explicitID = self.explicitWindowID;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *listError = nil;
        NSArray<HaSeLinuxWindow *> *windows = FetchLinuxWindows(bottle, &listError);
        HaSeLinuxWindow *target = nil;
        if ([explicitID length] > 0) {
            for (HaSeLinuxWindow *w in windows) {
                if ([w.windowID caseInsensitiveCompare:explicitID] == NSOrderedSame) {
                    target = w;
                    break;
                }
            }
            if (!target) {
                target = [[HaSeLinuxWindow alloc] init];
                target.windowID = explicitID;
                target.title = explicitID;
            }
        } else {
            target = [windows count] > 0 ? windows[0] : nil;
        }

        if (!target) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self.refreshInFlight = NO;
                [self setStatus:listError ?: @"No Linux windows found on display :99." visible:YES];
            });
            return;
        }

        int rc = 0;
        NSString *captureError = nil;
        NSData *png = CaptureWindowPNG(bottle, target.windowID, &rc, &captureError);

        dispatch_async(dispatch_get_main_queue(), ^{
            self.refreshInFlight = NO;
            self.selectedWindow = target;
            self.window.title = [NSString stringWithFormat:@"HaSe %@ - %@",
                                 self.bottle,
                                 [target.title length] ? target.title : target.windowID];

            if (rc != 0 || !HasPNGSignature(png)) {
                NSString *err = [captureError length] ? captureError : @"Window capture failed.";
                [self setStatus:err visible:YES];
                return;
            }

            NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:png];
            NSImage *img = [[NSImage alloc] initWithData:png];
            if (!img) {
                [self setStatus:@"Captured data was not a readable PNG image." visible:YES];
                return;
            }
            if (rep) {
                img.size = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
                self.guestImageSize = img.size;
                if (!self.sizedFromGuest) {
                    [self resizeForGuestImage:img.size];
                    self.sizedFromGuest = YES;
                }
            }
            self.imageView.image = img;
            [self setStatus:@"" visible:NO];
        });
    });
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
