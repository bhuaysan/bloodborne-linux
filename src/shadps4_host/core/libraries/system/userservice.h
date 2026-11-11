// The piece of shadPS4's user service header that Pad, AudioOut, ImeDialog and the controller
// use: the user id type and its special values. libSceUserService itself is ours
// (src/hle/system/user_service.cpp), and its one user is the one core/user_settings.h names.
#pragma once

#include "common/types.h"

namespace Libraries::UserService {

using OrbisUserServiceUserId = s32;

// The system pseudo-user and "no user", as upstream. Ours returns no_user (-1) for "no user" too
// (src/hle/system/system.hpp).
constexpr int ORBIS_USER_SERVICE_USER_ID_SYSTEM = 0xFF;
constexpr int ORBIS_USER_SERVICE_USER_ID_INVALID = -1;

}  // namespace Libraries::UserService
