// Платформенный слой macOS: нативное окно Cocoa + NSOpenGLContext (без GLFW/SDL).
// Также предоставляет offscreen-контекст CGL для headless-режима / тестов.
#include "crossrender/platform/Window.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_MACOS)

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#if defined(ENG_METAL)
#import <QuartzCore/CAMetalLayer.h>
#include "MetalContext.h"
#endif

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <atomic>
#include <string>
#include <vector>
#include <cstring>

namespace crossrender {
namespace {

Key KeyFromMacKeycode(unsigned short code) {
    switch (code) {
        case 0: return Key::A;
        case 1: return Key::S;
        case 2: return Key::D;
        case 3: return Key::F;
        case 4: return Key::H;
        case 5: return Key::G;
        case 6: return Key::Z;
        case 7: return Key::X;
        case 8: return Key::C;
        case 9: return Key::V;
        case 11: return Key::B;
        case 12: return Key::Q;
        case 13: return Key::W;
        case 14: return Key::E;
        case 15: return Key::R;
        case 16: return Key::Y;
        case 17: return Key::T;
        case 18: return Key::Num1;
        case 19: return Key::Num2;
        case 20: return Key::Num3;
        case 21: return Key::Num4;
        case 22: return Key::Num6;
        case 23: return Key::Num5;
        case 24: return Key::Equal;
        case 25: return Key::Num9;
        case 26: return Key::Num7;
        case 27: return Key::Minus;
        case 28: return Key::Num8;
        case 29: return Key::Num0;
        case 30: return Key::RightBracket;
        case 31: return Key::O;
        case 32: return Key::U;
        case 33: return Key::LeftBracket;
        case 34: return Key::I;
        case 35: return Key::P;
        case 36: return Key::Enter;
        case 37: return Key::L;
        case 38: return Key::J;
        case 39: return Key::Apostrophe;
        case 40: return Key::K;
        case 41: return Key::Semicolon;
        case 42: return Key::Backslash;
        case 43: return Key::Comma;
        case 44: return Key::Slash;
        case 45: return Key::N;
        case 46: return Key::M;
        case 47: return Key::Period;
        case 48: return Key::Tab;
        case 49: return Key::Space;
        case 50: return Key::Grave;
        case 51: return Key::Backspace;
        case 53: return Key::Escape;
        case 54: return Key::RightShift;
        case 55: return Key::LeftShift;
        case 56: return Key::LeftControl;
        case 57: return Key::CapsLock;
        case 58: return Key::LeftAlt;
        case 59: return Key::LeftSuper;
        case 60: return Key::RightShift;
        case 61: return Key::RightAlt;
        case 62: return Key::RightControl;
        case 63: return Key::Menu;
        case 64: return Key::F17;
        case 65: return Key::KeypadDecimal;
        case 67: return Key::KeypadMultiply;
        case 69: return Key::KeypadAdd;
        case 71: return Key::KeypadEqual;
        case 75: return Key::KeypadDivide;
        case 76: return Key::KeypadEnter;
        case 78: return Key::KeypadSubtract;
        case 81: return Key::KeypadEqual;
        case 82: return Key::Keypad0;
        case 83: return Key::Keypad1;
        case 84: return Key::Keypad2;
        case 85: return Key::Keypad3;
        case 86: return Key::Keypad4;
        case 87: return Key::Keypad5;
        case 88: return Key::Keypad6;
        case 89: return Key::Keypad7;
        case 91: return Key::Keypad8;
        case 92: return Key::Keypad9;
        case 96: return Key::F5;
        case 97: return Key::F6;
        case 98: return Key::F7;
        case 99: return Key::F3;
        case 100: return Key::F8;
        case 101: return Key::F9;
        case 103: return Key::F11;
        case 105: return Key::F13;
        case 106: return Key::F16;
        case 107: return Key::F14;
        case 109: return Key::F10;
        case 111: return Key::F12;
        case 115: return Key::Home;
        case 116: return Key::PageUp;
        case 117: return Key::Delete;
        case 119: return Key::End;
        case 121: return Key::PageDown;
        case 122: return Key::F1;
        case 123: return Key::Left;
        case 124: return Key::Right;
        case 125: return Key::Down;
        case 126: return Key::Up;
        default: return Key::Unknown;
    }
}

void ResolveDlAddr(void** fn, const char* name) {
    if (*fn) return;
    *fn = dlsym(RTLD_DEFAULT, name);
    if (!*fn) {
        // macOS экспонирует GL через фреймворк OpenGL; RTLD_DEFAULT работает, когда
        // фреймворк загружен, что гарантирует линковка.
        static void* handle = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
        if (handle) *fn = dlsym(handle, name);
    }
}

void* MacGLGetProcAddress(const char* name) {
    void* p = nullptr;
    ResolveDlAddr(&p, name);
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// View: пересылает события AppKit в состояние Input движка.
// ---------------------------------------------------------------------------
}  // namespace crossrender

@interface EngView : NSView
@property(nonatomic, assign) crossrender::Window* engOwner;
@end

@implementation EngView {
    NSTrackingArea* _tracking;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

- (void)updateTrackingAreas {
    if (_tracking) [self removeTrackingArea:_tracking];
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                 NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
    _tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                             options:opts
                                               owner:self
                                            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (NSPoint)flipPoint:(NSEvent*)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    // Начало координат AppKit — снизу слева; движок использует верхний левый с y вниз.
    p.y = [self bounds].size.height - p.y;
    return p;
}

- (void)mouseDown:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Left, true, {(float)p.x, (float)p.y});
}
- (void)mouseUp:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Left, false, {(float)p.x, (float)p.y});
}
- (void)rightMouseDown:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Right, true, {(float)p.x, (float)p.y});
}
- (void)rightMouseUp:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Right, false, {(float)p.x, (float)p.y});
}
- (void)otherMouseDown:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Middle, true, {(float)p.x, (float)p.y});
}
- (void)otherMouseUp:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseButton(crossrender::MouseButton::Middle, false, {(float)p.x, (float)p.y});
}
- (void)mouseMoved:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    NSPoint p = [self flipPoint:e];
    w->GetInput().OnMouseMove({(float)p.x, (float)p.y});
}
- (void)mouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)rightMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)otherMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)scrollWheel:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    float sx = (float)[e scrollingDeltaX];
    float sy = (float)[e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) {
        sx *= 0.1f;
        sy *= 0.1f;
    }
    w->GetInput().OnScroll({sx, sy});
}
- (void)magnifyWithEvent:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    w->GetInput().OnScroll({0.0f, (float)[e magnification] * 10.0f});
}

