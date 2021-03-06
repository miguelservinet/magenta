// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2016, Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <debug.h>
#include <dev/interrupt.h>
#include <dev/pcie.h>
#include <err.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <kernel/vm.h>
#include <list.h>
#include <new.h>
#include <pow2.h>
#include <string.h>
#include <trace.h>

#include "pcie_priv.h"

#define LOCAL_TRACE 0

/******************************************************************************
 *
 * Helper routines common to all IRQ modes.
 *
 ******************************************************************************/
static void pcie_reset_common_irq_bookkeeping(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);

    if (dev->irq.handler_count > 1) {
        DEBUG_ASSERT(dev->irq.handlers != &dev->irq.singleton_handler);
        free(dev->irq.handlers);
    }

    memset(&dev->irq.singleton_handler, 0, sizeof(dev->irq.singleton_handler));
    dev->irq.mode          = PCIE_IRQ_MODE_DISABLED;
    dev->irq.handlers      = NULL;
    dev->irq.handler_count = 0;
}

static status_t pcie_alloc_irq_handlers(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                        uint requested_irqs) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(requested_irqs);
    DEBUG_ASSERT(!dev->irq.handlers);
    DEBUG_ASSERT(!dev->irq.handler_count);

    if (requested_irqs == 1) {
        memset(&dev->irq.singleton_handler, 0, sizeof(dev->irq.singleton_handler));
        dev->irq.handlers      = &dev->irq.singleton_handler;
        dev->irq.handler_count = 1;
        goto finish_bookkeeping;
    }

    dev->irq.handlers = static_cast<pcie_irq_handler_state_t*>(calloc(requested_irqs,
                                                                      sizeof(*dev->irq.handlers)));
    if (!dev->irq.handlers)
        return ERR_NO_MEMORY;
    dev->irq.handler_count = requested_irqs;

finish_bookkeeping:
    for (uint i = 0; i < dev->irq.handler_count; ++i) {
        pcie_irq_handler_state_t* h = &dev->irq.handlers[i];
        h->dev        = dev.get();
        h->pci_irq_id = i;
    }
    return NO_ERROR;
}

/******************************************************************************
 *
 * Legacy IRQ mode routines.
 *
 ******************************************************************************/
mxtl::RefPtr<SharedLegacyIrqHandler> SharedLegacyIrqHandler::Create(uint irq_id) {
    AllocChecker ac;

    SharedLegacyIrqHandler* handler = new (&ac) SharedLegacyIrqHandler(irq_id);
    if (!ac.check()) {
        TRACEF("Failed to create shared legacry IRQ handler for system IRQ ID %u\n", irq_id);
        return nullptr;
    }

    return mxtl::AdoptRef(handler);
}

SharedLegacyIrqHandler::SharedLegacyIrqHandler(uint irq_id)
    : irq_id_(irq_id) {
    list_initialize(&device_handler_list_);
    mask_interrupt(irq_id_);  // This should not be needed, but just in case.
    register_int_handler(irq_id_, HandlerThunk, this);
}

SharedLegacyIrqHandler::~SharedLegacyIrqHandler() {
    DEBUG_ASSERT(list_is_empty(&device_handler_list_));
    mask_interrupt(irq_id_);
    register_int_handler(irq_id_, NULL, NULL);
}

enum handler_return SharedLegacyIrqHandler::Handler() {
    bool need_resched = false;

    /* Go over the list of device's which share this legacy IRQ and give them a
     * chance to handle any interrupts which may be pending in their device.
     * Keep track of whether or not any device has requested a re-schedule event
     * at the end of this IRQ. */
    AutoSpinLock list_lock(device_handler_list_lock_);

    if (list_is_empty(&device_handler_list_)) {
        TRACEF("Received legacy PCI INT (system IRQ %u), but there are no devices registered to "
               "handle this interrupt.  This is Very Bad.  Disabling the interrupt at the system "
               "IRQ level to prevent meltdown.\n",
               irq_id_);
        mask_interrupt(irq_id_);
        return INT_NO_RESCHEDULE;
    }

