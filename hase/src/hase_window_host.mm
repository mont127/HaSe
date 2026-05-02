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

@interface HaSeWindowHostController : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, copy) NSString *bottle;
@property(nonatomic, copy) NSString *explicitWindowID;
@property(nonatomic, strong) HaSeLinuxWindow *selectedWindow;
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) NSImageView *imageView;
@property(nonatomic, strong) NSTextField *statusLabel;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic) BOOL refreshInFlight;
@property(nonatomic) BOOL sizedFromGuest;
@end

@implementation HaSeWindowHostController

- (instancetype)initWithBottle:(NSString *)bottle explicitWindowID:(NSString *)windowID {
    self = [super init];
    if (!self) return nil;
    _bottle = [bottle copy];
    _explicitWindowID = [windowID copy];
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

    NSView *content = [[NSView alloc] initWithFrame:frame];
    content.autoresizesSubviews = YES;

    self.imageView = [[NSImageView alloc] initWithFrame:content.bounds];
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
