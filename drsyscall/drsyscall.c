/* **********************************************************
 * Copyright (c) 2010-2012 Google, Inc.  All rights reserved.
 * Copyright (c) 2007-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Dr. Syscall top-level code */

#include "dr_api.h"
#include "drmgr.h"
#include "drsyscall.h"
#include "drsyscall_os.h"
#include "drmemory_framework.h"
#include "../framework/drmf.h"
#include "utils.h"
#include <string.h>

#ifdef SYSCALL_DRIVER
# include "syscall_driver.h"
#endif

/* Keep this in synch with drsys_param_type_t */
const char * const param_type_names[] = {
    "<invalid>",                /* DRSYS_TYPE_INVALID */
    "<unknown>",                /* DRSYS_TYPE_UNKNOWN */
    "void",                     /* DRSYS_TYPE_VOID */
    "bool",                     /* DRSYS_TYPE_BOOL */
    "int",                      /* DRSYS_TYPE_INT */
    "int",                      /* DRSYS_TYPE_SIGNED_INT */
    "unsigned int",             /* DRSYS_TYPE_UNSIGNED_INT */
    "HANDLE",                   /* DRSYS_TYPE_HANDLE */
    "NTSTATUS",                 /* DRSYS_TYPE_NTSTATUS */
    "ATOM",                     /* DRSYS_TYPE_ATOM */
    "void *",                   /* DRSYS_TYPE_POINTER */
    "<struct>",                 /* DRSYS_TYPE_STRUCT */
    "char *",                   /* DRSYS_TYPE_CSTRING */
    "wchar_t *",                /* DRSYS_TYPE_CWSTRING */
    "char[]",                   /* DRSYS_TYPE_CARRAY */
    "wchar_t[]",                /* DRSYS_TYPE_CWARRAY */
    "char **",                  /* DRSYS_TYPE_CSTRARRAY */
    "UNICODE_STRING",           /* DRSYS_TYPE_UNICODE_STRING */
    "LARGE_STRING",             /* DRSYS_TYPE_LARGE_STRING */
    "OBJECT_ATTRIBUTES",        /* DRSYS_TYPE_OBJECT_ATTRIBUTES */
    "SECURITY_DESCRIPTOR",      /* DRSYS_TYPE_SECURITY_DESCRIPTOR */
    "SECURITY_QOS",             /* DRSYS_TYPE_SECURITY_QOS */
    "PORT_MESSAGE",             /* DRSYS_TYPE_PORT_MESSAGE */
    "CONTEXT",                  /* DRSYS_TYPE_CONTEXT */
    "EXCEPTION_RECORD",         /* DRSYS_TYPE_EXCEPTION_RECORD */
    "DEVMODEW",                 /* DRSYS_TYPE_DEVMODEW */
    "WNDCLASSEXW",              /* DRSYS_TYPE_WNDCLASSEXW */
    "CLSMENUNAME",              /* DRSYS_TYPE_CLSMENUNAME */
    "MENUITEMINFOW",            /* DRSYS_TYPE_MENUITEMINFOW */
    "ALPC_PORT_ATTRIBUTES",     /* DRSYS_TYPE_ALPC_PORT_ATTRIBUTES */
    "ALPC_SECURITY_ATTRIBUTES", /* DRSYS_TYPE_ALPC_SECURITY_ATTRIBUTES */
    "LOGFONTW",                 /* DRSYS_TYPE_LOGFONTW */
    "NONCLIENTMETRICSW",        /* DRSYS_TYPE_NONCLIENTMETRICSW */
    "ICONMETRICSW",             /* DRSYS_TYPE_ICONMETRICSW */
    "SERIALKEYSW",              /* DRSYS_TYPE_SERIALKEYSW */
    "struct sockaddr",          /* DRSYS_TYPE_SOCKADDR */
    "struct msghdr",            /* DRSYS_TYPE_MSGHDR */
    "struct msgbuf",            /* DRSYS_TYPE_MSGBUF */
};
#define NUM_PARAM_TYPE_NAMES \
    (sizeof(param_type_names)/sizeof(param_type_names[0]))

int cls_idx_drsys = -1;

drsys_options_t drsys_ops;

static int drsys_init_count;

void *systable_lock;

static drsys_param_type_t
map_to_exported_type(uint sysarg_type, size_t *sz_out OUT);

/***************************************************************************
 * SYSTEM CALLS
 */

typedef enum {
    SYSCALL_GATEWAY_UNKNOWN,
    SYSCALL_GATEWAY_INT,
    SYSCALL_GATEWAY_SYSENTER,
    SYSCALL_GATEWAY_SYSCALL,
#ifdef WINDOWS
    SYSCALL_GATEWAY_WOW64,
#endif
} syscall_gateway_t;

static syscall_gateway_t syscall_gateway = SYSCALL_GATEWAY_UNKNOWN;

bool
is_using_sysenter(void)
{
    return (syscall_gateway == SYSCALL_GATEWAY_SYSENTER);
}

/* we assume 1st syscall reflects primary gateway */
bool
is_using_sysint(void)
{
    return (syscall_gateway == SYSCALL_GATEWAY_INT);
}

static void
check_syscall_gateway(instr_t *inst)
{
    if (instr_get_opcode(inst) == OP_sysenter) {
        if (syscall_gateway == SYSCALL_GATEWAY_UNKNOWN
            /* some syscalls use int, but consider sysenter the primary */
            IF_LINUX(|| syscall_gateway == SYSCALL_GATEWAY_INT))
            syscall_gateway = SYSCALL_GATEWAY_SYSENTER;
        else {
            ASSERT(syscall_gateway == SYSCALL_GATEWAY_SYSENTER,
                   "multiple system call gateways not supported");
        }
    } else if (instr_get_opcode(inst) == OP_syscall) {
        if (syscall_gateway == SYSCALL_GATEWAY_UNKNOWN)
            syscall_gateway = SYSCALL_GATEWAY_SYSCALL;
        else {
            ASSERT(syscall_gateway == SYSCALL_GATEWAY_SYSCALL
                   /* some syscalls use int */
                   IF_LINUX(|| syscall_gateway == SYSCALL_GATEWAY_INT),
                   "multiple system call gateways not supported");
        }
    } else if (instr_get_opcode(inst) == OP_int) {
        if (syscall_gateway == SYSCALL_GATEWAY_UNKNOWN)
            syscall_gateway = SYSCALL_GATEWAY_INT;
        else {
            ASSERT(syscall_gateway == SYSCALL_GATEWAY_INT
                   IF_LINUX(|| syscall_gateway == SYSCALL_GATEWAY_SYSENTER
                            || syscall_gateway == SYSCALL_GATEWAY_SYSCALL),
                   "multiple system call gateways not supported");
        }
#ifdef WINDOWS
    } else if (instr_is_wow64_syscall(inst)) {
        if (syscall_gateway == SYSCALL_GATEWAY_UNKNOWN)
            syscall_gateway = SYSCALL_GATEWAY_WOW64;
        else {
            ASSERT(syscall_gateway == SYSCALL_GATEWAY_WOW64,
                   "multiple system call gateways not supported");
        }
#endif
    } else
        ASSERT(false, "unknown system call gateway");
}

