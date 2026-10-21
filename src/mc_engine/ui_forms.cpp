// The game's own menu screens, made from C++. See ui_forms.h and.

#include "ui_forms.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>

#include <atomic>
#include <cstring>

#include "hooks/hooks_internal.h"
#include "logging.h"

REX_EXTERN(__imp__rex_sub_826537D0);  // mcSimpleListBaseForm key handler (vfunc+0x20)
REX_EXTERN(__imp__rex_sub_82654318);  // mcPerspectiveForm ctor
REX_EXTERN(__imp__rex_sub_82130528);  // operator new
REX_EXTERN(__imp__rex_sub_821E6800);  // play a UI sound by name

namespace larecomp::ui {
namespace {

// dword_8286D8D4 + 0x13F80 is mcMainForm (vtable 0x82097214), the owner of every
// form. Its children -- +68 array, +72 u16 count, +74 u16 capacity -- are the
// forms by id (sub_82641CB0(mainForm, id) = children[id]); its Update
// (sub_82643B50) steps every child and then renders whichever open ones are
// dirty. +212 / +216 are the first and last node of the open-form stack.
constexpr uint32_t kGamePtr        = 0x8286D8D4;
constexpr uint32_t kMainFormOffset = 0x13F80;
constexpr uint32_t kMainFormVtable = 0x82097214;
constexpr uint32_t kChildren    = 68;
constexpr uint32_t kChildCount  = 72;
constexpr uint32_t kChildCap    = 74;
constexpr uint32_t kOpenTail    = 216;
constexpr uint32_t kNodeBelow   = 8;
constexpr uint32_t kHiddenForm  = 78672;  // sub_82641CC8 never reports this one as on top
constexpr uint32_t kChildGrowth = 16;

constexpr uint32_t kPauseFormId       = 71;  // mcPauseForm, the pause screen
constexpr uint32_t kOptionsFormId     = 38;  // mcOptionsForm, SETTINGS > OPTIONS
constexpr uint32_t kPauseFormVtable   = 0x820A2FEC;
constexpr uint32_t kOptionsFormVtable = 0x82098874;

// sizeof(mcPerspectiveForm): mcMainForm embeds a plain one at +178000 and the
// next form at +178992, and mcOptionsForm's own fields start at +992.
constexpr uint32_t kFormSize = 992;

// What sub_8264D4F8 (mcOptionsForm's ctor) hands mcPerspectiveForm's: the
// description "options_desc" ("What do you want to adjust?"), one column, and
// its prompt strip -- prompt_edit + prompt_back, a static table the game fills
// at boot.
constexpr uint32_t kOptionsDescKey     = 0x82098BB0;
constexpr uint32_t kOptionsHeaderCap   = 1;
constexpr uint32_t kOptionsPrompts     = 0x829071FC;
constexpr uint32_t kOptionsPromptCount = 2;

// vtable slots (mcPerspectiveForm, 0x8209E42C)
constexpr uint32_t kVfRender   = 0xB8;   // sub_826302B8
constexpr uint32_t kVfSetMovie = 0xC4;   // sub_82632F00, recurses into the children
constexpr uint32_t kVfOpen     = 0x2C0;  // sub_82630318(form, from)
constexpr uint32_t kVfClose    = 0x2E0;  // sub_82630580(form)

// mcForm fields
constexpr uint32_t kObjFlags        = 16;   // 0x20 = active (vfunc+0x3C, sub_8221CDD0)
constexpr uint32_t kFormMovie       = 56;   // the swfCONTEXT it renders into
constexpr uint32_t kFormTemplate    = 65;   // u8, tune "Template"
constexpr uint32_t kFormFrom        = 220;  // the form it was opened from
constexpr uint32_t kFormTopMost     = 244;  // u8, tune "TopMost"
constexpr uint32_t kFormOpen        = 246;  // u8, set by open, cleared by close
constexpr uint32_t kFormIcon        = 248;  // tune "Icon"
constexpr uint32_t kFormList        = 672;  // its mcListViewFixed
constexpr uint32_t kFormCameraIndex = 952;  // tune "CameraIndex"

// mcListViewFixed fields
constexpr uint32_t kListRows   = 176;  // row pointer array
constexpr uint32_t kListCount  = 180;  // u16
constexpr uint32_t kListCap    = 182;  // u16
constexpr uint32_t kListSelect = 184;
constexpr uint32_t kListMax    = 188;  // rows on screen, tune "ItemHeight"
constexpr uint32_t kListScroll  = 256;  // first row on screen...
constexpr uint32_t kListScrolls = 260;  // ...when this is set

// The sound the original plays after a handled A: a one-entry char* table.
constexpr uint32_t kSoundSelect = 0x8282600C;  // "UI_MENU_SELECT"

constexpr uint32_t kKeyAccept = 55;
constexpr uint32_t kKeyCancel = 56;

struct Entry {
    uint32_t form = 0;
    FormHandler handler;
};
constexpr int kMaxForms = 32;
Entry g_forms[kMaxForms];  // guest thread only
int g_form_count = 0;

uint8_t* Base() { return rex::Runtime::instance()->virtual_membase(); }

uint32_t R32(uint32_t ea) { return ReadGuestU32(Base(), ea); }
void W32(uint32_t ea, uint32_t v) { WriteGuestU32(Base(), ea, v); }
uint16_t R16(uint32_t ea) {
    const uint8_t* b = Base();
    return uint16_t((uint32_t(b[ea]) << 8) | b[ea + 1]);
}
void W16(uint32_t ea, uint16_t v) {
    uint8_t* b = Base();
    b[ea] = uint8_t(v >> 8);
    b[ea + 1] = uint8_t(v);
}

bool Plausible(uint32_t ea) { return ea >= 0x10000u && ea < 0xC0000000u; }

// A guest function by address, for the vtable slots.
template <typename... Args>
uint32_t CallAt(uint32_t fn_addr, Args... args) {
    auto* rt = rex::Runtime::instance();
    auto* dispatcher = rt ? rt->function_dispatcher() : nullptr;
    PPCFunc* fn = dispatcher ? dispatcher->GetFunction(fn_addr) : nullptr;
    if (!fn) {
        MC_WARN("[ui-forms] guest fn 0x{:08X} not in the dispatcher", fn_addr);
        return 0;
    }
    return rex::ppc::GuestToHostFunction<uint32_t>(fn, uint32_t(args)...);
}

template <typename... Args>
uint32_t VCall(uint32_t obj, uint32_t slot, Args... args) {
    const uint32_t vtable = Plausible(obj) ? R32(obj) : 0;
    const uint32_t fn = Plausible(vtable) ? R32(vtable + slot) : 0;
    return fn ? CallAt(fn, obj, args...) : 0;
}

uint32_t NewGuest(uint32_t size) {
    return rex::ppc::GuestToHostFunction<uint32_t>(__imp__rex_sub_82130528, size);
}

uint32_t NewGuestString(const char* s) {
    const size_t n = std::strlen(s) + 1;
    const uint32_t p = NewGuest(uint32_t(n));
    if (p) std::memcpy(Base() + p, s, n);
    return p;
}

uint32_t MainForm() {
    const uint32_t game = R32(kGamePtr);
    if (!Plausible(game)) return 0;
    const uint32_t main = game + kMainFormOffset;
    return R32(main) == kMainFormVtable ? main : 0;
}

uint32_t FormById(uint32_t id, uint32_t vtable) {
    const uint32_t main = MainForm();
    if (!main || id >= R16(main + kChildCount)) return 0;
    const uint32_t form = R32(R32(main + kChildren) + 4 * id);
    return Plausible(form) && R32(form) == vtable ? form : 0;
}

uint32_t OptionsForm() { return FormById(kOptionsFormId, kOptionsFormVtable); }

const Entry* Find(uint32_t form) {
    for (int i = 0; i < g_form_count; ++i)
        if (g_forms[i].form == form) return &g_forms[i];
    return nullptr;
}

bool IsActive(uint32_t form) { return (R32(form + kObjFlags) & 0x20) != 0; }

// The boot-time forms are mcMainForm's children, which is how they get their
// per-frame Update and the movie when it is set again. The array was sized
// for exactly the 112 the game builds, so it grows the first time.
bool RegisterWithMainForm(uint32_t form) {
    const uint32_t main = MainForm();
    if (!main) return false;
    uint32_t arr = R32(main + kChildren);
    const uint16_t count = R16(main + kChildCount);
    const uint16_t cap = R16(main + kChildCap);
    if (count >= cap) {
        const uint16_t grown = uint16_t(cap + kChildGrowth);
        const uint32_t larger = NewGuest(4u * grown);
        if (!larger) return false;
        std::memcpy(Base() + larger, Base() + arr, 4u * count);
        W32(main + kChildren, larger);
        W16(main + kChildCap, grown);
        MC_INFO("[ui-forms] mcMainForm's form array grown {} -> {} (0x{:08X} -> 0x{:08X})", cap,
                grown, arr, larger);
        // The old one is left where it is: nothing points into it but this field.
        arr = larger;
    }
    W32(arr + 4u * count, form);
    W16(main + kChildCount, uint16_t(count + 1));
    return true;
}

void PlaySound(uint32_t table) {
    if (const uint32_t name = R32(table))
        rex::ppc::GuestToHostFunction<void>(__imp__rex_sub_821E6800, name);
}

// Guest calls made from the key handler start a frame below the thread's own
// context's r1, which is only the live stack pointer while that context is the
// one running. Logged once if the handler is ever reached from a nested one.
void CheckContext(const PPCContext& ctx) {
    static bool told = false;
    if (told) return;
    auto* ts = rex::runtime::ThreadState::Get();
    if (ts && ts->context() != &ctx) {
        told = true;
        MC_WARN("[ui-forms] a form key handler ran on a nested guest context; guest calls "
                "made from it share the outer frame");
    }
}

}  // namespace

uint32_t CreateListForm(const char* title_key, const char* desc_key, int rows,
                        const FormHandler& handler) {
    uint8_t* base = Base();
    const uint32_t options = OptionsForm();
    if (!base || !options || rows <= 0 || g_form_count >= kMaxForms) {
        MC_WARN("[ui-forms] cannot create form '{}': options=0x{:08X} rows={} forms={}",
                title_key, options, rows, g_form_count);
        return 0;
    }

    // The form keeps both pointers (the title is also its tune name, +4; the
    // description is its mcLabel's), so the strings live as long as the form
    // does: always.
    const uint32_t name = NewGuestString(title_key);
    const uint32_t desc = desc_key ? NewGuestString(desc_key) : kOptionsDescKey;
    const uint32_t form = name && desc ? NewGuest(kFormSize) : 0;
    if (!form) return 0;
    std::memset(base + form, 0, kFormSize);
    rex::ppc::GuestToHostFunction<uint32_t>(__imp__rex_sub_82654318, form, name, desc,
                                            uint32_t(rows), kOptionsHeaderCap, kOptionsPrompts,
                                            kOptionsPromptCount);

    // What OPTIONS' tune file gave it at boot (vfunc+0xC parses Template, Icon,
    // TopMost, ItemHeight, CameraIndex); ours never loads one.
    base[form + kFormTemplate] = base[options + kFormTemplate];
    base[form + kFormTopMost] = base[options + kFormTopMost];
    W32(form + kFormIcon, R32(options + kFormIcon));
    W32(form + kFormCameraIndex, R32(options + kFormCameraIndex));
    W32(form + kFormList + kListMax, R32(options + kFormList + kListMax));

    const bool registered = RegisterWithMainForm(form);
    g_forms[g_form_count++] = Entry{form, handler};
    MC_INFO("[ui-forms] form '{}' created at 0x{:08X}: {} rows, template {}, icon {}, camera {}{}",
            title_key, form, rows, base[form + kFormTemplate], R32(form + kFormIcon),
            R32(form + kFormCameraIndex), registered ? "" : " (NOT registered with mcMainForm)");
    return form;
}

bool AddFormRow(uint32_t form, uint32_t row) {
    if (!Plausible(form) || !Plausible(row)) return false;
    const uint32_t list = form + kFormList;
    const uint32_t arr = R32(list + kListRows);
    const uint16_t count = R16(list + kListCount);
    if (!Plausible(arr) || count >= R16(list + kListCap)) return false;
    W32(arr + 4u * count, row);
    W16(list + kListCount, uint16_t(count + 1));
    return true;
}

uint32_t FormList(uint32_t form) { return Plausible(form) ? form + kFormList : 0; }

int FormSelectedRow(uint32_t form) {
    if (!Plausible(form)) return -1;
    const int count = R16(form + kFormList + kListCount);
    const int sel = int(int32_t(R32(form + kFormList + kListSelect)));
    return sel >= 0 && sel < count ? sel : -1;
}

// mcListViewFixed's vfunc+0x294 (0x8228F4E8): +256 while +260 is set, else 0.
int FormScroll(uint32_t form) {
    if (!Plausible(form)) return 0;
    const uint32_t list = form + kFormList;
    return R32(list + kListScrolls) ? int(int32_t(R32(list + kListScroll))) : 0;
}

// What CONTROLLER's key handler does after it changes a row. mcListViewFixed's
// render (sub_826326D0 -> sub_82631FD0) ends with TransitionLevel 1, the
// in-place update; the form's (sub_82632F78) sets 0, which the AS plays as the
// whole menu fading out and back in.
void RefreshFormList(uint32_t form) {
    if (FormIsOpen(form)) VCall(form + kFormList, kVfRender);
}

void OpenForm(uint32_t form, uint32_t from) {
    if (!Plausible(form) || FormIsOpen(form)) return;
    // The movie OPTIONS renders into (PAUSEMOVIE), read at every open: a reload
    // of the movie gives it a new context.
    if (const uint32_t options = OptionsForm())
        if (const uint32_t movie = R32(options + kFormMovie)) VCall(form, kVfSetMovie, movie);
    VCall(form, kVfOpen, from);
    MC_INFO("[ui-forms] form 0x{:08X} opened from 0x{:08X} (top now 0x{:08X})", form, from,
            TopForm());
}

void CloseForm(uint32_t form) {
    if (!FormIsOpen(form)) return;
    VCall(form, kVfClose);
    MC_INFO("[ui-forms] form 0x{:08X} closed (top now 0x{:08X})", form, TopForm());
}

bool FormIsOpen(uint32_t form) { return Plausible(form) && Base()[form + kFormOpen] != 0; }

uint32_t FormOpenedFrom(uint32_t form) { return FormIsOpen(form) ? R32(form + kFormFrom) : 0; }

uint32_t PauseForm() { return FormById(kPauseFormId, kPauseFormVtable); }

// sub_82641CC8, read instead of called: the form of the open stack's last node,
// except mcMainForm+78672.
uint32_t TopForm() {
    const uint32_t main = MainForm();
    const uint32_t node = main ? R32(main + kOpenTail) : 0;
    const uint32_t form = Plausible(node) ? R32(node) : 0;
    return Plausible(form) && form != main + kHiddenForm ? form : 0;
}

bool IsOurForm(uint32_t form) { return form && Find(form) != nullptr; }

// The stack's nodes are form+176: +0 the form, +4 the node above, +8 the one
// below (sub_82633F38 links a new node in after the tail).
uint32_t TopmostOurForm() {
    const uint32_t main = MainForm();
    uint32_t node = main ? R32(main + kOpenTail) : 0;
    for (int guard = 0; Plausible(node) && guard < 128; ++guard, node = R32(node + kNodeBelow)) {
        const uint32_t form = R32(node);
        if (Find(form)) return form;
    }
    return 0;
}

}  // namespace larecomp::ui

