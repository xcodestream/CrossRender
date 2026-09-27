// Платформенный слой iOS: нативное окно UIKit UIWindow + собственный UIView на
// базе CAEAGLLayer и EAGLContext (OpenGL ES 3.0). Без GLFW/SDL, без storyboards.
//
// Этот файл владеет:
//   * нативной точкой входа платформы: парой EngAppDelegate /
//     EngViewController, которую запускает UIApplicationMain(),
//   * прокруткой кадров CADisplayLink, движущей crossrender::AppHooks,
//   * всеми сервисами Platform.h на базе UIKit (чисто C++ живут в
//     PlatformIOS.cpp, который не может включать UIKit).
#include "crossrender/platform/Window.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Platform.h"

#include "IOSPlatform.h"

#if defined(ENG_PLATFORM_IOS)

// Движок намеренно использует (устаревший с iOS 12) API OpenGL ES.
#ifndef GLES_SILENCE_DEPRECATION
#define GLES_SILENCE_DEPRECATION 1
#endif

#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES3/gl.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <string>
#include <vector>
#include <cstring>

@class EngGLView;

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Состояние процесса. В приложении iOS ровно один UIWindow / одна GL-поверхность,
// поэтому все экземпляры Window делят этот view (задокументированное ограничение).
// ---------------------------------------------------------------------------
Window* g_activeWindow = nullptr;  // Window, которую показывает прокрутчик кадров
EngGLView* g_sharedView = nil;     // создаётся лениво, только главный поток
UIWindow* g_mainWindow = nil;      // ключевое окно приложения

AppHooks g_hooks;  // копия хуков, переданных в RunAppIOS()
bool g_hooksValid = false;
// Увеличивается Window::SwapBuffers(), чтобы прокрутчик кадров показывал кадр
// за приложение, только когда onFrame() сам не показал (onFrame, вызывающий
// Engine::Step(), уже делает swap в конце кадра).
u32 g_swapCount = 0;
bool g_runtimeStarted = false;
bool g_shutdownCalled = false;
bool g_quitRequested = false;

// ---------------------------------------------------------------------------
// USB HID usage -> crossrender::Key. UIKey.keyCode сообщает сырые id HID usage из
// страницы Keyboard/Keypad (0x07); числовые значения не привязывают таблицу
// к именованию констант UIKeyboardHIDUsage.
// ---------------------------------------------------------------------------
Key KeyFromHIDUsage(int usage) {
    // Буквы (0x04..0x1D) и цифры (0x1E..0x27).
    if (usage >= 0x04 && usage <= 0x1D)
        return static_cast<Key>(static_cast<int>(Key::A) + (usage - 0x04));
    if (usage >= 0x1E && usage <= 0x26)
        return static_cast<Key>(static_cast<int>(Key::Num1) + (usage - 0x1E));
    if (usage == 0x27) return Key::Num0;
    switch (usage) {
        case 0x28: return Key::Enter;
        case 0x29: return Key::Escape;
        case 0x2A: return Key::Backspace;
        case 0x2B: return Key::Tab;
        case 0x2C: return Key::Space;
        case 0x2D: return Key::Minus;
        case 0x2E: return Key::Equal;
        case 0x2F: return Key::LeftBracket;
        case 0x30: return Key::RightBracket;
        case 0x31: return Key::Backslash;
        case 0x33: return Key::Semicolon;
        case 0x34: return Key::Apostrophe;
        case 0x35: return Key::Grave;
        case 0x36: return Key::Comma;
        case 0x37: return Key::Period;
        case 0x38: return Key::Slash;
        case 0x39: return Key::CapsLock;
        case 0x3A: return Key::F1;
        case 0x3B: return Key::F2;
        case 0x3C: return Key::F3;
        case 0x3D: return Key::F4;
        case 0x3E: return Key::F5;
        case 0x3F: return Key::F6;
        case 0x40: return Key::F7;
        case 0x41: return Key::F8;
        case 0x42: return Key::F9;
        case 0x43: return Key::F10;
        case 0x44: return Key::F11;
        case 0x45: return Key::F12;
        case 0x49: return Key::Insert;
        case 0x4A: return Key::Home;
        case 0x4B: return Key::PageUp;
        case 0x4C: return Key::Delete;
        case 0x4D: return Key::End;
        case 0x4E: return Key::PageDown;
        case 0x4F: return Key::Right;
        case 0x50: return Key::Left;
        case 0x51: return Key::Down;
        case 0x52: return Key::Up;
        case 0x53: return Key::NumLock;
        case 0x54: return Key::KeypadDivide;
        case 0x55: return Key::KeypadMultiply;
        case 0x56: return Key::KeypadSubtract;
        case 0x57: return Key::KeypadAdd;
        case 0x58: return Key::KeypadEnter;
        case 0x63: return Key::KeypadDecimal;
        case 0x67: return Key::KeypadEqual;
        case 0xE0: return Key::LeftControl;
        case 0xE1: return Key::LeftShift;
        case 0xE2: return Key::LeftAlt;
        case 0xE3: return Key::LeftSuper;
        case 0xE4: return Key::RightControl;
        case 0xE5: return Key::RightShift;
        case 0xE6: return Key::RightAlt;
        case 0xE7: return Key::RightSuper;
        default: return Key::Unknown;
    }
}