- (void)keyDown:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    crossrender::Key k = crossrender::KeyFromMacKeycode([e keyCode]);
    bool repeat = [e isARepeat];
    w->GetInput().OnKey(k, crossrender::KeyAction::Press, repeat);
    if (repeat) w->GetInput().OnKey(k, crossrender::KeyAction::Repeat, true);
    // Текстовый ввод (пропускаем при зажатом модификаторе command).
    if (!([e modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
        NSString* chars = [e characters];
        if (chars) {
            for (NSUInteger i = 0; i < [chars length]; ++i) {
                unichar c = [chars characterAtIndex:i];
                if (c >= 0xF700 && c <= 0xF8FF) continue;  // функциональные клавиши
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < [chars length]) {
                    unichar lo = [chars characterAtIndex:++i];
                    w->GetInput().OnText(0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00));
                } else if (c >= 32) {
                    w->GetInput().OnText(c);
                }
            }
        }
    }
}
- (void)keyUp:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    w->GetInput().OnKey(crossrender::KeyFromMacKeycode([e keyCode]), crossrender::KeyAction::Release, false);
}
- (void)flagsChanged:(NSEvent*)e {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    crossrender::Key k = crossrender::KeyFromMacKeycode([e keyCode]);
    bool down = ([e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask) != 0;
    w->GetInput().OnKey(k, down ? crossrender::KeyAction::Press : crossrender::KeyAction::Release, false);
}

- (void)viewDidChangeBackingProperties {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    if (w->callbacks.onDpiChanged) w->callbacks.onDpiChanged(w->DpiScale());
    if (w->callbacks.onResize) w->callbacks.onResize(w->FramebufferWidth(), w->FramebufferHeight());
}

- (void)drawRect:(NSRect)dirty { (void)dirty; }
@end

namespace crossrender {

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    NSWindow* window = nil;
    EngView* view = nil;
    NSOpenGLContext* context = nil;
    NSOpenGLPixelFormat* pixelFormat = nil;
    CGLContextObj cglContext = nullptr;
    bool headless = false;
    bool shouldClose = false;
    bool resized = false;
    bool focused = true;
    bool minimized = false;
    WindowMode mode = WindowMode::Windowed;
    WindowDesc desc;
    int width = 0, height = 0;
    int fbWidth = 0, fbHeight = 0;
    f32 dpiScale = 1;
    std::string title;
    int cursorMode = 0;
    std::string dropPath;
    void* delegate = nullptr;
    std::vector<std::string> droppedFiles;
};

namespace {
Window::Impl* g_lastImpl = nullptr;

void EnsureApp() {
    static bool done = false;
    if (done) return;
    done = true;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        // Минимальное меню, чтобы работал Cmd+Q.
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menubar addItem:appItem];
        [NSApp setMainMenu:menubar];
        NSMenu* appMenu = [[NSMenu alloc] init];
        NSString* quitTitle = [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
        NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                         action:@selector(terminate:)
                                                  keyEquivalent:@"q"];
        [appMenu addItem:quitItem];
        [appItem setSubmenu:appMenu];
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
}  // namespace crossrender

@interface EngWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) crossrender::Window* owner;
@end

