#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static NSInteger IntegerFromCString(const char *s, NSInteger fallback) {
    if (!s || !*s) return fallback;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s) return fallback;
    return (NSInteger)value;
}

static double DoubleFromCString(const char *s, double fallback) {
    if (!s || !*s) return fallback;
    char *end = NULL;
    double value = strtod(s, &end);
    if (end == s || !isfinite(value)) return fallback;
    return value;
}

static BOOL BoolFromCString(const char *s, BOOL fallback) {
    if (!s || !*s) return fallback;
    if (!strcmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on")) return YES;
    if (!strcmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off")) return NO;
    return fallback;
}

static NSArray<HaSeLinuxWindow *> *FetchLinuxWindows(NSString *bottle, NSString **errorText) {
    NSString *vmName = VMNameForBottle(bottle);
    NSString *guestScript =
        @"export DISPLAY=\"${HASE_DISPLAY:-:99}\"; "
         "if command -v wmctrl >/dev/null 2>&1; then "
         "DISPLAY=\"$DISPLAY\" wmctrl -lG -p 2>/dev/null | "
         "awk 'BEGIN { OFS=\"\\t\" } { id=$1; pid=$3; x=$4; y=$5; w=$6; h=$7; title=\"\"; for (i=9; i<=NF; ++i) title=title (i==9 ? \"\" : \" \") $i; if (w >= 80 && h >= 80) print id, pid, x, y, w, h, title }'; "
         "exit 0; "
         "fi; "
         "if command -v xwininfo >/dev/null 2>&1; then "
         "xwininfo -root -tree 2>/dev/null | "
         "sed -nE 's/^[[:space:]]+(0x[0-9a-fA-F]+).* ([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+)[[:space:]].*/\\1\\t\\4\\t\\5\\t\\2\\t\\3/p' | "
         "while IFS='	' read id x y w h; do "
         "[ -n \"$w\" ] && [ -n \"$h\" ] || continue; "
         "[ \"$w\" -ge 80 ] && [ \"$h\" -ge 80 ] || continue; "
         "pid=$(xprop -id \"$id\" _NET_WM_PID 2>/dev/null | awk -F'= ' '/_NET_WM_PID/ {print $2; exit}'); "
         "class=$(xprop -id \"$id\" WM_CLASS 2>/dev/null | sed -n 's/.*= //p' | head -n1 | tr '\t' ' '); "
         "title=$(xprop -id \"$id\" _NET_WM_NAME WM_NAME 2>/dev/null | sed -n 's/.*= //p' | head -n1 | tr -d '\"' | tr '\t' ' '); "
         "[ -n \"$title$class$pid\" ] || continue; "
         "printf '0x%x\t%s\t%s\t%s\t%s\t%s\t%s\n' \"$id\" \"${pid:-0}\" \"${x:-0}\" \"${y:-0}\" \"$w\" \"$h\" \"${title:-$class}\"; "
         "done; fi";

    int rc = 0;
    NSString *err = nil;
    NSData *data = RunShellData(LimactlShellScript(vmName, guestScript), &rc, &err);
    if (rc != 0 && errorText) *errorText = err;

    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
    NSMutableArray<HaSeLinuxWindow *> *windows = [NSMutableArray array];
    NSArray<NSString *> *lines = [text componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    for (NSString *line in lines) {
        if ([line length] == 0) continue;
        NSArray<NSString *> *parts = [line componentsSeparatedByString:@"\t"];
        if ([parts count] < 7) continue;

        HaSeLinuxWindow *w = [[HaSeLinuxWindow alloc] init];
        w.windowID = parts[0];
        w.processID = IntegerFromCString([parts[1] UTF8String], 0);
        w.x = IntegerFromCString([parts[2] UTF8String], 0);
        w.y = IntegerFromCString([parts[3] UTF8String], 0);
        w.width = IntegerFromCString([parts[4] UTF8String], 0);
        w.height = IntegerFromCString([parts[5] UTF8String], 0);
        w.title = parts[6];
        if ([w.windowID length] > 0 && w.width >= 80 && w.height >= 80) {
            [windows addObject:w];
        }
    }

    [windows sortUsingComparator:^NSComparisonResult(HaSeLinuxWindow *a, HaSeLinuxWindow *b) {
        long long areaA = (long long)a.width * (long long)a.height;
        long long areaB = (long long)b.width * (long long)b.height;
        if (areaA > areaB) return NSOrderedAscending;
        if (areaA < areaB) return NSOrderedDescending;
        return [a.windowID compare:b.windowID];
    }];

    if ([windows count] > 0) return windows;

    HaSeLinuxWindow *root = [[HaSeLinuxWindow alloc] init];
    root.windowID = @"root";
    root.processID = 0;
    root.x = 0;
    root.y = 0;
    root.width = 960;
    root.height = 540;
    root.title = @"HaSe Hidden Session";
    return @[root];
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

static BOOL FileStamp(NSString *path, unsigned long long *mtimeNS, unsigned long long *size) {
    struct stat st;
    if (stat([path fileSystemRepresentation], &st) != 0) return NO;
    if (mtimeNS) {
        *mtimeNS = ((unsigned long long)st.st_mtimespec.tv_sec * 1000000000ULL) +
                   (unsigned long long)st.st_mtimespec.tv_nsec;
    }
    if (size) *size = (unsigned long long)st.st_size;
    return st.st_size > 0;
}

static NSTimeInterval RefreshIntervalFromEnvironment(void) {
    const char *intervalEnv = getenv("HASE_HOST_REFRESH_INTERVAL");
    NSTimeInterval interval = intervalEnv && *intervalEnv ? atof(intervalEnv) : 0.0;
    if (interval <= 0.0) {
        const char *fpsEnv = getenv("HASE_HOST_TARGET_FPS");
        double fps = fpsEnv && *fpsEnv ? atof(fpsEnv) : 30.0;
        if (fps <= 0.0) fps = 30.0;
        interval = 1.0 / fps;
    }

    const char *minFpsEnv = getenv("HASE_HOST_MIN_FPS");
    double minFps = minFpsEnv && *minFpsEnv ? atof(minFpsEnv) : 20.0;
    if (minFps > 0.0) {
        NSTimeInterval maxInterval = 1.0 / minFps;
        if (interval > maxInterval) interval = maxInterval;
    }
    if (interval < 0.001) interval = 0.001;
    return interval;
}

static void ForceMouseRelease(void) {
    CGAssociateMouseAndMouseCursorPosition(true);
    CGDisplayShowCursor(kCGDirectMainDisplay);
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

static NSImage *ImageFromXWDDataCropped(NSData *data,
                                        NSInteger cropX,
                                        NSInteger cropY,
                                        NSInteger cropWidth,
                                        NSInteger cropHeight,
                                        NSSize *imageSize) {
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
    uint32_t byteOrder = fields[7];
    uint32_t bitsPerPixel = fields[11];
    uint32_t bytesPerLine = fields[12];
    uint32_t redMask = fields[14];
    uint32_t greenMask = fields[15];
    uint32_t blueMask = fields[16];
    uint32_t ncolors = fields[19];
    NSUInteger pixelOffset = (NSUInteger)headerSize + ((NSUInteger)ncolors * 12U);

    if (width == 0 || height == 0 || width > 4096 || height > 4096) return nil;
    if (bitsPerPixel != 16 && bitsPerPixel != 24 && bitsPerPixel != 32) return nil;
    if (pixelOffset >= length) return nil;

    NSUInteger bytesPerPixel = (bitsPerPixel + 7U) / 8U;
    NSUInteger needed = pixelOffset + ((NSUInteger)height * (NSUInteger)bytesPerLine);
    if (needed > length || bytesPerLine < (NSUInteger)width * bytesPerPixel) return nil;

    NSInteger sx = 0;
    NSInteger sy = 0;
    NSInteger sw = (NSInteger)width;
    NSInteger sh = (NSInteger)height;
    if (cropWidth > 0 && cropHeight > 0) {
        sx = MAX((NSInteger)0, MIN(cropX, (NSInteger)width - 1));
        sy = MAX((NSInteger)0, MIN(cropY, (NSInteger)height - 1));
        sw = MAX((NSInteger)1, MIN(cropWidth, (NSInteger)width - sx));
        sh = MAX((NSInteger)1, MIN(cropHeight, (NSInteger)height - sy));
    }
    uint32_t outWidth = (uint32_t)sw;
    uint32_t outHeight = (uint32_t)sh;

    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:(NSInteger)outWidth
                      pixelsHigh:(NSInteger)outHeight
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:(NSInteger)outWidth * 4
                    bitsPerPixel:32];
    if (!rep) return nil;

    uint8_t *dst = [rep bitmapData];
    BOOL msbFirst = (byteOrder != 0);
    for (uint32_t y = 0; y < outHeight; ++y) {
        const uint8_t *srcRow = bytes + pixelOffset + ((NSUInteger)(sy + (NSInteger)y) * bytesPerLine);
        uint8_t *dstRow = dst + ((NSUInteger)y * outWidth * 4U);
        for (uint32_t x = 0; x < outWidth; ++x) {
            const uint8_t *src = srcRow + ((NSUInteger)(sx + (NSInteger)x) * bytesPerPixel);
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

    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(outWidth, outHeight)];
    [img addRepresentation:rep];
    if (imageSize) *imageSize = NSMakeSize(outWidth, outHeight);
    return img;
}

static NSImage *ImageFromXWDData(NSData *data, NSSize *imageSize) {
    return ImageFromXWDDataCropped(data, 0, 0, 0, 0, imageSize);
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
        case 0x0020: return @"space";
        case 0x007f: return @"BackSpace";
        case '-': return @"minus";
        case '=': return @"equal";
        case '[': return @"bracketleft";
        case ']': return @"bracketright";
        case '\\': return @"backslash";
        case ';': return @"semicolon";
        case '\'': return @"apostrophe";
        case ',': return @"comma";
        case '.': return @"period";
        case '/': return @"slash";
        case '`': return @"grave";
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
        default:
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
                unichar lower = (unichar)tolower((int)ch);
                return [NSString stringWithFormat:@"%c", (char)lower];
            }
            if (ch >= '0' && ch <= '9') {
                return [NSString stringWithFormat:@"%c", (char)ch];
            }
            return nil;
    }
}

static NSString *ModifierKeyNameForMask(NSEventModifierFlags mask) {
    if (mask == NSEventModifierFlagShift) return @"Shift_L";
    if (mask == NSEventModifierFlagControl) return @"Control_L";
    if (mask == NSEventModifierFlagOption) return @"Alt_L";
    return nil;
}

static BOOL TitleLooksSteamLike(NSString *title) {
    NSString *lower = [title lowercaseString] ?: @"";
    return [lower containsString:@"steam"] ||
           [lower containsString:@"friends"] ||
           [lower containsString:@"sign in"] ||
           [lower containsString:@"settings"];
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
@property(nonatomic, strong) id eventMonitor;
@property(nonatomic, strong) dispatch_queue_t inputQueue;
@property(nonatomic) NSSize guestImageSize;
@property(nonatomic) NSTimeInterval lastMouseMoveSent;
@property(nonatomic) BOOL refreshInFlight;
@property(nonatomic) BOOL refreshQueued;
@property(nonatomic) BOOL sizedFromGuest;
@property(nonatomic) BOOL haveSeenWindow;
@property(nonatomic) BOOL mouseCaptured;
@property(nonatomic) BOOL cursorHidden;
@property(nonatomic) NSInteger refreshCounter;
@property(nonatomic) NSInteger windowRelistInterval;
@property(nonatomic) double mouseSensitivity;
@property(nonatomic) NSEventModifierFlags lastModifierFlags;
@property(nonatomic) unsigned long long lastFrameMTimeNS;
@property(nonatomic) unsigned long long lastFrameSize;
@property(nonatomic, copy) NSString *lastFramePath;
- (void)handleMouseEvent:(NSEvent *)event button:(NSInteger)button pressed:(BOOL)pressed;
- (void)handleMouseMoveEvent:(NSEvent *)event;
- (void)handleScrollEvent:(NSEvent *)event;
- (void)handleKeyEvent:(NSEvent *)event pressed:(BOOL)pressed;
- (void)handleFlagsChanged:(NSEvent *)event;
- (void)setMouseCaptured:(BOOL)captured;
- (void)releaseHeldGameKeys;
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
    [self.controller handleKeyEvent:event pressed:YES];
}

- (void)keyUp:(NSEvent *)event {
    [self.controller handleKeyEvent:event pressed:NO];
}

- (void)flagsChanged:(NSEvent *)event {
    [self.controller handleFlagsChanged:event];
}

@end

@implementation HaSeWindowHostController

- (instancetype)initWithBottle:(NSString *)bottle explicitWindowID:(NSString *)windowID {
    self = [super init];
    if (!self) return nil;
    _bottle = [bottle copy];
    _explicitWindowID = [windowID copy];
    _inputQueue = dispatch_queue_create("dev.hase.window-host.input", DISPATCH_QUEUE_SERIAL);
    _windowRelistInterval = IntegerFromCString(getenv("HASE_WINDOW_RELIST_INTERVAL"), 120);
    if (_windowRelistInterval < 10) _windowRelistInterval = 10;
    _mouseSensitivity = DoubleFromCString(getenv("HASE_MOUSE_SENSITIVITY"), 1.0);
    if (_mouseSensitivity <= 0.01) _mouseSensitivity = 1.0;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    ForceMouseRelease();
    self.cursorHidden = NO;
    [self createHostWindow:NSMakeSize(720, 420)];
    [self installEventMonitor];
    [self refreshNow];
    NSTimeInterval interval = RefreshIntervalFromEnvironment();
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

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if (self.eventMonitor) {
        [NSEvent removeMonitor:self.eventMonitor];
        self.eventMonitor = nil;
    }
    [self setMouseCaptured:NO];
    [self releaseHeldGameKeys];
}

- (void)windowDidResignKey:(NSNotification *)notification {
    (void)notification;
    [self setMouseCaptured:NO];
    [self releaseHeldGameKeys];
}

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [self setMouseCaptured:NO];
    [self releaseHeldGameKeys];
    [self.timer invalidate];
    [NSApp terminate:nil];
}

- (void)installEventMonitor {
    if (self.eventMonitor) return;

    NSEventMask mask = NSEventMaskLeftMouseDown |
                       NSEventMaskLeftMouseUp |
                       NSEventMaskRightMouseDown |
                       NSEventMaskRightMouseUp |
                       NSEventMaskOtherMouseDown |
                       NSEventMaskOtherMouseUp |
                       NSEventMaskMouseMoved |
                       NSEventMaskLeftMouseDragged |
                       NSEventMaskRightMouseDragged |
                       NSEventMaskOtherMouseDragged |
                       NSEventMaskScrollWheel |
                       NSEventMaskKeyDown |
                       NSEventMaskKeyUp |
                       NSEventMaskFlagsChanged;

    __weak typeof(self) weakSelf = self;
    self.eventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
        HaSeWindowHostController *strongSelf = weakSelf;
        if (!strongSelf || event.window != strongSelf.window) return event;

        if (event.type == NSEventTypeLeftMouseDown ||
            event.type == NSEventTypeLeftMouseUp ||
            event.type == NSEventTypeRightMouseDown ||
            event.type == NSEventTypeRightMouseUp ||
            event.type == NSEventTypeOtherMouseDown ||
            event.type == NSEventTypeOtherMouseUp ||
            event.type == NSEventTypeMouseMoved ||
            event.type == NSEventTypeLeftMouseDragged ||
            event.type == NSEventTypeRightMouseDragged ||
            event.type == NSEventTypeOtherMouseDragged ||
            event.type == NSEventTypeScrollWheel) {
            NSPoint local = [strongSelf.imageView convertPoint:event.locationInWindow fromView:nil];
            if (!NSPointInRect(local, strongSelf.imageView.bounds)) return event;
        }

        switch (event.type) {
            case NSEventTypeLeftMouseDown:
                [NSApp activateIgnoringOtherApps:YES];
                [strongSelf.window makeKeyAndOrderFront:nil];
                [strongSelf.window makeFirstResponder:strongSelf.imageView];
                [strongSelf handleMouseEvent:event button:1 pressed:YES];
                return nil;
            case NSEventTypeLeftMouseUp:
                [strongSelf handleMouseEvent:event button:1 pressed:NO];
                return nil;
            case NSEventTypeRightMouseDown:
                [NSApp activateIgnoringOtherApps:YES];
                [strongSelf.window makeKeyAndOrderFront:nil];
                [strongSelf.window makeFirstResponder:strongSelf.imageView];
                [strongSelf handleMouseEvent:event button:3 pressed:YES];
                return nil;
            case NSEventTypeRightMouseUp:
                [strongSelf handleMouseEvent:event button:3 pressed:NO];
                return nil;
            case NSEventTypeOtherMouseDown:
                [NSApp activateIgnoringOtherApps:YES];
                [strongSelf.window makeKeyAndOrderFront:nil];
                [strongSelf.window makeFirstResponder:strongSelf.imageView];
                [strongSelf handleMouseEvent:event button:2 pressed:YES];
                return nil;
            case NSEventTypeOtherMouseUp:
                [strongSelf handleMouseEvent:event button:2 pressed:NO];
                return nil;
            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged:
                [strongSelf handleMouseMoveEvent:event];
                return nil;
            case NSEventTypeScrollWheel:
                [strongSelf handleScrollEvent:event];
                return nil;
            case NSEventTypeKeyDown:
                [strongSelf handleKeyEvent:event pressed:YES];
                return nil;
            case NSEventTypeKeyUp:
                [strongSelf handleKeyEvent:event pressed:NO];
                return nil;
            case NSEventTypeFlagsChanged:
                [strongSelf handleFlagsChanged:event];
                return nil;
            default:
                return event;
        }
    }];
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

    NSInteger localX = (NSInteger)llround(unitX * (w - 1.0));
    NSInteger localY = (NSInteger)llround(unitY * (h - 1.0));
    NSInteger originX = [self.selectedWindow.windowID isEqualToString:@"root"] ? 0 : self.selectedWindow.x;
    NSInteger originY = [self.selectedWindow.windowID isEqualToString:@"root"] ? 0 : self.selectedWindow.y;

    if (outX) *outX = originX + localX;
    if (outY) *outY = originY + localY;
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
        });
    });
}

