// SPDX-FileCopyrightText: 2026 Froststrap
// SPDX-FileCopyrightText: 2026 Voidstrap
//
// SPDX-License-Identifier: MPL-2.0

#include "virtualdisplay.h"

#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define VD_NAME_CAPACITY 128
#define VD_MAX_RATES 16
#define VD_MAX_DISPLAYS 32
#define VD_MAX_SIZE 16384
#define VD_MAX_RATE 1000.0
#define VD_MAX_ATTEMPTS 30
#define VD_MAX_FORCES 3
#define VD_MAX_WAITS 100
#define VD_RETRY_NS (50 * NSEC_PER_MSEC)
#define VD_RECHECK_NS (300 * NSEC_PER_MSEC)
#define VD_DEFAULT_NAME "Voidstrap Virtual Display"

extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

typedef struct {
    CGDirectDisplayID display;
    int32_t x;
    int32_t y;
} vd_origin;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_width;
    uint32_t pixel_height;
    CGSize size_mm;
    bool hidpi;
} vd_geometry;

typedef struct {
    char name[VD_NAME_CAPACITY];
    uint32_t width;
    uint32_t height;
    double rates[VD_MAX_RATES];
    size_t rate_count;
} vd_settings;

typedef struct {
    int (*run)(void *);
    void *argument;
    int result;
} vd_call;

static vd_settings settings;
static bool configured;

static struct {
    id display;
    CGDirectDisplayID virtual_display;
    CGDirectDisplayID physical;
    CGDisplayModeRef physical_mode;
    vd_origin origins[VD_MAX_DISPLAYS];
    uint32_t origin_count;
    uint32_t attempts;
    uint32_t forces;
    uint32_t waits;
    uintptr_t generation;
    bool swapped;
    bool settled;
    bool ever_settled;
    bool retry_armed;
} session;

static volatile int refresh_queued;

static void vd_log(const char *message, long value)
{
    fprintf(stderr, "[virtualdisplay] %s %ld\n", message, value);
}

static SEL selector(const char *name)
{
    return sel_registerName(name);
}

static id class_named(const char *name)
{
    Class found = objc_getClass(name);
    if (!found)
        fprintf(stderr, "[virtualdisplay] macOS does not provide %s\n", name);
    return (id)found;
}

static id msg(id target, const char *name)
{
    return ((id (*)(id, SEL))objc_msgSend)(target, selector(name));
}

static void send_object(id target, const char *name, id value)
{
    ((void (*)(id, SEL, id))objc_msgSend)(target, selector(name), value);
}

static void send_u32(id target, const char *name, uint32_t value)
{
    ((void (*)(id, SEL, uint32_t))objc_msgSend)(target, selector(name), value);
}

static void send_size(id target, const char *name, CGSize value)
{
    ((void (*)(id, SEL, CGSize))objc_msgSend)(target, selector(name), value);
}

static void release(id object)
{
    if (object)
        ((void (*)(id, SEL))objc_msgSend)(object, selector("release"));
}

static void invoke(void *context)
{
    vd_call *call = context;
    call->result = call->run(call->argument);
}

static int on_main(int (*run)(void *), void *argument)
{
    vd_call call = { run, argument, 0 };
    if (pthread_main_np())
        invoke(&call);
    else
        dispatch_sync_f(dispatch_get_main_queue(), &call, invoke);
    return call.result;
}

static CGDisplayConfigRef begin_configuration(void)
{
    CGDisplayConfigRef config = NULL;
    CGError error = CGBeginDisplayConfiguration(&config);
    if (error != kCGErrorSuccess || !config) {
        vd_log("could not begin a display configuration, error", error);
        return NULL;
    }
    return config;
}