const char* MessageBoxTypeLabel(int type) {
    switch (type) {
        case 1: return "warning";
        case 2: return "error";
        case 3: return "question";
        default: return "info";
    }
}

}  // namespace

// Объявлены здесь, чтобы код Objective-C ниже мог их достать; определены после
// классов Objective-C (им нужно UIKit-состояние, которым владеют эти классы).
void EngIOSFrameTick(f32 dt);
void EngIOSSetMainWindow(UIWindow* window);
void EngIOSStartRuntime();
void EngIOSSetForeground(bool foreground);
void EngIOSMemoryWarning();
void EngIOSShutdown();
EngGLView* EngIOSSharedView();
void EngIOSUpdateSafeArea(UIEdgeInsets insets);

}  // namespace crossrender

// ---------------------------------------------------------------------------
// EngGLView: UIView на базе CAEAGLLayer, владеющий контекстом EAGL,
// объектами фреймбуфера, вводом касаний/клавиатуры и display link.
// ---------------------------------------------------------------------------
@interface EngGLView : UIView <UIKeyInput>
@property(nonatomic, assign) crossrender::Window* engOwner;
@property(nonatomic, strong) EAGLContext* glContext;
@property(nonatomic, assign) BOOL wantsDepth;
@property(nonatomic, assign) BOOL wantsStencil;
@property(nonatomic, assign) GLuint framebuffer;
@property(nonatomic, assign) GLuint colorRenderbuffer;
@property(nonatomic, assign) GLuint depthRenderbuffer;
@property(nonatomic, strong) CADisplayLink* displayLink;
- (void)configureGL;
- (void)teardownGL;
- (void)ensureFramebuffer;
- (void)destroyStorage;
- (void)makeCurrent;
- (void)presentFrame;
- (void)startFramePump;
- (void)stopFramePump;
- (int)framebufferWidth;
- (int)framebufferHeight;
- (void)requestSoftKeyboard:(BOOL)visible;
@end

@implementation EngGLView {
    GLint _fbW;
    GLint _fbH;
    BOOL _glReady;
    BOOL _needsFramebuffer;
    // Идентичность касания -> стабильный touch id движка (Input::OnTouch нужен
    // стабильный id указателя на протяжении Down/Move/Up).
    const void* _touchSlots[crossrender::kMaxTouches];
    int _touchIds[crossrender::kMaxTouches];
    int _nextTouchId;
    crossrender::f64 _lastFrameTime;
    UIEdgeInsets _lastInsets;
}

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.multipleTouchEnabled = YES;
        self.opaque = YES;
        self.backgroundColor = [UIColor blackColor];
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        _fbW = 0;
        _fbH = 0;
        _glReady = NO;
        _needsFramebuffer = YES;
        _nextTouchId = 1;
        _lastFrameTime = 0.0;
        _lastInsets = UIEdgeInsetsZero;
        for (int i = 0; i < crossrender::kMaxTouches; ++i) {
            _touchSlots[i] = nullptr;
            _touchIds[i] = -1;
        }
        CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
        layer.opaque = YES;
        layer.drawableProperties = @{
            kEAGLDrawablePropertyRetainedBacking : @NO,
            kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8
        };
    }
    return self;
}

- (void)dealloc {
    [self stopFramePump];
    [self teardownGL];
}

// ---------------------------------------------------------------------------
// Настройка / разборка GL
// ---------------------------------------------------------------------------
- (void)configureGL {
    if (_glReady) return;
    // Только ES3: EAGL предлагает корзины ES1/ES2/ES3, поэтому WindowDesc::glMajor/glMinor
    // учитываются лишь при выборе API ES3.
    self.glContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    if (!self.glContext) {
        ENG_LOGE("platform", "failed to create EAGLContext (kEAGLRenderingAPIOpenGLES3)");
        return;
    }
    if (![EAGLContext setCurrentContext:self.glContext]) {
        ENG_LOGE("platform", "failed to make the EAGL context current");
        self.glContext = nil;
        return;
    }
    _glReady = YES;
    _needsFramebuffer = YES;
    [self ensureFramebuffer];
    ENG_LOGI("platform", "EAGLContext ready (OpenGL ES 3)");
}

- (void)teardownGL {
    [self destroyStorage];
    if ([EAGLContext currentContext] == self.glContext) {
        [EAGLContext setCurrentContext:nil];
    }
    self.glContext = nil;
    _glReady = NO;
}

