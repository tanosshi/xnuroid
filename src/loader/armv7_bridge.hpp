// Bridge between the host Objective-C runtime and 32-bit guest code.
//
// The armv7 loader owns the interpreter, so it registers the hooks below while it runs. The host side (app.hpp's
// send()) routes a message to the interpreter when the resolved IMP lives in guest code, and app_uikit registers the
// guest's classes so the host can look them up by name. Everything is a plain function pointer so the header stays
// usable from both the Android build and the device-free host runner.
#pragma once

#include <cstdint>

namespace armv7bridge {

// True when an IMP address belongs to 32-bit guest code (and therefore must not be called natively).
inline bool (*is_imp)(uint64_t imp) = nullptr;
// Calls a guest method: args are the message arguments after (self, sel), packed as 64-bit slots.
inline uint64_t (*send)(uint64_t imp, uint64_t self, uint64_t sel, const uint64_t* args, int nargs) = nullptr;
// A guest message for a host object: args are the values the guest put in r2/r3.
inline uint64_t (*host_send)(uint64_t receiver, uint64_t selector, const uint64_t* args, int nargs) = nullptr;
// A struct-returning guest message for a host object: the result is written into the guest buffer at `buffer`.
inline void (*host_send_stret)(uint64_t receiver, uint64_t selector, uint64_t buffer) = nullptr;
// A guest message whose single argument is a 32-bit-floats CGRect spread over r2, r3 and two stack words.
inline uint64_t (*host_send_rect)(uint64_t receiver, uint64_t selector, uint64_t w0, uint64_t w1, uint64_t w2,
                                  uint64_t w3) = nullptr;
// Declares a host-allocated range the guest may read and write (host objects are kept below 4 GB for this reason).
inline void (*add_region)(uint64_t lo, uint64_t hi) = nullptr;
// Registers a guest class (and its method names/IMPs) with the host runtime. `guest_class` is the image address of
// the class_t, which guest code compares against its own metadata and reads back as `self.isa`.
inline void (*register_class)(const char* name, const char* super_name, uint32_t instance_size, uint32_t guest_class,
                              const char* const* selectors, const uint64_t* imps, int count) = nullptr;
// Resolves a host class by name (for `_OBJC_CLASS_$_…` import stubs used as class objects).
inline uint64_t (*class_object)(const char* name) = nullptr;

}  // namespace armv7bridge
