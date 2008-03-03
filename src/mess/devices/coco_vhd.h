/***************************************************************************

	coco_vhd.h

	Color Computer Virtual Hard Drives

***************************************************************************/

#ifndef COCOVHD_H
#define COCOVHD_H

#include "device.h"
#include "image.h"


READ8_HANDLER(coco_vhd_io_r);
WRITE8_HANDLER(coco_vhd_io_w);

void coco_vhd_device_getinfo(const mess_device_class *devclass, UINT32 state, union devinfo *info);


#endif /* COCOVHD_H */