    pcie_device_state_t* dev;
    list_for_every_entry(&device_handler_list_,
                         dev,
                         pcie_device_state_t,
                         irq.legacy.shared_handler_node) {
        uint16_t command, status;
        pcie_config_t* cfg = dev->cfg;

        {
            AutoSpinLock(dev->cmd_reg_lock);
            command = pcie_read16(&cfg->base.command);
            status  = pcie_read16(&cfg->base.status);
        }

        if ((status & PCIE_CFG_STATUS_INT_STS) && !(command & PCIE_CFG_COMMAND_INT_DISABLE)) {
            DEBUG_ASSERT(dev);
            pcie_irq_handler_state_t* hstate  = &dev->irq.handlers[0];

            if (hstate) {
                pcie_irq_handler_retval_t irq_ret = PCIE_IRQRET_MASK;
                AutoSpinLock device_handler_lock(hstate->lock);

                if (hstate->handler) {
                    if (!hstate->masked)
                        irq_ret = hstate->handler(*dev, 0, hstate->ctx);

                    if (irq_ret & PCIE_IRQRET_RESCHED)
                        need_resched = true;
                } else {
                    TRACEF("Received legacy PCI INT (system IRQ %u) for %02x:%02x.%02x, but no irq "
                           "handler has been registered by the driver.  Force disabling the "
                           "interrupt.\n",
                           irq_id_, dev->bus_id, dev->dev_id, dev->func_id);
                }

                if (irq_ret & PCIE_IRQRET_MASK) {
                    hstate->masked = true;
                    {
                        AutoSpinLock(dev->cmd_reg_lock);
                        command = pcie_read16(&cfg->base.command);
                        pcie_write16(&cfg->base.command, command | PCIE_CFG_COMMAND_INT_DISABLE);
                    }
                }
            } else {
                TRACEF("Received legacy PCI INT (system IRQ %u) for %02x:%02x.%02x , but no irq "
                       "handlers have been allocated!  Force disabling the interrupt.\n",
                       irq_id_, dev->bus_id, dev->dev_id, dev->func_id);

                {
                    AutoSpinLock(dev->cmd_reg_lock);
                    command = pcie_read16(&cfg->base.command);
                    pcie_write16(&cfg->base.command, command | PCIE_CFG_COMMAND_INT_DISABLE);
                }
            }
        }
    }

    return need_resched ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
}

void SharedLegacyIrqHandler::AddDevice(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.legacy.shared_handler.get() == this);
    DEBUG_ASSERT(!list_in_list(&dev->irq.legacy.shared_handler_node));

    /* Make certain that the device's legacy IRQ has been masked at the PCI
     * device level.  Then add this dev to the handler's list.  If this was the
     * first device added to the handler list, unmask the handler IRQ at the top
     * level. */
    AutoSpinLockIrqSave lock(device_handler_list_lock_);

    pcie_write16(&dev->cfg->base.command, pcie_read16(&dev->cfg->base.command) |
                                          PCIE_CFG_COMMAND_INT_DISABLE);

    bool first_device = list_is_empty(&device_handler_list_);
    list_add_tail(&device_handler_list_, &dev->irq.legacy.shared_handler_node);

    if (first_device)
        unmask_interrupt(irq_id_);
}

void SharedLegacyIrqHandler::RemoveDevice(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.legacy.shared_handler.get() == this);
    DEBUG_ASSERT(list_in_list(&dev->irq.legacy.shared_handler_node));

    /* Make absolutely sure we have been masked at the PCIe config level, then
     * remove the device from the shared handler list.  If this was the last
     * device on the list, mask the top level IRQ */
    AutoSpinLockIrqSave lock(device_handler_list_lock_);

    pcie_write16(&dev->cfg->base.command, pcie_read16(&dev->cfg->base.command) |
                                          PCIE_CFG_COMMAND_INT_DISABLE);
    list_delete(&dev->irq.legacy.shared_handler_node);

    if (list_is_empty(&device_handler_list_))
        mask_interrupt(irq_id_);
}

static inline status_t pcie_mask_unmask_legacy_irq(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                                   bool mask) {

    if (!dev->irq.handlers || !dev->irq.handler_count)
        return ERR_INVALID_ARGS;

    pcie_config_t*            cfg    = dev->cfg;
    pcie_irq_handler_state_t* hstate = &dev->irq.handlers[0];
    uint16_t val;

    AutoSpinLockIrqSave lock(hstate->lock);

    val = pcie_read16(&cfg->base.command);
    if (mask) val = static_cast<uint16_t>(val | PCIE_CFG_COMMAND_INT_DISABLE);
    else      val = static_cast<uint16_t>(val & ~PCIE_CFG_COMMAND_INT_DISABLE);
    pcie_write16(&cfg->base.command, val);
    hstate->masked = mask;

    return NO_ERROR;
}