- (void)createStorage {
    [EAGLContext setCurrentContext:self.glContext];
    CAEAGLLayer* layer = (CAEAGLLayer*)self.layer;
    const CGFloat scale = self.contentScaleFactor > 0.0 ? self.contentScaleFactor : 1.0;
    layer.contentsScale = scale;

    if (self.framebuffer == 0) glGenFramebuffers(1, &_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, self.framebuffer);

    if (self.colorRenderbuffer == 0) glGenRenderbuffers(1, &_colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, self.colorRenderbuffer);
    if (![self.glContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer]) {
        // У слоя пока нет drawable (view не в иерархии окна); повторим
        // из layoutSubviews()/didMoveToWindow().
        ENG_LOGD("platform", "no CAEAGLLayer drawable yet; deferring framebuffer setup");
        _needsFramebuffer = YES;
        return;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              self.colorRenderbuffer);

    GLint w = 0;
    GLint h = 0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
    _fbW = w;
    _fbH = h;

    if (self.wantsDepth || self.wantsStencil) {
        if (self.depthRenderbuffer == 0) glGenRenderbuffers(1, &_depthRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, self.depthRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER,
                              self.wantsStencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                  self.depthRenderbuffer);
        if (self.wantsStencil) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                      self.depthRenderbuffer);
        }
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ENG_LOGE("platform", "iOS framebuffer incomplete (0x%04x) %dx%d", (unsigned)status, (int)w,
                 (int)h);
        _needsFramebuffer = YES;
        return;
    }
    glViewport(0, 0, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, self.framebuffer);
    _needsFramebuffer = NO;
    ENG_LOGI("platform", "iOS drawable %dx%d (scale %.2f)", (int)w, (int)h, (double)scale);
}

- (void)destroyStorage {
    if (!self.glContext) return;
    [EAGLContext setCurrentContext:self.glContext];
    if (self.framebuffer) {
        glDeleteFramebuffers(1, &_framebuffer);
        self.framebuffer = 0;
    }
    if (self.colorRenderbuffer) {
        glDeleteRenderbuffers(1, &_colorRenderbuffer);
        self.colorRenderbuffer = 0;
    }
    if (self.depthRenderbuffer) {
        glDeleteRenderbuffers(1, &_depthRenderbuffer);
        self.depthRenderbuffer = 0;
    }
    _fbW = 0;
    _fbH = 0;
}

- (void)ensureFramebuffer {
    if (!_glReady) return;
    const CGSize bounds = self.bounds.size;
    if (bounds.width < 1.0 || bounds.height < 1.0) {
        _needsFramebuffer = YES;
        return;
    }
    // Пересоздаём при каждом изменении размера drawable (поворот, split view, ...).
    const CGFloat scale = self.contentScaleFactor > 0.0 ? self.contentScaleFactor : 1.0;
    const GLint wantW = (GLint)(bounds.width * scale);
    const GLint wantH = (GLint)(bounds.height * scale);
    if (!_needsFramebuffer && wantW == _fbW && wantH == _fbH) return;

    [self destroyStorage];
    [self createStorage];
    if (self.engOwner && self.engOwner->callbacks.onResize) {
        self.engOwner->callbacks.onResize((int)_fbW, (int)_fbH);
    }
}

- (void)makeCurrent {
    if (!self.glContext) return;
    [EAGLContext setCurrentContext:self.glContext];
    if (self.framebuffer) glBindFramebuffer(GL_FRAMEBUFFER, self.framebuffer);
}

- (void)presentFrame {
    if (!self.glContext || self.colorRenderbuffer == 0) return;
    [EAGLContext setCurrentContext:self.glContext];
    glBindRenderbuffer(GL_RENDERBUFFER, self.colorRenderbuffer);
    if (![self.glContext presentRenderbuffer:GL_RENDERBUFFER]) {
        ENG_LOGW("platform", "presentRenderbuffer failed");
    }
}

- (int)framebufferWidth { return _fbW > 0 ? (int)_fbW : 1; }
- (int)framebufferHeight { return _fbH > 0 ? (int)_fbH : 1; }

// ---------------------------------------------------------------------------
// Раскладка / safe area
// ---------------------------------------------------------------------------
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window) return;
    const CGFloat scale = self.window.screen ? self.window.screen.scale : [UIScreen mainScreen].scale;
    self.contentScaleFactor = scale;
    self.layer.contentsScale = scale;
    crossrender::EngIOSSetMainWindow(self.window);
    [self ensureFramebuffer];
    [self reportSafeArea];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self ensureFramebuffer];
    [self reportSafeArea];
}

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self reportSafeArea];
}

- (void)reportSafeArea {
    const UIEdgeInsets insets = self.safeAreaInsets;
    if (UIEdgeInsetsEqualToEdgeInsets(insets, _lastInsets)) return;
    _lastInsets = insets;
    crossrender::EngIOSUpdateSafeArea(insets);
}