@implementation EngWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (self.owner) self.owner->RequestClose();
    return NO;
}
- (void)windowDidResize:(NSNotification*)note {
    (void)note;
    if (self.owner && self.owner->callbacks.onResize)
        self.owner->callbacks.onResize(self.owner->FramebufferWidth(),
                                       self.owner->FramebufferHeight());
}
- (void)windowDidBecomeKey:(NSNotification*)note {
    (void)note;
    if (self.owner && self.owner->callbacks.onFocus) self.owner->callbacks.onFocus(true);
}
- (void)windowDidResignKey:(NSNotification*)note {
    (void)note;
    if (self.owner && self.owner->callbacks.onFocus) self.owner->callbacks.onFocus(false);
}
- (void)windowDidMiniaturize:(NSNotification*)note {
    (void)note;
}
- (void)windowDidDeminiaturize:(NSNotification*)note {
    (void)note;
}
@end

namespace crossrender {

Window::Window() : impl_(new Impl()) { g_lastImpl = impl_.get(); }

Window::~Window() {
    Destroy();
    if (g_lastImpl == impl_.get()) g_lastImpl = nullptr;
    impl_.reset();
}

bool Window::Create(const WindowDesc& desc) {
    @autoreleasepool {
        impl_->desc = desc;
        impl_->width = desc.width;
        impl_->height = desc.height;
        impl_->title = desc.title;
        impl_->headless = false;

        EnsureApp();

        NSRect frame = NSMakeRect(0, 0, desc.width, desc.height);
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable;
        if (desc.resizable) style |= NSWindowStyleMaskResizable;
        impl_->window = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        if (!impl_->window) {
            ENG_LOGE("platform", "failed to create NSWindow");
            return false;
        }
        [impl_->window setTitle:[NSString stringWithUTF8String:desc.title.c_str()]];
        [impl_->window setReleasedWhenClosed:NO];
        [impl_->window center];
        [impl_->window setMinSize:NSMakeSize(desc.minWidth, desc.minHeight)];
        [impl_->window setAcceptsMouseMovedEvents:YES];
        [impl_->window setRestorable:NO];

        EngView* view = [[EngView alloc] initWithFrame:frame];
        view.engOwner = this;
#if defined(ENG_METAL)
        // Слой совместимости Metal: CAMetalLayer стоит за фреймбуфером по умолчанию,
        // а GL-загрузчик резолвится в реализации Metal (engine/metal).
        // NSOpenGLContext не создаётся.
        [view setWantsLayer:YES];
        CAMetalLayer* metalLayer = [CAMetalLayer layer];
        metalLayer.frame = [view bounds];
        [view setLayer:metalLayer];
        [impl_->window setContentView:view];
        impl_->view = view;
        NSRect metalBacking = [view convertRectToBacking:[view bounds]];
        crossrender::mtlgl::CreateContext();
        crossrender::mtlgl::AttachLayer((__bridge void*)metalLayer,
                                 (uint32_t)metalBacking.size.width,
                                 (uint32_t)metalBacking.size.height);
        ENG_LOGI("platform", "window surface: CAMetalLayer (GL-over-Metal)");
#else
        [view setWantsBestResolutionOpenGLSurface:desc.highDpi ? YES : NO];
        [impl_->window setContentView:view];
        impl_->view = view;

        // Пиксельный формат: OpenGL 3.3 core (максимальный профиль, который экспонирует macOS — 4.1).
        NSOpenGLPixelFormatAttribute attrs[32];
        int ai = 0;
        attrs[ai++] = NSOpenGLPFAOpenGLProfile;
        attrs[ai++] = NSOpenGLProfileVersion3_2Core;
        attrs[ai++] = NSOpenGLPFAColorSize;
        attrs[ai++] = 24;
        attrs[ai++] = NSOpenGLPFAAlphaSize;
        attrs[ai++] = 8;
        if (desc.depthBuffer) {
            attrs[ai++] = NSOpenGLPFADepthSize;
            attrs[ai++] = 24;
        }
        if (desc.stencilBuffer) {
            attrs[ai++] = NSOpenGLPFAStencilSize;
            attrs[ai++] = 8;
        }
        attrs[ai++] = NSOpenGLPFADoubleBuffer;
        attrs[ai++] = NSOpenGLPFAAccelerated;
        attrs[ai++] = NSOpenGLPFANoRecovery;
        if (desc.msaaSamples > 1) {
            attrs[ai++] = NSOpenGLPFAMultisample;
            attrs[ai++] = NSOpenGLPFASampleBuffers;
            attrs[ai++] = 1;
            attrs[ai++] = NSOpenGLPFASamples;
            attrs[ai++] = static_cast<NSOpenGLPixelFormatAttribute>(desc.msaaSamples);
        }
        attrs[ai++] = 0;

        impl_->pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!impl_->pixelFormat && desc.msaaSamples > 1) {
            // Повтор без MSAA (некоторые драйверы отвергают её с core profile).
            ENG_LOGW("platform", "MSAA %d unavailable, retrying without", desc.msaaSamples);
            ai = 0;
            attrs[ai++] = NSOpenGLPFAOpenGLProfile;
            attrs[ai++] = NSOpenGLProfileVersion3_2Core;
            attrs[ai++] = NSOpenGLPFAColorSize;
            attrs[ai++] = 24;
            attrs[ai++] = NSOpenGLPFAAlphaSize;
            attrs[ai++] = 8;
            attrs[ai++] = NSOpenGLPFADepthSize;
            attrs[ai++] = 24;
            attrs[ai++] = NSOpenGLPFAStencilSize;
            attrs[ai++] = 8;
            attrs[ai++] = NSOpenGLPFADoubleBuffer;
            attrs[ai++] = NSOpenGLPFAAccelerated;
            attrs[ai++] = 0;
            impl_->pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        }
        if (!impl_->pixelFormat) {
            ENG_LOGE("platform", "failed to create NSOpenGLPixelFormat");
            return false;
        }
        impl_->context = [[NSOpenGLContext alloc] initWithFormat:impl_->pixelFormat shareContext:nil];
        if (!impl_->context) {
            ENG_LOGE("platform", "failed to create NSOpenGLContext");
            return false;
        }
        SetVSync(desc.vsync);
        [impl_->context setView:view];
        [impl_->context makeCurrentContext];
        impl_->cglContext = (CGLContextObj)[impl_->context CGLContextObj];
#endif  // ENG_METAL

        EngWindowDelegate* del = [[EngWindowDelegate alloc] init];
        del.owner = this;
        impl_->delegate = (void*)CFBridgingRetain(del);
        [impl_->window setDelegate:del];

        if (desc.mode == WindowMode::Fullscreen) SetMode(WindowMode::Fullscreen);
        else if (desc.mode == WindowMode::Borderless)
            [impl_->window setStyleMask:NSWindowStyleMaskBorderless];

        [impl_->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [impl_->view updateTrackingAreas];

        NSRect backing = [impl_->view convertRectToBacking:[impl_->view bounds]];
        impl_->fbWidth = static_cast<int>(backing.size.width);
        impl_->fbHeight = static_cast<int>(backing.size.height);
        impl_->width = static_cast<int>([impl_->view bounds].size.width);
        impl_->height = static_cast<int>([impl_->view bounds].size.height);
        impl_->dpiScale = static_cast<f32>([impl_->window backingScaleFactor]);

        ENG_LOGI("platform", "window created %dx%d (fb %dx%d, dpi %.2f)", impl_->width,
                 impl_->height, impl_->fbWidth, impl_->fbHeight, impl_->dpiScale);
        return true;
    }
}

void Window::Destroy() {
    if (!impl_) return;
    @autoreleasepool {
        if (impl_->context) {
            [NSOpenGLContext clearCurrentContext];
            impl_->context = nil;
        }
        impl_->pixelFormat = nil;
        if (impl_->window) {
            [impl_->window setDelegate:nil];
            [impl_->window close];
            impl_->window = nil;
        }
        if (impl_->delegate) {
            CFBridgingRelease(impl_->delegate);
            impl_->delegate = nullptr;
        }
        impl_->view = nil;
        impl_->cglContext = nullptr;
    }
}

void Window::PollEvents() {
    @autoreleasepool {
        for (;;) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) break;
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];

