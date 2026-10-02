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

#include "PvZ/Lawn/Widget/GameOverDialog.h"
#include "PvZ/Lawn/LawnApp.h"
#include "PvZ/NetPlay.h"

void GameOverDialog::ButtonDepress(int theId) {
    if (theId == 1001) {
        if (gIsServerModeSpectator || gIsReplayMode) {
            mApp->PlaySample(Sexy::SOUND_BUZZER);
            return;
        }
        if (IsRemoteClient()) {
            BaseEvent event = {EventType::EVENT_CLIENT_BOARD_GAMEOVER_EXIT};
            netplay::PutEvent(event);
            return;
        }
        if (IsRemoteServer()) {
            mApp->RequestGameOverExit();
            return;
        }
    }
    if (theId == 1000) {
        if (IsRemoteClientOrViewer()) {
            mApp->PlaySample(Sexy::SOUND_BUZZER);
            return;
        }
        if (IsRemoteServer()) {
            LawnApp *aApp = mApp;
            const GameMode aGameMode = aApp->mGameMode;
            U16_Event event = {{EventType::EVENT_SERVER_BOARD_RETRY}, uint16_t(aGameMode)};
            netplay::PutEvent(event);
            aApp->RetryOnlineGame(aGameMode);
            return;
        }
    }
    ButtonDepress_Origin(theId);
}

void GameOverDialog::ButtonDepress_Origin(int theId) {
    LawnApp *aApp = mApp;
    if (theId == 1001) {
        aApp->ExitGameOver();
    } else if (theId == 1000) {
        aApp->PostLeaveLevel();
        aApp->PostEnterLevel();
        aApp->KillDialog(Dialogs::DIALOG_GAME_OVER);
        aApp->EndLevel();
    }
}