static bool finish_configuration(CGDisplayConfigRef config, CGError error, CGConfigureOption option, const char *action)
{
    if (error != kCGErrorSuccess) {
        CGCancelDisplayConfiguration(config);
        fprintf(stderr, "[virtualdisplay] %s was rejected, error %d\n", action, error);
        return false;
    }
    error = CGCompleteDisplayConfiguration(config, option);
    if (error != kCGErrorSuccess) {
        fprintf(stderr, "[virtualdisplay] %s could not be applied, error %d\n", action, error);
        return false;
    }
    return true;
}

static uint32_t active_displays(CGDirectDisplayID *displays)
{
    uint32_t count = 0;
    if (CGGetActiveDisplayList(VD_MAX_DISPLAYS, displays, &count) != kCGErrorSuccess)
        return 0;
    return count < VD_MAX_DISPLAYS ? count : VD_MAX_DISPLAYS;
}

static bool contains(const CGDirectDisplayID *displays, uint32_t count, CGDirectDisplayID display)
{
    for (uint32_t index = 0; index < count; index++)
        if (displays[index] == display)
            return true;
    return false;
}

static bool mirrors(CGDirectDisplayID master)
{
    return master && master != session.physical && CGDisplayMirrorsDisplay(session.physical) == master;
}

static bool unmirror(void)
{
    if (!CGDisplayIsInMirrorSet(session.physical))
        return true;
    CGDisplayConfigRef config = begin_configuration();
    if (!config)
        return false;
    return finish_configuration(config, CGConfigureDisplayMirrorOfDisplay(config, session.physical, kCGNullDirectDisplay), kCGConfigureForAppOnly, "Breaking the mirror");
}

static bool mirror_onto(CGDirectDisplayID master)
{
    if (mirrors(master))
        return true;
    if (!master || master == session.physical)
        return false;
    if (!session.physical_mode)
        session.physical_mode = CGDisplayCopyDisplayMode(session.physical);
    if (!unmirror())
        return false;
    CGDisplayConfigRef config = begin_configuration();
    if (!config)
        return false;
    if (!finish_configuration(config, CGConfigureDisplayMirrorOfDisplay(config, session.physical, master), kCGConfigureForAppOnly, "Mirroring"))
        return false;
    return mirrors(master);
}

static bool make_main(CGDirectDisplayID target)
{
    if (CGDisplayIsMain(target))
        return true;
    CGDirectDisplayID displays[VD_MAX_DISPLAYS];
    uint32_t count = active_displays(displays);
    if (!contains(displays, count, target))
        return false;
    CGRect target_bounds = CGDisplayBounds(target);
    int32_t dx = (int32_t)lround(target_bounds.origin.x);
    int32_t dy = (int32_t)lround(target_bounds.origin.y);
    if (dx == 0 && dy == 0)
        return false;

    vd_origin current[VD_MAX_DISPLAYS];
    for (uint32_t index = 0; index < count; index++) {
        CGRect bounds = CGDisplayBounds(displays[index]);
        current[index] = (vd_origin){ displays[index], (int32_t)lround(bounds.origin.x), (int32_t)lround(bounds.origin.y) };
    }

    CGDisplayConfigRef config = begin_configuration();
    if (!config)
        return false;
    CGError error = kCGErrorSuccess;
    for (uint32_t index = 0; index < count && error == kCGErrorSuccess; index++)
        error = CGConfigureDisplayOrigin(config, current[index].display, current[index].x - dx, current[index].y - dy);
    if (!finish_configuration(config, error, kCGConfigureForSession, "Moving the virtual display to the main position"))
        return false;

    if (!session.swapped) {
        memcpy(session.origins, current, sizeof(vd_origin) * count);
        session.origin_count = count;
        session.swapped = true;
    }
    return CGDisplayIsMain(target);
}