        // После изменения размера — или перетаскивания на дисплей с другим масштабом —
        // GL drawable всё ещё имеет старый размер, тогда как движок раскладывает кадр
        // по новому фреймбуферу. Без -update кадр рисуется с новым вьюпортом в
        // устаревший drawable: всё оказывается обрезанным и смещённым,
        // а текст заметно срезан. Обновляем его раз в кадр,
        // до отрисовки чего-либо.
        if (impl_->context && impl_->view) {
            NSRect backing = [impl_->view convertRectToBacking:[impl_->view bounds]];
            const int w = static_cast<int>(backing.size.width);
            const int h = static_cast<int>(backing.size.height);
            const f32 dpi = static_cast<f32>([impl_->window backingScaleFactor]);
            if (w != impl_->fbWidth || h != impl_->fbHeight) {
                [impl_->context update];
                impl_->fbWidth = w;
                impl_->fbHeight = h;
            }
            // Backing scale может измениться без изменения размера (окно
            // перетащили на Retina или не-Retina дисплей).
            if (dpi != impl_->dpiScale) {
                impl_->dpiScale = dpi;
                if (callbacks.onDpiChanged) callbacks.onDpiChanged(dpi);
            }
        }
#if defined(ENG_METAL)
        else if (impl_->view) {
            NSRect backing = [impl_->view convertRectToBacking:[impl_->view bounds]];
            const int w = static_cast<int>(backing.size.width);
            const int h = static_cast<int>(backing.size.height);
            const f32 dpi = static_cast<f32>([impl_->window backingScaleFactor]);
            if (w != impl_->fbWidth || h != impl_->fbHeight) {
                impl_->fbWidth = w;
                impl_->fbHeight = h;
                crossrender::mtlgl::SurfaceResized((uint32_t)w, (uint32_t)h);
            }
            if (dpi != impl_->dpiScale) {
                impl_->dpiScale = dpi;
                if (callbacks.onDpiChanged) callbacks.onDpiChanged(dpi);
            }
        }
#endif  // ENG_METAL
    }
}

