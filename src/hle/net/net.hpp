// The network and PSN libraries, reported as offline (docs/design/m3-core-os.md, "Network and
// PSN"). The init calls on the start-up path succeed and hand out small distinct ids, the state
// queries report no connection and no signed-in user, and nothing touches the host network. Every
// other import of these libraries stays a stub.
//
// Addresses in the comments are the eboot call sites whose checks fixed each result.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bb::hle::net {

// libSceNet

// Always 0. 0xfd1ffa and 0x29f35ff accept any non-negative result, 0xdc7992 only 0 (or one
// error it ignores). Several call sites initialise independently (0x45748d, 0x9fd228, 0xfd1ffa,
// 0xfd2063), so a second call succeeds as well.
std::int32_t sceNetInit();

// A new pool id, from 1. 0xfd200f, 0x2b97fd6 and 0x2b9850d store it when it is non-negative.
std::int32_t sceNetPoolCreate(const char* name, std::int32_t size, std::int32_t flags);

// Blocks for `timeout_usec` microseconds, forever if it is negative, then returns 0 (no event):
// no socket ever becomes ready. The network thread polls this in a loop (docs/m3-run.md), so
// returning at once kept a core busy. `events` and `max_events` are not touched.
std::int32_t sceNetEpollWait(std::int32_t epoll, void* events, std::int32_t max_events,
                             std::int32_t timeout_usec);

// libSceSsl

// A new SSL context id, from 1. 0xfd201e stores it when it is non-negative.
std::int32_t sceSslInit(std::size_t pool_size);

// libSceHttp

// A new HTTP context id, from 1. 0xfd2032 stores it when it is non-negative.
std::int32_t sceHttpInit(std::int32_t net_pool, std::int32_t ssl_context, std::size_t pool_size);

// A new template id, from 1. 0xfd1d32 does not check it. The destructor at 0xfd1dfa treats only
// ids above 0 as valid, which is why ids start at 1.
std::int32_t sceHttpCreateTemplate(std::int32_t http_context, const char* user_agent,
                                   std::int32_t http_version, std::int32_t auto_proxy);

// Writes a new 64-bit handle, from 1, to `*handle` and returns 0. 0xfd1d82 passes the address of
// an 8-byte field; 0xfd1dec and 0xfd2844 read it back as a qword.
std::int32_t sceHttpCreateEpoll(std::int32_t http_context, std::uint64_t* handle);

// NID s2-NPIvz+iA, sceHttpSetNonblock. 0xfd1d41 calls it as (template id, 1) right after creating
// the template and ignores the result. Returns 0.
std::int32_t http_template_set_int(std::int32_t http_template, std::int32_t value);

// NID EY28T2bkN7k, sceHttpAddRequestHeader. 0xfd1ed5 calls it as (template id, string, string, 0)
// and reports success when the result is 0. Returns 0.
std::int32_t http_template_set_string_pair(std::int32_t http_template, const char* first,
                                           const char* second, std::int32_t mode);

// libSceNetCtl

// Writes 0 to `*state` and returns 0. 0xcb839f takes state 3 as connected, 0x1c413f3 maps the
// states 0 to 3 through a table; 0 is the lowest. 0x9fd2d6 polls while the call succeeds and the
// state is 0, at most ten times.
std::int32_t sceNetCtlGetState(std::int32_t* state);

// Returns net_ctl_offline and leaves `*info` alone. 0x9fd2b9 and 0x9fd445 ask for code 14 and
// print the result as a string ("IP ADDR: %s") only when the call returns a non-negative value; a
// stub's 0 would print an uninitialised stack buffer.
std::int32_t sceNetCtlGetInfo(std::int32_t code, void* info);

// Our own negative result for sceNetCtlGetInfo. The call sites check only the sign.
inline constexpr std::int32_t net_ctl_offline = static_cast<std::int32_t>(0x80000001u);

// libSceNpManager

// Writes 1 to `*state` and returns 0. 0x1c4129a takes state 2 as signed in and 0x1c4129f
// handles 1 as the other known state; 0xcb83d4 stores the state when the call returns 0.
std::int32_t sceNpGetState(std::int32_t user_id, std::int32_t* state);

// NID A2CQ3kgSopQ, sceNpSetContentRestriction. 0x1fb704f passes a 0x18-byte block and ignores the
// result. The caller fills it at 0x1fb6fc6..0x1fb7047: its own size (0x18) as a qword at +0, a byte
// from a table at +8, a 32-bit value at +0xc and a qword (a table entry or 0) at +0x10. Returns 0.
std::int32_t np_manager_sized_request(const void* block);

}  // namespace bb::hle::net