// ---------------------------------------------------------------------------
// Прокрутка кадров
// ---------------------------------------------------------------------------
- (void)startFramePump {
    if (self.displayLink) return;
    _lastFrameTime = CACurrentMediaTime();
    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(drawFrame:)];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    ENG_LOGI("platform", "display link started");
}

- (void)stopFramePump {
    if (!self.displayLink) return;
    [self.displayLink invalidate];
    self.displayLink = nil;
}

- (void)drawFrame:(CADisplayLink*)link {
    (void)link;
    const crossrender::f64 now = CACurrentMediaTime();
    crossrender::f32 dt = (crossrender::f32)(now - _lastFrameTime);
    _lastFrameTime = now;
    if (!(dt > 0.0f) || dt > 0.5f) dt = 1.0f / 60.0f;
    crossrender::EngIOSFrameTick(dt);
}

// ---------------------------------------------------------------------------
// Ввод касаний
// ---------------------------------------------------------------------------
- (int)touchIdFor:(UITouch*)touch phase:(crossrender::TouchPhase)phase {
    const void* key = (__bridge const void*)touch;
    int slot = -1;
    for (int i = 0; i < crossrender::kMaxTouches; ++i) {
        if (_touchSlots[i] == key) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < crossrender::kMaxTouches; ++i) {
            if (_touchSlots[i] == nullptr) {
                _touchSlots[i] = key;
                _touchIds[i] = _nextTouchId++;
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return -1;  // больше kMaxTouches одновременных указателей
    const int id = _touchIds[slot];
    if (phase == crossrender::TouchPhase::Up || phase == crossrender::TouchPhase::Cancel) {
        _touchSlots[slot] = nullptr;
        _touchIds[slot] = -1;
    }
    return id;
}

- (void)dispatchTouches:(NSSet<UITouch*>*)touches phase:(crossrender::TouchPhase)phase {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    for (UITouch* t in touches) {
        const int id = [self touchIdFor:t phase:phase];
        if (id < 0) continue;
        // Points, а не пиксели: вьюпорт движка = framebuffer/dpiScale.
        const CGPoint p = [t locationInView:self];
        crossrender::TouchPoint tp;
        tp.id = id;
        tp.pos = crossrender::Vec2{(crossrender::f32)p.x, (crossrender::f32)p.y};
        tp.start = tp.pos;
        tp.phase = phase;
        tp.pressure = (crossrender::f32)(t.force > 0.0 ? t.force : 1.0);
        // Input::OnTouch уже кормит виртуальную мышь/левую кнопку, поэтому UI-код,
        // опрашивающий мышь, работает и с касаниями без двойного учёта дельт.
        w->GetInput().OnTouch(tp);
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self dispatchTouches:touches phase:crossrender::TouchPhase::Down];
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self dispatchTouches:touches phase:crossrender::TouchPhase::Move];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self dispatchTouches:touches phase:crossrender::TouchPhase::Up];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    (void)event;
    [self dispatchTouches:touches phase:crossrender::TouchPhase::Cancel];
}

// ---------------------------------------------------------------------------
// Аппаратная клавиатура (UIPress)
// ---------------------------------------------------------------------------
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    crossrender::Window* w = self.engOwner;
    if (!w) {
        [super pressesBegan:presses withEvent:event];
        return;
    }
    BOOL handled = NO;
    for (UIPress* p in presses) {
        UIKey* key = p.key;
        if (!key) continue;
        const crossrender::Key k = crossrender::KeyFromHIDUsage((int)key.keyCode);
        if (k != crossrender::Key::Unknown) {
            w->GetInput().OnKey(k, crossrender::KeyAction::Press, NO);
            handled = YES;
        }
        NSString* chars = key.characters;
        if (chars.length > 0) {
            for (NSUInteger i = 0; i < chars.length; ++i) {
                const unichar c = [chars characterAtIndex:i];
                if (c < 32) continue;
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars.length) {
                    const unichar lo = [chars characterAtIndex:++i];
                    w->GetInput().OnText(0x10000u + ((crossrender::u32)(c - 0xD800) << 10) + (crossrender::u32)(lo - 0xDC00));
                } else {
                    w->GetInput().OnText(c);
                }
            }
            handled = YES;
        }
    }
    if (!handled) [super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    crossrender::Window* w = self.engOwner;
    if (!w) {
        [super pressesEnded:presses withEvent:event];
        return;
    }
    for (UIPress* p in presses) {
        UIKey* key = p.key;
        if (!key) continue;
        const crossrender::Key k = crossrender::KeyFromHIDUsage((int)key.keyCode);
        if (k != crossrender::Key::Unknown) w->GetInput().OnKey(k, crossrender::KeyAction::Release, NO);
    }
}

- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self pressesEnded:presses withEvent:event];
}

// ---------------------------------------------------------------------------
// Экранная клавиатура (UIKeyInput)
// ---------------------------------------------------------------------------
- (BOOL)canBecomeFirstResponder { return YES; }