- (NSString *)focusScriptForWindowID:(NSString *)windowID {
    if ([windowID length] == 0 || [windowID isEqualToString:@"root"]) return @"";
    return [NSString stringWithFormat:@"xdotool windowfocus %@ 2>/dev/null || true;",
                                      ShellQuote(windowID)];
}

- (BOOL)linuxCenterX:(NSInteger *)outX y:(NSInteger *)outY {
    if (!self.selectedWindow || [self.selectedWindow.windowID length] == 0) return NO;
    NSInteger originX = [self.selectedWindow.windowID isEqualToString:@"root"] ? 0 : self.selectedWindow.x;
    NSInteger originY = [self.selectedWindow.windowID isEqualToString:@"root"] ? 0 : self.selectedWindow.y;
    NSInteger w = self.selectedWindow.width > 0 ? self.selectedWindow.width : 960;
    NSInteger h = self.selectedWindow.height > 0 ? self.selectedWindow.height : 540;
    if (outX) *outX = originX + MAX((NSInteger)0, w / 2);
    if (outY) *outY = originY + MAX((NSInteger)0, h / 2);
    return YES;
}

- (BOOL)shouldCaptureMouseForSelectedWindow {
    if (!BoolFromCString(getenv("HASE_MOUSE_AUTO_CAPTURE"), NO)) return NO;
    if (!self.selectedWindow) return NO;

    NSString *title = [self.selectedWindow.title lowercaseString] ?: @"";
    if ([title containsString:@"ultrakill"]) return YES;
    if (TitleLooksSteamLike(self.selectedWindow.title)) return NO;
    return self.selectedWindow.width >= 640 && self.selectedWindow.height >= 360;
}

