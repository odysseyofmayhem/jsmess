/*
    Nintendo 64

    Driver by Ville Linde
    Reformatted to share hardware by R. Belmont
    Additional work by Ryan Holtz
*/

#include "driver.h"
#include "cpu/mips/mips3.h"
#include "sound/custom.h"
#include "devices/cartslot.h"
#include "streams.h"
#include "includes/n64.h"

static ADDRESS_MAP_START( n64_map, ADDRESS_SPACE_PROGRAM, 32 )
	AM_RANGE(0x00000000, 0x007fffff) AM_RAM	AM_BASE(&rdram)				// RDRAM
	AM_RANGE(0x04000000, 0x04000fff) AM_RAM AM_SHARE(1)				// RSP DMEM
	AM_RANGE(0x04001000, 0x04001fff) AM_RAM AM_SHARE(2)				// RSP IMEM
	AM_RANGE(0x04040000, 0x040fffff) AM_READWRITE(n64_sp_reg_r, n64_sp_reg_w)	// RSP
	AM_RANGE(0x04100000, 0x041fffff) AM_READWRITE(n64_dp_reg_r, n64_dp_reg_w)	// RDP
	AM_RANGE(0x04300000, 0x043fffff) AM_READWRITE(n64_mi_reg_r, n64_mi_reg_w)	// MIPS Interface
	AM_RANGE(0x04400000, 0x044fffff) AM_READWRITE(n64_vi_reg_r, n64_vi_reg_w)	// Video Interface
	AM_RANGE(0x04500000, 0x045fffff) AM_READWRITE(n64_ai_reg_r, n64_ai_reg_w)	// Audio Interface
	AM_RANGE(0x04600000, 0x046fffff) AM_READWRITE(n64_pi_reg_r, n64_pi_reg_w)	// Peripheral Interface
	AM_RANGE(0x04700000, 0x047fffff) AM_READWRITE(n64_ri_reg_r, n64_ri_reg_w)	// RDRAM Interface
	AM_RANGE(0x04800000, 0x048fffff) AM_READWRITE(n64_si_reg_r, n64_si_reg_w)	// Serial Interface
	AM_RANGE(0x08000000, 0x08007fff) AM_RAM										// Cartridge SRAM
	AM_RANGE(0x10000000, 0x13ffffff) AM_ROM AM_REGION(REGION_USER2, 0)	// Cartridge
	AM_RANGE(0x1fc00000, 0x1fc007bf) AM_ROM AM_REGION(REGION_USER1, 0)	// PIF ROM
	AM_RANGE(0x1fc007c0, 0x1fc007ff) AM_READWRITE(n64_pif_ram_r, n64_pif_ram_w)
ADDRESS_MAP_END

static ADDRESS_MAP_START( rsp_map, ADDRESS_SPACE_PROGRAM, 32 )
	AM_RANGE(0x04000000, 0x04000fff) AM_RAM AM_BASE(&rsp_dmem) AM_SHARE(1)
	AM_RANGE(0x04001000, 0x04001fff) AM_RAM AM_BASE(&rsp_imem) AM_SHARE(2)
ADDRESS_MAP_END

static INPUT_PORTS_START( n64 )
	PORT_START_TAG("P1")
		PORT_BIT( 0x8000, IP_ACTIVE_HIGH, IPT_BUTTON1 )        PORT_PLAYER(1) PORT_NAME("Button A")
		PORT_BIT( 0x4000, IP_ACTIVE_HIGH, IPT_BUTTON2 )        PORT_PLAYER(1) PORT_NAME("Button B")
		PORT_BIT( 0x2000, IP_ACTIVE_HIGH, IPT_BUTTON3 )        PORT_PLAYER(1) PORT_NAME("Button Z")
		PORT_BIT( 0x1000, IP_ACTIVE_HIGH, IPT_START )          PORT_PLAYER(1) PORT_NAME("Start")
		PORT_BIT( 0x0800, IP_ACTIVE_HIGH, IPT_JOYSTICK_UP )    PORT_PLAYER(1) PORT_NAME("Joypad \xE2\x86\x91") /* Up */
		PORT_BIT( 0x0400, IP_ACTIVE_HIGH, IPT_JOYSTICK_DOWN )  PORT_PLAYER(1) PORT_NAME("Joypad \xE2\x86\x93") /* Down */
		PORT_BIT( 0x0200, IP_ACTIVE_HIGH, IPT_JOYSTICK_LEFT )  PORT_PLAYER(1) PORT_NAME("Joypad \xE2\x86\x90") /* Left */
		PORT_BIT( 0x0100, IP_ACTIVE_HIGH, IPT_JOYSTICK_RIGHT ) PORT_PLAYER(1) PORT_NAME("Joypad \xE2\x86\x92") /* Right */
		PORT_BIT( 0x00c0, IP_ACTIVE_HIGH, IPT_UNUSED )
		PORT_BIT( 0x0020, IP_ACTIVE_HIGH, IPT_BUTTON4 )        PORT_PLAYER(1) PORT_NAME("Button L")
		PORT_BIT( 0x0010, IP_ACTIVE_HIGH, IPT_BUTTON5 )        PORT_PLAYER(1) PORT_NAME("Button R")
		PORT_BIT( 0x0008, IP_ACTIVE_HIGH, IPT_BUTTON6 )        PORT_PLAYER(1) PORT_NAME("Button C \xE2\x86\x91") /* Up */
		PORT_BIT( 0x0004, IP_ACTIVE_HIGH, IPT_BUTTON7 )        PORT_PLAYER(1) PORT_NAME("Button C \xE2\x86\x93") /* Down */
		PORT_BIT( 0x0002, IP_ACTIVE_HIGH, IPT_BUTTON8 )        PORT_PLAYER(1) PORT_NAME("Button C \xE2\x86\x90") /* Left */
		PORT_BIT( 0x0001, IP_ACTIVE_HIGH, IPT_BUTTON9 )        PORT_PLAYER(1) PORT_NAME("Button C \xE2\x86\x92") /* Right */

	PORT_START_TAG("P1_ANALOG_X")
		PORT_BIT( 0xff, 0x80, IPT_AD_STICK_X ) PORT_MINMAX(0x00,0xff) PORT_SENSITIVITY(30) PORT_KEYDELTA(30) PORT_PLAYER(1)

	PORT_START_TAG("P1_ANALOG_Y")
		PORT_BIT( 0xff, 0x80, IPT_AD_STICK_Y ) PORT_MINMAX(0xff,0x00) PORT_SENSITIVITY(30) PORT_KEYDELTA(30) PORT_PLAYER(1)
