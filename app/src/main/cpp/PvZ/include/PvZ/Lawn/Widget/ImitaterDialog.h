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

#ifndef PVZ_LAWN_WIDGET_IMITATER_DIALOG_H
#define PVZ_LAWN_WIDGET_IMITATER_DIALOG_H

#include "PvZ/Lawn/Board/ToolTipWidget.h"
#include "PvZ/Lawn/Common/ConstEnums.h"

#include "HelpBarWidget.h"
#include "LawnDialog.h"

class ImitaterDialog : public LawnDialog {
public:
    // The game's constructor fills these in. They have to be declared even
    // though nothing here reads them: `new ImitaterDialog` sizes the object
    // from this class, and without them the constructor writes 20 bytes past
    // the end of it, over whatever the heap put next.
    ToolTipWidget *mToolTip;       // 191
    SeedType mToolTipSeed;         // 192
    int unk1;                      // 193
    int mPlayerIndex;              // 194
    HelpBarWidget *mHelpBarWidget; // 195

    ImitaterDialog(int thePlayerIndex) {
        _constructor(thePlayerIndex);
    }

    SeedType SeedHitTest(int x, int y) {
        return reinterpret_cast<SeedType (*)(ImitaterDialog *, int, int)>(ImitaterDialog_SeedHitTestAddr)(this, x, y);
    }

    void ShowToolTip();
    bool KeyDown(Sexy::KeyCode theKey);
    void MouseDown(int x, int y, int theCount);

protected:
    friend void InitHookFunction();

    void _constructor(int thePlayerIndex);
};

// What the game itself passes to operator new before calling the constructor.
#if PVZ_VERSION == 111
static_assert(sizeof(ImitaterDialog) == 0x318);
#else
static_assert(sizeof(ImitaterDialog) == 0x310);
#endif


inline void (*old_ImitaterDialog_ImitaterDialog)(ImitaterDialog *, int);

inline void (*old_ImitaterDialog_ShowToolTip)(ImitaterDialog *);

inline bool (*old_ImitaterDialog_KeyDown)(ImitaterDialog *, Sexy::KeyCode);

inline void (*old_ImitaterDialog_MouseDown)(ImitaterDialog *, int, int, int);

#endif // PVZ_LAWN_WIDGET_IMITATER_DIALOG_H