static void pcie_leave_legacy_irq_mode(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    /* Disable legacy IRQs and unregister from the shared legacy handler */
    pcie_mask_unmask_legacy_irq(dev, true);
    dev->irq.legacy.shared_handler->RemoveDevice(dev);

    /* Release any handler storage and reset all of our bookkeeping */
    pcie_reset_common_irq_bookkeeping(dev);
}

static status_t pcie_enter_legacy_irq_mode(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                           uint requested_irqs) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(requested_irqs);

    if (!dev->irq.legacy.pin || (requested_irqs > 1))
        return ERR_NOT_SUPPORTED;

    /* We can never fail to allocated a single handlers (since we are going to
     * use the pre-allocated singleton) */
    __UNUSED status_t res = pcie_alloc_irq_handlers(dev, requested_irqs);
    DEBUG_ASSERT(res == NO_ERROR);
    DEBUG_ASSERT(dev->irq.handlers == &dev->irq.singleton_handler);

    dev->irq.mode = PCIE_IRQ_MODE_LEGACY;

    dev->irq.legacy.shared_handler->AddDevice(dev);
    return NO_ERROR;
}

/******************************************************************************
 *
 * MSI IRQ mode routines.
 *
 ******************************************************************************/
static inline void pcie_set_msi_enb(const mxtl::RefPtr<pcie_device_state_t>& dev, bool enb) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.msi.cfg);

    volatile uint16_t* ctrl_reg = &dev->irq.msi.cfg->ctrl;
    pcie_write16(ctrl_reg, PCIE_CAP_MSI_CTRL_SET_ENB(enb, pcie_read16(ctrl_reg)));
}

static inline bool pcie_mask_unmask_msi_irq_locked(pcie_device_state_t& dev,
                                                   uint irq_id,
                                                   bool mask) {
    DEBUG_ASSERT(dev.irq.mode == PCIE_IRQ_MODE_MSI);
    DEBUG_ASSERT(irq_id < dev.irq.handler_count);
    DEBUG_ASSERT(dev.irq.handlers);

    pcie_irq_handler_state_t* hstate  = &dev.irq.handlers[irq_id];
    DEBUG_ASSERT(hstate->lock.IsHeld());

    /* Internal code should not be calling this function if they want to mask
     * the interrupt, but it is not possible to do so. */
    DEBUG_ASSERT(!mask ||
                 dev.bus_drv.platform().supports_msi_masking() ||
                 dev.irq.msi.pvm_mask_reg);

    /* If we can mask at the PCI device level, do so. */
    if (dev.irq.msi.pvm_mask_reg) {
        DEBUG_ASSERT(irq_id < PCIE_MAX_MSI_IRQS);
        uint32_t  val  = pcie_read32(dev.irq.msi.pvm_mask_reg);
        if (mask) val |=  ((uint32_t)1 << irq_id);
        else      val &= ~((uint32_t)1 << irq_id);
        pcie_write32(dev.irq.msi.pvm_mask_reg, val);
    }


    /* If we can mask at the platform interrupt controller level, do so. */
    DEBUG_ASSERT(dev.irq.msi.irq_block.allocated);
    DEBUG_ASSERT(irq_id < dev.irq.msi.irq_block.num_irq);
    if (dev.bus_drv.platform().supports_msi_masking())
        dev.bus_drv.platform().MaskUnmaskMsi(&dev.irq.msi.irq_block, irq_id, mask);

    bool ret = hstate->masked;
    hstate->masked = mask;
    return ret;
}

static inline status_t pcie_mask_unmask_msi_irq(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                                uint irq_id,
                                                bool mask) {
    DEBUG_ASSERT(dev);

    if (irq_id >= dev->irq.handler_count)
        return ERR_INVALID_ARGS;

    /* If a mask is being requested, and we cannot mask at either the platform
     * interrupt controller or the PCI device level, tell the caller that the
     * operation is unsupported. */
    if (mask && !dev->bus_drv.platform().supports_msi_masking() && !dev->irq.msi.pvm_mask_reg)
        return ERR_NOT_SUPPORTED;

    DEBUG_ASSERT(dev->irq.handlers);

    {
        AutoSpinLockIrqSave handler_lock(dev->irq.handlers[irq_id].lock);
        pcie_mask_unmask_msi_irq_locked(*dev, irq_id, mask);
    }

    return NO_ERROR;
}