static void restore_origins(void)
{
    if (!session.swapped)
        return;
    session.swapped = false;
    CGDirectDisplayID displays[VD_MAX_DISPLAYS];
    uint32_t count = active_displays(displays);
    CGDisplayConfigRef config = begin_configuration();
    if (!config)
        return;
    CGError error = kCGErrorSuccess;
    bool physical_saved = false;
    for (uint32_t index = 0; index < session.origin_count && error == kCGErrorSuccess; index++) {
        vd_origin origin = session.origins[index];
        if (!contains(displays, count, origin.display))
            continue;
        physical_saved |= origin.display == session.physical;
        error = CGConfigureDisplayOrigin(config, origin.display, origin.x, origin.y);
    }
    if (error == kCGErrorSuccess && !physical_saved)
        error = CGConfigureDisplayOrigin(config, session.physical, 0, 0);
    session.origin_count = 0;
    finish_configuration(config, error, kCGConfigureForSession, "Restoring the display arrangement");
}

static void restore(void)
{
    bool mirrored = CGDisplayIsInMirrorSet(session.physical);
    if (mirrored || session.physical_mode) {
        CGDisplayConfigRef config = begin_configuration();
        if (config) {
            CGError error = kCGErrorSuccess;
            if (mirrored)
                error = CGConfigureDisplayMirrorOfDisplay(config, session.physical, kCGNullDirectDisplay);
            if (error == kCGErrorSuccess && session.physical_mode)
                error = CGConfigureDisplayWithDisplayMode(config, session.physical, session.physical_mode, NULL);
            finish_configuration(config, error, kCGConfigureForAppOnly, "Restoring the physical display");
        }
    }
    restore_origins();
    if (session.physical_mode) {
        CGDisplayModeRelease(session.physical_mode);
        session.physical_mode = NULL;
    }
}

static bool ensure(void)
{
    CGDirectDisplayID display = session.virtual_display;
    if (!session.display)
        return false;
    if (!CGDisplayIsActive(display)) {
        if (session.ever_settled)
            return false;
        if (session.waits++ % 20 == 0)
            vd_log("waiting for the virtual display to become active, check", session.waits);
        return session.waits < VD_MAX_WAITS;
    }
    session.waits = 0;

    bool mirrored = mirrors(display);
    if (mirrored && CGDisplayIsMain(display)) {
        if (!session.settled)
            vd_log("virtual display is main and mirrored, display", display);
        session.settled = true;
        session.ever_settled = true;
        session.attempts = 0;
        session.forces = 0;
        return false;
    }
    if (session.settled)
        vd_log("macOS changed the arrangement, applying it again for display", display);
    session.settled = false;

    if (session.attempts >= VD_MAX_ATTEMPTS)
        return false;
    if (++session.attempts == VD_MAX_ATTEMPTS)
        vd_log("giving up after attempts", VD_MAX_ATTEMPTS);

    if (!mirrored && !CGDisplayIsMain(display) && unmirror())
        make_main(display);

    if (!mirrored) {
        mirrored = mirror_onto(display);
        if (!mirrored) {
            restore_origins();
            return true;
        }
    }

    if (CGDisplayIsMain(display) || session.forces >= VD_MAX_FORCES)
        return true;
    vd_log("mirrored but not main, forcing the main role, try", ++session.forces);
    if (unmirror())
        make_main(display);
    if (!mirror_onto(display)) {
        restore_origins();
        mirror_onto(display);
    }
    return true;
}

static void retry(void *context)
{
    if ((uintptr_t)context != session.generation)
        return;
    session.retry_armed = false;
    if (ensure()) {
        session.retry_armed = true;
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, VD_RETRY_NS), dispatch_get_main_queue(), context, retry);
    }
}

static void arm_retry(void)
{
    if (session.retry_armed)
        return;
    session.retry_armed = true;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, VD_RETRY_NS), dispatch_get_main_queue(), (void *)session.generation, retry);
}

static void recheck(void *context)
{
    if ((uintptr_t)context != session.generation)
        return;
    session.retry_armed = false;
    if (ensure())
        arm_retry();
}

static void refresh(void *context)
{
    (void)context;
    __atomic_store_n(&refresh_queued, 0, __ATOMIC_RELEASE);
    if (session.retry_armed || !session.display)
        return;
    if (session.settled) {
        session.retry_armed = true;
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, VD_RECHECK_NS), dispatch_get_main_queue(), (void *)session.generation, recheck);
        return;
    }
    if (ensure())
        arm_retry();
}

