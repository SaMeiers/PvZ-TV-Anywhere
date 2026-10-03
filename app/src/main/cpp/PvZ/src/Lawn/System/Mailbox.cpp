/*
 * Copyright (C) 2023-2026  PvZ TV Touch Team
 *
 * This file is part of PlantsVsZombies-AndroidTV.
 *
 * PlantsVsZombies-AndroidTV is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * PlantsVsZombies-AndroidTV is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * PlantsVsZombies-AndroidTV.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "PvZ/Lawn/System/Mailbox.h"
#include "Homura/Logger.h"
#include "PvZ/Lawn/LawnApp.h"
#include "PvZ/Lawn/System/PlayerInfo.h"

#include <cstring>

int Mailbox::GetNumUnseenMessages() {
    if (!mApp || !mApp->mPlayerInfo) {
        return 0;
    }

    int unseenCount = 0;
    [[maybe_unused]] int visibleMessageCount = 0;
    [[maybe_unused]] int seenCount = 0;
    [[maybe_unused]] int hiddenByLevelCount = 0;
    [[maybe_unused]] int invalidCount = 0;

    LawnPlayerInfo *playerInfo = mApp->mPlayerInfo;
    int levelGate = playerInfo->mLevel;
    if (playerInfo->GetFlag(1)) {
        levelGate = 50;
    }

    const auto *seenBitsBase = reinterpret_cast<const unsigned char *>(playerInfo) + 1875;

    // 主菜单每帧都会调用本函数，逐条读取 256 封邮件的开销很大；结果只取决于存档、关卡、已读标记和邮件列表，
    // 这些不变时直接返回上次的结果。游戏内部也可能刷新邮件列表，所以每 64 次调用仍重新计算一次。
    // The main menu asks every frame, and walking all 256 messages costs far
    // more than it is worth; the answer only depends on the profile, its level,
    // the seen bits and the message list, so reuse it while those stay the
    // same. The game can refresh the list on its own, so still recount every
    // 64 calls.
    static unsigned sCallsSinceCount = 0;
    static const Mailbox *sCachedMailbox = nullptr;
    static const LawnPlayerInfo *sCachedPlayer = nullptr;
    static int sCachedLevelGate = -1;
    static unsigned char sCachedSeenBits[32];
    static int sCachedUnseen = 0;
    if (++sCallsSinceCount < 64 && sCachedMailbox == this && sCachedPlayer == playerInfo && sCachedLevelGate == levelGate && memcmp(sCachedSeenBits, seenBitsBase, sizeof(sCachedSeenBits)) == 0) {
        return sCachedUnseen;
    }

    for (int block = 0; block < 32; ++block) {
        for (int i = 0; i < 8; ++i) {
            const int messageIndex = i + block * 8;
            const unsigned char mask = static_cast<unsigned char>(1u << i);
            const bool seen = (seenBitsBase[block] & mask) != 0;

            int *message = reinterpret_cast<int *>(GetMessageByIndex(messageIndex, false));
            const int requiredLevel = *reinterpret_cast<int *>(reinterpret_cast<char *>(message) + 64);
            const bool valid = *reinterpret_cast<unsigned char *>(reinterpret_cast<char *>(message) + 70) != 0;

            if (!valid) {
                ++invalidCount;
            } else if (levelGate < requiredLevel) {
                ++hiddenByLevelCount;
            } else if (seen) {
                ++seenCount;
            } else {
                ++unseenCount;
            }

            if (valid && levelGate >= requiredLevel) {
                ++visibleMessageCount;
            }
        }
    }

    sCallsSinceCount = 0;
    sCachedMailbox = this;
    sCachedPlayer = playerInfo;
    sCachedLevelGate = levelGate;
    memcpy(sCachedSeenBits, seenBitsBase, sizeof(sCachedSeenBits));
    sCachedUnseen = unseenCount;
    return unseenCount;
}
