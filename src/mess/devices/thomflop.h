/**********************************************************************

  Copyright (C) Antoine Mine' 2006

  Thomson 8-bit computers

**********************************************************************/

extern UINT8 to7_controller_type; /* set during init */
extern UINT8 to7_floppy_bank;

/* number of external floppy controller ROM banks */
#define TO7_NB_FLOP_BANK 9

/* external floppy / network controller active */
#define THOM_FLOPPY_EXT (to7_controller_type >= 1)

/* internal floppy controller active (no or network extension) */
#define THOM_FLOPPY_INT (to7_controller_type == 0 || to7_controller_type > 4)


/* external controllers */
extern void to7_floppy_init  ( void* base );
extern void to7_floppy_reset ( void );
extern READ8_HANDLER  ( to7_floppy_r );
extern WRITE8_HANDLER ( to7_floppy_w );

/* TO9 internal (WD2793) & external controllers */
extern void to9_floppy_init  ( void* int_base, void* ext_base );
extern void to9_floppy_reset ( void );
extern READ8_HANDLER  ( to9_floppy_r );
extern WRITE8_HANDLER ( to9_floppy_w );

/* TO8 internal (THMFC1) controller */
extern void thmfc_floppy_init  ( void );
extern void thmfc_floppy_reset ( void );
extern READ8_HANDLER  ( thmfc_floppy_r );
extern WRITE8_HANDLER ( thmfc_floppy_w );