static void reconfigured(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *context)
{
    (void)display;
    (void)context;
    if (flags & kCGDisplayBeginConfigurationFlag)
        return;
    if (__atomic_exchange_n(&refresh_queued, 1, __ATOMIC_ACQ_REL))
        return;
    dispatch_async_f(dispatch_get_main_queue(), NULL, refresh);
}

static void teardown(void)
{
    if (!session.display)
        return;
    CGDisplayRemoveReconfigurationCallback(reconfigured, NULL);
    id display = session.display;
    session.display = nil;
    session.generation++;
    session.retry_armed = false;
    session.settled = false;
    session.ever_settled = false;
    session.attempts = 0;
    session.forces = 0;
    session.waits = 0;
    restore();
    void *pool = objc_autoreleasePoolPush();
    release(display);
    objc_autoreleasePoolPop(pool);
    session.virtual_display = kCGNullDirectDisplay;
}

static void terminated(void)
{
    if (!session.display)
        return;
    vd_log("macOS removed the virtual display", session.virtual_display);
    teardown();
}

static bool measure(CGDirectDisplayID physical, vd_geometry *geometry)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(physical);
    if (!mode)
        return false;
    uint64_t mode_width = CGDisplayModeGetWidth(mode);
    uint64_t mode_height = CGDisplayModeGetHeight(mode);
    uint64_t mode_pixel_width = CGDisplayModeGetPixelWidth(mode);
    CGDisplayModeRelease(mode);

    bool native = settings.width == 0 && settings.height == 0;
    uint64_t scale = native && mode_width ? mode_pixel_width / mode_width : 1;
    if (scale == 0)
        scale = 1;
    uint64_t width = settings.width ? settings.width : mode_width;
    uint64_t height = settings.height ? settings.height : mode_height;
    if (!width || !height || width * scale > VD_MAX_SIZE || height * scale > VD_MAX_SIZE)
        return false;

    CGSize size_mm = CGDisplayScreenSize(physical);
    if (!(size_mm.width > 0 && size_mm.height > 0))
        size_mm = CGSizeMake(600, 340);

    *geometry = (vd_geometry){
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .pixel_width = (uint32_t)(width * scale),
        .pixel_height = (uint32_t)(height * scale),
        .size_mm = size_mm,
        .hidpi = native && scale > 1,
    };
    return true;
}

static id create_display(const vd_geometry *geometry)
{
    id descriptor_class = class_named("CGVirtualDisplayDescriptor");
    id display_class = class_named("CGVirtualDisplay");
    id mode_class = class_named("CGVirtualDisplayMode");
    id settings_class = class_named("CGVirtualDisplaySettings");
    id string_class = class_named("NSString");
    id array_class = class_named("NSArray");
    if (!descriptor_class || !display_class || !mode_class || !settings_class || !string_class || !array_class)
        return nil;

    id descriptor = msg(descriptor_class, "new");
    if (!descriptor)
        return nil;
    id name = ((id (*)(id, SEL, const char *))objc_msgSend)(string_class, selector("stringWithUTF8String:"), settings.name);
    send_object(descriptor, "setQueue:", (id)dispatch_get_main_queue());
    send_object(descriptor, "setName:", name);
    send_u32(descriptor, "setMaxPixelsWide:", geometry->pixel_width);
    send_u32(descriptor, "setMaxPixelsHigh:", geometry->pixel_height);
    send_size(descriptor, "setSizeInMillimeters:", geometry->size_mm);
    send_u32(descriptor, "setProductID:", 0x1234);
    send_u32(descriptor, "setVendorID:", 0x3456);
    send_u32(descriptor, "setSerialNum:", 0x0002);
    send_object(descriptor, "setTerminationHandler:", (id)^(id removed, id error) {
        (void)removed;
        (void)error;
        terminated();
    });

    id display = ((id (*)(id, SEL, id))objc_msgSend)(msg(display_class, "alloc"), selector("initWithDescriptor:"), descriptor);
    release(descriptor);
    if (!display)
        return nil;

    id modes[VD_MAX_RATES];
    size_t count = 0;
    for (; count < settings.rate_count; count++) {
        modes[count] = ((id (*)(id, SEL, uint32_t, uint32_t, double))objc_msgSend)(
            msg(mode_class, "alloc"), selector("initWithWidth:height:refreshRate:"), geometry->width, geometry->height, settings.rates[count]);
        if (!modes[count])
            break;
    }

    BOOL applied = NO;
    id display_settings = count == settings.rate_count ? msg(settings_class, "new") : nil;
    if (display_settings) {
        id list = ((id (*)(id, SEL, const id *, unsigned long))objc_msgSend)(array_class, selector("arrayWithObjects:count:"), modes, count);
        send_u32(display_settings, "setHiDPI:", geometry->hidpi);
        send_object(display_settings, "setModes:", list);
        applied = ((BOOL (*)(id, SEL, id))objc_msgSend)(display, selector("applySettings:"), display_settings);
        release(display_settings);
    }
    for (size_t index = 0; index < count; index++)
        release(modes[index]);
    if (!applied) {
        release(display);
        return nil;
    }
    return display;
}