INPUT_PORTS_END

/* ?? */
static const struct mips3_config config =
{
	16384,				/* code cache size */
	8192,				/* data cache size */
	62500000			/* system clock */
};

static INTERRUPT_GEN( n64_vblank )
{
	signal_rcp_interrupt(VI_INTERRUPT);
}

static MACHINE_RESET( n64 )
{
	n64_machine_reset();
}

static MACHINE_DRIVER_START( n64 )
	/* basic machine hardware */
	MDRV_CPU_ADD(R4600BE, 93750000)
	MDRV_CPU_CONFIG(config)
	MDRV_CPU_PROGRAM_MAP(n64_map, 0)
	MDRV_CPU_VBLANK_INT( n64_vblank, 1 )

	MDRV_CPU_ADD(RSP, 62500000)
	MDRV_CPU_CONFIG(n64_rsp_config)
	MDRV_CPU_PROGRAM_MAP(rsp_map, 0)

	MDRV_MACHINE_RESET( n64 )
	MDRV_INTERLEAVE(10)

	/* video hardware */
	MDRV_SCREEN_ADD("main", RASTER)
	MDRV_SCREEN_REFRESH_RATE(60)
	MDRV_SCREEN_VBLANK_TIME(ATTOSECONDS_IN_USEC(0))
	MDRV_SCREEN_FORMAT(BITMAP_FORMAT_RGB32)
	MDRV_SCREEN_SIZE(640, 525)
	MDRV_SCREEN_VISIBLE_AREA(0, 639, 0, 239)
	MDRV_PALETTE_LENGTH(0x1000)

	MDRV_VIDEO_START(n64)
	MDRV_VIDEO_UPDATE(n64)

	MDRV_SPEAKER_STANDARD_STEREO("left", "right")

	MDRV_SOUND_ADD(DMADAC, 0)
	MDRV_SOUND_ROUTE(ALL_OUTPUTS, "left", 1.0)
	MDRV_SOUND_ADD(DMADAC, 0)
	MDRV_SOUND_ROUTE(ALL_OUTPUTS, "right", 1.0)
MACHINE_DRIVER_END

#ifdef UNUSED_FUNCTION
DRIVER_INIT( n64 )
{
}
#endif

ROM_START( n64)
    ROM_REGION( 0x800000, REGION_CPU1, ROMREGION_ERASEFF )      /* dummy region for R4300 */
    ROM_REGION32_BE( 0x800, REGION_USER1, 0 )
    ROM_LOAD( "pifdata.bin", 0x0000, 0x0800, CRC(5ec82be9) SHA1(9174eadc0f0ea2654c95fd941406ab46b9dc9bdd) )
    ROM_REGION32_BE( 0x4000000, REGION_USER2, ROMREGION_ERASEFF)
ROM_END

static DEVICE_LOAD(n64_cart)
{
	int i, length;
	UINT8 *cart = memory_region(REGION_USER2);

	length = image_fread(image, cart, 0x4000000);

	if (cart[0] == 0x37 && cart[1] == 0x80)
	{
		for (i=0; i < length; i+=4)
		{
			UINT8 b1 = cart[i+0];
			UINT8 b2 = cart[i+1];
			UINT8 b3 = cart[i+2];
			UINT8 b4 = cart[i+3];
			cart[i+0] = b3;
			cart[i+1] = b4;
			cart[i+2] = b1;
			cart[i+3] = b2;
		}
	}
	else
	{
		for (i=0; i < length; i+=4)
		{
			UINT8 b1 = cart[i+0];
			UINT8 b2 = cart[i+1];
			UINT8 b3 = cart[i+2];
			UINT8 b4 = cart[i+3];
			cart[i+0] = b4;
			cart[i+1] = b3;
			cart[i+2] = b2;
			cart[i+3] = b1;
		}
	}

	logerror("cart length = %d\n", length);
	return INIT_PASS;
}

static void n64_cartslot_getinfo(const mess_device_class *devclass, UINT32 state, union devinfo *info)
{
	/* cartslot */
	switch(state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case MESS_DEVINFO_INT_COUNT:							info->i = 1; break;
		case MESS_DEVINFO_INT_MUST_BE_LOADED:				info->i = 1; break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case MESS_DEVINFO_PTR_LOAD:							info->load = device_load_n64_cart; break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case MESS_DEVINFO_STR_FILE_EXTENSIONS:				strcpy(info->s = device_temp_str(), "v64,z64,rom,n64,bin"); break;

		default:										cartslot_device_getinfo(devclass, state, info); break;
	}
}

SYSTEM_CONFIG_START(n64)
	CONFIG_DEVICE(n64_cartslot_getinfo)
SYSTEM_CONFIG_END


CONS(1996, n64, 	0,		0,		n64, 	n64, 	0,	n64,	"Nintendo", "Nintendo 64", 0 )
