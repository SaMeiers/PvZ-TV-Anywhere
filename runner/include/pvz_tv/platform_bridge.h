#pragma once

/* The few things the guest asks of the host that only a platform can answer.
 *
 * The game talks to its Java side by queuing a Runnable and waiting for the UI
 * thread to run it. There is no Java here, so the runner runs those itself --
 * but some of them are requests that need a real answer: "show the keyboard"
 * is the one that matters, because the game blocks until text comes back.
 * Each platform implements this its own way; leaving it unimplemented hangs
 * the game the moment it asks for the keyboard. */

#include <cstdint>

namespace pvz_tv {

struct GuestCall;

/* Called on the guest thread that just queued work, before the work runs.
 * `native_base` is where libnative_code.so is mapped, so the work's vtable
 * can be recognised by offset. */
void inspect_pending_works(GuestCall &c, std::uint32_t native_app, std::uint32_t native_base);

}  // namespace pvz_tv
