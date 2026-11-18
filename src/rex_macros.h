// Yoinked from https://github.com/twist84/halo3_cache_release_recomp/blob/06c7f0740d9bee47e2c0be047a5c7562fe9e1fa5/halo3/source/rex_macros.h

#include <rex/app/title_app.h>
#include <rex/ppc/call.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/ppc/hook.h>

#include <type_traits>
#include "larecomp_guest.h"

/* ---------- constants */

#define REX_PPC_EXTERN_IMPORT(function) \
	REX_EXTERN(__imp__rex_##function)

#define REX_PPC_INVOKE(function, ...) \
	larecomp::guest::Call<function_return_t<decltype(function)>>(__imp__rex_##function, __VA_ARGS__)

#define REX_PPC_INVOKE2(return_type, function, ...) \
	larecomp::guest::Call<return_type>(__imp__rex_##function, __VA_ARGS__)

#define REX_PPC_HOOK(function) \
    REX_HOOK(rex_##function, function##_Hook)

#define REX_DATA_REFERENCE_DECLARE(address, type, name) \
	type& name = *reinterpret_cast<type*>(0x100000000 + address)

#define REX_DATA_REFERENCE_DECLARE_ARRAY(address, type, name, count) \
	type(&name)[count] = *reinterpret_cast<type(*)[count]>(0x100000000 + address)

#define REX_PPC_CONTEXT_REF(name) \
	auto* current_thread_state = rex::kernel::ThreadState::Get(); \
	assert(current_thread_state != nullptr); \
	auto* context = current_thread_state->GetContext(); \
	assert(context != nullptr); \
	PPCContext& __restrict name = *context

#define REX_PPC_MEMBASE_PTR(name) \
auto* runtime = REX_RUNTIME(); \
auto* memory = larecomp::guest::AddressSpace(runtime); \
uint8_t* name = memory->GetVirtualMembase()


/* ---------- definitions */

template <typename t_type>
struct function_t;

template <typename t_return_type, typename... t_args>
struct function_t<t_return_type(t_args...)>
{
	using return_type = t_return_type;
};

template <typename t_return_type, typename... t_args>
struct function_t<t_return_type(*)(t_args...)>
{
	using return_type = t_return_type;
};

template <typename t_return_type, typename t_class, typename... t_args>
struct function_t<t_return_type(t_class::*)(t_args...)>
{
	using return_type = t_return_type;
};

template <typename t_return_type, typename t_class, typename... t_args>
struct function_t<t_return_type(t_class::*)(t_args...) const>
{
	using return_type = t_return_type;
};

template <typename t_function>
using function_return_t =
typename function_t<std::remove_cvref_t<t_function>>::return_type;

/* ---------- prototypes */

/* ---------- globals */

/* ---------- public code */

/* ---------- private code */