static void pcie_mask_all_msi_vectors(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.msi.cfg);

    for (uint i = 0; i < dev->irq.handler_count; i++)
        pcie_mask_unmask_msi_irq(dev, i, true);

    /* In theory, this should not be needed as all of the relevant bits should
     * have already been masked during the calls to pcie_mask_unmask_msi_irq.
     * Just to be careful, however, we explicitly mask all of the upper bits as well. */
    if (dev->irq.msi.pvm_mask_reg)
        pcie_write32(dev->irq.msi.pvm_mask_reg, 0xFFFFFFFF);
}

static void pcie_set_msi_target(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                uint64_t tgt_addr,
                                uint32_t tgt_data) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.msi.cfg);
    DEBUG_ASSERT(dev->irq.msi.is64bit || !(tgt_addr >> 32));
    DEBUG_ASSERT(!(tgt_data >> 16));

    /* Make sure MSI is disabled and all vectors masked (if possible) before
     * changing the target address and data. */
    pcie_set_msi_enb(dev, false);
    pcie_mask_all_msi_vectors(dev);

    /* lower bits of the address register are common to all forms of the MSI
     * capability structure.  Upper address bits and data position depend on
     * whether this is a 64 bit or 32 bit version */
    pcie_write32(&dev->irq.msi.cfg->addr, (uint32_t)(tgt_addr & 0xFFFFFFFF));
    if (dev->irq.msi.is64bit) {
        pcie_write32(&dev->irq.msi.cfg->nopvm_64bit.addr_upper, (uint32_t)(tgt_addr >> 32));
        pcie_write16(&dev->irq.msi.cfg->nopvm_64bit.data,       (uint16_t)(tgt_data & 0xFFFF));
    } else {
        pcie_write16(&dev->irq.msi.cfg->nopvm_32bit.data,       (uint16_t)(tgt_data & 0xFFFF));
    }
}

static enum handler_return pcie_msi_irq_handler(void *arg) {
    DEBUG_ASSERT(arg);
    pcie_irq_handler_state_t* hstate  = (pcie_irq_handler_state_t*)arg;
    pcie_device_state_t*      dev     = hstate->dev;
    DEBUG_ASSERT(dev);

    /* No need to save IRQ state; we are in an IRQ handler at the moment. */
    DEBUG_ASSERT(hstate);
    AutoSpinLock handler_lock(hstate->lock);

    /* Mask our IRQ if we can. */
    bool was_masked;
    if (dev->bus_drv.platform().supports_msi_masking() || dev->irq.msi.pvm_mask_reg) {
        was_masked = pcie_mask_unmask_msi_irq_locked(*dev, hstate->pci_irq_id, true);
    } else {
        DEBUG_ASSERT(!hstate->masked);
        was_masked = false;
    }

    /* If the IRQ was masked or the handler removed by the time we got here,
     * leave the IRQ masked, unlock and get out. */
    if (was_masked || !hstate->handler)
        return INT_NO_RESCHEDULE;

    /* Dispatch */
    pcie_irq_handler_retval_t irq_ret = hstate->handler(*dev, hstate->pci_irq_id, hstate->ctx);

    /* Re-enable the IRQ if asked to do so */
    if (!(irq_ret & PCIE_IRQRET_MASK))
        pcie_mask_unmask_msi_irq_locked(*dev, hstate->pci_irq_id, false);

    /* Request a reschedule if asked to do so */
    return (irq_ret & PCIE_IRQRET_RESCHED) ? INT_RESCHEDULE : INT_NO_RESCHEDULE;
}

static void pcie_free_msi_block(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);
    PcieBusDriver& bus_drv = dev->bus_drv;

    /* If no block has been allocated, there is nothing to do */
    if (!dev->irq.msi.irq_block.allocated)
        return;

    DEBUG_ASSERT(bus_drv.platform().supports_msi());

    /* Mask the IRQ at the platform interrupt controller level if we can, and
     * unregister any registered handler. */
    const pcie_msi_block_t* b = &dev->irq.msi.irq_block;
    for (uint i = 0; i < b->num_irq; i++) {
        if (bus_drv.platform().supports_msi_masking())
            bus_drv.platform().MaskUnmaskMsi(b, i, true);
        bus_drv.platform().RegisterMsiHandler(b, i, NULL, NULL);
    }

    /* Give the block of IRQs back to the plaform */
    bus_drv.platform().FreeMsiBlock(&dev->irq.msi.irq_block);
    DEBUG_ASSERT(!dev->irq.msi.irq_block.allocated);
}

