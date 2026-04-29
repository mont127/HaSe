#import "host.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

@interface CheeseBridgeContentView : NSView
@end

@implementation CheeseBridgeContentView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)isOpaque { return YES; }
@end

NSWindow *host_create_window(uint32_t width, uint32_t height) {
    __block NSWindow *out = nil;
    dispatch_block_t make = ^{
        NSRect frame = NSMakeRect(0, 0, width, height);
        NSUInteger style = NSWindowStyleMaskTitled |
                           NSWindowStyleMaskClosable |
                           NSWindowStyleMaskResizable |
                           NSWindowStyleMaskMiniaturizable;
        NSWindow *w = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        [w setTitle:@"CheeseBridge"];
        CheeseBridgeContentView *v = [[CheeseBridgeContentView alloc] initWithFrame:frame];
        v.wantsLayer = YES;
        CAMetalLayer *layer = (CAMetalLayer *)v.layer;
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.contentsScale = w.backingScaleFactor;
        [w setContentView:v];
        [w center];
        [w makeKeyAndOrderFront:nil];
        out = w;
    };
    if ([NSThread isMainThread]) make();
    else dispatch_sync(dispatch_get_main_queue(), make);
    return out;
}

CAMetalLayer *host_window_layer(NSWindow *w) {
    return (CAMetalLayer *)w.contentView.layer;
}