static int start_on_main(void *argument)
{
    (void)argument;
    if (session.display)
        return 0;
    if (!configured)
        return -1;
    CGDirectDisplayID physical = CGDisplayPrimaryDisplay(CGMainDisplayID());
    vd_geometry geometry;
    if (!physical || !measure(physical, &geometry))
        return -1;

    session.physical = physical;
    session.generation++;
    CGDisplayRegisterReconfigurationCallback(reconfigured, NULL);
    void *pool = objc_autoreleasePoolPush();
    session.display = create_display(&geometry);
    objc_autoreleasePoolPop(pool);
    if (!session.display) {
        CGDisplayRemoveReconfigurationCallback(reconfigured, NULL);
        return -1;
    }
    session.virtual_display = ((CGDirectDisplayID (*)(id, SEL))objc_msgSend)(session.display, selector("displayID"));
    if (!session.virtual_display) {
        teardown();
        return -1;
    }
    if (ensure())
        arm_retry();
    return 0;
}

static int running_on_main(void *argument)
{
    (void)argument;
    return session.display ? 1 : 0;
}

static int stop_on_main(void *argument)
{
    (void)argument;
    teardown();
    return 0;
}

static bool valid_rate(double rate)
{
    if (!isfinite(rate) || rate <= 0 || rate > VD_MAX_RATE)
        return false;
    double hundredths = rate * 100;
    return fabs(hundredths - round(hundredths)) <= 1e-6;
}

static int configure_on_main(void *argument)
{
    settings = *(const vd_settings *)argument;
    configured = true;
    return 0;
}

int vd_configure(const char *name, uint32_t width, uint32_t height, const double *rates, size_t count)
{
    vd_settings requested = { .width = width, .height = height, .rate_count = count };
    const char *chosen = name && *name ? name : VD_DEFAULT_NAME;
    size_t length = strlen(chosen);
    if (length >= VD_NAME_CAPACITY || width > VD_MAX_SIZE || height > VD_MAX_SIZE)
        return -1;
    if (!rates || count == 0 || count > VD_MAX_RATES)
        return -1;
    for (size_t index = 0; index < count; index++) {
        if (!valid_rate(rates[index]))
            return -1;
        requested.rates[index] = rates[index];
    }
    memcpy(requested.name, chosen, length + 1);
    return on_main(configure_on_main, &requested);
}

int vd_start(void)
{
    return on_main(start_on_main, NULL);
}

int vd_running(void)
{
    return on_main(running_on_main, NULL);
}

void vd_stop(void)
{
    on_main(stop_on_main, NULL);
}
