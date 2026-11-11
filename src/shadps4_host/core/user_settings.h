// shadPS4's user manager, as far as Pad and the controller ask it: which user plays on which pad
// slot. The port has the one user of our user service (src/hle/system/user_service.cpp), as
// player 1 with upstream's first user colour.
#pragma once

#include "common/types.h"

struct User {
    s32 user_id = -1;
    u32 user_color = 1;
    int player_index = 0;  // 1-4
    bool logged_in = false;
};

class UserManager {
public:
    UserManager();

    // The user with this id or player index, or null.
    User* GetUserByID(s32 user_id);
    User* GetUserByPlayerIndex(s32 index);

    // Marks the user logged in. Our user service has it logged in from the start anyway.
    void LoginUser(User* u, s32 player_index);

private:
    User user;
};

UserManager& HostUserManager();

#define UserManagement (HostUserManager())