static void pcie_set_msi_multi_message_enb(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                           uint requested_irqs) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->irq.msi.cfg);
    DEBUG_ASSERT((requested_irqs >= 1) && (requested_irqs <= PCIE_MAX_MSI_IRQS));

    uint log2 = log2_uint_ceil(requested_irqs);

    DEBUG_ASSERT(log2 <= 5);
    DEBUG_ASSERT(!log2 || ((0x1u << (log2 - 1)) < requested_irqs));
    DEBUG_ASSERT((0x1u << log2) >= requested_irqs);

    volatile uint16_t* ctrl_reg = &dev->irq.msi.cfg->ctrl;
    pcie_write16(ctrl_reg,
                 PCIE_CAP_MSI_CTRL_SET_MME(log2, pcie_read16(ctrl_reg)));
}

static void pcie_leave_msi_irq_mode(const mxtl::RefPtr<pcie_device_state_t>& dev) {
    DEBUG_ASSERT(dev);

    /* Disable MSI, mask all vectors and zero out the target */
    pcie_set_msi_target(dev, 0x0, 0x0);

    /* Return any allocated irq block to the platform, unregistering with
     * the interrupt controller and synchronizing with the dispatchers in
     * the process. */
    pcie_free_msi_block(dev);

    /* Reset our common state, free any allocated handlers */
    pcie_reset_common_irq_bookkeeping(dev);
}

static status_t pcie_enter_msi_irq_mode(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                        uint requested_irqs) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(requested_irqs);

    status_t res = NO_ERROR;

    /* We cannot go into MSI mode if we don't support MSI at all, or we
     * don't support the number of IRQs requested */
    if (!dev->irq.msi.cfg                         ||
        !dev->bus_drv.platform().supports_msi()    ||
        (requested_irqs > dev->irq.msi.max_irqs))
        return ERR_NOT_SUPPORTED;

    /* Ask the platform for a chunk of MSI compatible IRQs */
    DEBUG_ASSERT(!dev->irq.msi.irq_block.allocated);
    res = dev->bus_drv.platform().AllocMsiBlock(requested_irqs,
                                                dev->irq.msi.is64bit,
                                                false,  /* is_msix == false */
                                                &dev->irq.msi.irq_block);
    if (res != NO_ERROR) {
        LTRACEF("Failed to allocate a block of %u MSI IRQs for device "
                "%02x:%02x.%01x (res %d)\n",
                requested_irqs, dev->bus_id, dev->dev_id, dev->func_id, res);
        goto bailout;
    }

    /* Allocate our handler table */
    res = pcie_alloc_irq_handlers(dev, requested_irqs);
    if (res != NO_ERROR)
        goto bailout;

    /* Record our new IRQ mode */
    dev->irq.mode = PCIE_IRQ_MODE_MSI;

    /* Program the target write transaction into the MSI registers.  As a side
     * effect, this will ensure that...
     *
     * 1) MSI mode has been disabled at the top level
     * 2) Each IRQ has been masked at system level (if supported)
     * 3) Each IRQ has been masked at the PCI PVM level (if supported)
     */
    DEBUG_ASSERT(dev->irq.msi.irq_block.allocated);
    pcie_set_msi_target(dev,
                        dev->irq.msi.irq_block.tgt_addr,
                        dev->irq.msi.irq_block.tgt_data);

    /* Properly program the multi-message enable field in the control register */
    pcie_set_msi_multi_message_enb(dev, requested_irqs);

    /* Register each IRQ with the dispatcher */
    DEBUG_ASSERT(dev->irq.handler_count <= dev->irq.msi.irq_block.num_irq);
    for (uint i = 0; i < dev->irq.handler_count; ++i) {
        dev->bus_drv.platform().RegisterMsiHandler(&dev->irq.msi.irq_block,
                                                   i,
                                                   pcie_msi_irq_handler,
                                                   dev->irq.handlers + i);
    }

    /* Enable MSI at the top level */
    pcie_set_msi_enb(dev, true);

bailout:
    if (res != NO_ERROR)
        pcie_leave_msi_irq_mode(dev);

    return res;
}

/******************************************************************************
 *
 * Internal implementation of the Kernel facing API.
 *
 ******************************************************************************/