- (BOOL)selectedWindowLooksGame {
    if (!self.selectedWindow) return NO;
    if (TitleLooksSteamLike(self.selectedWindow.title)) return NO;
    return self.selectedWindow.width >= 320 && self.selectedWindow.height >= 240;
}

- (void)setMouseCaptured:(BOOL)captured {
    if (captured == _mouseCaptured) {
        if (!captured) {
            ForceMouseRelease();
            self.cursorHidden = NO;
        }
        return;
    }
    _mouseCaptured = captured;

    if (captured) {
        [NSApp activateIgnoringOtherApps:YES];
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.imageView];
        if (!self.cursorHidden) {
            CGDisplayHideCursor(kCGDirectMainDisplay);
            self.cursorHidden = YES;
        }
        CGAssociateMouseAndMouseCursorPosition(false);

        NSInteger x = 0, y = 0;
        if ([self linuxCenterX:&x y:&y]) {
            NSString *script = [NSString stringWithFormat:@"%@ xdotool mousemove %ld %ld",
                                [self focusScriptForWindowID:self.selectedWindow.windowID],
                                (long)x, (long)y];
            [self sendInputScript:script];
        }
    } else {
        ForceMouseRelease();
        if (self.cursorHidden) {
            self.cursorHidden = NO;
        }
    }
}

