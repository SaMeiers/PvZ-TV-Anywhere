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

#include "PvZ/Lawn/Board/LawnMower.h"
#include "Homura/Logger.h"
#include "PvZ/GlobalVariable.h"
#include "PvZ/Lawn/LawnApp.h"
#include "PvZ/NetPlay.h"
#include "PvZ/TodLib/Effect/Reanimator.h"

void LawnMower::Update() {
    if (!requestPause || (IsOnlineServerModeActive() && !gIsReplayMode)) {
        old_LawnMower_Update(this);
    }
}

void LawnMower::SquishMower() {
    if (IsRemoteClientOrViewer())
        return;

    if (mApp->mGameScene == SCENE_PLAYING && IsRemoteServer()) {
        U16_Event event = {{EventType::EVENT_SERVER_BOARD_LAWNMOWER_SQUISH}, uint16_t(mRow)};
        netplay::PutEvent(event);
    }

    SquishMower_Origin();
}

void LawnMower::SquishMower_Origin() {
    Reanimation *aMowerReanim = mApp->ReanimationGet(mReanimID);
    aMowerReanim->OverrideScale(0.85f, 0.22f);
    aMowerReanim->SetPosition(-11.0f, 65.0f);
    mMowerState = LawnMowerState::MOWER_SQUISHED;
    mSquishedCounter = 500;
    mApp->PlayFoley(FoleyType::FOLEY_SQUISH);
}

void LawnMower::StartMower() {
    if (IsRemoteClientOrViewer())
        return;

    if (mApp->mGameScene == SCENE_PLAYING) {
        if (IsRemoteServer()) {
            U16_Event event = {{EventType::EVENT_SERVER_BOARD_LAWNMOWER_START}, uint16_t(mRow)};
            netplay::PutEvent(event);
            // 小推车战损
            netplay::MetricsRecordMowerLoss();
        }
    }

    old_LawnMower_StartMower(this);
}
