#pragma once
//
// The game's own menu screens (mcForm), made from C++..
//
// Every list screen of the pause menu -- OPTIONS, CONTROLLER, GAME OPTIONS -- is
// an mcPerspectiveForm: a title, a description, the button prompts and a list
// of row objects, which the form renders into PAUSEMOVIE itself. One form opens
// another with vfunc+0x2C0 (sub_82630318), which puts it on top of mcMainForm's
// open-form stack, and the pad goes to the top of that stack. B closes it
// (vfunc+0x2E0, sub_82630580): it comes off the stack and the form it was
// opened from renders itself again, so title, tabs and list come back exactly
// as the game draws them -- because it is the game drawing them.
//
// Forms made here are that class, built by its constructor with the arguments
// and tune values OPTIONS has, and registered with mcMainForm like the 112 the
// game builds at boot. Rows are whatever row objects the caller hands over.
//
// Guest thread only: the UI update and the key handlers all run on it.

#include <cstdint>

namespace larecomp::ui {

// What a form made here does with the pad. Every callback is optional and runs
// on the guest thread, at the top of the form's key handler (sub_826537D0).
struct FormHandler {
    // A on row `row` (-1 = no row). True = handled; the select sound plays.
    bool (*accept)(uint32_t form, int row) = nullptr;
    // B, before the form closes. True = consumed: the form stays open and no
    // sound plays (the callback plays its own).
    bool (*cancel)(uint32_t form) = nullptr;
    // After B has closed the form.
    void (*closed)(uint32_t form) = nullptr;
    // Any other key, before the list sees it. True = consumed.
    bool (*key)(uint32_t form, uint32_t key) = nullptr;
};

// A list form titled by the string-table key `title_key` and described by
// `desc_key` (null = OPTIONS' own "What do you want to adjust?"), with room for
// `rows` rows. Both keys must resolve already: the constructor looks each one
// up once. 0 on failure.
uint32_t CreateListForm(const char* title_key, const char* desc_key, int rows,
                        const FormHandler& handler);

// Appends a row object (any of the game's row classes) to the form's list.
// False when the list is full.
bool AddFormRow(uint32_t form, uint32_t row);

uint32_t FormList(uint32_t form);    // its mcListViewFixed (form+672)
int FormSelectedRow(uint32_t form);  // the highlighted row, -1 for none
int FormScroll(uint32_t form);       // first row on screen
// Renders the form's list again in place, after a row changed (list
// vfunc+0xB8, sub_8262FD20: TransitionLevel 1). The form's own render would
// ask for TransitionLevel 0 -- the full transition, a fade out and in.
void RefreshFormList(uint32_t form);

// Opens `form` on top of `from` -- PauseForm() for a first level, another form
// for a nested one. No-op when it is open already.
void OpenForm(uint32_t form, uint32_t from);
// Closes it the way B does: off the stack, and the one under it renders again.
void CloseForm(uint32_t form);
bool FormIsOpen(uint32_t form);
uint32_t FormOpenedFrom(uint32_t form);  // 0 when closed or opened from nothing

uint32_t PauseForm();  // mcPauseForm (form 71), 0 before the UI exists
uint32_t TopForm();    // the form the pad goes to, 0 for none
bool IsOurForm(uint32_t form);
// The highest of the forms made here on the open-form stack, 0 if none is open.
// Closing them in this order closes a nested one before the one it came from.
uint32_t TopmostOurForm();

}  // namespace larecomp::ui
