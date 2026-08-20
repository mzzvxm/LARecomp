#pragma once

#include <rex/ppc/context.h>

void InitPauseMenuHooks();

void Hook_CapturePMContinue(PPCRegister& r3);
bool Hook_EnablePMSave(PPCRegister& r3, PPCRegister& r4);
bool Hook_EnablePMTeste(PPCRegister& r3, PPCRegister& r4);
bool Hook_PMLodTrafficClick(PPCRegister& r3, PPCRegister& r31);
bool Hook_PopulateRedirect(PPCRegister& r3);
bool MCLA_MenuListNullTableGuard(PPCRegister& r30);
bool Hook_RexGlueCancel(PPCRegister& r31);
bool Hook_FlashCommandLog(PPCRegister& r5);
bool Hook_ListViewPopulate(PPCRegister& r3);

// Options > Controller key dispatch (sub_8264D0D8). Handles accept/customise
// on the extra "PLAYSTATION BUTTONS" row that Hook_ListViewPopulate appends to
// that screen, so the game never applies a control preset for it.
bool Hook_ControllerMenuKey(PPCRegister& r3, PPCRegister& r4);
bool Hook_MenuKeyDispatch(PPCRegister& r31, PPCRegister& r28, PPCRegister& r27);
void Hook_CarbonGarageProbe(PPCRegister& r3);
void Hook_CarbonGarageSelect(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                             PPCRegister& r6);
void Hook_CarbonUiTick(PPCRegister& r3);

// Called from the SCXML state-activate hook (0x8268DDD8) with the name of the
// state being entered. Selecting a garage menu item activates that item's
// state, which is the only click signal the garage screen gives us.
void CarbonOnStateActivate(const char* name, uint32_t state);