DR_EXPORT
drmf_status_t
drsys_number_to_syscall(drsys_sysnum_t sysnum, drsys_syscall_t **syscall OUT)
{
    syscall_info_t *sysinfo = syscall_lookup(sysnum);
    if (syscall == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    /* All unknown-detail syscalls are now in the tables, so we only return
     * NULL on error.
     */
    if (sysinfo == NULL)
        return DRMF_ERROR_NOT_FOUND;
    *syscall = (drsys_syscall_t *) sysinfo;
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_name_to_syscall(const char *name, drsys_syscall_t **syscall OUT)
{
    drsys_sysnum_t sysnum;
    syscall_info_t *sysinfo;
    bool ok;
    if (name == NULL || syscall == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    ok = os_syscall_get_num(name, &sysnum);
    if (!ok)
        return DRMF_ERROR_NOT_FOUND;
    sysinfo = syscall_lookup(sysnum);
    if (sysinfo == NULL) {
        ASSERT(false, "name2num should return num in systable");
        return DRMF_ERROR_NOT_FOUND;
    }
#ifdef DEBUG
    ASSERT(stri_eq(sysinfo->name, name) ||
           /* account for NtUser*, etc. prefix differences */
           strcasestr(sysinfo->name, name) != NULL , "name<->num mismatch");
#endif
    *syscall = (drsys_syscall_t *) sysinfo;
    return DRMF_SUCCESS;
}

/* to avoid heap-allocated data we use pointers to temporary drsys_sysnum_t */
uint
sysnum_hash(void *val)
{
    drsys_sysnum_t *num = (drsys_sysnum_t *) val;
    /* Most primaries are < 0x3fff and secondaries are < 0x1ff so we
     * simply combine the most-likely-meaningful bits.
     */
    return (num->secondary << 14) | num->number;
}

/* to avoid heap-allocated data we use pointers to temporary drsys_sysnum_t */
bool
sysnum_cmp(void *v1, void *v2)
{
    drsys_sysnum_t *num1 = (drsys_sysnum_t *) v1;
    drsys_sysnum_t *num2 = (drsys_sysnum_t *) v2;
    return drsys_sysnums_equal(num1, num2);
}

syscall_info_t *
syscall_lookup(drsys_sysnum_t num)
{
    syscall_info_t *res;
    dr_recurlock_lock(systable_lock);
    res = (syscall_info_t *) hashtable_lookup(&systable, (void *) &num);
    dr_recurlock_unlock(systable_lock);
    return res;
}

/***************************************************************************
 * UNKNOWN SYSCALL HANDLING
 */

static const byte UNKNOWN_SYSVAL_SENTINEL = 0xab;

static const syscall_info_t unknown_info_template =
    {{0,0},"<unknown>", 0/*UNKNOWN*/, DRSYS_TYPE_UNKNOWN, };

DR_EXPORT
drmf_status_t
drsys_syscall_is_known(drsys_syscall_t *syscall, bool *known OUT)
{
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    if (syscall == NULL || known == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *known = TEST(SYSINFO_ALL_PARAMS_KNOWN, sysinfo->flags);
    return DRMF_SUCCESS;
}

static bool
is_byte_addressable(byte *addr)
{
    if (drsys_ops.is_byte_addressable == NULL)
        return true; /* have to assume it is */
    else
        return (*drsys_ops.is_byte_addressable)(addr);
}

static bool
is_byte_defined(byte *addr)
{
    if (drsys_ops.is_byte_defined == NULL)
        return is_byte_addressable(addr); /* have to assume it is */
    else
        return (*drsys_ops.is_byte_defined)(addr);
}

static bool
is_byte_undefined(byte *addr)
{
    if (drsys_ops.is_byte_defined == NULL)
        return false; /* have to assume it's not */
    else
        return !(*drsys_ops.is_byte_defined)(addr);
}

static bool
is_register_defined(reg_id_t reg)
{
    if (drsys_ops.is_register_defined == NULL)
        return true; /* have to assume it is */
    else
        return (*drsys_ops.is_register_defined)(reg);
}

/* For syscall we do not have specific parameter info for, we do a
 * memory comparison to find what has been written.
 * We will not catch passing undefined values in that are read, of course.
 */
static void
handle_pre_unknown_syscall(void *drcontext, cls_syscall_t *cpt,
                           sysarg_iter_info_t *ii)
{
    app_pc start;
    int i, j;
    bool defined;
    drsys_arg_t arg_loc = *ii->arg; /* set up mc, etc. */
    IF_DEBUG(drsys_sysnum_t sysnum = ii->arg->sysnum;)
    IF_DEBUG(syscall_info_t *sysinfo = cpt->sysinfo;)

    if (!drsys_ops.analyze_unknown_syscalls)
        return;
    LOG(SYSCALL_VERBOSE, "unknown system call #"SYSNUM_FMT"."SYSNUM_FMT" %s\n",
        sysnum.number, sysnum.secondary, sysinfo == NULL ? "" : sysinfo->name);
    /* PR 484069: reduce global logfile size */
    DO_ONCE(ELOGF(0, f_global, "WARNING: unhandled system calls found\n"));
    for (i=0; i<SYSCALL_NUM_ARG_TRACK; i++) {
        cpt->sysarg_ptr[i] = NULL;

        drsyscall_os_get_sysparam_location(cpt, i, &arg_loc);
        if (arg_loc.reg != DR_REG_NULL)
            defined = is_register_defined(arg_loc.reg);
        else
            defined = is_byte_defined(arg_loc.start_addr);

        if (defined) {
            start = (app_pc) dr_syscall_get_param(drcontext, i);
            LOG(2, "pre-unknown-syscall #"SYSNUM_FMT"."SYSNUM_FMT": param %d == "PFX"\n",
                sysnum.number, sysnum.secondary, i, start);
            if (ALIGNED(start, 4) && is_byte_addressable(start)) {
                /* This looks like a memory parameter.  It might contain OUT
                 * values mixed with IN, so we do not stop at the first undefined
                 * byte: instead we stop at an unaddr or at the max size.
                 * We need two passes to know how far we can safely read,
                 * so we go ahead and use dynamically sized memory as well.
                 */
                byte *s_at = NULL;
                int prev;
                bool overlap = false;
                for (j=0; j<SYSCALL_ARG_TRACK_MAX_SZ; j++) {
                    for (prev=0; prev<i; prev++) {
                        if (cpt->sysarg_ptr[prev] < start + j &&
                            cpt->sysarg_ptr[prev] + cpt->sysarg_sz[prev] > start) {
                            /* overlap w/ prior arg.  while we could miss some
                             * data due to the max sz we just bail for simplicity.
                             */
                            overlap = true;
                            break;
                        }
                    }
                    if (overlap || !is_byte_addressable(start + j))
                        break;
                }
                if (j > 0) {
                    LOG(SYSCALL_VERBOSE,
                        "pre-unknown-syscall #"PIFX": param %d == "PFX" %d bytes\n",
                        sysnum, i, start, j);
                    /* Make a copy of the arg values */
                    if (j > cpt->sysarg_val_bytes[i]) {
                        if (cpt->sysarg_val_bytes[i] > 0) {
                            thread_free(drcontext, cpt->sysarg_val[i],
                                        cpt->sysarg_val_bytes[i], HEAPSTAT_MISC);
                        } else
                            ASSERT(cpt->sysarg_val[i] == NULL, "leak");
                        cpt->sysarg_val_bytes[i] = ALIGN_FORWARD(j, 64);
                        cpt->sysarg_val[i] =
                            thread_alloc(drcontext, cpt->sysarg_val_bytes[i],
                                         HEAPSTAT_MISC);
                    }
                    if (safe_read(start, j, cpt->sysarg_val[i])) {
                        cpt->sysarg_ptr[i] = start;
                        cpt->sysarg_sz[i] = j;
                    } else {
                        LOG(SYSCALL_VERBOSE,
                            "WARNING: unable to read syscall arg "PFX"-"PFX"!\n",
                            start, start + j);
                        cpt->sysarg_sz[i] = 0;
                    }
                }
                if (drsys_ops.syscall_sentinels) {
                    for (j=0; j<cpt->sysarg_sz[i]; j++) {
                        if (is_byte_undefined(start + j)) {
                            size_t written;
                            /* Detect writes to data that happened to have the same
                             * value beforehand (happens often with 0) by writing
                             * a sentinel.
                             * XXX: want more-performant safe write on Windows:
                             * xref PR 605237
                             * XXX: another thread could read the data (after
                             * all we're not sure it's really syscall data) and
                             * unexpectedly read the sentinel value
                             */
                            if (s_at == NULL)
                                s_at = start + j;
                            if (!dr_safe_write(start + j, 1,
                                               &UNKNOWN_SYSVAL_SENTINEL, &written) ||
                                written != 1) {
                                /* if page is read-only then assume rest is not OUT */
                                LOG(1, "WARNING: unable to write sentinel value @"PFX"\n",
                                    start + j);
                                break;
                            }
                        } else if (s_at != NULL) {
                            LOG(2, "writing sentinel value to "PFX"-"PFX" %d %d "PFX"\n",
                                s_at, start + j, i, j, cpt->sysarg_ptr[i]);
                            s_at = NULL;
                        }
                    }
                    if (s_at != NULL) {
                        LOG(2, "writing sentinel value to "PFX"-"PFX"\n", s_at, start + j);
                        s_at = NULL;
                    }
                }
            }
        }
    }
}

/* If ii is NULL, performs post-syscall final actions */
static void
handle_post_unknown_syscall(void *drcontext, cls_syscall_t *cpt,
                            sysarg_iter_info_t *ii)
{
    int i, j;
    byte *w_at = NULL;
    byte post_val[SYSCALL_ARG_TRACK_MAX_SZ];
    if (!drsys_ops.analyze_unknown_syscalls)
        return;
    /* we analyze params even if syscall failed, since in some cases
     * some params are still written (xref i#486, i#358)
     */
    for (i=0; i<SYSCALL_NUM_ARG_TRACK; i++) {
        if (cpt->sysarg_ptr[i] != NULL) {
            if (safe_read(cpt->sysarg_ptr[i], cpt->sysarg_sz[i], post_val)) {
                for (j = 0; j < cpt->sysarg_sz[i]; j++) {
                    byte *pc = cpt->sysarg_ptr[i] + j;
                    if (is_byte_undefined(pc)) {
                        /* kernel could have written sentinel.
                         * XXX: we won't mark as defined if pre-syscall value
                         * matched sentinel and kernel wrote sentinel!
                         */
                        LOG(4, "\targ %d "PFX" %d comparing %x to %x\n", i,
                            cpt->sysarg_ptr[i], j,
                            post_val[j], cpt->sysarg_val[i][j]);
                        if ((drsys_ops.syscall_sentinels &&
                             post_val[j] != UNKNOWN_SYSVAL_SENTINEL) ||
                            (!drsys_ops.syscall_sentinels &&
                             post_val[j] != cpt->sysarg_val[i][j])) {
                            if (w_at == NULL)
                                w_at = pc;
                            /* I would assert that this is still marked undefined, to
                             * see if we hit any races, but we have overlapping syscall
                             * args and I don't want to check for them
                             */
                            ASSERT(is_byte_addressable(pc), "");
                            if (drsys_ops.syscall_dword_granularity) {
                                /* w/o sentinels (which are dangerous) we often miss
                                 * seemingly unchanged bytes (often zero) so mark
                                 * the containing dword (i#477)
                                 */
                                report_memarg_type(ii, i, SYSARG_WRITE,
                                                   (byte *)ALIGN_BACKWARD(pc, 4), 4, NULL,
                                                   DRSYS_TYPE_UNKNOWN, NULL);
                            } else {
                                report_memarg_type(ii, i, SYSARG_WRITE, pc, 1, NULL,
                                                   DRSYS_TYPE_UNKNOWN, NULL);
                            }
                        } else if (ii == NULL /* => restore */) {
                            if (post_val[j] == UNKNOWN_SYSVAL_SENTINEL &&
                                cpt->sysarg_val[i][j] != UNKNOWN_SYSVAL_SENTINEL) {
                                /* kernel didn't write so restore app value that
                                 * we clobbered w/ our sentinel.
                                 */
                                size_t written;
                                LOG(4, "restoring app sysval @"PFX"\n", pc);
                                if (!dr_safe_write(pc, 1, &cpt->sysarg_val[i][j],
                                                   &written) || written != 1) {
                                    LOG(1, "WARNING: unable to restore app sysval @"PFX"\n",
                                        pc);
                                }
                            }
                            if (w_at != NULL) {
                                LOG(SYSCALL_VERBOSE, "unknown-syscall #"SYSNUM_FMT
                                    ": param %d written "PFX" %d bytes\n",
                                    ii->arg->sysnum.number, i, w_at, pc - w_at);
                                w_at = NULL;
                            }
                        }
                    } else {
                        LOG(4, "\targ %d "PFX" byte %d defined\n", i,
                            cpt->sysarg_ptr[i], j);
                    }
                }
                if (w_at != NULL) {
                    LOG(SYSCALL_VERBOSE, "unknown-syscall #"SYSNUM_FMT": param %d written "
                        PFX" %d bytes\n", ii->arg->sysnum.number,
                        i, w_at, (cpt->sysarg_ptr[i] + j) - w_at);
                    w_at = NULL;
                }
            } else {
                /* If we can't read I assume we are also unable to write to undo
                 * sentinel writes: though should try since param could span pages
                 */
                LOG(1, "WARNING: unable to read app sysarg @"PFX"\n", cpt->sysarg_ptr[i]);
            }
        }
    }
}

/***************************************************************************
 * QUERY ROUTINES
 */

static drsys_syscall_t *
get_cur_syscall(cls_syscall_t *pt)
{
    /* We can't return NULL b/c the caller will pass it to our query routines.
     * So we pass a sentinel entry, which is per-thread so we can modify it.
     * We only use this for dynamic queries where the caller shouldn't keep
     * the pointer around.
     */
    if (pt->sysinfo == NULL) {
        /* We do need to fill in the syscall number */
        memcpy(&pt->unknown_info, &unknown_info_template, sizeof(pt->unknown_info));
        pt->unknown_info.num = pt->sysnum;
        return (drsys_syscall_t *) &pt->unknown_info;
    } else
        return (drsys_syscall_t *) pt->sysinfo;
}

DR_EXPORT
drmf_status_t
drsys_cur_syscall(void *drcontext, drsys_syscall_t **syscall OUT)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    if (drcontext == NULL || syscall == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *syscall = get_cur_syscall(pt);
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_syscall_succeeded(drsys_syscall_t *syscall, reg_t result, bool *success OUT)
{
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    if (syscall == NULL || success == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *success = os_syscall_succeeded(sysinfo->num, sysinfo, result);
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_pre_syscall_arg(void *drcontext, uint argnum, ptr_uint_t *value OUT)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    if (value == NULL || argnum >= SYSCALL_NUM_ARG_STORE)
        return DRMF_ERROR_INVALID_PARAMETER;
    *value = pt->sysarg[argnum];
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_syscall_name(drsys_syscall_t *syscall, const char **name OUT)
{
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    if (syscall == NULL || name == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *name = sysinfo->name;
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_syscall_number(drsys_syscall_t *syscall, drsys_sysnum_t *sysnum OUT)
{
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    if (syscall == NULL || sysnum == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *sysnum = sysinfo->num;
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_get_mcontext(void *drcontext, dr_mcontext_t **mc OUT)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    if (mc == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    *mc = &pt->mc;
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_syscall_return_type(drsys_syscall_t *syscall, drsys_param_type_t *type OUT)
{
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    if (syscall == NULL || type == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    /* XXX: should we provide size too?  They can iterate to get that. */
    *type = map_to_exported_type(sysinfo->return_type, NULL);
    return DRMF_SUCCESS;
}

/***************************************************************************
 * REGULAR SYSCALL HANDLING
 */

/* Assumes that arg fields on the context (drcontext, sysnum, pre, and
 * mc) have already been filled in.
 *
 * Fills in arg->valid with true.
 * XXX: should we get rid of the valid field?  For the all-args
 * dynamic iterator we use the sysparam addr and don't do a deref; and
 * for memargs, not reading usually means not knowing the bounds of a
 * sub-field where there's no type or other info and so it's not worth
 * invoking the callback.
 *
 * Sets ii->abort according to return value.
 */
bool
report_memarg_ex(sysarg_iter_info_t *ii,
                 int ordinal, drsys_param_mode_t mode,
                 app_pc ptr, size_t sz, const char *id,
                 drsys_param_type_t type, const char *type_name,
                 drsys_param_type_t containing_type)
{
    drsys_arg_t *arg = ii->arg;

    arg->type = type;
    if (type_name == NULL && type != DRSYS_TYPE_UNKNOWN &&
        type != DRSYS_TYPE_INVALID) {
        ASSERT(type < NUM_PARAM_TYPE_NAMES, "invalid type enum val");
        arg->type_name = param_type_names[type];
    } else
        arg->type_name = type_name;
    arg->containing_type = containing_type;
    arg->arg_name = id;

    arg->ordinal = ordinal;
    arg->mode = mode;

    arg->reg = DR_REG_NULL;
    arg->start_addr = ptr;
    arg->size = sz;

    /* We can't short-circuit on first iter b/c we have too much code that
     * stores extra info in pre for post that's after several reports.
     * Thus we just suppress future callbacks on first iter.
     */
    if (!ii->abort) {
        if (!(*ii->cb_mem)(arg, ii->user_data))
            ii->abort = true;
    } else {
        ASSERT(ii->pt->first_iter,
               "other than 1st iter, shouldn't report after abort");
    }
    return ii->pt->first_iter || !ii->abort;
}

static drsys_param_mode_t
mode_from_flags(uint arg_flags)
{
    drsys_param_mode_t mode = 0;
    if (TEST(SYSARG_WRITE, arg_flags))
        mode |= DRSYS_PARAM_OUT;
    if (TEST(SYSARG_READ, arg_flags))
        mode |= DRSYS_PARAM_IN;
    return mode;
}

static drsys_param_type_t
map_to_exported_type(uint sysarg_type, size_t *sz_out OUT)
{
    size_t sz = 0;
    drsys_param_type_t type = (drsys_param_type_t) sysarg_type;
    /* map to exported types */
    if (sysarg_type == SYSARG_TYPE_UNICODE_STRING_NOLEN) {
        type = DRSYS_TYPE_UNICODE_STRING;
    } else if (sysarg_type == SYSARG_TYPE_SINT32) {
        type = DRSYS_TYPE_SIGNED_INT;
        sz = 4;
    } else if (sysarg_type == SYSARG_TYPE_UINT32) {
        type = DRSYS_TYPE_UNSIGNED_INT;
        sz = 4;
    } else if (sysarg_type == SYSARG_TYPE_SINT16) {
        type = DRSYS_TYPE_SIGNED_INT;
        sz = 2;
    } else if (sysarg_type == SYSARG_TYPE_UINT16) {
        type = DRSYS_TYPE_UNSIGNED_INT;
        sz = 2;
    } else if (sysarg_type == SYSARG_TYPE_BOOL8) {
        type = DRSYS_TYPE_BOOL;
        sz = 1;
    } else if (sysarg_type == SYSARG_TYPE_BOOL32) {
        type = DRSYS_TYPE_BOOL;
        sz = 4;
#ifdef WINDOWS
    } else if (sysarg_type == DRSYS_TYPE_NTSTATUS) {
        sz = sizeof(NTSTATUS);
#endif
    }
    ASSERT(type < NUM_PARAM_TYPE_NAMES, "invalid type enum val");
    if (sz_out != NULL && sz > 0)
        *sz_out = sz;
    return type;
}

static drsys_param_type_t
type_from_arg_info(const syscall_arg_t *arg_info)
{
    drsys_param_type_t type = DRSYS_TYPE_INVALID;
    if (SYSARG_MISC_HAS_TYPE(arg_info->flags)) {
        /* we don't need size b/c it's encoded in arg_info already */
        type = map_to_exported_type(arg_info->misc, NULL);
    }
    return type;
}

bool
report_memarg_type(sysarg_iter_info_t *ii,
                   int ordinal, uint arg_flags,
                   app_pc ptr, size_t sz, const char *id,
                   drsys_param_type_t type, const char *type_name)
{
    return report_memarg_ex(ii, ordinal, mode_from_flags(arg_flags), ptr, sz, id,
                            type, type_name, DRSYS_TYPE_INVALID);
}

/* For memargs, we report their fields, so the arg type is the containing type.
 * This routine allows specifying the type of the subfield.
 */
bool
report_memarg_field(sysarg_iter_info_t *ii,
                    const syscall_arg_t *arg_info,
                    app_pc ptr, size_t sz, const char *id,
                    drsys_param_type_t type, const char *type_name)
{
    drsys_param_type_t containing_type = type_from_arg_info(arg_info);
    return report_memarg_ex(ii, arg_info->param, mode_from_flags(arg_info->flags),
                            ptr, sz, id, type, type_name, containing_type);
}

/* When we're not reporting sub-fields, stored type is reported type
 * and not just containing type.
 */
bool
report_memarg_nonfield(sysarg_iter_info_t *ii,
                       const syscall_arg_t *arg_info,
                       app_pc ptr, size_t sz, const char *id)
{
    return report_memarg_type(ii, arg_info->param, arg_info->flags,
                              ptr, sz, id, type_from_arg_info(arg_info), NULL);
}

/* For memargs, we report their fields, so the arg type is the containing type. */
bool
report_memarg(sysarg_iter_info_t *ii,
              const syscall_arg_t *arg_info,
              app_pc ptr, size_t sz, const char *id)
{
    return report_memarg_field(ii, arg_info, ptr, sz, id, DRSYS_TYPE_STRUCT, NULL);
}

bool
report_sysarg(sysarg_iter_info_t *ii, int ordinal, uint arg_flags)
{
    drsys_arg_t *arg = ii->arg;
    arg->ordinal = ordinal;
    arg->size = sizeof(reg_t);
    drsyscall_os_get_sysparam_location(ii->pt, ordinal, arg);
    arg->value = ii->pt->sysarg[ordinal];
    arg->type = DRSYS_TYPE_UNKNOWN;
    arg->type_name = NULL;
    arg->mode = mode_from_flags(arg_flags);

    /* We can't short-circuit on first iter b/c we have too much code that
     * stores extra info in pre for post that's after several reports.
     * Thus we just suppress future callbacks on first iter.
     */
    if (!ii->abort) {
        if (!(*ii->cb_arg)(arg, ii->user_data))
            ii->abort = true;
    }
    else
        ASSERT(ii->pt->first_iter, "other than 1st iter, shouldn't report after abort");
    return ii->pt->first_iter || !ii->abort;
}

bool
sysarg_invalid(syscall_arg_t *arg)
{
    return (arg->param == 0 && arg->size == 0 && arg->flags == 0);
}

#ifndef MAX_PATH
# define MAX_PATH 4096
#endif

/* pass 0 for size if there is no max size */
bool
handle_cstring(sysarg_iter_info_t *ii, int ordinal, uint arg_flags, const char *id,
               byte *start, size_t size/*in bytes*/, char *safe, bool check_addr)
{
    /* the kernel wrote a wide string to the buffer: only up to the terminating
     * null should be marked as defined
     */
    uint i;
    char c;
    /* input params have size 0: for safety stopping at MAX_PATH */
    size_t maxsz = (size == 0) ? (MAX_PATH*sizeof(char)) : size;
    if (start == NULL)
        return false; /* nothing to do */
    if (ii->arg->pre && !TEST(SYSARG_READ, arg_flags)) {
        if (!check_addr)
            return false;
        if (size > 0) {
            /* if max size specified, on pre-write check whole thing for addr */
            report_memarg_type(ii, ordinal, arg_flags, start, size, id,
                               DRSYS_TYPE_CSTRING, NULL);
            return true;
        }
    }
    if (!ii->arg->pre && !TEST(SYSARG_WRITE, arg_flags))
        return false; /*nothing to do */
    for (i = 0; i < maxsz; i += sizeof(char)) {
        if (safe != NULL)
            c = safe[i/sizeof(char)];
        else if (!safe_read(start + i, sizeof(c), &c)) {
            WARN("WARNING: unable to read syscall param string\n");
            break;
        }
        if (c == L'\0')
            break;
    }
    report_memarg_type(ii, ordinal, arg_flags, start, i + sizeof(char), id,
                       DRSYS_TYPE_CSTRING, NULL);
    return true;
}

/* assumes pt->sysarg[] has already been filled in */
static ptr_uint_t
sysarg_get_size(void *drcontext, cls_syscall_t *pt, sysarg_iter_info_t *ii,
                syscall_arg_t *arg, int argnum, bool pre, byte *start)
{
    ptr_uint_t size = 0;
    if (arg->size == SYSARG_POST_SIZE_RETVAL) {
        /* XXX: some syscalls (in particular NtGdi* and NtUser*) return
         * the capacity needed when the input buffer is NULL or
         * size of input buffer is given as 0.  For the buffer being NULL
         * we won't erroneously mark as defined, but for size being 0
         * if buffer is non-NULL we could: entry should use
         * SYSARG_NO_WRITE_IF_COUNT_0 in such cases.
         */
        if (pre) {
            /* Can't ask for retval on pre but we have a few syscalls where the
             * pre-size is only known if the app makes a prior syscall (w/ NULL
             * buffer, usually) to find it out: i#485.  Today we don't handle that
             * and thus don't check for unaddr until after the kernel writes.
             */
            size = 0;
        } else {
            size = dr_syscall_get_result(drcontext);
        }
    } else if (arg->size == SYSARG_SIZE_IN_FIELD) {
        if (pre) {
            /* 4-byte size field in struct */
            uint sz;
            byte *field = start + arg->misc/*offs of size field */;
            if (start != NULL) {
                /* by using this flag, os-specific code gives up first access
                 * rights (i.e., to skip this check, don't use this flag)
                 */
                if (!report_memarg_type(ii, arg->param, SYSARG_READ, field,
                                        sizeof(sz), NULL, DRSYS_TYPE_INT, NULL))
                    return 0;
                if (safe_read(field, sizeof(sz), &sz))
                    size = sz;
                else
                    WARN("WARNING: cannot read struct size field\n");
            }
            /* Even if we failed to get the size, initialize this for
             * post-syscall checks.
             */
            /* We don't check pt->first_iter b/c drsys_iterate_args() does not
             * invoke this code.  No harm in setting every time.
             */
            store_extra_info(pt, EXTRA_INFO_SIZE_FROM_FIELD, size);
        } else {
            /* i#992: The kernel can overwrite these struct fields during the
             * syscall, so we save them in the pre-syscall event and use them
             * post-syscall.
             */
            size = release_extra_info(pt, EXTRA_INFO_SIZE_FROM_FIELD);
        }
    } else {
        ASSERT(arg->size > 0 || -arg->size < SYSCALL_NUM_ARG_STORE,
               "reached max syscall args stored");
        size = (arg->size > 0) ? arg->size : ((uint) pt->sysarg[-arg->size]);
        if (TEST(SYSARG_LENGTH_INOUT, arg->flags)) {
            /* for x64 can't just take cur val of size so recompute */
            size_t *ptr;
            ASSERT(arg->size <= 0, "inout can't be immed");
            ptr = (size_t *) pt->sysarg[-arg->size];
            /* XXX: in some cases, ptr isn't checked for definedness until
             * after this de-ref (b/c the SYSARG_READ entry is after this
             * entry in the arg array: we could re-arrange the entries?
             */
            if (ptr == NULL || !safe_read((void *)ptr, sizeof(size), &size))
                size = 0;
        } else if (TEST(SYSARG_POST_SIZE_IO_STATUS, arg->flags)) {
#ifdef WINDOWS
            IO_STATUS_BLOCK *status = (IO_STATUS_BLOCK *) pt->sysarg[-arg->size];
            ULONG sz;
            ASSERT(sizeof(status->Information) == sizeof(sz), "");
            ASSERT(!pre, "post-io flag should be on dup entry only");
            ASSERT(arg->size <= 0, "io block can't be immed");
            if (safe_read((void *)(&status->Information), sizeof(sz), &sz))
                size = sz;
            else
                WARN("WARNING: cannot read IO_STATUS_BLOCK\n");
#else
            ASSERT(false, "linux should not have io_status flag set");
#endif
        }
    }
    if (TEST(SYSARG_SIZE_IN_ELEMENTS, arg->flags)) {
        ASSERT(arg->misc > 0 || -arg->misc < SYSCALL_NUM_ARG_STORE,
               "reached max syscall args stored");
        size *= ((arg->misc > 0) ? arg->misc : ((int) pt->sysarg[-arg->misc]));
    }
    return size;
}

/* Assumes that arg fields drcontext, sysnum, pre, and mc have already been filled in */
static void
process_pre_syscall_reads_and_writes(cls_syscall_t *pt, sysarg_iter_info_t *ii)
{
    void *drcontext = ii->arg->drcontext;
    syscall_info_t *sysinfo = pt->sysinfo;
    app_pc start;
    ptr_uint_t size;
    uint num_args;
    int i, last_param = -1;
    char idmsg[32];

    LOG(SYSCALL_VERBOSE, "processing pre system call #"SYSNUM_FMT"."SYSNUM_FMT" %s\n",
        pt->sysnum.number, pt->sysnum.secondary, sysinfo->name);
    num_args = sysinfo->arg_count;
    for (i=0; i<num_args; i++) {
        LOG(SYSCALL_VERBOSE, "\t  pre considering arg %d %d %x\n", sysinfo->arg[i].param,
            sysinfo->arg[i].size, sysinfo->arg[i].flags);
        if (sysarg_invalid(&sysinfo->arg[i]))
            break;

        /* The length written may not match that requested, so we check whether
         * addressable at pre-syscall point but only mark as defined (i.e.,
         * commit the write) at post-syscall when know true length.  This also
         * waits to determine syscall success before committing, but it opens up
         * more possibilities for races (PR 408540).  When the pre and post
         * sizes differ, we indicate what the post-syscall write size is via a
         * second entry w/ the same param#.
         * Xref PR 408536.
         */
        if (sysinfo->arg[i].param == last_param) {
            /* Only used in post-syscall */
            continue;
        }
        last_param = sysinfo->arg[i].param;

        if (TEST(SYSARG_INLINED, sysinfo->arg[i].flags))
            continue;

        start = (app_pc) pt->sysarg[sysinfo->arg[i].param];
        size = sysarg_get_size(drcontext, pt, ii, &sysinfo->arg[i], i,
                               true/*pre*/, start);
        if (ii->abort)
            break;

        /* FIXME PR 406355: we don't record which params are optional 
         * FIXME: some OUT params may not be written if the IN is bogus:
         * we should check here since harder to undo post-syscall on failure.
         */
        if (start != NULL && size > 0) {
            bool skip = os_handle_pre_syscall_arg_access(ii, &sysinfo->arg[i],
                                                         start, size);
            if (ii->abort)
                break;
            /* i#502-c#5 some arg should be ignored if next is NULL */
            if (!skip &&
                TESTALL(SYSARG_READ | SYSARG_IGNORE_IF_NEXT_NULL,
                        sysinfo->arg[i].flags) &&
                (app_pc) pt->sysarg[sysinfo->arg[i+1].param] == NULL) {
                ASSERT(i+1 < sysinfo->arg_count, "sysarg index out of bound");
                skip = true;
            }
            /* pass syscall # as pc for reporting purposes */
            /* we treat in-out read-and-write as simply read, since if
             * not defined we'll report and then mark as defined anyway.
             */
            if (!skip) {
                /* indicate which syscall arg (i#510) */
                IF_DEBUG(int res = )
                    dr_snprintf(idmsg, BUFFER_SIZE_ELEMENTS(idmsg), "parameter #%d",
                                sysinfo->arg[i].param);
                ASSERT(res > 0 && res < BUFFER_SIZE_ELEMENTS(idmsg),
                       "message buffer too small");
                NULL_TERMINATE_BUFFER(idmsg);

                if (!report_memarg_nonfield(ii, &sysinfo->arg[i], start, size, idmsg))
                    break;
            }
        }
    }
}

/* Assumes that arg fields drcontext, sysnum, pre, and mc have already been filled in */
static void
process_post_syscall_reads_and_writes(cls_syscall_t *pt, sysarg_iter_info_t *ii)
{
    void *drcontext = ii->arg->drcontext;
    syscall_info_t *sysinfo = pt->sysinfo;
    app_pc start;
    ptr_uint_t size, last_size = 0;
    uint num_args;
    int i, last_param = -1;
    IF_DEBUG(int res;)
    char idmsg[32];
#ifdef WINDOWS
    ptr_int_t result = dr_syscall_get_result(drcontext);
#endif

    LOG(SYSCALL_VERBOSE, "processing post system call #"SYSNUM_FMT"."SYSNUM_FMT,
        pt->sysnum.number, pt->sysnum.secondary);
    LOG(SYSCALL_VERBOSE, " %s res="PIFX"\n",
        sysinfo->name, dr_syscall_get_result(drcontext));
    num_args = sysinfo->arg_count;
    for (i=0; i<num_args; i++) {
        LOG(SYSCALL_VERBOSE, "\t  post considering arg %d %d %x "PFX"\n",
            sysinfo->arg[i].param, sysinfo->arg[i].size, sysinfo->arg[i].flags,
            pt->sysarg[sysinfo->arg[i].param]);
        if (sysarg_invalid(&sysinfo->arg[i]))
            break;
        ASSERT(i < SYSCALL_NUM_ARG_STORE, "not storing enough args");
        if (!TEST(SYSARG_WRITE, sysinfo->arg[i].flags))
            continue;
        ASSERT(!TEST(SYSARG_INLINED, sysinfo->arg[i].flags),
               "inlined should not be written");
#ifdef WINDOWS
        /* i#486, i#531, i#932: for too-small buffer, only last param written */
        if (TEST(SYSINFO_RET_SMALL_WRITE_LAST, sysinfo->flags) &&
            (result == STATUS_BUFFER_TOO_SMALL ||
             result == STATUS_BUFFER_OVERFLOW ||
             result == STATUS_INFO_LENGTH_MISMATCH) &&
            i+1 < num_args &&
            !sysarg_invalid(&sysinfo->arg[i+1]))
            continue;
#endif

        start = (app_pc) pt->sysarg[sysinfo->arg[i].param];
        size = sysarg_get_size(drcontext, pt, ii, &sysinfo->arg[i], i,
                               false/*!pre*/, start);
        if (ii->abort)
            break;
        /* indicate which syscall arg (i#510) */
        IF_DEBUG(res = )
            dr_snprintf(idmsg, BUFFER_SIZE_ELEMENTS(idmsg), "parameter #%d",
                        sysinfo->arg[i].param);
        ASSERT(res > 0 && res < BUFFER_SIZE_ELEMENTS(idmsg), "message buffer too small");
        NULL_TERMINATE_BUFFER(idmsg);

        if (sysinfo->arg[i].param == last_param) {
            /* For a double entry, the 2nd indicates the actual written size */
            if (size == 0
                IF_WINDOWS(/* i#798: On async write, use capacity, not OUT size. */
                           || result == STATUS_PENDING
                           /* i#486, i#531: don't use OUT size on partial write */
                           || result == STATUS_BUFFER_TOO_SMALL
                           || result == STATUS_BUFFER_OVERFLOW)) {
                /* we use SYSARG_LENGTH_INOUT for some optional params: in that
                 * case use the 1st entry's max size.
                 * XXX: we could put in our own param when the app supplies NULL
                 */
                size = last_size;
            }
            if (TEST(SYSARG_NO_WRITE_IF_COUNT_0, sysinfo->arg[i].flags)) {
                /* Currently used only for NtUserGetKeyboardLayoutList.
                 * If the count (passed in a param indicated by the first entry's
                 * size field) is zero, the kernel returns the capacity needed,
                 * but doesn't write anything, regardless of the buffer value.
                 */
                ASSERT(i > 0, "logic error");
                ASSERT(sysinfo->arg[i-1].size <= 0, "invalid syscall table entry");
                if (pt->sysarg[-sysinfo->arg[i-1].size] == 0)
                    size = 0;
            }
            if (start != NULL && size > 0) {
                bool skip = os_handle_post_syscall_arg_access
                    (ii, &sysinfo->arg[i], start, size);
                if (!skip) {
                    if (!report_memarg_nonfield(ii, &sysinfo->arg[i], start, size, idmsg))
                        break;
                }
            }
            continue;
        }
        last_param = sysinfo->arg[i].param;
        last_size = size;
        /* If the first in a double entry, give 2nd entry precedence, but
         * keep size in last_size in case 2nd was optional OUT and is NULL
         */
        if (i < num_args && sysinfo->arg[i+1].param == last_param &&
            !sysarg_invalid(&sysinfo->arg[i+1]))
            continue;
        LOG(SYSCALL_VERBOSE, "\t     start "PFX", size "PIFX"\n", start, size);
        if (start != NULL && size > 0) {
            bool skip = os_handle_post_syscall_arg_access(ii, &sysinfo->arg[i],
                                                          start, size);
            if (!skip) {
                if (!report_memarg_nonfield(ii, &sysinfo->arg[i], start, size, idmsg))
                    break;
            }
        }
    }
}

static syscall_info_t *
get_sysinfo(cls_syscall_t *pt, int initial_num, drsys_sysnum_t *sysnum OUT)
{
    syscall_info_t *sysinfo;
    ASSERT(sysnum != NULL, "invalid param");
    sysnum->number = initial_num;
    sysnum->secondary = 0;
    sysinfo = syscall_lookup(*sysnum);
    if (sysinfo != NULL) {
        if (TEST(SYSINFO_SECONDARY_TABLE, sysinfo->flags)) {
            uint code;
            ASSERT(sysinfo->arg_count >= 1, "at least 1 arg for code");
            code = pt->sysarg[sysinfo->arg[0].param];
            sysnum->secondary = code;
            /* get a new sysinfo */
            sysinfo = syscall_lookup(*sysnum);
        }
    }
    return sysinfo;
}

/* used to ignore either memargs or regular args while iterating the other */
static bool
nop_iter_cb(drsys_arg_t *arg, void *user_data)
{
    return true; /* must keep going to find the other type */
}

DR_EXPORT
drmf_status_t
drsys_iterate_memargs(void *drcontext, drsys_iter_cb_t cb, void *user_data)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    drsys_arg_t arg;
    sysarg_iter_info_t iter_info = {&arg, cb, nop_iter_cb, user_data, pt, false};

    if (!pt->memargs_iterated) {
        if (pt->pre)
            pt->memargs_iterated = true;
        else /* can't call post w/o having called pre, b/c of extra_info */
            return DRMF_ERROR_INVALID_CALL;
    }

    arg.drcontext = drcontext;
    arg.syscall = get_cur_syscall(pt);
    arg.sysnum = pt->sysnum;
    arg.pre = pt->pre;
    arg.mc = &pt->mc;
    arg.valid = true;
    arg.value = 0; /* only used for arg iterator */

    if (pt->pre) {
        if (pt->sysinfo != NULL) {
            process_pre_syscall_reads_and_writes(pt, &iter_info);
            os_handle_pre_syscall(drcontext, pt, &iter_info);
        }
        if (!pt->known) {
            handle_pre_unknown_syscall(drcontext, pt, &iter_info);
        }
    } else {
#ifdef SYSCALL_DRIVER
        if (drsys_ops.syscall_driver)
            driver_process_writes(drcontext, sysnum);
#endif
        if (pt->sysinfo != NULL) {
            if (!os_syscall_succeeded(pt->sysnum, pt->sysinfo,
                                      (ptr_int_t)dr_syscall_get_result(drcontext))) {
                LOG(SYSCALL_VERBOSE,
                    "system call #"SYSNUM_FMT"."SYSNUM_FMT" %s failed with "PFX"\n",
                    pt->sysnum.number, pt->sysnum.secondary,
                    pt->sysinfo->name, dr_syscall_get_result(drcontext));
            } else {
                process_post_syscall_reads_and_writes(pt, &iter_info);
            }
            os_handle_post_syscall(drcontext, pt, &iter_info);
        }
        if (!pt->known)
            handle_post_unknown_syscall(drcontext, pt, &iter_info);
    }
    pt->first_iter = false;
    return DRMF_SUCCESS;
}

/* Pass pt==NULL for static iteration.
 * arg need not be initialized.
 */
static drmf_status_t
drsys_iterate_args_common(void *drcontext, cls_syscall_t *pt, syscall_info_t *sysinfo,
                          drsys_arg_t *arg, drsys_iter_cb_t cb, void *user_data)
{
    int i, compacted;

    if (sysinfo == NULL)
        return DRMF_ERROR_DETAILS_UNKNOWN;

    LOG(2, "iterating over args for syscall #"SYSNUM_FMT"."SYSNUM_FMT" %s\n",
        sysinfo->num.number, sysinfo->num.secondary, sysinfo->name);

    arg->drcontext = drcontext;
    arg->syscall = (drsys_syscall_t *) sysinfo;
    arg->sysnum = sysinfo->num;
    if (pt == NULL) {
        arg->pre = true; /* arbitrary */
        arg->mc = NULL;
        arg->valid = false;
    } else {
        arg->valid = true;
        arg->pre = pt->pre;
        arg->mc = &pt->mc;
    }

    arg->arg_name = NULL;
    arg->containing_type = DRSYS_TYPE_INVALID;
    
    /* Treat all parameters as IN.
     * There are no inlined OUT params anyway: have to at least set
     * to NULL, unless truly ignored based on another parameter.
     */
    for (i = 0, compacted = 0; i < sysinfo->arg_count; i++) {
        arg->ordinal = i;
        arg->size = sizeof(void*);
        if (pt == NULL) {
            arg->reg = DR_REG_NULL;
            arg->start_addr = NULL;
            arg->value = 0;
        } else {
            drsyscall_os_get_sysparam_location(pt, i, arg);
            arg->value = pt->sysarg[i];
        }
        arg->type = DRSYS_TYPE_UNKNOWN;
        arg->type_name = NULL;
        arg->mode = DRSYS_PARAM_IN;

        /* FIXME i#1089: add type info for the non-memory-complex-type args */
        if (!sysarg_invalid(&sysinfo->arg[compacted]) &&
            sysinfo->arg[compacted].param == i) {
            if (SYSARG_MISC_HAS_TYPE(sysinfo->arg[compacted].flags)) {
                arg->type = type_from_arg_info(&sysinfo->arg[compacted]);
            }
            if (TEST(SYSARG_INLINED, sysinfo->arg[compacted].flags)) {
                int sz = sysinfo->arg[compacted].size;
                ASSERT(sz > 0, "inlined must have regular size in bytes");
                arg->size = sz;
            }
            arg->mode = mode_from_flags(sysinfo->arg[compacted].flags);
            /* Go to next entry.  Skip double entries. */
            while (sysinfo->arg[compacted].param == i &&
                   !sysarg_invalid(&sysinfo->arg[compacted]))
                compacted++;
            ASSERT(compacted <= MAX_NONINLINED_ARGS, "error in table entry");
        }

        if (!(*cb)(arg, user_data))
            break;
    }

    /* return value */
    arg->ordinal = -1;
    arg->reg = DR_REG_NULL;
    arg->start_addr = NULL;
    if (pt != NULL && !pt->pre)
        arg->value = dr_syscall_get_result(drcontext);
    else
        arg->value = 0;
    arg->size = sizeof(reg_t);
    /* get exported type and size if different from reg_t */
    arg->type = map_to_exported_type(sysinfo->return_type, &arg->size);
    arg->type_name = param_type_names[arg->type];
    arg->mode = DRSYS_PARAM_RETVAL;
    (*cb)(arg, user_data);

    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_iterate_args(void *drcontext, drsys_iter_cb_t cb, void *user_data)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    drmf_status_t res;
    drsys_arg_t arg;
    sysarg_iter_info_t iter_info = {&arg, nop_iter_cb, cb, user_data, pt, false};

    ASSERT(pt->sysinfo == NULL ||
           drsys_sysnums_equal(&pt->sysnum, &pt->sysinfo->num), "sysnum mismatch");

    res = drsys_iterate_args_common(drcontext, pt, pt->sysinfo, &arg, cb, user_data);
    if (res == DRMF_SUCCESS) {
        /* Handle dynamically-determined parameters.  For simpler code, we pay the
         * cost of calls to nop_iter_cb for all the memargs.  An alternative would
         * be to pass in a flag and check it before each report_{memarg,sysarg},
         * or to split the routines up (but that would duplicate a lot of code).
         */
        os_handle_pre_syscall(drcontext, pt, &iter_info);

        pt->first_iter = false;
    }

    return res;
}

DR_EXPORT
drmf_status_t
drsys_iterate_arg_types(drsys_syscall_t *syscall, drsys_iter_cb_t cb, void *user_data)
{
    void *drcontext = dr_get_current_drcontext();
    syscall_info_t *sysinfo = (syscall_info_t *) syscall;
    drsys_arg_t arg;
    if (syscall == NULL)
        return DRMF_ERROR_INVALID_PARAMETER;
    return drsys_iterate_args_common(drcontext, NULL/*==static*/, sysinfo,
                                     &arg, cb, user_data);
}

DR_EXPORT
drmf_status_t
drsys_iterate_syscalls(bool (*cb)(drsys_sysnum_t sysnum, drsys_syscall_t *syscall,
                                  void *user_data),
                       void *user_data)
{
    uint i;
    /* we need a recursive lock to support queries during iteration */
    dr_recurlock_lock(systable_lock);
    for (i = 0; i < HASHTABLE_SIZE(systable.table_bits); i++) {
        hash_entry_t *he;
        for (he = systable.table[i]; he != NULL; he = he->next) {
            syscall_info_t *sysinfo = (syscall_info_t *) he->payload;
            if (!(*cb)(sysinfo->num, (drsys_syscall_t *) sysinfo, user_data))
                break;
        }
    }
    dr_recurlock_unlock(systable_lock);
    return DRMF_SUCCESS;
}

static bool
drsys_event_pre_syscall(void *drcontext, int initial_num)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
    int i;

    /* cache values for dynamic iteration */
    pt->pre = true;
    pt->first_iter = true;
    pt->memargs_iterated = false;

    pt->mc.size = sizeof(pt->mc);
    pt->mc.flags = DR_MC_CONTROL|DR_MC_INTEGER; /* don't need xmm */
    dr_get_mcontext(drcontext, &pt->mc);

    /* save params for post-syscall access 
     * FIXME: it's possible for a pathological app to crash us here
     * by setting up stack so that our blind reading of SYSCALL_NUM_ARG_STORE
     * params will hit unreadable page: should use TRY/EXCEPT
     */
    LOG(SYSCALL_VERBOSE, "app xsp="PFX"\n", pt->mc.xsp);
    for (i = 0; i < SYSCALL_NUM_ARG_STORE; i++) {
        pt->sysarg[i] = dr_syscall_get_param(drcontext, i);
        LOG(SYSCALL_VERBOSE, "\targ %d = "PIFX"\n", i, pt->sysarg[i]);
    }
    DODEBUG({
        /* release_extra_info() calls can be bypassed if syscalls or safe reads
         * fail so we always clear up front
         */
        memset(pt->extra_inuse, 0, sizeof(pt->extra_inuse));
    });

    /* now that we have pt->sysarg set, get sysinfo and sysnum */
    pt->sysinfo = get_sysinfo(pt, initial_num, &pt->sysnum);
    pt->known = (pt->sysinfo != NULL &&
                 TEST(SYSINFO_ALL_PARAMS_KNOWN, pt->sysinfo->flags));

#ifdef SYSCALL_DRIVER
    /* do this as late as possible to avoid our own syscalls from corrupting
     * the list of writes.
     * the current plan is to query the driver on all syscalls, not just unknown,
     * as a sanity check on both sides.
     */
    if (drsys_ops.syscall_driver)
        driver_pre_syscall(drcontext, pt->sysnum);
#endif

    return true;
}

static void
drsys_event_post_syscall(void *drcontext, int sysnum)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);

#ifdef SYSCALL_DRIVER
    /* do this as early as possible to avoid drmem's own syscalls.
     * unfortunately the module load event runs before this: so we skip
     * NtMapViewOfSection.
     */
    if (drsys_ops.syscall_driver) {
        const char *name = get_syscall_name(sysnum);
        if (name == NULL || strcmp(name, "NtMapViewOfSection") != 0)
            driver_freeze_writes(drcontext);
        else
            driver_reset_writes(drcontext);
    }
#endif

    /* cache values for dynamic iteration */
    ASSERT(pt->mc.size == sizeof(pt->mc), "mc was clobbered");
    ASSERT(pt->mc.flags == (DR_MC_CONTROL|DR_MC_INTEGER), "mc was clobbered");
    dr_get_mcontext(drcontext, &pt->mc);
    pt->pre = false;
}


static void
drsys_event_post_syscall_last(void *drcontext, int sysnum)
{
    cls_syscall_t *pt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);

    /* The client's post-syscall event occurs prior to this due to our large
     * priority value.  Thus, all iterations are now done and we can perform
     * a final iteration that enacts any necessary state changes.
     */
#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver)
        driver_reset_writes(drcontext);
#endif
    if (!pt->known)
        handle_post_unknown_syscall(drcontext, pt, NULL);
}

/***************************************************************************
 * Filters
 */

/* We keep a table as a convenience so that the client can use a
 * static iterator and simply call our filter registration for each
 * interesting syscall found.
 */
static bool filter_all;
#define FILTERED_TABLE_HASH_BITS 6
/* Operates on DR's simple "int sysnum" */
static hashtable_t filtered_table;

static bool
drsys_event_filter_syscall(void *drcontext, int sysnum)
{
    return (filter_all ||
            (hashtable_lookup(&filtered_table, (void *)(ptr_int_t)sysnum) != NULL));
}

DR_EXPORT
drmf_status_t
drsys_filter_syscall(drsys_sysnum_t sysnum)
{
    /* DR only gives us the primary number, so we over-filter */
    hashtable_add(&filtered_table, (void *)(ptr_uint_t)sysnum.number,
                  (void *)(ptr_uint_t)sysnum.number);
    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_filter_all_syscalls(void)
{
    filter_all = true;
    return DRMF_SUCCESS;
}

/***************************************************************************
 * Events and Top-Level
 */

static dr_emit_flags_t
drsys_event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                        bool for_trace, bool translating, void **user_data)
{
    instr_t *inst;
    for (inst = instrlist_first(bb); inst != NULL; inst = instr_get_next(inst)) {
        if (instr_is_syscall(inst))
            check_syscall_gateway(inst);
    }

    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
drsys_event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *inst,
                      bool for_trace, bool translating, void *user_data)
{
    return DR_EMIT_DEFAULT;
}

static void
syscall_module_load(void *drcontext, const module_data_t *info, bool loaded)
{
    drsyscall_os_module_load(drcontext, info, loaded);
}

static void
syscall_reset_per_thread(void *drcontext, cls_syscall_t *cpt)
{
    int i;
    for (i = 0; i < SYSCALL_NUM_ARG_TRACK; i++) {
        if (cpt->sysarg_val_bytes[i] > 0) {
            ASSERT(cpt->sysarg_val[i] != NULL, "sysarg alloc error");
            thread_free(drcontext, cpt->sysarg_val[i], cpt->sysarg_val_bytes[i],
                        HEAPSTAT_MISC);
        } else {
            ASSERT(cpt->sysarg_val[i] == NULL, "sysarg alloc error");
        }
    }
}

static void
syscall_context_init(void *drcontext, bool new_depth)
{
    cls_syscall_t *cpt;
    if (new_depth) {
        cpt = (cls_syscall_t *) thread_alloc(drcontext, sizeof(*cpt), HEAPSTAT_MISC);
        drmgr_set_cls_field(drcontext, cls_idx_drsys, cpt);
    } else {
        cpt = (cls_syscall_t *) drmgr_get_cls_field(drcontext, cls_idx_drsys);
        syscall_reset_per_thread(drcontext, cpt);
    }
    memset(cpt, 0, sizeof(*cpt));

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver &&
        /* exclude thread init */
        !new_depth || drmgr_get_parent_cls_field(drcontext, cls_idx_drsys) != NULL)
        driver_handle_callback(drcontext);
#endif
}

static void
syscall_context_exit(void *drcontext, bool thread_exit)
{
    if (thread_exit) {
        cls_syscall_t *cpt = (cls_syscall_t *)
            drmgr_get_cls_field(drcontext, cls_idx_drsys);
        syscall_reset_per_thread(drcontext, cpt);
        thread_free(drcontext, cpt, sizeof(*cpt), HEAPSTAT_MISC);
    }
    /* else, nothing to do: we leave the struct for re-use on next callback */

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver && !thread_exit)
        driver_handle_cbret(drcontext);
#endif
}

static void
syscall_thread_init(void *drcontext)
{
    /* we lazily initialize sysarg_ arrays */

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver)
        driver_thread_init(drcontext);
#endif

    drsyscall_os_thread_init(drcontext);
}

static void
syscall_thread_exit(void *drcontext)
{
    drsyscall_os_thread_exit(drcontext);

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver)
        driver_thread_exit(drcontext);
#endif
}

DR_EXPORT
drmf_status_t
drsys_init(client_id_t client_id, drsys_options_t *ops)
{
    void *drcontext = dr_get_current_drcontext();
    drmf_status_t res;
    drmgr_priority_t pri_modload =
        {sizeof(pri_modload), DRMGR_PRIORITY_NAME_DRSYS, NULL, NULL,
         DRMGR_PRIORITY_MODLOAD_DRSYS};
    drmgr_priority_t pri_presys =
        {sizeof(pri_presys), DRMGR_PRIORITY_NAME_DRSYS, NULL, NULL,
         DRMGR_PRIORITY_PRESYS_DRSYS};
    drmgr_priority_t pri_postsys =
        {sizeof(pri_postsys), DRMGR_PRIORITY_NAME_DRSYS, NULL, NULL,
         DRMGR_PRIORITY_POSTSYS_DRSYS};
    drmgr_priority_t pri_postsys_last =
        {sizeof(pri_postsys_last), DRMGR_PRIORITY_NAME_DRSYS_LAST, NULL, NULL,
         DRMGR_PRIORITY_POSTSYS_DRSYS_LAST};
    /* we don't insert anything so priority shouldn't matter */
    drmgr_priority_t pri_bb =
        {sizeof(pri_bb), DRMGR_PRIORITY_NAME_DRSYS, NULL, NULL, 0};

    /* handle multiple sets of init/exit calls */
    int count = dr_atomic_add32_return_sum(&drsys_init_count, 1);
    if (count > 1)
        return true;

    res = drmf_check_version(client_id);
    if (res != DRMF_SUCCESS)
        return res;

    drmgr_init();

    if (ops->struct_size > sizeof(drsys_ops))
        return DRMF_ERROR_INCOMPATIBLE_VERSION;
    /* once we start appending new options we'll replace this */
    if (ops->struct_size != sizeof(drsys_ops))
        return DRMF_ERROR_INCOMPATIBLE_VERSION;
    memcpy(&drsys_ops, ops, sizeof(drsys_ops));

    drmgr_register_thread_init_event(syscall_thread_init);
    drmgr_register_thread_exit_event(syscall_thread_exit);
    drmgr_register_module_load_event_ex(syscall_module_load, &pri_modload);

    cls_idx_drsys = drmgr_register_cls_field(syscall_context_init, syscall_context_exit);
    ASSERT(cls_idx_drsys > -1, "unable to reserve CLS field");
    if (cls_idx_drsys == -1)
        return DRMF_ERROR;

    systable_lock = dr_recurlock_create();

    res = drsyscall_os_init(drcontext);
    if (res != DRMF_SUCCESS)
        return res;

    /* We used to handle all the gory details of Windows pre- and
     * post-syscall hooking ourselves, including system call parameter
     * bases varying by syscall type, and post-syscall hook complexity.
     * Old notes to highlight some of the past issues:
     *
     *   Since we aren't allowed to add code after a syscall instr, we have to
     *   find the post-syscall app instr: but for vsyscall sysenter, that ret
     *   is executed natively, so we have to step one level out to the wrapper.
     *   Simpler to set a flag and assume next bb is the one rather than
     *   identify the vsyscall call up front.
     *
     *   We used to also do pre-syscall via the wrapper, to avoid
     *   worrying about system call numbers or differences in where the parameters are
     *   located between int and sysenter, but now that we're checking syscall args at
     *   the syscall point itself anyway we do our pre-syscall checks there and only
     *   use these to find the post-syscall wrapper points.  Eventually we'll do
     *   post-syscall checks after the syscall point instead of using the wrappers and
     *   then we'll get rid of all of this and will properly handle hand-rolled system
     *   calls.
     *
     * But now that DR 1.3 has syscall events we use those, which also makes it
     * easier to port to Linux.
     */
    drmgr_register_pre_syscall_event_ex(drsys_event_pre_syscall, &pri_presys);
    drmgr_register_post_syscall_event_ex(drsys_event_post_syscall, &pri_postsys);
    drmgr_register_post_syscall_event_ex(drsys_event_post_syscall_last, &pri_postsys_last);

    dr_register_filter_syscall_event(drsys_event_filter_syscall);
    hashtable_init(&filtered_table, FILTERED_TABLE_HASH_BITS, HASH_INTPTR,
                   false/*!strdup*/);

    if (!drmgr_register_bb_instrumentation_event
        (drsys_event_bb_analysis, drsys_event_bb_insert, &pri_bb)) {
        ASSERT(false, "drmgr registration failed");
    }

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver)
        driver_init();
#endif

    return DRMF_SUCCESS;
}

DR_EXPORT
drmf_status_t
drsys_exit(void)
{
    /* handle multiple sets of init/exit calls */
    int count = dr_atomic_add32_return_sum(&drsys_init_count, -1);
    if (count > 0)
        return DRMF_SUCCESS;
    if (count < 0)
        return DRMF_ERROR;

#ifdef SYSCALL_DRIVER
    if (drsys_ops.syscall_driver)
        driver_exit();
#endif
 
    hashtable_delete(&filtered_table);

    drsyscall_os_exit();

    dr_recurlock_destroy(systable_lock);
    systable_lock = NULL;

    drmgr_unregister_cls_field(syscall_context_init, syscall_context_exit,
                               cls_idx_drsys);

    drmgr_exit();

    return DRMF_SUCCESS;
}

/***************************************************************************
 * EXTRA_INFO SLOT USAGE
 */

void
store_extra_info(cls_syscall_t *pt, int index, ptr_int_t value)
{
    ASSERT(pt->first_iter ||
           /* exception for sysarg_get_size() */
           index == EXTRA_INFO_SIZE_FROM_FIELD,
           "only store on first iter");
    ASSERT(index <= EXTRA_INFO_MAX, "index too high");
    DODEBUG({
        ASSERT(!pt->extra_inuse[index], "sysarg extra info conflict");
        pt->extra_inuse[index] = true;
    });
    pt->extra_info[index] = value;
}

ptr_int_t
release_extra_info(cls_syscall_t *pt, int index)
{
    ptr_int_t value;
    ASSERT(index <= EXTRA_INFO_MAX, "index too high");
    value = pt->extra_info[index];
    DODEBUG({
        ASSERT(pt->extra_inuse[index],
               "extra info used improperly (iterating memargs in post but not pre?)");
        /* we can't set to false b/c there are multiple iters */
    });
    return value;
}