- (BOOL)becomeFirstResponder {
    const BOOL ok = [super becomeFirstResponder];
    if (ok) ENG_LOGI("platform", "soft keyboard shown");
    return ok;
}

- (BOOL)resignFirstResponder {
    const BOOL ok = [super resignFirstResponder];
    if (ok) ENG_LOGI("platform", "soft keyboard hidden");
    return ok;
}

- (BOOL)hasText { return NO; }

- (void)insertText:(NSString*)text {
    crossrender::Window* w = self.engOwner;
    if (!w || !text) return;
    for (NSUInteger i = 0; i < text.length; ++i) {
        const unichar c = [text characterAtIndex:i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.length) {
            const unichar lo = [text characterAtIndex:++i];
            w->GetInput().OnText(0x10000u + ((crossrender::u32)(c - 0xD800) << 10) + (crossrender::u32)(lo - 0xDC00));
        } else {
            w->GetInput().OnText(c);
        }
    }
}

- (void)deleteBackward {
    crossrender::Window* w = self.engOwner;
    if (!w) return;
    w->GetInput().OnKey(crossrender::Key::Backspace, crossrender::KeyAction::Press, NO);
    w->GetInput().OnKey(crossrender::Key::Backspace, crossrender::KeyAction::Release, NO);
}

- (UIKeyboardType)keyboardType { return UIKeyboardTypeDefault; }
- (UITextAutocorrectionType)autocorrectionType { return UITextAutocorrectionTypeNo; }
- (UITextAutocapitalizationType)autocapitalizationType { return UITextAutocapitalizationTypeNone; }
- (UITextSpellCheckingType)spellCheckingType { return UITextSpellCheckingTypeNo; }

- (void)requestSoftKeyboard:(BOOL)visible {
    if (visible) {
        if (![self isFirstResponder]) [self becomeFirstResponder];
    } else if ([self isFirstResponder]) {
        [self resignFirstResponder];
    }
}

@end

// ---------------------------------------------------------------------------
// Корневой контроллер вида: его view — общий EngGLView.
// ---------------------------------------------------------------------------
@interface EngViewController : UIViewController
@end

@implementation EngViewController
- (void)loadView {
    self.view = crossrender::EngIOSSharedView();
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskAll;
}
@end

// ---------------------------------------------------------------------------
// Делегат приложения: создаёт главный UIWindow, затем передаёт управление
// исполняемой среде движка.
// ---------------------------------------------------------------------------
@interface EngAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation EngAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    (void)application;
    (void)launchOptions;
    UIWindow* window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    EngViewController* vc = [[EngViewController alloc] init];
    window.rootViewController = vc;
    [window makeKeyAndVisible];
    self.window = window;
    crossrender::EngIOSSetMainWindow(window);
    crossrender::EngIOSStartRuntime();
    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
    (void)application;
    crossrender::EngIOSSetForeground(true);
}

- (void)applicationWillResignActive:(UIApplication*)application {
    (void)application;
    crossrender::EngIOSSetForeground(false);
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
    (void)application;
    // Drawable EAGL отбрасываются в фоне: сбрасываем фреймбуфер и позволяем
    // следующему проходу раскладки пересоздать его.
    if (crossrender::g_sharedView) [crossrender::g_sharedView destroyStorage];
}

- (void)applicationWillEnterForeground:(UIApplication*)application {
    (void)application;
    if (crossrender::g_sharedView) [crossrender::g_sharedView ensureFramebuffer];
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication*)application {
    (void)application;
    ENG_LOGW("platform", "received a memory warning");
    crossrender::EngIOSMemoryWarning();
}

- (void)applicationWillTerminate:(UIApplication*)application {
    (void)application;
    crossrender::EngIOSShutdown();
}
@end