status_t pcie_query_irq_mode_capabilities_internal(const pcie_device_state_t& dev,
                                                   pcie_irq_mode_t mode,
                                                   pcie_irq_mode_caps_t* out_caps) {
    DEBUG_ASSERT(dev.plugged_in);
    DEBUG_ASSERT(dev.dev_lock.IsHeld());
    DEBUG_ASSERT(out_caps);

    PcieBusDriver& bus_drv = dev.bus_drv;
    memset(out_caps, 0, sizeof(*out_caps));

    switch (mode) {
    case PCIE_IRQ_MODE_LEGACY:
        if (!dev.irq.legacy.pin)
            return ERR_NOT_SUPPORTED;

        out_caps->max_irqs = 1;
        out_caps->per_vector_masking_supported = true;
        break;

    case PCIE_IRQ_MODE_MSI:
        /* If the platform does not support MSI, then we don't support MSI,
         * even if the device does. */
        if (!bus_drv.platform().supports_msi())
            return ERR_NOT_SUPPORTED;

        /* If the device supports MSI, it will have a pointer to the control
         * structure in config. */
        if (!dev.irq.msi.cfg)
            return ERR_NOT_SUPPORTED;

        /* We support PVM if either the device does, or if the platform is
         * capable of masking and unmasking individual IRQs from an MSI block
         * allocation. */
        out_caps->max_irqs = dev.irq.msi.max_irqs;
        out_caps->per_vector_masking_supported = (dev.irq.msi.pvm_mask_reg != NULL)
                                               || (bus_drv.platform().supports_msi_masking());
        break;

    case PCIE_IRQ_MODE_MSI_X:
        /* If the platform does not support MSI, then we don't support MSI,
         * even if the device does. */
        if (!bus_drv.platform().supports_msi())
            return ERR_NOT_SUPPORTED;

        /* TODO(johngro) : finish MSI-X implementation. */
        return ERR_NOT_SUPPORTED;

    default:
        return ERR_INVALID_ARGS;
    }

    return NO_ERROR;
}

status_t pcie_get_irq_mode_internal(const pcie_device_state_t& dev,
                                    pcie_irq_mode_info_t* out_info) {
    DEBUG_ASSERT(dev.plugged_in);
    DEBUG_ASSERT(dev.dev_lock.IsHeld());
    DEBUG_ASSERT(out_info);

    out_info->mode                = dev.irq.mode;
    out_info->max_handlers        = dev.irq.handler_count;
    out_info->registered_handlers = dev.irq.registered_handler_count;

    return NO_ERROR;
}

status_t pcie_set_irq_mode_internal(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                    pcie_irq_mode_t                          mode,
                                    uint                                     requested_irqs) {
    DEBUG_ASSERT(dev && dev->plugged_in);
    DEBUG_ASSERT(dev->dev_lock.IsHeld());

    /* Are we disabling IRQs? */
    if (mode == PCIE_IRQ_MODE_DISABLED) {
        /* If so, and we are already disabled, cool!  Run some sanity checks and we are done */
        if (dev->irq.mode == PCIE_IRQ_MODE_DISABLED) {
            DEBUG_ASSERT(!dev->irq.handlers);
            DEBUG_ASSERT(!dev->irq.handler_count);
            return NO_ERROR;
        }

        DEBUG_ASSERT(dev->irq.handlers);
        DEBUG_ASSERT(dev->irq.handler_count);

        switch (dev->irq.mode) {
        case PCIE_IRQ_MODE_LEGACY:
            DEBUG_ASSERT(list_in_list(&dev->irq.legacy.shared_handler_node));

            pcie_leave_legacy_irq_mode(dev);

            DEBUG_ASSERT(!dev->irq.registered_handler_count);
            return NO_ERROR;

        case PCIE_IRQ_MODE_MSI:
            DEBUG_ASSERT(dev->irq.msi.cfg);
            DEBUG_ASSERT(dev->irq.msi.irq_block.allocated);

            pcie_leave_msi_irq_mode(dev);

            DEBUG_ASSERT(!dev->irq.registered_handler_count);
            return NO_ERROR;

        /* Right now, there should be no way to get into MSI-X mode */
        case PCIE_IRQ_MODE_MSI_X:
            DEBUG_ASSERT(false);
            return ERR_NOT_SUPPORTED;

        default:
            /* mode is not one of the valid enum values, this should be impossible */
            DEBUG_ASSERT(false);
            return ERR_INTERNAL;
        }
    }

    /* We are picking an active IRQ mode, sanity check the args */
    if (requested_irqs < 1)
        return ERR_INVALID_ARGS;

    /* If we are picking an active IRQ mode, we need to currently be in the
     * disabled state */
    if (dev->irq.mode != PCIE_IRQ_MODE_DISABLED)
        return ERR_BAD_STATE;

    switch (mode) {
    case PCIE_IRQ_MODE_LEGACY: return pcie_enter_legacy_irq_mode(dev, requested_irqs);
    case PCIE_IRQ_MODE_MSI:    return pcie_enter_msi_irq_mode   (dev, requested_irqs);
    case PCIE_IRQ_MODE_MSI_X:  return ERR_NOT_SUPPORTED;
    default:                   return ERR_INVALID_ARGS;
    }
}