- (void)releaseHeldGameKeys {
    if (!self.selectedWindow) return;
    NSString *script = [NSString stringWithFormat:
        @"%@ xdotool keyup w a s d q e r f z x c v 1 2 3 4 5 6 7 8 9 0 space Shift_L Control_L Alt_L Left Right Up Down",
        [self focusScriptForWindowID:self.selectedWindow.windowID]];
    [self sendInputScript:script];
    self.lastModifierFlags = 0;
}

- (void)handleMouseMoveEvent:(NSEvent *)event {
    NSTimeInterval minInterval = self.mouseCaptured ? 0.008 : 0.016;
    if (event.timestamp - self.lastMouseMoveSent < minInterval) return;
    self.lastMouseMoveSent = event.timestamp;

    if (self.mouseCaptured) {
        if (!self.selectedWindow) return;
        NSInteger dx = (NSInteger)llround(event.deltaX * self.mouseSensitivity);
        NSInteger dy = (NSInteger)llround(-event.deltaY * self.mouseSensitivity);
        if (dx == 0 && dy == 0) return;
        dx = MAX((NSInteger)-400, MIN((NSInteger)400, dx));
        dy = MAX((NSInteger)-400, MIN((NSInteger)400, dy));

        NSString *script = [NSString stringWithFormat:
            @"%@ xdotool mousemove_relative -- %ld %ld",
            [self focusScriptForWindowID:self.selectedWindow.windowID],
            (long)dx, (long)dy];
        [self sendInputScript:script];
        return;
    }

    if (self.cursorHidden) {
        [self setMouseCaptured:NO];
    }

    NSInteger x = 0, y = 0;
    if (![self linuxPointForEvent:event x:&x y:&y]) return;

    NSString *script = [NSString stringWithFormat:
        @"%@ xdotool mousemove %ld %ld",
        [self focusScriptForWindowID:self.selectedWindow.windowID],
        (long)x, (long)y];
    [self sendInputScript:script];
}