void Window::SwapBuffers() {
#if defined(ENG_METAL)
    if (!impl_->context) {
        crossrender::mtlgl::Present();
        return;
    }
#endif
    if (impl_->context) [impl_->context flushBuffer];
}

bool Window::ShouldClose() const { return impl_->shouldClose; }
void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
}

void Window::SetTitle(const std::string& title) {
    impl_->title = title;
    if (impl_->window)
        [impl_->window setTitle:[NSString stringWithUTF8String:title.c_str()]];
}

void Window::SetSize(int w, int h) {
    if (!impl_->window) return;
    @autoreleasepool {
        NSRect frame = [impl_->window frame];
        NSRect content = [impl_->window contentRectForFrameRect:frame];
        content.size = NSMakeSize(w, h);
        NSRect newFrame = [impl_->window frameRectForContentRect:content];
        [impl_->window setFrame:newFrame display:YES];
        impl_->width = w;
        impl_->height = h;
    }
}

void Window::SetMode(WindowMode mode) {
    impl_->mode = mode;
    if (!impl_->window) return;
    @autoreleasepool {
        if (mode == WindowMode::Fullscreen) {
            if (!([impl_->window styleMask] & NSWindowStyleMaskFullScreen))
                [impl_->window toggleFullScreen:nil];
        } else {
            if ([impl_->window styleMask] & NSWindowStyleMaskFullScreen)
                [impl_->window toggleFullScreen:nil];
            NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable;
            if (impl_->desc.resizable) style |= NSWindowStyleMaskResizable;
            if (mode == WindowMode::Borderless) style = NSWindowStyleMaskBorderless;
            [impl_->window setStyleMask:style];
        }
    }
}