// ---------------------------------------------------------------------------
// Исполняемая среда C++ + реализация Window
// ---------------------------------------------------------------------------
namespace crossrender {

EngGLView* EngIOSSharedView() {
    if (g_sharedView == nil) {
        g_sharedView = [[EngGLView alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    }
    return g_sharedView;
}

void EngIOSSetMainWindow(UIWindow* window) {
    if (window == nil) return;
    g_mainWindow = window;
    EngGLView* view = EngIOSSharedView();
    // Window::Create() могла выполниться до того, как UIApplicationMain() создал
    // окно; ставим общий view корневым, если он ещё не установлен.
    UIViewController* vc = window.rootViewController;
    if (vc && vc.isViewLoaded && vc.view != view) vc.view = view;
    [view ensureFramebuffer];
}

void EngIOSUpdateSafeArea(UIEdgeInsets insets) {
    eng_ios_set_safe_area((float)insets.left, (float)insets.top, (float)insets.right,
                          (float)insets.bottom);
}

void EngIOSStartRuntime() {
    if (g_runtimeStarted) return;
    g_runtimeStarted = true;
    EngGLView* view = EngIOSSharedView();
    if (g_activeWindow) {
        // Окно создано до подъёма run loop: завершаем настройку GL теперь,
        // когда у view есть настоящий drawable.
        view.engOwner = g_activeWindow;
        [view configureGL];
        [view ensureFramebuffer];
    }
    if (g_hooksValid && g_hooks.onInit) g_hooks.onInit(g_hooks.user);
    [view startFramePump];
}

void EngIOSSetForeground(bool foreground) {
    // У AppHooks нет хука фокуса, а у WindowCallbacks — хука потери контекста,
    // поэтому фокус сообщается через WindowCallbacks::onFocus и логируется
    // (см. отчёт по платформе).
    ENG_LOGI("platform", "app %s (no context-lost hook in WindowCallbacks: mapped to onFocus)",
             foreground ? "foreground" : "background");
    if (g_activeWindow && g_activeWindow->callbacks.onFocus) {
        g_activeWindow->callbacks.onFocus(foreground);
    }
    if (foreground && g_sharedView) [g_sharedView ensureFramebuffer];
}

void EngIOSMemoryWarning() {
    if (g_hooksValid && g_hooks.onLowMemory) g_hooks.onLowMemory(g_hooks.user);
}

void EngIOSShutdown() {
    if (g_shutdownCalled) return;
    g_shutdownCalled = true;
    if (g_sharedView) [g_sharedView stopFramePump];
    if (g_hooksValid && g_hooks.onShutdown) g_hooks.onShutdown(g_hooks.user);
}

void EngIOSFrameTick(f32 dt) {
    if (g_quitRequested) return;
    Window* w = g_activeWindow;
    if (w) w->PollEvents();
    if (g_hooksValid && g_hooks.onFrame) {
        const u32 swapsBefore = g_swapCount;
        if (!g_hooks.onFrame(g_hooks.user, dt)) {
            ENG_LOGI("platform", "onFrame requested exit");
            g_quitRequested = true;
            EngIOSShutdown();
            // UIApplicationMain() никогда не вернётся в RunAppIOS(), поэтому контракт
            // десктопного RunApp (вернуться после onShutdown) соблюдаем выходом
            // из процесса после запуска хука остановки.
            exit(0);
        }
        // Показываем кадр за приложение, если оно ещё не показало (onFrame,
        // вызывающий Engine::Step(), делает swap внутри).
        if (w && g_swapCount == swapsBefore) w->SwapBuffers();
    } else if (w) {
        w->SwapBuffers();
    }
}

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    EngGLView* view = nil;
    bool shouldClose = false;
    bool cursorVisible = true;
    int cursorMode = 0;
    int width = 0;
    int height = 0;
    f32 dpiScale = 1.0f;
    WindowMode mode = WindowMode::Windowed;
    WindowDesc desc;
    std::string title;
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
Window::Window() : impl_(new Impl()) { g_activeWindow = this; }

Window::~Window() {
    Destroy();
    if (g_activeWindow == this) g_activeWindow = nullptr;
    impl_.reset();
}

bool Window::Create(const WindowDesc& desc) {
    impl_->desc = desc;
    impl_->title = desc.title;
    impl_->width = desc.width;
    impl_->height = desc.height;
    impl_->mode = desc.mode;
    impl_->dpiScale = (f32)(g_mainWindow && g_mainWindow.screen ? g_mainWindow.screen.scale
                                                               : [UIScreen mainScreen].scale);

    // В iOS нет свободно живущих нативных окон: привязываемся к главному UIWindow
    // приложения (созданному EngAppDelegate либо позже через EngIOSSetMainWindow(),
    // когда Create() выполняется до UIApplicationMain()).
    EngGLView* view = EngIOSSharedView();
    impl_->view = view;
    g_activeWindow = this;
    view.engOwner = this;
    view.wantsDepth = desc.depthBuffer ? YES : NO;
    view.wantsStencil = desc.stencilBuffer ? YES : NO;
    if (desc.msaaSamples > 1) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("platform",
                     "MSAA (%d samples) is not implemented on iOS; the default framebuffer is "
                     "single-sampled",
                     desc.msaaSamples);
        }
    }

    [view configureGL];
    if (!view.glContext) {
        ENG_LOGE("platform", "window creation failed: no EAGL context");
        return false;
    }
    [view ensureFramebuffer];
    SetVSync(desc.vsync);

    if (g_mainWindow) {
        UIViewController* vc = g_mainWindow.rootViewController;
        if (vc) {
            if (!vc.isViewLoaded) [vc loadView];
            if (vc.view != view) vc.view = view;
        }
        [g_mainWindow makeKeyAndVisible];
    }

    impl_->dpiScale = (f32)view.contentScaleFactor;
    impl_->width = (int)(view.bounds.size.width > 0 ? view.bounds.size.width : desc.width);
    impl_->height = (int)(view.bounds.size.height > 0 ? view.bounds.size.height : desc.height);

    ENG_LOGI("platform", "window attached to the main UIWindow %dx%d (fb %dx%d, dpi %.2f)",
             impl_->width, impl_->height, [view framebufferWidth], [view framebufferHeight],
             impl_->dpiScale);
    return true;
}

void Window::Destroy() {
    if (!impl_) return;
    [impl_->view stopFramePump];
    [impl_->view teardownGL];
    impl_->view = nil;
    impl_->shouldClose = false;
}

void Window::PollEvents() {
    // UIKit доставляет события в главном run loop; дренировать очередь нечего.
    // Ввод приходит через колбэки касаний/нажатий EngGLView.
}

void Window::SwapBuffers() {
    if (!impl_->view) return;
    ++g_swapCount;
    [impl_->view presentFrame];
}

bool Window::ShouldClose() const { return impl_->shouldClose; }

void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
}

