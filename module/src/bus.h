/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _SURFACE_AGGREGATOR_BUS_H
#define _SURFACE_AGGREGATOR_BUS_H

#include "../include/linux/surface_aggregator_module.h"


void ssam_controller_remove_clients(struct ssam_controller *ctrl);

int ssam_bus_register(void);
void ssam_bus_unregister(void);

#endif /* _SURFACE_AGGREGATOR_BUS_H */