void Window::SetVSync(bool enabled) {
    if (!impl_->context) return;
    GLint swapInterval = enabled ? 1 : 0;
    [impl_->context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
}

void Window::Minimize() {
    if (impl_->window) [impl_->window miniaturize:nil];
}
void Window::Maximize() {
    if (impl_->window) [impl_->window zoom:nil];
}
void Window::Restore() {
    if (impl_->window) [impl_->window deminiaturize:nil];
}
void Window::Show() {
    if (impl_->window) [impl_->window orderFront:nil];
}
void Window::Hide() {
    if (impl_->window) [impl_->window orderOut:nil];
}
void Window::Focus() {
    if (impl_->window) {
        [impl_->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void Window::MakeCurrent() {
    if (impl_->context) [impl_->context makeCurrentContext];
}

void Window::WaitEventsTimeout(f32 seconds) {
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event) [NSApp sendEvent:event];
    }
}

void Window::SetCursorVisible(bool visible) {
    if (visible)
        [NSCursor unhide];
    else
        [NSCursor hide];
}

void Window::SetCursorMode(int mode) {
    impl_->cursorMode = mode;
    if (!impl_->window) return;
    if (mode == 2) {
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
    } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        if (mode == 0) [NSCursor unhide];
    }
}

void Window::SetClipboardText(const std::string& text) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:text.c_str()] forType:NSPasteboardTypeString];
    }
}

std::string Window::GetClipboardText() const {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* s = [pb stringForType:NSPasteboardTypeString];
        return s ? std::string([s UTF8String]) : std::string();
    }
}