void Window::SetTitle(const std::string& title) {
    // У окон iOS нет строки заголовка; храним значение для Title().
    impl_->title = title;
}

void Window::SetSize(int w, int h) {
    // Окном приложения владеет UIKit, программно изменить его размер нельзя.
    impl_->width = w;
    impl_->height = h;
    ENG_LOGI("platform", "SetSize(%d, %d) ignored on iOS (window is owned by UIKit)", w, h);
}

void Window::SetMode(WindowMode mode) {
    // Окна iOS всегда полноэкранные; меняется только учёт.
    impl_->mode = mode;
    impl_->desc.mode = mode;
}

void Window::SetVSync(bool enabled) {
    // CADisplayLink по природе привязан к vsync; preferredFramesPerSecond = 0
    // означает «так быстро, как позволяет дисплей».
    impl_->desc.vsync = enabled;
    if (impl_->view && impl_->view.displayLink) {
        impl_->view.displayLink.preferredFramesPerSecond = enabled ? 60 : 0;
    }
}

void Window::Minimize() {}
void Window::Maximize() {}
void Window::Restore() {}

void Window::Show() {
    if (g_mainWindow) g_mainWindow.hidden = NO;
}
void Window::Hide() {
    if (g_mainWindow) g_mainWindow.hidden = YES;
}
void Window::Focus() {
    if (g_mainWindow) [g_mainWindow makeKeyAndVisible];
}

void Window::MakeCurrent() {
    if (impl_->view) [impl_->view makeCurrent];
}

void Window::WaitEventsTimeout(f32 seconds) {
    // Run loop принадлежит UIKit; сон сохраняет этот вызов без побочных эффектов.
    if (seconds > 0.0f) crossrender::SleepMs((u32)(seconds * 1000.0f));
}

void Window::SetCursorVisible(bool visible) { impl_->cursorVisible = visible; }
void Window::SetCursorMode(int mode) { impl_->cursorMode = mode; }

void Window::SetClipboardText(const std::string& text) {
    eng_ios_set_clipboard(text.c_str());
}

std::string Window::GetClipboardText() const {
    char buffer[4096];
    buffer[0] = '\0';
    eng_ios_get_clipboard(buffer, (int)sizeof(buffer));
    return std::string(buffer);
}

int Window::Width() const {
    if (impl_->view && impl_->view.bounds.size.width > 0) return (int)impl_->view.bounds.size.width;
    return impl_->width;
}

int Window::Height() const {
    if (impl_->view && impl_->view.bounds.size.height > 0)
        return (int)impl_->view.bounds.size.height;
    return impl_->height;
}

int Window::FramebufferWidth() const {
    if (impl_->view) return [impl_->view framebufferWidth];
    return impl_->width;
}

int Window::FramebufferHeight() const {
    if (impl_->view) return [impl_->view framebufferHeight];
    return impl_->height;
}

f32 Window::DpiScale() const {
    if (impl_->view && impl_->view.contentScaleFactor > 0) return (f32)impl_->view.contentScaleFactor;
    return impl_->dpiScale > 0 ? impl_->dpiScale : 1.0f;
}

f32 Window::Aspect() const {
    const int h = FramebufferHeight();
    return h > 0 ? (f32)FramebufferWidth() / (f32)h : 1.0f;
}

bool Window::IsFocused() const {
    if (g_mainWindow == nil) return false;
    return [UIApplication sharedApplication].applicationState == UIApplicationStateActive &&
           g_mainWindow.isKeyWindow;
}

bool Window::IsMinimized() const { return false; }
bool Window::IsFullscreen() const { return true; }  // приложение iOS всегда владеет экраном

Vec2 Window::MousePosition() const { return GetInput().MousePos(); }