- (void)handleMouseEvent:(NSEvent *)event button:(NSInteger)button pressed:(BOOL)pressed {
    NSInteger x = 0, y = 0;
    BOOL doubleClickCapture = pressed && button == 1 && [event clickCount] >= 2 && [self selectedWindowLooksGame];
    if (pressed && button == 1 && ([self shouldCaptureMouseForSelectedWindow] || doubleClickCapture)) {
        [self setMouseCaptured:YES];
    }
    if (self.mouseCaptured) {
        if (![self linuxCenterX:&x y:&y]) return;
    } else if (![self linuxPointForEvent:event x:&x y:&y]) {
        return;
    }

    NSString *verb = pressed ? @"mousedown" : @"mouseup";
    NSString *script = [NSString stringWithFormat:
        @"%@ xdotool mousemove %ld %ld %@ %ld",
        [self focusScriptForWindowID:self.selectedWindow.windowID],
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

- (void)handleKeyEvent:(NSEvent *)event pressed:(BOOL)pressed {
    if (!self.selectedWindow) return;

    BOOL commandDown = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    NSString *keyName = KeyNameForEvent(event);
    if (commandDown) {
        if (pressed && [keyName isEqualToString:@"g"]) {
            [self setMouseCaptured:!self.mouseCaptured];
        } else if (pressed && [keyName isEqualToString:@"Escape"]) {
            [self setMouseCaptured:NO];
            [self releaseHeldGameKeys];
        }
        return;
    }

    if (!keyName) return;
    if (pressed && [event isARepeat]) return;
    if (pressed && !self.mouseCaptured && BoolFromCString(getenv("HASE_CAPTURE_ON_WASD"), NO) &&
        [self selectedWindowLooksGame] &&
        ([keyName isEqualToString:@"w"] || [keyName isEqualToString:@"a"] ||
         [keyName isEqualToString:@"s"] || [keyName isEqualToString:@"d"])) {
        [self setMouseCaptured:YES];
    }
    if (pressed && self.mouseCaptured && [keyName isEqualToString:@"Escape"]) {
        [self setMouseCaptured:NO];
    }

    NSString *verb = pressed ? @"keydown" : @"keyup";
    NSString *script = [NSString stringWithFormat:@"%@ xdotool %@ %@",
                        [self focusScriptForWindowID:self.selectedWindow.windowID],
                        verb, ShellQuote(keyName)];
    [self sendInputScript:script];
}

- (void)handleFlagsChanged:(NSEvent *)event {
    if (!self.selectedWindow) return;

    NSEventModifierFlags mask = NSEventModifierFlagShift |
                                NSEventModifierFlagControl |
                                NSEventModifierFlagOption;
    NSEventModifierFlags current = event.modifierFlags & mask;
    NSEventModifierFlags previous = self.lastModifierFlags & mask;
    self.lastModifierFlags = current;

    NSEventModifierFlags keys[] = {
        NSEventModifierFlagShift,
        NSEventModifierFlagControl,
        NSEventModifierFlagOption
    };
    for (NSUInteger i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        NSEventModifierFlags bit = keys[i];
        BOOL wasDown = (previous & bit) != 0;
        BOOL isDown = (current & bit) != 0;
        if (wasDown == isDown) continue;

        NSString *keyName = ModifierKeyNameForMask(bit);
        if (!keyName) continue;
        NSString *verb = isDown ? @"keydown" : @"keyup";
        NSString *script = [NSString stringWithFormat:@"%@ xdotool %@ %@",
                            [self focusScriptForWindowID:self.selectedWindow.windowID],
                            verb, ShellQuote(keyName)];
        [self sendInputScript:script];
    }
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
    /* Window discovery crosses the VM boundary, so keep it off the hot path. */
    HaSeLinuxWindow *cachedWindow = self.selectedWindow;
    self.refreshCounter++;
    NSInteger relistEvery = self.windowRelistInterval > 0 ? self.windowRelistInterval : 120;
    BOOL needsList = (!cachedWindow || self.refreshCounter % relistEvery == 0);

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
        NSString *liveFramebufferPath = [[bottlePath stringByAppendingPathComponent:@"runtime/fb"]
            stringByAppendingPathComponent:@"Xvfb_screen0"];
        NSString *windowXwdPath = nil;
        if (target && ![target.windowID isEqualToString:@"root"] && IsValidWindowID(target.windowID)) {
            windowXwdPath = [[bottlePath stringByAppendingPathComponent:@"runtime/window-frames"]
                stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.xwd", target.windowID]];
        }
        NSString *xwdPath = [bottlePath stringByAppendingPathComponent:@"runtime/frame.xwd"];
        NSString *bmpPath = [bottlePath stringByAppendingPathComponent:@"runtime/frame.bmp"];
        unsigned long long frameMTimeNS = 0;
        unsigned long long frameSize = 0;
        NSString *framePath = nil;
        BOOL frameIsWindow = NO;
        BOOL frameIsLiveFramebuffer = NO;

        if (FileStamp(liveFramebufferPath, &frameMTimeNS, &frameSize)) {
            framePath = liveFramebufferPath;
            frameIsLiveFramebuffer = YES;
        } else if (windowXwdPath && FileStamp(windowXwdPath, &frameMTimeNS, &frameSize)) {
            framePath = windowXwdPath;
            frameIsWindow = YES;
        } else if (FileStamp(xwdPath, &frameMTimeNS, &frameSize)) {
            framePath = xwdPath;
        } else if (FileStamp(bmpPath, &frameMTimeNS, &frameSize)) {
            framePath = bmpPath;
        }

        if (framePath &&
            [framePath isEqualToString:self.lastFramePath ?: @""] &&
            frameMTimeNS == self.lastFrameMTimeNS &&
            frameSize == self.lastFrameSize &&
            !frameIsLiveFramebuffer &&
            !needsList) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [self finishRefresh];
            });
            return;
        }

        if (framePath && ([framePath isEqualToString:xwdPath] || frameIsLiveFramebuffer)) {
            NSData *xwd = [NSData dataWithContentsOfFile:framePath options:NSDataReadingMappedIfSafe error:nil];
            if ([target.windowID isEqualToString:@"root"]) {
                img = ImageFromXWDData(xwd, &decodedSize);
            } else {
                img = ImageFromXWDDataCropped(xwd, target.x, target.y, target.width, target.height, &decodedSize);
            }
        } else if (framePath && frameIsWindow) {
            NSData *xwd = [NSData dataWithContentsOfFile:framePath options:NSDataReadingMappedIfSafe error:nil];
            img = ImageFromXWDData(xwd, &decodedSize);
        } else if (framePath && [framePath isEqualToString:bmpPath]) {
            NSData *bmp = [NSData dataWithContentsOfFile:bmpPath options:NSDataReadingMappedIfSafe error:nil];
            if (HasBMPSignature(bmp)) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:bmp];
                img = [[NSImage alloc] initWithData:bmp];
                if (rep) decodedSize = NSMakeSize(rep.pixelsWide, rep.pixelsHigh);
            }
        }

        if (!img && !framePath) {
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
            NSString *previousWindowID = self.selectedWindow.windowID;
            self.selectedWindow = target;
            self.window.title = [NSString stringWithFormat:@"HaSe %@ - %@",
                                 self.bottle,
                                 [target.title length] ? target.title : target.windowID];
            if (![target.windowID isEqualToString:@"root"] &&
                ![previousWindowID isEqualToString:target.windowID]) {
                NSString *script = [NSString stringWithFormat:
                    @"xdotool windowraise %@ 2>/dev/null || true; xdotool windowfocus %@ 2>/dev/null || true",
                    ShellQuote(target.windowID), ShellQuote(target.windowID)];
                [self sendInputScript:script];
            }

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
            if (framePath) {
                self.lastFramePath = framePath;
                self.lastFrameMTimeNS = frameMTimeNS;
                self.lastFrameSize = frameSize;
            }
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

static BOOL PidIsAlive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0;
}