int Window::Width() const {
    if (!impl_->view) return impl_->width;
    return static_cast<int>([impl_->view bounds].size.width);
}
int Window::Height() const {
    if (!impl_->view) return impl_->height;
    return static_cast<int>([impl_->view bounds].size.height);
}
int Window::FramebufferWidth() const {
    if (!impl_->view) return impl_->fbWidth;
    NSRect backing = [impl_->view convertRectToBacking:[impl_->view bounds]];
    return static_cast<int>(backing.size.width);
}
int Window::FramebufferHeight() const {
    if (!impl_->view) return impl_->fbHeight;
    NSRect backing = [impl_->view convertRectToBacking:[impl_->view bounds]];
    return static_cast<int>(backing.size.height);
}
f32 Window::DpiScale() const {
    if (!impl_->window) return impl_->dpiScale;
    return static_cast<f32>([impl_->window backingScaleFactor]);
}
f32 Window::Aspect() const {
    int h = FramebufferHeight();
    return h > 0 ? static_cast<f32>(FramebufferWidth()) / static_cast<f32>(h) : 1.0f;
}
bool Window::IsFocused() const {
    return impl_->window ? [impl_->window isKeyWindow] : false;
}
bool Window::IsMinimized() const {
    return impl_->window ? [impl_->window isMiniaturized] : false;
}
bool Window::IsFullscreen() const {
    return impl_->window ? ([impl_->window styleMask] & NSWindowStyleMaskFullScreen) != 0 : false;
}
Vec2 Window::MousePosition() const {
    if (!impl_->view || !impl_->window) return {};
    @autoreleasepool {
        NSPoint p = [impl_->window mouseLocationOutsideOfEventStream];
        NSPoint local = [impl_->view convertPoint:p fromView:nil];
        local.y = [impl_->view bounds].size.height - local.y;
        return {static_cast<f32>(local.x), static_cast<f32>(local.y)};
    }
}
const std::string& Window::Title() const { return impl_->title; }
void* Window::NativeHandle() const { return (__bridge void*)impl_->window; }
void* Window::NativeDisplay() const { return nullptr; }
void* (*Window::GLGetProcAddress() const)(const char*) { return MacGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Сервисы платформы
// ---------------------------------------------------------------------------
bool PlatformInit() { return true; }

void PlatformShutdown() {}

std::string PlatformName() { return "macOS"; }

void* (*PlatformGLGetProcAddress())(const char*) { return MacGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Headless-контекст (используется тестами и примером --headless).
// ---------------------------------------------------------------------------
namespace {
CGLContextObj g_headlessContext = nullptr;
CGLPixelFormatObj g_headlessFormat = nullptr;
int g_headlessRefCount = 0;
}  // namespace

bool CreateHeadlessGLContext() {
    if (g_headlessContext) {
        ++g_headlessRefCount;
        CGLSetCurrentContext(g_headlessContext);
        return true;
    }
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAColorSize,     (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,     (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize,     (CGLPixelFormatAttribute)24,
        kCGLPFAStencilSize,   (CGLPixelFormatAttribute)8,
        kCGLPFAAccelerated,   (CGLPixelFormatAttribute)0,
    };
    GLint npix = 0;
    CGLError err = CGLChoosePixelFormat(attrs, &g_headlessFormat, &npix);
    if (err != kCGLNoError || !g_headlessFormat) {
        // Запас через программный рендерер (CI-машины без GPU).
        CGLPixelFormatAttribute soft[] = {
            kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
            kCGLPFAColorSize,     (CGLPixelFormatAttribute)24,
            kCGLPFADepthSize,     (CGLPixelFormatAttribute)24,
            (CGLPixelFormatAttribute)0,
        };
        err = CGLChoosePixelFormat(soft, &g_headlessFormat, &npix);
    }
    if (err != kCGLNoError || !g_headlessFormat) {
        ENG_LOGW("platform", "headless GL unavailable (CGLChoosePixelFormat: %d)", (int)err);
        return false;
    }
    err = CGLCreateContext(g_headlessFormat, nullptr, &g_headlessContext);
    if (err != kCGLNoError || !g_headlessContext) {
        ENG_LOGW("platform", "headless GL context creation failed (%d)", (int)err);
        CGLDestroyPixelFormat(g_headlessFormat);
        g_headlessFormat = nullptr;
        return false;
    }
    CGLSetCurrentContext(g_headlessContext);
    g_headlessRefCount = 1;
    ENG_LOGI("platform", "headless OpenGL 3.2 core context created");
    return true;
}

void DestroyHeadlessGLContext() {
    if (!g_headlessContext) return;
    if (--g_headlessRefCount > 0) return;
    CGLSetCurrentContext(nullptr);
    CGLDestroyContext(g_headlessContext);
    g_headlessContext = nullptr;
    if (g_headlessFormat) {
        CGLDestroyPixelFormat(g_headlessFormat);
        g_headlessFormat = nullptr;
    }
}

bool HasHeadlessGLContext() { return g_headlessContext != nullptr; }

void* HeadlessGLGetProcAddress(const char* name) { return MacGLGetProcAddress(name); }

}  // namespace crossrender

#else

// В сборках без macOS эта единица трансляции пуста; каждая платформа
// предоставляет свой файл в engine/src/platform/<os>/.
namespace crossrender {
bool CreateHeadlessGLContext() { return false; }
void DestroyHeadlessGLContext() {}
bool HasHeadlessGLContext() { return false; }
void* HeadlessGLGetProcAddress(const char* name) {
    (void)name;
    return nullptr;
}
}  // namespace crossrender

#endif