// mcSimpleListBaseForm's key handler, shared by every list form: 55 runs the
// form's OnAccept (vfunc+0x300) and plays the select sound, 56 its OnCancel
// (vfunc+0x304, which closes it) and the back sound, anything else goes to the
// children -- the list, and through it the highlighted row. Forms made by
// CreateListForm put their handler in front of that; every other form takes the
// original untouched.
extern "C" REX_FUNC(rex_sub_826537D0) {
    using namespace larecomp::ui;
    const uint32_t form = ctx.r3.u32;
    const Entry* e = Find(form);
    // An inactive form takes no key, exactly as in the original.
    if (!e || !IsActive(form)) {
        __imp__rex_sub_826537D0(ctx, base);
        return;
    }
    CheckContext(ctx);
    const uint32_t key = ctx.r4.u32;
    const FormHandler& h = e->handler;

    if (key == kKeyAccept && h.accept) {
        if (h.accept(form, FormSelectedRow(form))) PlaySound(kSoundSelect);
        ctx.r3.u64 = 1;
        return;
    }
    if (key == kKeyCancel) {
        if (h.cancel && h.cancel(form)) {
            ctx.r3.u64 = 1;
            return;
        }
        __imp__rex_sub_826537D0(ctx, base);
        if (h.closed && !FormIsOpen(form)) {
            const uint64_t r3 = ctx.r3.u64;
            h.closed(form);
            ctx.r3.u64 = r3;
        }
        return;
    }
    if (h.key && h.key(form, key)) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__rex_sub_826537D0(ctx, base);
}

#else  // REXGLUE_HAS_XEO3_TARGET

namespace larecomp::ui {
uint32_t CreateListForm(const char*, const char*, int, const FormHandler&) { return 0; }
bool AddFormRow(uint32_t, uint32_t) { return false; }
uint32_t FormList(uint32_t) { return 0; }
int FormSelectedRow(uint32_t) { return -1; }
int FormScroll(uint32_t) { return 0; }
void RefreshFormList(uint32_t) {}
void OpenForm(uint32_t, uint32_t) {}
void CloseForm(uint32_t) {}
bool FormIsOpen(uint32_t) { return false; }
uint32_t FormOpenedFrom(uint32_t) { return 0; }
uint32_t PauseForm() { return 0; }
uint32_t TopForm() { return 0; }
bool IsOurForm(uint32_t) { return false; }
uint32_t TopmostOurForm() { return 0; }
}  // namespace larecomp::ui

#endif  // REXGLUE_HAS_XEO3_TARGET
