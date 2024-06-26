/*
 * CXL host parameter parsing routines
 *
 * Copyright (c) 2022 Huawei
 * Modeled loosely on the NUMA options handling in hw/core/numa.c
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/qtest.h"
#include "hw/boards.h"

#include "qapi/qapi-visit-machine.h"
#include "hw/cxl/cxl.h"
#include "hw/cxl/cxl_host.h"
#include "hw/cxl/cxl_type1_hcoh.h"
#include "hw/cxl/cxl_type2_hcoh.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_port.h"
#include "hw/pci-bridge/pci_expander_bridge.h"
#include "trace.h"

static void cxl_fixed_memory_window_config(CXLState *cxl_state,
                                           CXLFixedMemoryWindowOptions *object,
                                           Error **errp)
{
    g_autofree CXLFixedWindow *fw = g_malloc0(sizeof(*fw));
    strList *target;
    int i;

    for (target = object->targets; target; target = target->next) {
        fw->num_targets++;
    }

    fw->enc_int_ways = cxl_interleave_ways_enc(fw->num_targets, errp);
    if (*errp) {
        return;
    }

    fw->targets = g_malloc0_n(fw->num_targets, sizeof(*fw->targets));
    for (i = 0, target = object->targets; target; i++, target = target->next) {
        /* This link cannot be resolved yet, so stash the name for now */
        fw->targets[i] = g_strdup(target->value);
    }

    if (object->size % (256 * MiB)) {
        error_setg(
            errp,
            "Size of a CXL fixed memory window must be a multiple of 256MiB");
        return;
    }
    fw->size = object->size;

    if (object->has_interleave_granularity) {
        fw->enc_int_gran = cxl_interleave_granularity_enc(
            object->interleave_granularity, errp);
        if (*errp) {
            return;
        }
    } else {
        /* Default to 256 byte interleave */
        fw->enc_int_gran = 0;
    }

    cxl_state->fixed_windows =
        g_list_append(cxl_state->fixed_windows, g_steal_pointer(&fw));

    return;
}

void cxl_fmws_link_targets(CXLState *cxl_state, Error **errp)
{
    if (cxl_state && cxl_state->fixed_windows) {
        GList *it;

        for (it = cxl_state->fixed_windows; it; it = it->next) {
            CXLFixedWindow *fw = it->data;
            int i;

            for (i = 0; i < fw->num_targets; i++) {
                Object *o;
                bool ambig;

                o = object_resolve_path_type(fw->targets[i],
                                             TYPE_PXB_CXL_DEVICE, &ambig);
                if (!o) {
                    error_setg(errp, "Could not resolve CXLFM target %s",
                               fw->targets[i]);
                    return;
                }
                fw->target_hbs[i] = PXB_CXL_DEV(o);
            }
        }
    }
}

/* TODO: support, multiple hdm decoders */
static bool cxl_hdm_find_target(uint32_t *cache_mem, hwaddr addr,
                                uint8_t *target)
{
    uint32_t ctrl;
    uint32_t ig_enc;
    uint32_t iw_enc;
    uint32_t target_idx;

    ctrl = cache_mem[R_CXL_HDM_DECODER0_CTRL];
    if (!FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, COMMITTED)) {
        return false;
    }

    ig_enc = FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, IG);
    iw_enc = FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, IW);
    target_idx = (addr / cxl_decode_ig(ig_enc)) % (1 << iw_enc);

    if (target_idx < 4) {
        *target = extract32(cache_mem[R_CXL_HDM_DECODER0_TARGET_LIST_LO],
                            target_idx * 8, 8);
    } else {
        *target = extract32(cache_mem[R_CXL_HDM_DECODER0_TARGET_LIST_HI],
                            (target_idx - 4) * 8, 8);
    }

    return true;
}

static PCIDevice *cxl_cfmws_find_device(CXLFixedWindow *fw, hwaddr addr)
{
    CXLComponentState *hb_cstate;
    PCIHostState *hb;
    int rb_index;
    uint32_t *cache_mem;
    uint8_t target;
    bool target_found;
    PCIDevice *rp, *d;

    /* Address is relative to memory region. Convert to HPA */
    addr += fw->base;

    rb_index = (addr / cxl_decode_ig(fw->enc_int_gran)) % fw->num_targets;
    hb = PCI_HOST_BRIDGE(fw->target_hbs[rb_index]->cxl.cxl_host_bridge);
    if (!hb || !hb->bus || !pci_bus_is_cxl(hb->bus)) {
        return NULL;
    }

    if (cxl_get_hb_passthrough(hb)) {
        trace_cxl_debug_message("CXL host bridge is passthrough");
        rp = pcie_find_port_first(hb->bus);
        if (!rp) {
            trace_cxl_debug_message("CXL root port not found");
            return NULL;
        }
    } else {
        hb_cstate = cxl_get_hb_cstate(hb);
        if (!hb_cstate) {
            trace_cxl_debug_message("CXL host bridge cstate doesn't exist");
            return NULL;
        }

        cache_mem = hb_cstate->crb.cache_mem_registers;

        target_found = cxl_hdm_find_target(cache_mem, addr, &target);
        if (!target_found) {
            return NULL;
        }

        rp = pcie_find_port_by_pn(hb->bus, target);
        if (!rp) {
            return NULL;
        }
    }

    if (cxl_is_remote_root_port(rp)) {
        trace_cxl_debug_message("CXL Root Port: Remote mode is enabled");
        return rp;
    }

    d = pci_bridge_get_sec_bus(PCI_BRIDGE(rp))->devices[0];
    if (!d) {
        return NULL;
    }

    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_TYPE3)) {
        return d;
    }

    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_TYPE2)) {
        return d;
    }

    if (object_dynamic_cast(OBJECT(d), TYPE_CXL_TYPE1)) {
        return d;
    }

    return NULL;
}