const std::string& Window::Title() const { return impl_->title; }
void* Window::NativeHandle() const { return (__bridge void*)g_mainWindow; }
void* Window::NativeDisplay() const { return (__bridge void*)impl_->view; }
void* (*Window::GLGetProcAddress() const)(const char*) { return &IOSGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Сервисы платформы, которым нужен UIKit (чисто C++ половина живёт в PlatformIOS.cpp)
// ---------------------------------------------------------------------------
extern "C" void eng_ios_show_message_box(const char* title, const char* message, int type) {
    const std::string t = title ? title : "";
    const std::string m = message ? message : "";
    // UIAlertController не умеет блокировать, поэтому вызов асинхронный: он возвращается
    // до того, как пользователь закроет диалог (логируем, чтобы не вводить в заблуждение).
    ENG_LOGI("platform", "message box [%s] %s: %s", MessageBoxTypeLabel(type), t.c_str(), m.c_str());
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController* root = g_mainWindow ? g_mainWindow.rootViewController : nil;
        if (!root) {
            ENG_LOGW("platform", "no root view controller; message box not shown");
            return;
        }
        UIAlertController* alert =
            [UIAlertController alertControllerWithTitle:[NSString stringWithUTF8String:t.c_str()]
                                               message:[NSString stringWithUTF8String:m.c_str()]
                                        preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        UIViewController* presenter = root;
        while (presenter.presentedViewController) presenter = presenter.presentedViewController;
        [presenter presentViewController:alert animated:YES completion:nil];
    });
}

extern "C" bool eng_ios_open_url(const char* url) {
    if (!url || !*url) return false;
    NSString* s = [NSString stringWithUTF8String:url];
    NSURL* nsurl = [NSURL URLWithString:s];
    if (!nsurl) {
        ENG_LOGW("platform", "OpenUrl: invalid url '%s'", url);
        return false;
    }
    [[UIApplication sharedApplication] openURL:nsurl
                                       options:@{}
                             completionHandler:^(BOOL success) {
                                 if (!success) ENG_LOGW("platform", "OpenUrl failed: %s", url);
                             }];
    return true;
}

extern "C" void eng_ios_get_screen_size(int* outW, int* outH) {
    const CGRect bounds = [UIScreen mainScreen].bounds;
    if (outW) *outW = (int)bounds.size.width;
    if (outH) *outH = (int)bounds.size.height;
}

extern "C" void eng_ios_get_clipboard(char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    NSString* s = [UIPasteboard generalPasteboard].string;
    if (!s) return;
    const char* utf8 = [s UTF8String];
    if (!utf8) return;
    std::strncpy(out, utf8, (size_t)cap - 1);
    out[cap - 1] = '\0';
}

extern "C" void eng_ios_set_clipboard(const char* utf8) {
    if (!utf8) return;
    NSString* s = [NSString stringWithUTF8String:utf8];
    if (!s) return;
    [UIPasteboard generalPasteboard].string = s;
}

extern "C" void eng_ios_set_soft_keyboard(bool visible) {
    EngGLView* view = g_sharedView;
    if (!view) return;
    if ([NSThread isMainThread]) {
        [view requestSoftKeyboard:visible ? YES : NO];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [view requestSoftKeyboard:visible ? YES : NO];
        });
    }
}

extern "C" void eng_ios_set_keep_awake(bool enabled) {
    if ([NSThread isMainThread]) {
        [UIApplication sharedApplication].idleTimerDisabled = enabled ? YES : NO;
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [UIApplication sharedApplication].idleTimerDisabled = enabled ? YES : NO;
        });
    }
}

extern "C" bool eng_ios_is_app_foreground(void) {
    return [UIApplication sharedApplication].applicationState == UIApplicationStateActive;
}

// ---------------------------------------------------------------------------
// Точка входа жизненного цикла приложения (вызывается RunApp из PlatformIOS.cpp)
// ---------------------------------------------------------------------------
int RunAppIOS(const AppHooks& hooks) {
    g_hooks = hooks;
    g_hooksValid = true;
    g_quitRequested = false;
    g_shutdownCalled = false;

    // UIApplicationMain требует argc/argv; восстанавливаем их из NSProcessInfo,
    // потому что точка входа RunApp движка не получает аргументы процесса.
    std::vector<std::string> argStore;
    std::vector<char*> argv;
    for (NSString* a in [[NSProcessInfo processInfo] arguments]) {
        argStore.emplace_back(a ? [a UTF8String] : "");
    }
    if (argStore.empty()) argStore.emplace_back("gameengine");
    for (std::string& s : argStore) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    ENG_LOGI("platform", "starting UIApplicationMain with %d arguments", (int)argStore.size());
    UIApplicationMain((int)argStore.size(), argv.data(), nil, @"EngAppDelegate");
    // UIApplicationMain возвращается только после завершения приложения.
    EngIOSShutdown();
    return 0;
}

}  // namespace crossrender

#else

// В сборках без iOS эта единица трансляции пуста; каждая платформа
// предоставляет свой файл в engine/src/platform/<os>/.
namespace crossrender {}

#endif  // ENG_PLATFORM_IOS