static NSTimeInterval WatchIntervalFromEnvironment(void) {
    const char *env = getenv("HASE_WINDOW_WATCH_INTERVAL");
    NSTimeInterval interval = env && *env ? atof(env) : 0.5;
    if (interval < 0.1) interval = 0.1;
    return interval;
}

static int RunWindowWatcher(NSString *bottle, const char *argv0) {
    char exePath[PATH_MAX];
    if (!realpath(argv0, exePath)) {
        snprintf(exePath, sizeof exePath, "%s", argv0);
    }

    NSMutableDictionary<NSString *, NSNumber *> *children = [NSMutableDictionary dictionary];
    NSTimeInterval interval = WatchIntervalFromEnvironment();

    while (true) {
        int status = 0;
        while (waitpid(-1, &status, WNOHANG) > 0) {}

        for (NSString *windowID in [[children allKeys] copy]) {
            pid_t pid = (pid_t)[children[windowID] intValue];
            if (!PidIsAlive(pid)) {
                [children removeObjectForKey:windowID];
            }
        }

        NSString *errorText = nil;
        NSArray<HaSeLinuxWindow *> *windows = FetchLinuxWindows(bottle, &errorText);
        BOOL hasGameWindow = NO;
        for (HaSeLinuxWindow *w in windows) {
            if ([w.windowID isEqualToString:@"root"]) continue;
            if (!TitleLooksSteamLike(w.title)) {
                hasGameWindow = YES;
                break;
            }
        }

        BOOL showSteamWhileGame = BoolFromCString(getenv("HASE_SHOW_STEAM_WHILE_GAME"), NO);
        NSMutableSet<NSString *> *currentWindowIDs = [NSMutableSet set];
        for (HaSeLinuxWindow *w in windows) {
            if ([w.windowID isEqualToString:@"root"]) continue;
            if (hasGameWindow && !showSteamWhileGame && TitleLooksSteamLike(w.title)) continue;
            [currentWindowIDs addObject:w.windowID];
        }

        for (NSString *windowID in [[children allKeys] copy]) {
            if ([currentWindowIDs containsObject:windowID]) continue;
            pid_t pid = (pid_t)[children[windowID] intValue];
            if (PidIsAlive(pid)) {
                kill(pid, SIGTERM);
            }
            [children removeObjectForKey:windowID];
        }

        for (HaSeLinuxWindow *w in windows) {
            if ([w.windowID isEqualToString:@"root"]) continue;
            if (hasGameWindow && !showSteamWhileGame && TitleLooksSteamLike(w.title)) continue;
            if ([children objectForKey:w.windowID]) continue;

            pid_t pid = fork();
            if (pid == 0) {
                setenv("HASE_HOST_TARGET_FPS", "30", 0);
                setenv("HASE_HOST_MIN_FPS", "20", 0);
                setenv("HASE_CAPTURE_ON_WASD", "0", 0);
                setenv("HASE_WINDOW_RELIST_INTERVAL", "30", 0);
                execl(exePath, exePath, [bottle UTF8String], [w.windowID UTF8String], (char *)NULL);
                _exit(127);
            }
            if (pid > 0) {
                children[w.windowID] = @(pid);
                fprintf(stderr, "HaSe window watcher: attached %s (%s) as pid %ld\n",
                        [w.windowID UTF8String],
                        [[w.title stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String],
                        (long)pid);
            }
        }

        usleep((useconds_t)(interval * 1000000.0));
    }
}

static void PrintUsage(FILE *out) {
    fprintf(out,
        "Usage:\n"
        "  hase_window_host <bottle> [window-id]\n"
        "  hase_window_host --watch <bottle>\n"
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
        BOOL watchMode = NO;
        NSString *bottle = nil;
        NSString *windowID = nil;

        if (!strcmp(argv[1], "--list")) {
            if (argc != 3) {
                PrintUsage(stderr);
                return 2;
            }
            listOnly = YES;
            bottle = [NSString stringWithUTF8String:argv[2]];
        } else if (!strcmp(argv[1], "--watch")) {
            if (argc != 3) {
                PrintUsage(stderr);
                return 2;
            }
            watchMode = YES;
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

        if (watchMode) {
            return RunWindowWatcher(bottle, argv[0]);
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