status_t pcie_register_irq_handler_internal(const mxtl::RefPtr<pcie_device_state_t>&  dev,
                                            uint                                      irq_id,
                                            pcie_irq_handler_fn_t                     handler,
                                            void*                                     ctx) {
    DEBUG_ASSERT(dev && dev->plugged_in);
    DEBUG_ASSERT(dev->dev_lock.IsHeld());

    /* Cannot register a handler if we are currently disabled */
    if (dev->irq.mode == PCIE_IRQ_MODE_DISABLED)
        return ERR_BAD_STATE;

    DEBUG_ASSERT(dev->irq.handlers);
    DEBUG_ASSERT(dev->irq.handler_count);

    /* Make sure that the IRQ ID is within range */
    if (irq_id >= dev->irq.handler_count)
        return ERR_INVALID_ARGS;

    /* Looks good, register (or unregister the handler) and we are done. */
    pcie_irq_handler_state_t* hstate = &dev->irq.handlers[irq_id];

    /* Update our registered handler bookkeeping.  Perform some sanity checks as we do so */
    if (hstate->handler) {
        DEBUG_ASSERT(dev->irq.registered_handler_count);
        if (!handler)
            dev->irq.registered_handler_count--;
    } else {
        if (handler)
            dev->irq.registered_handler_count++;
    }
    DEBUG_ASSERT(dev->irq.registered_handler_count <= dev->irq.handler_count);

    {
        AutoSpinLockIrqSave handler_lock(hstate->lock);
        hstate->handler = handler;
        hstate->ctx     = handler ? ctx : NULL;
    }

    return NO_ERROR;
}

status_t pcie_mask_unmask_irq_internal(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                       uint irq_id,
                                       bool mask) {
    DEBUG_ASSERT(dev && dev->plugged_in);
    DEBUG_ASSERT(dev->dev_lock.IsHeld());

    /* Cannot manipulate mask status while in the DISABLED state */
    if (dev->irq.mode == PCIE_IRQ_MODE_DISABLED)
        return ERR_BAD_STATE;

    DEBUG_ASSERT(dev->irq.handlers);
    DEBUG_ASSERT(dev->irq.handler_count);

    /* Make sure that the IRQ ID is within range */
    if (irq_id >= dev->irq.handler_count)
        return ERR_INVALID_ARGS;

    /* If we are unmasking (enabling), then we need to make sure that there is a
     * handler in place for the IRQ we are enabling. */
    pcie_irq_handler_state_t* hstate = &dev->irq.handlers[irq_id];
    if (!mask && !hstate->handler)
        return ERR_BAD_STATE;

    /* OK, everything looks good.  Go ahead and make the change based on the
     * mode we are curently in. */
    switch (dev->irq.mode) {
    case PCIE_IRQ_MODE_LEGACY: return pcie_mask_unmask_legacy_irq(dev, mask);
    case PCIE_IRQ_MODE_MSI:    return pcie_mask_unmask_msi_irq(dev, irq_id, mask);
    case PCIE_IRQ_MODE_MSI_X:  return ERR_NOT_SUPPORTED;
    default:
        DEBUG_ASSERT(false); /* This should be un-possible! */
        return ERR_INTERNAL;
    }

    return NO_ERROR;
}

/******************************************************************************
 *
 * Kernel API; prototypes in dev/pcie_irqs.h
 *
 ******************************************************************************/
status_t pcie_query_irq_mode_capabilities(const pcie_device_state_t& dev,
                                          pcie_irq_mode_t mode,
                                          pcie_irq_mode_caps_t* out_caps) {
    if (!out_caps)
        return ERR_INVALID_ARGS;

    AutoLock dev_lock(dev.dev_lock);

    return (dev.plugged_in && !dev.disabled)
        ? pcie_query_irq_mode_capabilities_internal(dev, mode, out_caps)
        : ERR_BAD_STATE;
}

