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

#include "PvZ/Lawn/Widget/GameButton.h"

NewLawnButton *MakeModeSelectBackButton(int theId, Sexy::ButtonListener *theListener, Sexy::Widget *theWidget) {
    auto *button = MakeNewButton(
        theId, theListener, theWidget, "[BACK_TO_MODE_SELECT]", nullptr, Sexy::IMAGE_SEEDCHOOSER_BUTTON_DISABLED, Sexy::IMAGE_SEEDCHOOSER_BUTTON_GLOW, Sexy::IMAGE_SEEDCHOOSER_BUTTON_GLOW);
    button->mTextOffsetX = -2;
    button->mTextOffsetY = -4;
    button->mTextDownOffsetX = 1;
    button->mTextDownOffsetY = 1;
    button->SetFont(Sexy::FONT_DWARVENTODCRAFT18);
    (*button->mColors)[Sexy::ButtonWidget::COLOR_LABEL] = Sexy::Color(0, 205, 0);
    button->Resize(800, 540, 160, 50);
    return button;
}