static MemTxResult cxl_read_cfmws(void *opaque, hwaddr addr, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_ERROR;
    CXLFixedWindow *fw = opaque;
    PCIDevice *d;
    const char *type;

    d = cxl_cfmws_find_device(fw, addr);
    if (d == NULL) {
        trace_cxl_debug_message("CXL device not found");
        *data = 0;
        /* Reads to invalid address return poison */
        return result;
    }

    if (cxl_is_remote_root_port(d)) {
        result = cxl_remote_cxl_mem_read_with_cache(d, addr + fw->base, data,
                                                    size, attrs);
        trace_cxl_read_cfmws("CXL.mem via RP", addr, size, *data);
    } else {
        type = object_get_typename(OBJECT(d));
        if (g_strcmp0(type, "cxl-type1") == 0)
            result = cxl_type1_read(d, addr + fw->base, data, size, attrs);
        else if (g_strcmp0(type, "cxl-type2") == 0)
            result =
                cxl_host_type2_hcoh_read(d, addr + fw->base, data, size, attrs);
        else if (g_strcmp0(type, "cxl-type3") == 0) {
            result = cxl_type3_read(d, addr + fw->base, data, size, attrs);
            trace_cxl_read_cfmws("CXL.mem", addr, size, *data);
        } else {
            trace_cxl_debug_message("Unexpected CXL device type");
        }
    }

    return result;
}

static MemTxResult cxl_write_cfmws(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size, MemTxAttrs attrs)
{
    MemTxResult result = MEMTX_OK;
    CXLFixedWindow *fw = opaque;
    PCIDevice *d;
    const char *type;

    d = cxl_cfmws_find_device(fw, addr);
    if (d == NULL) {
        trace_cxl_debug_message("CXL device not found");
        /* Writes to invalid address are silent */
        return result;
    }

    if (cxl_is_remote_root_port(d)) {
        trace_cxl_write_cfmws("CXL.mem via RP", addr, size, data);
        result = cxl_remote_cxl_mem_write_with_cache(d, addr + fw->base, data,
                                                     size, attrs);
    } else {
        type = object_get_typename(OBJECT(d));
        if (g_strcmp0(type, "cxl-type1") == 0)
            result = cxl_type1_write(d, addr + fw->base, &data, size, attrs);
        else if (g_strcmp0(type, "cxl-type2") == 0)
            result = cxl_host_type2_hcoh_write(d, addr + fw->base, data, size,
                                               attrs);
        else if (g_strcmp0(type, "cxl-type3") == 0) {
            trace_cxl_write_cfmws("CXL.mem", addr, size, data);
            result = cxl_type3_write(d, addr + fw->base, data, size, attrs);
        } else {
            trace_cxl_debug_message("Unexpected CXL device type");
        }
    }
    return result;
}

const MemoryRegionOps cfmws_ops = {
    .read_with_attrs = cxl_read_cfmws,
    .write_with_attrs = cxl_write_cfmws,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid =
        {
            .min_access_size = 1,
            .max_access_size = 8,
            .unaligned = true,
        },
    .impl =
        {
            .min_access_size = 1,
            .max_access_size = 8,
            .unaligned = true,
        },
};

static void machine_get_cxl(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    CXLState *cxl_state = opaque;
    bool value = cxl_state->is_enabled;

    visit_type_bool(v, name, &value, errp);
}

static void machine_set_cxl(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    CXLState *cxl_state = opaque;
    bool value;

    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    cxl_state->is_enabled = value;
}

static void machine_get_cfmw(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CXLFixedMemoryWindowOptionsList **list = opaque;

    visit_type_CXLFixedMemoryWindowOptionsList(v, name, list, errp);
}

static void machine_set_cfmw(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    CXLState *state = opaque;
    CXLFixedMemoryWindowOptionsList *cfmw_list = NULL;
    CXLFixedMemoryWindowOptionsList *it;

    visit_type_CXLFixedMemoryWindowOptionsList(v, name, &cfmw_list, errp);
    if (!cfmw_list) {
        return;
    }

    for (it = cfmw_list; it; it = it->next) {
        cxl_fixed_memory_window_config(state, it->value, errp);
    }
    state->cfmw_list = cfmw_list;
}

void cxl_machine_init(Object *obj, CXLState *state)
{
    object_property_add(obj, "cxl", "bool", machine_get_cxl, machine_set_cxl,
                        NULL, state);
    object_property_set_description(obj, "cxl",
                                    "Set on/off to enable/disable "
                                    "CXL instantiation");

    object_property_add(obj, "cxl-fmw", "CXLFixedMemoryWindow",
                        machine_get_cfmw, machine_set_cfmw, NULL, state);
    object_property_set_description(obj, "cxl-fmw",
                                    "CXL Fixed Memory Windows (array)");
}

void cxl_hook_up_pxb_registers(PCIBus *bus, CXLState *state, Error **errp)
{
    /* Walk the pci busses looking for pxb busses to hook up */
    if (bus) {
        QLIST_FOREACH (bus, &bus->child, sibling) {
            if (!pci_bus_is_root(bus)) {
                continue;
            }
            if (pci_bus_is_cxl(bus)) {
                if (!state->is_enabled) {
                    error_setg(errp, "CXL host bridges present, but cxl=off");
                    return;
                }
                pxb_cxl_hook_up_registers(state, bus, errp);
            }
        }
    }
}