status_t pcie_get_irq_mode(const pcie_device_state_t& dev,
                           pcie_irq_mode_info_t* out_info) {
    if (!out_info)
        return ERR_INVALID_ARGS;

    AutoLock dev_lock(dev.dev_lock);

    return (dev.plugged_in && !dev.disabled)
        ? pcie_get_irq_mode_internal(dev, out_info)
        : ERR_BAD_STATE;
}

status_t pcie_set_irq_mode(const mxtl::RefPtr<pcie_device_state_t>& dev,
                           pcie_irq_mode_t                          mode,
                           uint                                     requested_irqs) {
    DEBUG_ASSERT(dev);

    AutoLock dev_lock(dev->dev_lock);
    return ((mode == PCIE_IRQ_MODE_DISABLED) || (dev->plugged_in && !dev->disabled))
        ? pcie_set_irq_mode_internal(dev, mode, requested_irqs)
        : ERR_BAD_STATE;
}

status_t pcie_register_irq_handler(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                   uint                                     irq_id,
                                   pcie_irq_handler_fn_t                    handler,
                                   void*                                    ctx) {
    DEBUG_ASSERT(dev);

    AutoLock dev_lock(dev->dev_lock);
    return (dev->plugged_in && !dev->disabled)
        ? pcie_register_irq_handler_internal(dev, irq_id, handler, ctx)
        : ERR_BAD_STATE;
}

status_t pcie_mask_unmask_irq(const mxtl::RefPtr<pcie_device_state_t>& dev,
                              uint                                     irq_id,
                              bool                                     mask) {
    DEBUG_ASSERT(dev);

    AutoLock dev_lock(dev->dev_lock);

    return (mask || (dev->plugged_in && !dev->disabled))
        ? pcie_mask_unmask_irq_internal(dev, irq_id, mask)
        : ERR_BAD_STATE;
}

/******************************************************************************
 *
 * Internal API; prototypes in pcie_priv.h
 *
 ******************************************************************************/
status_t pcie_init_device_irq_state(const mxtl::RefPtr<pcie_device_state_t>& dev,
                                    const mxtl::RefPtr<pcie_bridge_state_t>& upstream) {
    DEBUG_ASSERT(dev);
    DEBUG_ASSERT(dev->cfg);
    DEBUG_ASSERT(!dev->irq.legacy.pin);
    DEBUG_ASSERT(dev->irq.legacy.shared_handler == nullptr);
    DEBUG_ASSERT(dev->dev_lock.IsHeld());

    // Make certain that the device's legacy IRQ (if any) has been disabled.
    uint16_t cmd_reg = pcie_read16(&dev->cfg->base.command);
    cmd_reg = static_cast<uint16_t>(cmd_reg | PCIE_CFG_COMMAND_INT_DISABLE);
    pcie_write16(&dev->cfg->base.command, cmd_reg);

    dev->irq.legacy.pin = pcie_read8(&dev->cfg->base.interrupt_pin);
    if (dev->irq.legacy.pin) {
        uint irq_id;

        irq_id = dev->bus_drv.MapPinToIrq(dev.get(), upstream.get());
        dev->irq.legacy.shared_handler = dev->bus_drv.FindLegacyIrqHandler(irq_id);

        if (dev->irq.legacy.shared_handler == nullptr) {
            TRACEF("Failed to find or create shared legacy IRQ handler for "
                   "dev %02x:%02x.%01x (pin %u, irq id %u)\n",
                   dev->bus_id, dev->dev_id, dev->func_id,
                   dev->irq.legacy.pin, irq_id);
            return ERR_NO_RESOURCES;
        }
    }

    return NO_ERROR;
}

void PcieBusDriver::ShutdownIrqs() {
    /* Shut off all of our legacy IRQs and free all of our bookkeeping */
    AutoLock lock(legacy_irq_list_lock_);
    legacy_irq_list_.clear();
}

mxtl::RefPtr<SharedLegacyIrqHandler> PcieBusDriver::FindLegacyIrqHandler(uint irq_id) {
    /* Search to see if we have already created a shared handler for this system
     * level IRQ id already */
    AutoLock lock(legacy_irq_list_lock_);

    auto iter = legacy_irq_list_.begin();
    while (iter != legacy_irq_list_.end()) {
        if (irq_id == iter->irq_id())
            return iter.CopyPointer();
        ++iter;
    }

    auto handler = SharedLegacyIrqHandler::Create(irq_id);
    if (handler != nullptr)
        legacy_irq_list_.push_front(handler);

    return handler;
}
