/***************************************************************************

    ppcdrc.c

    Universal machine language-based PowerPC emulator.

    Copyright Aaron Giles
    Released for general non-commercial use under the MAME license
    Visit http://mamedev.org for licensing and usage restrictions.

****************************************************************************

    Future improvements/changes:

    * crxor a,a,a / creqv a,a,a / cror a,a,a

***************************************************************************/

#include "emu.h"
#include "debugger.h"
#include "ppccom.h"
#include "ppcfe.h"
#include "cpu/drcfe.h"
#include "cpu/drcuml.h"
#include "cpu/drcumlsh.h"

using namespace uml;

extern offs_t ppc_dasm_one(char *buffer, UINT32 pc, UINT32 op);



/***************************************************************************
    DEBUGGING
***************************************************************************/

#define FORCE_C_BACKEND					(0)
#define LOG_UML							(0)
#define LOG_NATIVE						(0)

#define DISABLE_FLAG_OPTIMIZATIONS		(0)
#define DISABLE_FAST_REGISTERS			(0)
#define SINGLE_INSTRUCTION_MODE			(0)

#define PRINTF_EXCEPTIONS				(0)
#define PRINTF_MMU						(0)

#define PROBE_ADDRESS					~0



/***************************************************************************
    CONSTANTS
***************************************************************************/

/* map variables */
#define MAPVAR_PC						M0
#define MAPVAR_CYCLES					M1
#define MAPVAR_DSISR					M2

/* mode bits */
#define MODE_LITTLE_ENDIAN				0x01
#define MODE_DATA_TRANSLATION			0x02		/* OEA */
#define MODE_PROTECTION					0x02		/* 4XX */
#define MODE_USER						0x04

/* size of the execution code cache */
#define CACHE_SIZE						(32 * 1024 * 1024)

/* compilation boundaries -- how far back/forward does the analysis extend? */
#define COMPILE_BACKWARDS_BYTES			128
#define COMPILE_FORWARDS_BYTES			512
#define COMPILE_MAX_INSTRUCTIONS		((COMPILE_BACKWARDS_BYTES/4) + (COMPILE_FORWARDS_BYTES/4))
#define COMPILE_MAX_SEQUENCE			64

/* exit codes */
#define EXECUTE_OUT_OF_CYCLES			0
#define EXECUTE_MISSING_CODE			1
#define EXECUTE_UNMAPPED_CODE			2
#define EXECUTE_RESET_CACHE				3



/***************************************************************************
    MACROS
***************************************************************************/

#define R32(reg)				ppc->impstate->regmap[reg]
#define R32Z(reg)				(((reg) == 0) ? parameter(0) : ppc->impstate->regmap[reg])
#define F64(reg)				ppc->impstate->fdregmap[reg]
#define CR32(reg)				mem(&ppc->cr[reg])
#define FPSCR32					mem(&ppc->fpscr)
#define MSR32					mem(&ppc->msr)
#define XERSO32					mem(&ppc->xerso)
#define SR32(reg)				mem(&ppc->sr[reg])
#define SPR32(reg)				mem(&ppc->spr[reg])

#define CRMASK(reg)				(0xf0000000 >> ((reg) * 4))

/* DSISR values for various addressing types */
#define DSISR_IMM(op)	((0) |										/* bits 15-16: cleared */			\
						 ((((op) >> (31- 5)) & 0x01) << (31-17)) |	/* bit  17:    opcode bit 5 */		\
						 ((((op) >> (31- 4)) & 0x0f) << (31-21)) |	/* bits 18-21: opcode bits 1-4 */	\
						 ((((op) >> (31-10)) & 0x1f) << (31-26)) |	/* bits 22-26: opcode bits 6-10 */	\
						 (0))										/* bits 27-31: undefined */

#define DSISR_IMMU(op)	((0) |										/* bits 15-16: cleared */			\
						 ((((op) >> (31- 5)) & 0x01) << (31-17)) |	/* bit  17:    opcode bit 5 */		\
						 ((((op) >> (31- 4)) & 0x0f) << (31-21)) |	/* bits 18-21: opcode bits 1-4 */	\
						 ((((op) >> (31-10)) & 0x1f) << (31-26)) |	/* bits 22-26: opcode bits 6-10 */	\
						 ((((op) >> (31-15)) & 0x1f) << (31-31)))	/* bits 27-31: opcode bits 11-15 */

#define DSISR_IDX(op)	(((((op) >> (31-30)) & 0x03) << (31-16)) |	/* bits 15-16: opcode bits 29-30 */	\
						 ((((op) >> (31-25)) & 0x01) << (31-17)) |	/* bit  17:    opcode bit 25 */		\
						 ((((op) >> (31-24)) & 0x0f) << (31-21)) |	/* bits 18-21: opcode bits 21-24 */	\
						 ((((op) >> (31-10)) & 0x1f) << (31-26)) |	/* bits 22-26: opcode bits 6-10 */	\
						 (0))										/* bits 27-31: undefined */

#define DSISR_IDXU(op)	(((((op) >> (31-30)) & 0x03) << (31-16)) |	/* bits 15-16: opcode bits 29-30 */	\
						 ((((op) >> (31-25)) & 0x01) << (31-17)) |	/* bit  17:    opcode bit 25 */		\
						 ((((op) >> (31-24)) & 0x0f) << (31-21)) |	/* bits 18-21: opcode bits 21-24 */	\
						 ((((op) >> (31-10)) & 0x1f) << (31-26)) |	/* bits 22-26: opcode bits 6-10 */	\
						 ((((op) >> (31-15)) & 0x1f) << (31-31)))	/* bits 27-31: opcode bits 11-15 */



/***************************************************************************
    STRUCTURES & TYPEDEFS
***************************************************************************/

/* fast RAM info */
typedef struct _fast_ram_info fast_ram_info;
struct _fast_ram_info
{
	offs_t				start;						/* start of the RAM block */
	offs_t				end;						/* end of the RAM block */
	UINT8				readonly;					/* TRUE if read-only */
	void *				base;						/* base in memory where the RAM lives */
};


/* hotspot info */
typedef struct _hotspot_info hotspot_info;
struct _hotspot_info
{
	offs_t				pc;							/* PC to consider */
	UINT32				opcode;						/* required opcode at that PC */
	UINT32				cycles;						/* number of cycles to eat when hit */
};


/* internal compiler state */
typedef struct _compiler_state compiler_state;
struct _compiler_state
{
	UINT32				cycles;						/* accumulated cycles */
	UINT8				checkints;					/* need to check interrupts before next instruction */
	UINT8				checksoftints;				/* need to check software interrupts before next instruction */
	code_label	labelnum;					/* index for local labels */
};


/* PowerPC implementation state */
struct _ppcimp_state
{
	/* core state */
	drc_cache *			cache;						/* pointer to the DRC code cache */
	drcuml_state *		drcuml;						/* DRC UML generator state */
	ppc_frontend *		drcfe;						/* pointer to the DRC front-end state */
	UINT32				drcoptions;					/* configurable DRC options */

	/* parameters for subroutines */
	UINT32				mode;						/* current global mode */
	const char *		format;						/* format string for printing */
	UINT32				arg0;						/* print_debug argument 1 */
	UINT32				arg1;						/* print_debug argument 2 */
	UINT32				updateaddr;					/* update address storage */
	UINT32				swcount;					/* counter for sw instructions */
	UINT32				tempaddr;					/* temporary address storage */
	drcuml_ireg			tempdata;					/* temporary data storage */
	double				fp0;						/* floating point 0 */

	/* tables */
	UINT8				fpmode[4];					/* FPU mode table */
	UINT8				sz_cr_table[32];			/* SZ CR table */
	UINT8				cmp_cr_table[32];			/* CMP CR table */
	UINT8				cmpl_cr_table[32];			/* CMPL CR table */
	UINT8				fcmp_cr_table[32];			/* FCMP CR table */

	/* internal stuff */
	UINT8				cache_dirty;				/* true if we need to flush the cache */

	/* register mappings */
	parameter	regmap[32];					/* parameter to register mappings for all 32 integer registers */
	parameter	fdregmap[32];				/* parameter to register mappings for all 32 floating point registers */

	/* subroutines */
	code_handle *	entry;						/* entry point */
	code_handle *	nocode;						/* nocode exception handler */
	code_handle *	out_of_cycles;				/* out of cycles exception handler */
	code_handle *	tlb_mismatch;				/* tlb mismatch handler */
	code_handle *	swap_tgpr;					/* swap TGPR handler */
	code_handle *	lsw[8][32];					/* lsw entries */
	code_handle *	stsw[8][32];				/* stsw entries */
	code_handle *	read8[8];					/* read byte */
	code_handle *	write8[8];					/* write byte */
	code_handle *	read16[8];					/* read half */
	code_handle *	read16mask[8];				/* read half */
	code_handle *	write16[8];					/* write half */
	code_handle *	write16mask[8];				/* write half */
	code_handle *	read32[8];					/* read word */
	code_handle *	read32align[8];				/* read word aligned */
	code_handle *	read32mask[8];				/* read word */
	code_handle *	write32[8];					/* write word */
	code_handle *	write32align[8];			/* write word aligned */
	code_handle *	write32mask[8];				/* write word */
	code_handle *	read64[8];					/* read double */
	code_handle *	read64mask[8];				/* read double */
	code_handle *	write64[8];					/* write double */
	code_handle *	write64mask[8];				/* write double */
	code_handle *	exception[EXCEPTION_COUNT];	/* array of exception handlers */
	code_handle *	exception_norecover[EXCEPTION_COUNT];	/* array of exception handlers */

	/* fast RAM */
	UINT32				fastram_select;
	fast_ram_info		fastram[PPC_MAX_FASTRAM];

	/* hotspots */
	UINT32				hotspot_select;
	hotspot_info		hotspot[PPC_MAX_HOTSPOTS];
};



/***************************************************************************
    FUNCTION PROTOTYPES
***************************************************************************/

static void code_flush_cache(powerpc_state *ppc);
static void code_compile_block(powerpc_state *ppc, UINT8 mode, offs_t pc);

static void cfunc_printf_exception(void *param);
static void cfunc_printf_probe(void *param);

static void static_generate_entry_point(powerpc_state *ppc);
static void static_generate_nocode_handler(powerpc_state *ppc);
static void static_generate_out_of_cycles(powerpc_state *ppc);
static void static_generate_tlb_mismatch(powerpc_state *ppc);
static void static_generate_exception(powerpc_state *ppc, UINT8 exception, int recover, const char *name);
static void static_generate_memory_accessor(powerpc_state *ppc, int mode, int size, int iswrite, int ismasked, const char *name, code_handle *&handleptr, code_handle *masked);
static void static_generate_swap_tgpr(powerpc_state *ppc);
static void static_generate_lsw_entries(powerpc_state *ppc, int mode);
static void static_generate_stsw_entries(powerpc_state *ppc, int mode);

static void generate_update_mode(powerpc_state *ppc, drcuml_block *block);
static void generate_update_cycles(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, parameter, int allow_exception);
static void generate_checksum_block(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *seqhead, const opcode_desc *seqlast);
static void generate_sequence_instruction(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);
static int generate_opcode(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);
static int generate_instruction_13(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);
static int generate_instruction_1f(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);
static int generate_instruction_3b(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);
static int generate_instruction_3f(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc);

static void log_add_disasm_comment(drcuml_block *block, UINT32 pc, UINT32 op);
static const char *log_desc_flags_to_string(UINT32 flags);
static void log_register_list(drcuml_state *drcuml, const char *string, const UINT32 *reglist, const UINT32 *regnostarlist);
static void log_opcode_desc(drcuml_state *drcuml, const opcode_desc *desclist, int indent);



/***************************************************************************
    PRIVATE GLOBAL VARIABLES
***************************************************************************/

/* lookup table for FP modes */
static const UINT8 fpmode_source[4] =
{
	ROUND_ROUND,
	ROUND_TRUNC,
	ROUND_CEIL,
	ROUND_FLOOR
};

/* flag lookup table for SZ */
static const UINT8 sz_cr_table_source[32] =
{
	/* ..... */	0x4,
	/* ....C */	0x4,
	/* ...V. */	0x4,
	/* ...VC */	0x4,
	/* ..Z.. */	0x2,
	/* ..Z.C */	0x2,
	/* ..ZV. */	0x2,
	/* ..ZVC */	0x2,
	/* .S... */	0x8,
	/* .S..C */	0x8,
	/* .S.V. */	0x8,
	/* .S.VC */	0x8,
	/* .SZ.. */	0x2,
	/* .SZ.C */	0x2,
	/* .SZV. */	0x2,
	/* .SZVC */	0x2,
	/* U.... */	0x4,
	/* U...C */	0x4,
	/* U..V. */	0x4,
	/* U..VC */	0x4,
	/* U.Z.. */	0x2,
	/* U.Z.C */	0x2,
	/* U.ZV. */	0x2,
	/* U.ZVC */	0x2,
	/* US... */	0x8,
	/* US..C */	0x8,
	/* US.V. */	0x8,
	/* US.VC */	0x8,
	/* USZ.. */	0x2,
	/* USZ.C */	0x2,
	/* USZV. */	0x2,
	/* USZVC */	0x2
};

/* flag lookup table for CMP */
static const UINT8 cmp_cr_table_source[32] =
{
	/* ..... */	0x4,
	/* ....C */	0x4,
	/* ...V. */	0x8,
	/* ...VC */	0x8,
	/* ..Z.. */	0x2,
	/* ..Z.C */	0x2,
	/* ..ZV. */	0x2,
	/* ..ZVC */	0x2,
	/* .S... */	0x8,
	/* .S..C */	0x8,
	/* .S.V. */	0x4,
	/* .S.VC */	0x4,
	/* .SZ.. */	0x2,
	/* .SZ.C */	0x2,
	/* .SZV. */	0x2,
	/* .SZVC */	0x2,
	/* U.... */	0x4,
	/* U...C */	0x4,
	/* U..V. */	0x8,
	/* U..VC */	0x8,
	/* U.Z.. */	0x2,
	/* U.Z.C */	0x2,
	/* U.ZV. */	0x2,
	/* U.ZVC */	0x2,
	/* US... */	0x8,
	/* US..C */	0x8,
	/* US.V. */	0x4,
	/* US.VC */	0x4,
	/* USZ.. */	0x2,
	/* USZ.C */	0x2,
	/* USZV. */	0x2,
	/* USZVC */	0x2
};

/* flag lookup table for CMPL */
static const UINT8 cmpl_cr_table_source[32] =
{
	/* ..... */	0x4,
	/* ....C */	0x8,
	/* ...V. */	0x4,
	/* ...VC */	0x8,
	/* ..Z.. */	0x2,
	/* ..Z.C */	0x2,
	/* ..ZV. */	0x2,
	/* ..ZVC */	0x2,
	/* .S... */	0x4,
	/* .S..C */	0x8,
	/* .S.V. */	0x4,
	/* .S.VC */	0x8,
	/* .SZ.. */	0x2,
	/* .SZ.C */	0x2,
	/* .SZV. */	0x2,
	/* .SZVC */	0x2,
	/* U.... */	0x4,
	/* U...C */	0x8,
	/* U..V. */	0x4,
	/* U..VC */	0x8,
	/* U.Z.. */	0x2,
	/* U.Z.C */	0x2,
	/* U.ZV. */	0x2,
	/* U.ZVC */	0x2,
	/* US... */	0x4,
	/* US..C */	0x8,
	/* US.V. */	0x4,
	/* US.VC */	0x8,
	/* USZ.. */	0x2,
	/* USZ.C */	0x2,
	/* USZV. */	0x2,
	/* USZVC */	0x2
};

/* flag lookup table for FCMP */
static const UINT8 fcmp_cr_table_source[32] =
{
	/* ..... */	0x4,
	/* ....C */	0x8,
	/* ...V. */	0x4,
	/* ...VC */	0x8,
	/* ..Z.. */	0x2,
	/* ..Z.C */	0xa,
	/* ..ZV. */	0x2,
	/* ..ZVC */	0xa,
	/* .S... */	0x4,
	/* .S..C */	0x8,
	/* .S.V. */	0x4,
	/* .S.VC */	0x8,
	/* .SZ.. */	0x2,
	/* .SZ.C */	0xa,
	/* .SZV. */	0x2,
	/* .SZVC */	0xa,
	/* U.... */	0x5,
	/* U...C */	0x9,
	/* U..V. */	0x5,
	/* U..VC */	0x9,
	/* U.Z.. */	0x3,
	/* U.Z.C */	0xb,
	/* U.ZV. */	0x3,
	/* U.ZVC */	0xb,
	/* US... */	0x5,
	/* US..C */	0x9,
	/* US.V. */	0x5,
	/* US.VC */	0x9,
	/* USZ.. */	0x3,
	/* USZ.C */	0xb,
	/* USZV. */	0x3,
	/* USZVC */	0xb
};



/***************************************************************************
    INLINE FUNCTIONS
***************************************************************************/

INLINE powerpc_state *get_safe_token(device_t *device)
{
	assert(device != NULL);
	assert(device->type() == PPC403GA ||
		   device->type() == PPC403GCX ||
		   device->type() == PPC601 ||
		   device->type() == PPC602 ||
		   device->type() == PPC603 ||
		   device->type() == PPC603E ||
		   device->type() == PPC603R ||
		   device->type() == PPC604 ||
		   device->type() == MPC8240);
	return *(powerpc_state **)downcast<legacy_cpu_device *>(device)->token();
}

/*-------------------------------------------------
    alloc_handle - allocate a handle if not
    already allocated
-------------------------------------------------*/

INLINE void alloc_handle(drcuml_state *drcuml, code_handle **handleptr, const char *name)
{
	if (*handleptr == NULL)
		*handleptr = drcuml->handle_alloc(name);
}


/*-------------------------------------------------
    load_fast_iregs - load any fast integer
    registers
-------------------------------------------------*/

INLINE void load_fast_iregs(powerpc_state *ppc, drcuml_block *block)
{
	int regnum;

	for (regnum = 0; regnum < ARRAY_LENGTH(ppc->impstate->regmap); regnum++)
		if (ppc->impstate->regmap[regnum].is_int_register())
			UML_MOV(block, ireg(ppc->impstate->regmap[regnum].ireg() - REG_I0), mem(&ppc->r[regnum]));
}


/*-------------------------------------------------
    save_fast_iregs - save any fast integer
    registers
-------------------------------------------------*/

INLINE void save_fast_iregs(powerpc_state *ppc, drcuml_block *block)
{
	int regnum;

	for (regnum = 0; regnum < ARRAY_LENGTH(ppc->impstate->regmap); regnum++)
		if (ppc->impstate->regmap[regnum].is_int_register())
			UML_MOV(block, mem(&ppc->r[regnum]), ireg(ppc->impstate->regmap[regnum].ireg() - REG_I0));
}


/*-------------------------------------------------
    compute_rlw_mask - compute the 32-bit mask
    for an rlw* instruction
-------------------------------------------------*/

INLINE UINT32 compute_rlw_mask(UINT8 mb, UINT8 me)
{
	if (mb <= me)
		return (0xffffffff >> mb) & (0xffffffff << (31 - me));
	else
		return (0xffffffff >> mb) | (0xffffffff << (31 - me));
}


/*-------------------------------------------------
    compute_crf_mask - compute the 32-bit mask
    for a mtcrf/mfcrf instruction
-------------------------------------------------*/

INLINE UINT32 compute_crf_mask(UINT8 crm)
{
	UINT32 mask = 0;
	if (crm & 0x80) mask |= 0xf0000000;
	if (crm & 0x40) mask |= 0x0f000000;
	if (crm & 0x20) mask |= 0x00f00000;
	if (crm & 0x10) mask |= 0x000f0000;
	if (crm & 0x08) mask |= 0x0000f000;
	if (crm & 0x04) mask |= 0x00000f00;
	if (crm & 0x02) mask |= 0x000000f0;
	if (crm & 0x01) mask |= 0x0000000f;
	return mask;
}


/*-------------------------------------------------
    compute_spr - compute the SPR index from the
    SPR field of an opcode
-------------------------------------------------*/

INLINE UINT32 compute_spr(UINT32 spr)
{
	return ((spr >> 5) | (spr << 5)) & 0x3ff;
}



/***************************************************************************
    CORE CALLBACKS
***************************************************************************/

/*-------------------------------------------------
    ppcdrc_init - initialize the processor
-------------------------------------------------*/

static void ppcdrc_init(powerpc_flavor flavor, UINT8 cap, int tb_divisor, legacy_cpu_device *device, device_irq_callback irqcallback)
{
	powerpc_state *ppc;
	drcbe_info beinfo;
	UINT32 flags = 0;
	drc_cache *cache;
	int regnum;

	/* allocate enough space for the cache and the core */
	cache = auto_alloc(device->machine(), drc_cache(CACHE_SIZE + sizeof(*ppc)));

	/* allocate the core from the near cache */
	*(powerpc_state **)device->token() = ppc = (powerpc_state *)cache->alloc_near(sizeof(*ppc));
	memset(ppc, 0, sizeof(*ppc));

	/* initialize the core */
	ppccom_init(ppc, flavor, cap, tb_divisor, device, irqcallback);

	/* allocate the implementation-specific state from the full cache */
	ppc->impstate = (ppcimp_state *)cache->alloc_near(sizeof(*ppc->impstate));
	memset(ppc->impstate, 0, sizeof(*ppc->impstate));
	ppc->impstate->cache = cache;

	/* initialize the UML generator */
	if (FORCE_C_BACKEND)
		flags |= DRCUML_OPTION_USE_C;
	if (LOG_UML)
		flags |= DRCUML_OPTION_LOG_UML;
	if (LOG_NATIVE)
		flags |= DRCUML_OPTION_LOG_NATIVE;
	ppc->impstate->drcuml = auto_alloc(device->machine(), drcuml_state(*device, *cache, flags, 8, 32, 2));

	/* add symbols for our stuff */
	ppc->impstate->drcuml->symbol_add(&ppc->pc, sizeof(ppc->pc), "pc");
	ppc->impstate->drcuml->symbol_add(&ppc->icount, sizeof(ppc->icount), "icount");
	for (regnum = 0; regnum < 32; regnum++)
	{
		char buf[10];
		sprintf(buf, "r%d", regnum);
		ppc->impstate->drcuml->symbol_add(&ppc->r[regnum], sizeof(ppc->r[regnum]), buf);
		sprintf(buf, "fpr%d", regnum);
		ppc->impstate->drcuml->symbol_add(&ppc->f[regnum], sizeof(ppc->r[regnum]), buf);
	}
	for (regnum = 0; regnum < 8; regnum++)
	{
		char buf[10];
		sprintf(buf, "cr%d", regnum);
		ppc->impstate->drcuml->symbol_add(&ppc->cr[regnum], sizeof(ppc->cr[regnum]), buf);
	}
	ppc->impstate->drcuml->symbol_add(&ppc->xerso, sizeof(ppc->xerso), "xerso");
	ppc->impstate->drcuml->symbol_add(&ppc->fpscr, sizeof(ppc->fpscr), "fpscr");
	ppc->impstate->drcuml->symbol_add(&ppc->msr, sizeof(ppc->msr), "msr");
	ppc->impstate->drcuml->symbol_add(&ppc->sr, sizeof(ppc->sr), "sr");
	ppc->impstate->drcuml->symbol_add(&ppc->spr[SPR_XER], sizeof(ppc->spr[SPR_XER]), "xer");
	ppc->impstate->drcuml->symbol_add(&ppc->spr[SPR_LR], sizeof(ppc->spr[SPR_LR]), "lr");
	ppc->impstate->drcuml->symbol_add(&ppc->spr[SPR_CTR], sizeof(ppc->spr[SPR_CTR]), "ctr");
	ppc->impstate->drcuml->symbol_add(&ppc->spr, sizeof(ppc->spr), "spr");
	ppc->impstate->drcuml->symbol_add(&ppc->dcr, sizeof(ppc->dcr), "dcr");
	ppc->impstate->drcuml->symbol_add(&ppc->param0, sizeof(ppc->param0), "param0");
	ppc->impstate->drcuml->symbol_add(&ppc->param1, sizeof(ppc->param1), "param1");
	ppc->impstate->drcuml->symbol_add(&ppc->irq_pending, sizeof(ppc->irq_pending), "irq_pending");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->mode, sizeof(ppc->impstate->mode), "mode");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->arg0, sizeof(ppc->impstate->arg0), "arg0");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->arg1, sizeof(ppc->impstate->arg1), "arg1");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->updateaddr, sizeof(ppc->impstate->updateaddr), "updateaddr");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->swcount, sizeof(ppc->impstate->swcount), "swcount");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->tempaddr, sizeof(ppc->impstate->tempaddr), "tempaddr");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->tempdata, sizeof(ppc->impstate->tempdata), "tempdata");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->fp0, sizeof(ppc->impstate->fp0), "fp0");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->fpmode, sizeof(ppc->impstate->fpmode), "fpmode");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->sz_cr_table, sizeof(ppc->impstate->sz_cr_table), "sz_cr_table");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->cmp_cr_table, sizeof(ppc->impstate->cmp_cr_table), "cmp_cr_table");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->cmpl_cr_table, sizeof(ppc->impstate->cmpl_cr_table), "cmpl_cr_table");
	ppc->impstate->drcuml->symbol_add(&ppc->impstate->fcmp_cr_table, sizeof(ppc->impstate->fcmp_cr_table), "fcmp_cr_table");

	/* initialize the front-end helper */
	ppc->impstate->drcfe = auto_alloc(device->machine(), ppc_frontend(*ppc, COMPILE_BACKWARDS_BYTES, COMPILE_FORWARDS_BYTES, SINGLE_INSTRUCTION_MODE ? 1 : COMPILE_MAX_SEQUENCE));

	/* initialize the implementation state tables */
	memcpy(ppc->impstate->fpmode, fpmode_source, sizeof(fpmode_source));
	memcpy(ppc->impstate->sz_cr_table, sz_cr_table_source, sizeof(sz_cr_table_source));
	memcpy(ppc->impstate->cmp_cr_table, cmp_cr_table_source, sizeof(cmp_cr_table_source));
	memcpy(ppc->impstate->cmpl_cr_table, cmpl_cr_table_source, sizeof(cmpl_cr_table_source));
	memcpy(ppc->impstate->fcmp_cr_table, fcmp_cr_table_source, sizeof(fcmp_cr_table_source));

	/* compute the register parameters */
	for (regnum = 0; regnum < 32; regnum++)
	{
		ppc->impstate->regmap[regnum] = mem(&ppc->r[regnum]);
		ppc->impstate->fdregmap[regnum] = mem(&ppc->f[regnum]);
	}

	/* if we have registers to spare, assign r0, r1, r2 to leftovers */
	if (!DISABLE_FAST_REGISTERS)
	{
		ppc->impstate->drcuml->get_backend_info(beinfo);
		if (beinfo.direct_iregs > 5)
			ppc->impstate->regmap[0] = I5;
		if (beinfo.direct_iregs > 6)
			ppc->impstate->regmap[1] = I6;
		if (beinfo.direct_iregs > 7)
			ppc->impstate->regmap[2] = I7;
	}

	/* mark the cache dirty so it is updated on next execute */
	ppc->impstate->cache_dirty = TRUE;
}


/*-------------------------------------------------
    ppcdrc_reset - reset the processor
-------------------------------------------------*/

static CPU_RESET( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);

	/* reset the common code and mark the cache dirty */
	ppccom_reset(ppc);
	ppc->impstate->mode = 0;
	ppc->impstate->cache_dirty = TRUE;
}


/*-------------------------------------------------
    ppcdrc_execute - execute the CPU for the
    specified number of cycles
-------------------------------------------------*/

static CPU_EXECUTE( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);
	drcuml_state *drcuml = ppc->impstate->drcuml;
	int execute_result;

	/* reset the cache if dirty */
	if (ppc->impstate->cache_dirty)
		code_flush_cache(ppc);
	ppc->impstate->cache_dirty = FALSE;

	/* execute */
	do
	{
		/* run as much as we can */
		execute_result = drcuml->execute(*ppc->impstate->entry);

		/* if we need to recompile, do it */
		if (execute_result == EXECUTE_MISSING_CODE)
			code_compile_block(ppc, ppc->impstate->mode, ppc->pc);
		else if (execute_result == EXECUTE_UNMAPPED_CODE)
			fatalerror("Attempted to execute unmapped code at PC=%08X\n", ppc->pc);
		else if (execute_result == EXECUTE_RESET_CACHE)
			code_flush_cache(ppc);

	} while (execute_result != EXECUTE_OUT_OF_CYCLES);
}


/*-------------------------------------------------
    ppcdrc_exit - cleanup from execution
-------------------------------------------------*/

static CPU_EXIT( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);
	ppccom_exit(ppc);

	/* clean up the DRC */
	auto_free(device->machine(), ppc->impstate->drcfe);
	auto_free(device->machine(), ppc->impstate->drcuml);
	auto_free(device->machine(), ppc->impstate->cache);
}


/*-------------------------------------------------
    ppcdrc_translate - perform virtual-to-physical
    address translation
-------------------------------------------------*/

static CPU_TRANSLATE( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);
	return ppccom_translate_address(ppc, space, intention, address);
}


/*-------------------------------------------------
    ppcdrc_dasm - disassemble an instruction
-------------------------------------------------*/

static CPU_DISASSEMBLE( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);
	return ppccom_dasm(ppc, buffer, pc, oprom, opram);
}


/*-------------------------------------------------
    ppcdrc_set_info - set information about a given
    CPU instance
-------------------------------------------------*/

static CPU_SET_INFO( ppcdrc )
{
	powerpc_state *ppc = get_safe_token(device);

	/* --- everything is handled generically --- */
	ppccom_set_info(ppc, state, info);
}


/*-------------------------------------------------
    ppcdrc_get_info - return information about a given
    CPU instance
-------------------------------------------------*/

static CPU_GET_INFO( ppcdrc )
{
	powerpc_state *ppc = (device != NULL && device->token() != NULL) ? get_safe_token(device) : NULL;
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */
		case CPUINFO_INT_CONTEXT_SIZE:					info->i = sizeof(powerpc_state *);				break;
		case CPUINFO_INT_PREVIOUSPC:					/* not implemented */							break;

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(ppcdrc);		break;
		case CPUINFO_FCT_INIT:							/* provided per-CPU */							break;
		case CPUINFO_FCT_RESET:							info->reset = CPU_RESET_NAME(ppcdrc);			break;
		case CPUINFO_FCT_EXIT:							info->exit = CPU_EXIT_NAME(ppcdrc);				break;
		case CPUINFO_FCT_EXECUTE:						info->execute = CPU_EXECUTE_NAME(ppcdrc);		break;
		case CPUINFO_FCT_DISASSEMBLE:					info->disassemble = CPU_DISASSEMBLE_NAME(ppcdrc);break;
		case CPUINFO_FCT_TRANSLATE:						info->translate = CPU_TRANSLATE_NAME(ppcdrc);	break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_SOURCE_FILE:						strcpy(info->s, __FILE__);						break;

		/* --- everything else is handled generically --- */
		default:										ppccom_get_info(ppc, state, info);				break;
	}
}


/*-------------------------------------------------
    ppcdrc_set_options - configure DRC options
-------------------------------------------------*/

void ppcdrc_set_options(device_t *device, UINT32 options)
{
	powerpc_state *ppc = get_safe_token(device);
	ppc->impstate->drcoptions = options;
}


/*-------------------------------------------------
    ppcdrc_add_fastram - add a new fastram
    region
-------------------------------------------------*/

void ppcdrc_add_fastram(device_t *device, offs_t start, offs_t end, UINT8 readonly, void *base)
{
	powerpc_state *ppc = get_safe_token(device);
	if (ppc->impstate->fastram_select < ARRAY_LENGTH(ppc->impstate->fastram))
	{
		ppc->impstate->fastram[ppc->impstate->fastram_select].start = start;
		ppc->impstate->fastram[ppc->impstate->fastram_select].end = end;
		ppc->impstate->fastram[ppc->impstate->fastram_select].readonly = readonly;
		ppc->impstate->fastram[ppc->impstate->fastram_select].base = base;
		ppc->impstate->fastram_select++;
	}
}


/*-------------------------------------------------
    ppcdrc_add_hotspot - add a new hotspot
-------------------------------------------------*/

void ppcdrc_add_hotspot(device_t *device, offs_t pc, UINT32 opcode, UINT32 cycles)
{
	powerpc_state *ppc = get_safe_token(device);
	if (ppc->impstate->hotspot_select < ARRAY_LENGTH(ppc->impstate->hotspot))
	{
		ppc->impstate->hotspot[ppc->impstate->hotspot_select].pc = pc;
		ppc->impstate->hotspot[ppc->impstate->hotspot_select].opcode = opcode;
		ppc->impstate->hotspot[ppc->impstate->hotspot_select].cycles = cycles;
		ppc->impstate->hotspot_select++;
	}
}



/***************************************************************************
    CACHE MANAGEMENT
***************************************************************************/

/*-------------------------------------------------
    code_flush_cache - flush the cache and
    regenerate static code
-------------------------------------------------*/

static void code_flush_cache(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	int mode;

	/* empty the transient cache contents */
	drcuml->reset();

	try
	{
		/* generate the entry point and out-of-cycles handlers */
		static_generate_entry_point(ppc);
		static_generate_nocode_handler(ppc);
		static_generate_out_of_cycles(ppc);
		static_generate_tlb_mismatch(ppc);
		if (ppc->cap & PPCCAP_603_MMU)
			static_generate_swap_tgpr(ppc);

		/* append exception handlers for various types */
		static_generate_exception(ppc, EXCEPTION_RESET,     TRUE,  "exception_reset");
		static_generate_exception(ppc, EXCEPTION_MACHCHECK, TRUE,  "exception_machine_check");
		static_generate_exception(ppc, EXCEPTION_DSI,       TRUE,  "exception_dsi");
		static_generate_exception(ppc, EXCEPTION_ISI,       TRUE,  "exception_isi");
		static_generate_exception(ppc, EXCEPTION_EI,        TRUE,  "exception_ei");
		static_generate_exception(ppc, EXCEPTION_EI,        FALSE, "exception_ei_norecover");
		static_generate_exception(ppc, EXCEPTION_ALIGN,     TRUE,  "exception_align");
		static_generate_exception(ppc, EXCEPTION_PROGRAM,   TRUE,  "exception_program");
		static_generate_exception(ppc, EXCEPTION_NOFPU,     TRUE,  "exception_fpu_unavailable");
		static_generate_exception(ppc, EXCEPTION_DECREMENT, TRUE,  "exception_decrementer");
		static_generate_exception(ppc, EXCEPTION_SYSCALL,   TRUE,  "exception_syscall");
		static_generate_exception(ppc, EXCEPTION_TRACE,     TRUE,  "exception_trace");
		static_generate_exception(ppc, EXCEPTION_FPASSIST,  TRUE,  "exception_floating_point_assist");
		if (ppc->cap & PPCCAP_603_MMU)
		{
			static_generate_exception(ppc, EXCEPTION_ITLBMISS,  TRUE,  "exception_itlb_miss");
			static_generate_exception(ppc, EXCEPTION_DTLBMISSL, TRUE,  "exception_dtlb_miss_load");
			static_generate_exception(ppc, EXCEPTION_DTLBMISSS, TRUE,  "exception_dtlb_miss_store");
		}

		/* add subroutines for memory accesses */
		for (mode = 0; mode < 8; mode++)
		{
			static_generate_memory_accessor(ppc, mode, 1, FALSE, FALSE, "read8",       ppc->impstate->read8[mode],       NULL);
			static_generate_memory_accessor(ppc, mode, 1, TRUE,  FALSE, "write8",      ppc->impstate->write8[mode],      NULL);
			static_generate_memory_accessor(ppc, mode, 2, FALSE, TRUE,  "read16mask",  ppc->impstate->read16mask[mode],  NULL);
			static_generate_memory_accessor(ppc, mode, 2, FALSE, FALSE, "read16",      ppc->impstate->read16[mode],      ppc->impstate->read16mask[mode]);
			static_generate_memory_accessor(ppc, mode, 2, TRUE,  TRUE,  "write16mask", ppc->impstate->write16mask[mode], NULL);
			static_generate_memory_accessor(ppc, mode, 2, TRUE,  FALSE, "write16",     ppc->impstate->write16[mode],     ppc->impstate->write16mask[mode]);
			static_generate_memory_accessor(ppc, mode, 4, FALSE, TRUE,  "read32mask",  ppc->impstate->read32mask[mode],  NULL);
			static_generate_memory_accessor(ppc, mode, 4, FALSE, FALSE, "read32align", ppc->impstate->read32align[mode], NULL);
			static_generate_memory_accessor(ppc, mode, 4, FALSE, FALSE, "read32",      ppc->impstate->read32[mode],      ppc->impstate->read32mask[mode]);
			static_generate_memory_accessor(ppc, mode, 4, TRUE,  TRUE,  "write32mask", ppc->impstate->write32mask[mode], NULL);
			static_generate_memory_accessor(ppc, mode, 4, TRUE,  FALSE, "write32align",ppc->impstate->write32align[mode],NULL);
			static_generate_memory_accessor(ppc, mode, 4, TRUE,  FALSE, "write32",     ppc->impstate->write32[mode],     ppc->impstate->write32mask[mode]);
			static_generate_memory_accessor(ppc, mode, 8, FALSE, TRUE,  "read64mask",  ppc->impstate->read64mask[mode],  NULL);
			static_generate_memory_accessor(ppc, mode, 8, FALSE, FALSE, "read64",      ppc->impstate->read64[mode],      ppc->impstate->read64mask[mode]);
			static_generate_memory_accessor(ppc, mode, 8, TRUE,  TRUE,  "write64mask", ppc->impstate->write64mask[mode], NULL);
			static_generate_memory_accessor(ppc, mode, 8, TRUE,  FALSE, "write64",     ppc->impstate->write64[mode],     ppc->impstate->write64mask[mode]);
			static_generate_lsw_entries(ppc, mode);
			static_generate_stsw_entries(ppc, mode);
		}
	}
	catch (drcuml_block::abort_compilation &)
	{
		fatalerror("Error generating PPC static handlers");
	}
}


/*-------------------------------------------------
    code_compile_block - compile a block of the
    given mode at the specified pc
-------------------------------------------------*/

static void code_compile_block(powerpc_state *ppc, UINT8 mode, offs_t pc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	compiler_state compiler = { 0 };
	const opcode_desc *seqhead, *seqlast;
	const opcode_desc *desclist;
	int override = FALSE;
	drcuml_block *block;
logerror("Compile %08X\n", pc);
	g_profiler.start(PROFILER_DRC_COMPILE);

	/* get a description of this sequence */
	desclist = ppc->impstate->drcfe->describe_code(pc);
	if (LOG_UML || LOG_NATIVE)
		log_opcode_desc(drcuml, desclist, 0);

	bool succeeded = false;
	while (!succeeded)
	{
		try
		{
			/* start the block */
			block = drcuml->begin_block(4096);

			/* loop until we get through all instruction sequences */
			for (seqhead = desclist; seqhead != NULL; seqhead = seqlast->next())
			{
				const opcode_desc *curdesc;
				UINT32 nextpc;

				/* add a code log entry */
				if (LOG_UML)
					block->append_comment("-------------------------");							// comment

				/* determine the last instruction in this sequence */
				for (seqlast = seqhead; seqlast != NULL; seqlast = seqlast->next())
					if (seqlast->flags & OPFLAG_END_SEQUENCE)
						break;
				assert(seqlast != NULL);

				/* if we don't have a hash for this mode/pc, or if we are overriding all, add one */
				if (override || !drcuml->hash_exists(mode, seqhead->pc))
					UML_HASH(block, mode, seqhead->pc);												// hash    mode,pc

				/* if we already have a hash, and this is the first sequence, assume that we */
				/* are recompiling due to being out of sync and allow future overrides */
				else if (seqhead == desclist)
				{
					override = TRUE;
					UML_HASH(block, mode, seqhead->pc);												// hash    mode,pc
				}

				/* otherwise, redispatch to that fixed PC and skip the rest of the processing */
				else
				{
					UML_LABEL(block, seqhead->pc | 0x80000000);										// label   seqhead->pc | 0x80000000
					UML_HASHJMP(block, ppc->impstate->mode, seqhead->pc, *ppc->impstate->nocode);
																									// hashjmp <mode>,seqhead->pc,nocode
					continue;
				}

				/* validate this code block if we're not pointing into ROM */
				if (ppc->program->get_write_ptr(seqhead->physpc) != NULL)
					generate_checksum_block(ppc, block, &compiler, seqhead, seqlast);				// <checksum>

				/* label this instruction, if it may be jumped to locally */
				if (seqhead->flags & OPFLAG_IS_BRANCH_TARGET)
					UML_LABEL(block, seqhead->pc | 0x80000000);										// label   seqhead->pc | 0x80000000

				/* iterate over instructions in the sequence and compile them */
				for (curdesc = seqhead; curdesc != seqlast->next(); curdesc = curdesc->next())
					generate_sequence_instruction(ppc, block, &compiler, curdesc);					// <instruction>

				/* if we need to return to the start, do it */
				if (seqlast->flags & OPFLAG_RETURN_TO_START)
					nextpc = pc;

				/* otherwise we just go to the next instruction */
				else
					nextpc = seqlast->pc + (seqlast->skipslots + 1) * 4;

				/* count off cycles and go there */
				generate_update_cycles(ppc, block, &compiler, nextpc, TRUE);					// <subtract cycles>

				/* if the last instruction can change modes, use a variable mode; otherwise, assume the same mode */
				if (seqlast->flags & OPFLAG_CAN_CHANGE_MODES)
					UML_HASHJMP(block, mem(&ppc->impstate->mode), nextpc, *ppc->impstate->nocode);// hashjmp <mode>,nextpc,nocode
				else if (seqlast->next() == NULL || seqlast->next()->pc != nextpc)
					UML_HASHJMP(block, ppc->impstate->mode, nextpc, *ppc->impstate->nocode);// hashjmp <mode>,nextpc,nocode
			}

			/* end the sequence */
			block->end();
			g_profiler.stop();
			succeeded = true;
		}
		catch (drcuml_block::abort_compilation &)
		{
			// flush the cache and try again
			code_flush_cache(ppc);
		}
	}
}



/***************************************************************************
    C FUNCTION CALLBACKS
***************************************************************************/

/*-------------------------------------------------
    cfunc_printf_exception - log any exceptions that
    aren't interrupts
-------------------------------------------------*/

static void cfunc_printf_exception(void *param)
{
	powerpc_state *ppc = (powerpc_state *)param;
	printf("Exception: type=%2d EPC=%08X MSR=%08X\n", ppc->param0, ppc->spr[SPROEA_SRR0], ppc->spr[SPROEA_SRR1]);
	cfunc_printf_probe(ppc);
}


/*-------------------------------------------------
    cfunc_printf_debug - generic printf for
    debugging
-------------------------------------------------*/

static void cfunc_printf_debug(void *param)
{
	powerpc_state *ppc = (powerpc_state *)param;
	printf(ppc->impstate->format, ppc->impstate->arg0, ppc->impstate->arg1);
}


/*-------------------------------------------------
    cfunc_printf_probe - print the current CPU
    state and return
-------------------------------------------------*/

static void cfunc_printf_probe(void *param)
{
	powerpc_state *ppc = (powerpc_state *)param;
	UINT32 pc = (UINT32)(FPTR)param;

	printf(" PC=%08X\n", pc);
	printf(" r0=%08X  r1=%08X  r2=%08X  r3=%08X\n",
		ppc->r[0], ppc->r[1], ppc->r[2], ppc->r[3]);
	printf(" r4=%08X  r5=%08X  r6=%08X  r7=%08X\n",
		ppc->r[4], ppc->r[5], ppc->r[6], ppc->r[7]);
	printf(" r8=%08X  r9=%08X r10=%08X r11=%08X\n",
		ppc->r[8], ppc->r[9], ppc->r[10], ppc->r[11]);
	printf("r12=%08X r13=%08X r14=%08X r15=%08X\n",
		ppc->r[12], ppc->r[13], ppc->r[14], ppc->r[15]);
	printf("r16=%08X r17=%08X r18=%08X r19=%08X\n",
		ppc->r[16], ppc->r[17], ppc->r[18], ppc->r[19]);
	printf("r20=%08X r21=%08X r22=%08X r23=%08X\n",
		ppc->r[20], ppc->r[21], ppc->r[22], ppc->r[23]);
	printf("r24=%08X r25=%08X r26=%08X r27=%08X\n",
		ppc->r[24], ppc->r[25], ppc->r[26], ppc->r[27]);
	printf("r28=%08X r29=%08X r30=%08X r31=%08X\n",
		ppc->r[28], ppc->r[29], ppc->r[30], ppc->r[31]);
}


/*-------------------------------------------------
    cfunc_unimplemented - handler for
    unimplemented opcdes
-------------------------------------------------*/

static void cfunc_unimplemented(void *param)
{
	powerpc_state *ppc = (powerpc_state *)param;
	UINT32 opcode = ppc->impstate->arg0;
	fatalerror("PC=%08X: Unimplemented op %08X", ppc->pc, opcode);
}



/***************************************************************************
    STATIC CODEGEN
***************************************************************************/

/*-------------------------------------------------
    static_generate_entry_point - generate a
    static entry point
-------------------------------------------------*/

static void static_generate_entry_point(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	code_label skip = 1;
	drcuml_block *block;

	/* begin generating */
	block = drcuml->begin_block(20);

	/* forward references */
	alloc_handle(drcuml, &ppc->impstate->nocode, "nocode");
	alloc_handle(drcuml, &ppc->impstate->exception_norecover[EXCEPTION_EI], "exception_ei_norecover");

	alloc_handle(drcuml, &ppc->impstate->entry, "entry");
	UML_HANDLE(block, *ppc->impstate->entry);												// handle  entry

	/* reset the FPU mode */
	UML_AND(block, I0, FPSCR32, 3);												// and     i0,fpscr,3
	UML_LOAD(block, I0, &ppc->impstate->fpmode[0], I0, SIZE_BYTE, SCALE_x1);		// load    i0,fpmode,i0,byte
	UML_SETFMOD(block, I0);															// setfmod i0

	/* load fast integer registers */
	load_fast_iregs(ppc, block);															// <load fastregs>

	/* check for interrupts */
	UML_TEST(block, mem(&ppc->irq_pending), ~0);										// test    [irq_pending],0
	UML_JMPc(block, COND_Z, skip);															// jmp     skip,Z
	UML_TEST(block, MSR32, MSR_EE);													// test    msr,MSR_EE
	UML_JMPc(block, COND_Z, skip);															// jmp     skip,Z
	UML_MOV(block, I0, mem(&ppc->pc));													// mov     i0,pc
	UML_MOV(block, I1, 0);														// mov     i1,0
	UML_CALLH(block, *ppc->impstate->exception_norecover[EXCEPTION_EI]);						// callh   exception_norecover
	UML_LABEL(block, skip);																// skip:

	/* generate a hash jump via the current mode and PC */
	UML_HASHJMP(block, mem(&ppc->impstate->mode), mem(&ppc->pc), *ppc->impstate->nocode);	// hashjmp <mode>,<pc>,nocode

	block->end();
}


/*-------------------------------------------------
    static_generate_nocode_handler - generate an
    exception handler for "out of code"
-------------------------------------------------*/

static void static_generate_nocode_handler(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;

	/* begin generating */
	block = drcuml->begin_block(10);

	/* generate a hash jump via the current mode and PC */
	alloc_handle(drcuml, &ppc->impstate->nocode, "nocode");
	UML_HANDLE(block, *ppc->impstate->nocode);												// handle  nocode
	UML_GETEXP(block, I0);																// getexp  i0
	UML_MOV(block, mem(&ppc->pc), I0);													// mov     [pc],i0
	save_fast_iregs(ppc, block);															// <save fastregs>
	UML_EXIT(block, EXECUTE_MISSING_CODE);												// exit    EXECUTE_MISSING_CODE

	block->end();
}


/*-------------------------------------------------
    static_generate_out_of_cycles - generate an
    out of cycles exception handler
-------------------------------------------------*/

static void static_generate_out_of_cycles(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;

	/* begin generating */
	block = drcuml->begin_block(10);

	/* generate a hash jump via the current mode and PC */
	alloc_handle(drcuml, &ppc->impstate->out_of_cycles, "out_of_cycles");
	UML_HANDLE(block, *ppc->impstate->out_of_cycles);										// handle  out_of_cycles
	UML_GETEXP(block, I0);																// getexp  i0
	UML_MOV(block, mem(&ppc->pc), I0);													// mov     <pc>,i0
	save_fast_iregs(ppc, block);															// <save fastregs>
	UML_EXIT(block, EXECUTE_OUT_OF_CYCLES);											// exit    EXECUTE_OUT_OF_CYCLES

	block->end();
}


/*-------------------------------------------------
    static_generate_tlb_mismatch - generate a
    TLB mismatch handler
-------------------------------------------------*/

static void static_generate_tlb_mismatch(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;
	int isi, exit, label = 1;

	/* forward references */
	alloc_handle(drcuml, &ppc->impstate->exception[EXCEPTION_ISI], "exception_isi");
	if (ppc->cap & PPCCAP_603_MMU)
		alloc_handle(drcuml, &ppc->impstate->exception[EXCEPTION_ITLBMISS], "exception_itlb_miss");

	/* begin generating */
	block = drcuml->begin_block(20);

	/* generate a hash jump via the current mode and PC */
	alloc_handle(drcuml, &ppc->impstate->tlb_mismatch, "tlb_mismatch");
	UML_HANDLE(block, *ppc->impstate->tlb_mismatch);											// handle  tlb_mismatch
	UML_RECOVER(block, I0, MAPVAR_PC);													// recover i0,PC
	UML_SHR(block, I1, I0, 12);												// shr     i1,i0,12
	UML_LOAD(block, I2, (void *)vtlb_table(ppc->vtlb), I1, SIZE_DWORD, SCALE_x4);	// load    i2,[vtlb],i1,dword
	UML_MOV(block, mem(&ppc->param0), I0);												// mov     [param0],i0
	UML_MOV(block, mem(&ppc->param1), TRANSLATE_FETCH);								// mov     [param1],TRANSLATE_FETCH
	UML_CALLC(block, (c_function)ppccom_tlb_fill, ppc);													// callc   tlbfill,ppc
	UML_LOAD(block, I1, (void *)vtlb_table(ppc->vtlb), I1, SIZE_DWORD, SCALE_x4);	// load    i1,[vtlb],i1,dword
	UML_TEST(block, I1, VTLB_FETCH_ALLOWED);										// test    i1,VTLB_FETCH_ALLOWED
	UML_JMPc(block, COND_Z, isi = label++);													// jmp     isi,z
	UML_CMP(block, I2, 0);														// cmp     i2,0
	UML_JMPc(block, COND_NZ, exit = label++);													// jmp     exit,nz
	UML_HASHJMP(block, mem(&ppc->impstate->mode), I0, *ppc->impstate->nocode);			// hashjmp <mode>,i0,nocode
	UML_LABEL(block, exit);																// exit:
	UML_MOV(block, mem(&ppc->pc), I0);													// mov     <pc>,i0
	save_fast_iregs(ppc, block);															// <save fastregs>
	UML_EXIT(block, EXECUTE_MISSING_CODE);												// exit    EXECUTE_MISSING_CODE
	UML_LABEL(block, isi);																// isi:
	if (!(ppc->cap & PPCCAP_603_MMU))
	{
		UML_MOV(block, SPR32(SPROEA_DSISR), mem(&ppc->param0));								// mov     [dsisr],[param0]
		UML_EXH(block, *ppc->impstate->exception[EXCEPTION_ISI], I0);					// exh     isi,i0
	}
	else
	{
		UML_MOV(block, SPR32(SPR603_IMISS), I0);										// mov     [imiss],i0
		UML_MOV(block, SPR32(SPR603_ICMP), mem(&ppc->mmu603_cmp));							// mov     [icmp],[mmu603_cmp]
		UML_MOV(block, SPR32(SPR603_HASH1), mem(&ppc->mmu603_hash[0]));						// mov     [hash1],[mmu603_hash][0]
		UML_MOV(block, SPR32(SPR603_HASH2), mem(&ppc->mmu603_hash[1]));						// mov     [hash2],[mmu603_hash][1]
		UML_EXH(block, *ppc->impstate->exception[EXCEPTION_ITLBMISS], I0);				// exh     itlbmiss,i0
	}

	block->end();
}


/*-------------------------------------------------
    static_generate_exception - generate a static
    exception handler
-------------------------------------------------*/

static void static_generate_exception(powerpc_state *ppc, UINT8 exception, int recover, const char *name)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	code_handle *&exception_handle = recover ? ppc->impstate->exception[exception] : ppc->impstate->exception_norecover[exception];
	UINT32 vector = exception << 8;
	code_label label = 1;
	drcuml_block *block;

	/* begin generating */
	block = drcuml->begin_block(1024);

	/* add a global entry for this */
	alloc_handle(drcuml, &exception_handle, name);
	UML_HANDLE(block, *exception_handle);													// handle  name

	/* exception parameter is expected to be the fault address in this case */
	if (exception == EXCEPTION_ISI || exception == EXCEPTION_DSI)
	{
		UML_GETEXP(block, I0);															// getexp  i0
		UML_MOV(block, SPR32(SPROEA_DAR), I0);											// mov     [dar],i0
	}

	/* fetch the PC and uncounted cycles */
	if (recover)
	{
		UML_RECOVER(block, I0, MAPVAR_PC);												// recover i0,PC
		UML_RECOVER(block, I1, MAPVAR_CYCLES);											// recover i1,CYCLES
	}

	/* OEA handling of SRR exceptions */
	if (ppc->cap & PPCCAP_OEA)
	{
		UINT32 msrandmask = MSROEA_POW | MSR_EE | MSR_PR | MSROEA_FP | MSROEA_FE0 | MSROEA_SE | MSROEA_BE | MSROEA_FE1 | MSROEA_IR | MSROEA_DR | MSROEA_RI | MSR_LE;
		UINT32 msrormask = 0;

		/* check registers to see the real source of our exception (EI exceptions only) */
		UML_MOV(block, I3, vector);												// mov     i3,vector
		if (exception == EXCEPTION_EI)
		{
			code_label not_decrementer;

			UML_TEST(block, mem(&ppc->irq_pending), 0x01);								// test    [irq_pending],0x01
			UML_JMPc(block, COND_NZ, not_decrementer = label++);								// jmp     not_decrementer,nz
			UML_MOV(block, I3, EXCEPTION_DECREMENT << 8);							// mov     i3,EXCEPTION_DECREMENT << 8
			UML_AND(block, mem(&ppc->irq_pending), mem(&ppc->irq_pending), ~0x02);		// and     [irq_pending],[irq_pending],~0x02
			UML_LABEL(block, not_decrementer);											// not_decrementer:
		}

		/* exception PC goes into SRR0 */
		UML_MOV(block, SPR32(SPROEA_SRR0), I0);										// mov     [srr0],i0

		/* MSR bits go into SRR1, along with some exception-specific data */
		UML_AND(block, SPR32(SPROEA_SRR1), MSR32, 0x87c0ffff);							// and     [srr1],[msr],0x87c0ffff
		if (exception == EXCEPTION_PROGRAM)
		{
			UML_GETEXP(block, I1);														// getexp  i1
			UML_OR(block, SPR32(SPROEA_SRR1), SPR32(SPROEA_SRR1), I1);					// or      [srr1],[srr1],i1
		}
		if (ppc->cap & PPCCAP_603_MMU)
		{
			if (exception == EXCEPTION_ITLBMISS)
				UML_OR(block, SPR32(SPROEA_SRR1), SPR32(SPROEA_SRR1), 0x00040000);		// or      [srr1],0x00040000
			else if (exception == EXCEPTION_DTLBMISSL)
				UML_OR(block, SPR32(SPROEA_SRR1), SPR32(SPROEA_SRR1), 0x00010000);		// or      [srr1],0x00010000
			if (exception == EXCEPTION_ITLBMISS || exception == EXCEPTION_DTLBMISSL || exception == EXCEPTION_DTLBMISSS)
				UML_ROLINS(block, SPR32(SPROEA_SRR1), CR32(0), 28, CRMASK(0));	// rolins  [srr1],[cr0],28,crmask(0)
		}

		/* update MSR */
		if (ppc->cap & PPCCAP_603_MMU)
		{
			if (exception == EXCEPTION_ITLBMISS || exception == EXCEPTION_DTLBMISSL || exception == EXCEPTION_DTLBMISSS)
				msrormask |= MSR603_TGPR;
			else
				msrandmask |= MSR603_TGPR;
			UML_MOV(block, I0, MSR32);													// mov     i0,[msr]
		}
		UML_AND(block, I2, MSR32, ~msrandmask);									// and     i2,[msr],~andmask
		UML_OR(block, I2, I2, msrormask);									// or      i2,i2,ormask
		UML_ROLINS(block, I2, I2, 16, MSR_LE);							// rolins  i2,u2,16,MSR_LE
		UML_MOV(block, MSR32, I2);														// mov     [msr],i2
		if (ppc->cap & PPCCAP_603_MMU)
		{
			UML_XOR(block, I0, I0, I2);										// xor     i0,i0,i2
			UML_TEST(block, I0, MSR603_TGPR);										// test    i0,tgpr
			UML_CALLHc(block, COND_NZ, *ppc->impstate->swap_tgpr);								// callh   swap_tgpr,nz
		}
		generate_update_mode(ppc, block);													// <update mode>

		/* determine our target PC */
		if (ppc->flavor == PPC_MODEL_602)
			UML_MOV(block, I0, SPR32(SPR602_IBR));										// mov     i0,[ibr]
		else
			UML_MOV(block, I0, 0x00000000);										// mov     i0,0x00000000
		UML_TEST(block, MSR32, MSROEA_IP);												// test    [msr],IP
		UML_MOVc(block, COND_NZ, I0, 0xfff00000);									// mov     i0,0xfff00000,nz
		UML_OR(block, I0, I0, I3);											// or      i0,i0,i3
	}

	/* 4XX handling of exceptions */
	if (ppc->cap & PPCCAP_4XX)
	{
		/* check registers to see the real source of our exception (EI exceptions only) */
		UML_MOV(block, I3, vector);												// mov     i3,vector
		if (exception == EXCEPTION_EI)
		{
			code_label notwdog, common;

			UML_TEST(block, SPR32(SPR4XX_TSR), PPC4XX_TSR_PIS);						// test    [tsr],PIS
			UML_MOVc(block, COND_NZ, I3, 0x1000);									// mov     i3,0x1000,NZ
			UML_TEST(block, SPR32(SPR4XX_TSR), PPC4XX_TSR_FIS);						// test    [tsr],FIS
			UML_MOVc(block, COND_NZ, I3, 0x1010);									// mov     i3,0x1010,NZ
			UML_TEST(block, SPR32(DCR4XX_EXISR), SPR32(DCR4XX_EXIER));						// test    [exisr],[exier]
			UML_MOVc(block, COND_NZ, I3, vector);									// mov     i3,vector,NZ
			UML_TEST(block, SPR32(SPR4XX_TSR), PPC4XX_TSR_WIS);						// test    [tsr],WIS
			UML_JMPc(block, COND_Z, notwdog = label++);										// jz      notwdog
			UML_MOV(block, I3, 0x1020);											// mov     i3,0x1020

			/* exception PC goes into SRR2, MSR goes to SRR3 */
			UML_MOV(block, SPR32(SPR4XX_SRR2), I0);									// mov     [srr2],i0
			UML_MOV(block, SPR32(SPR4XX_SRR3), MSR32);										// mov     [srr3],[msr]
			UML_AND(block, I2, MSR32, ~(MSR4XX_WE | MSR_PR | MSR4XX_CE | MSR_EE | MSR4XX_DE | MSR4XX_PE));
			UML_JMP(block, common = label++);												// jmp     common

			/* exception PC goes into SRR0, MSR goes to SRR1 */
			UML_LABEL(block, notwdog);													// notwdog:
			UML_MOV(block, SPR32(SPR4XX_SRR0), I0);									// mov     [srr0],i0
			UML_MOV(block, SPR32(SPR4XX_SRR1), MSR32);										// mov     [srr1],[msr]
			UML_AND(block, I2, MSR32, ~(MSR4XX_WE | MSR_PR | MSR_EE | MSR4XX_PE));// and     i2,[msr],~(bunch-o-flags)
			UML_LABEL(block, common);													// common:
		}
		else
		{
			/* exception PC goes into SRR0, MSR goes to SRR1 */
			UML_MOV(block, SPR32(SPR4XX_SRR0), I0);									// mov     [srr0],i0
			UML_MOV(block, SPR32(SPR4XX_SRR1), MSR32);										// mov     [srr1],[msr]
			UML_AND(block, I2, MSR32, ~(MSR4XX_WE | MSR_PR | MSR_EE | MSR4XX_PE));// and     i2,[msr],~(bunch-o-flags)
		}

		/* finish updating MSR */
		UML_ROLINS(block, I2, I2, 16, MSR_LE);							// rolins  i2,u2,16,MSR_LE
		UML_MOV(block, MSR32, I2);														// mov     [msr],i2
		generate_update_mode(ppc, block);													// <update mode>

		/* program exception flags go to ESR */
		if (exception == EXCEPTION_PROGRAM)
		{
			UML_GETEXP(block, I1);														// getexp  i1
			UML_SHL(block, SPR32(SPR4XX_ESR), I1, 8);								// shl     [esr],i1,8
		}

		/* determine our target PC */
		UML_ROLINS(block, I3, SPR32(SPR4XX_EVPR), 0, 0xffff0000);			// rolins  i3,[evpr],0,0xffff0000
		UML_MOV(block, I0, I3);													// mov     i0,i3
	}

	/* optionally print exceptions */
	if ((PRINTF_EXCEPTIONS && exception != EXCEPTION_EI && exception != EXCEPTION_SYSCALL) ||
		(PRINTF_MMU && (exception == EXCEPTION_ISI || exception == EXCEPTION_DSI)))
	{
		UML_MOV(block, mem(&ppc->param0), exception);									// mov     [param0],exception
		UML_CALLC(block, cfunc_printf_exception, ppc);										// callc   cfunc_printf_exception,ppc
	}

	/* adjust cycles */
	UML_SUB(block, mem(&ppc->icount), mem(&ppc->icount), I1);							// sub     icount,icount,cycles
	UML_EXHc(block, COND_S, *ppc->impstate->out_of_cycles, I0);							// exh     out_of_cycles,i0
	UML_HASHJMP(block, mem(&ppc->impstate->mode), I0, *ppc->impstate->nocode);			// hashjmp <mode>,i0,nocode

	block->end();
}


/*------------------------------------------------------------------
    static_generate_memory_accessor
------------------------------------------------------------------*/

static void static_generate_memory_accessor(powerpc_state *ppc, int mode, int size, int iswrite, int ismasked, const char *name, code_handle *&handleptr, code_handle *masked)
{
	/* on entry, address is in I0; data for writes is in I1; masks are in I2 */
	/* on exit, read result is in I0 */
	/* routine trashes I0-I3 */
	drcuml_state *drcuml = ppc->impstate->drcuml;
	int fastxor = BYTE8_XOR_BE(0) >> (int)(ppc->device->space_config(AS_PROGRAM)->m_databus_width < 64);
	drcuml_block *block;
	int translate_type;
	int tlbreturn = 0;
	int unaligned = 0;
	int alignex = 0;
	int tlbmiss = 0;
	int label = 1;
	int ramnum;

	if (mode & MODE_USER)
		translate_type = iswrite ? TRANSLATE_WRITE_USER : TRANSLATE_READ_USER;
	else
		translate_type = iswrite ? TRANSLATE_WRITE : TRANSLATE_READ;

	/* begin generating */
	block = drcuml->begin_block(1024);

	/* add a global entry for this */
	alloc_handle(drcuml, &handleptr, name);
	UML_HANDLE(block, *handleptr);															// handle  *handleptr

	/* check for unaligned accesses and break into two */
	if (!ismasked && size != 1)
	{
		/* in little-endian mode, anything misaligned generates an exception */
		if ((mode & MODE_LITTLE_ENDIAN) || masked == NULL || !(ppc->cap & PPCCAP_MISALIGNED))
		{
			UML_TEST(block, I0, size - 1);										// test    i0,size-1
			UML_JMPc(block, COND_NZ, alignex = label++);										// jmp     alignex,nz
		}

		/* in big-endian mode, it's more complicated */
		else
		{
			/* 8-byte accesses must be word-aligned */
			if (size == 8)
			{
				UML_TEST(block, I0, 3);											// test    i0,3
				UML_JMPc(block, COND_NZ, alignex = label++);									// jmp     alignex,nz
			}

			/* unaligned 2 and 4 byte accesses need to be broken up */
			else
			{
				UML_TEST(block, I0, size - 1);									// test    i0,size-1
				UML_JMPc(block, COND_NZ, unaligned = label++);								// jmp     unaligned,nz
			}
		}
	}

	/* general case: assume paging and perform a translation */
	if (((ppc->cap & PPCCAP_OEA) && (mode & MODE_DATA_TRANSLATION)) || (iswrite && (ppc->cap & PPCCAP_4XX) && (mode & MODE_PROTECTION)))
	{
		UML_SHR(block, I3, I0, 12);											// shr     i3,i0,12
		UML_LOAD(block, I3, (void *)vtlb_table(ppc->vtlb), I3, SIZE_DWORD, SCALE_x4);// load    i3,[vtlb],i3,dword
		UML_TEST(block, I3, (UINT64)1 << translate_type);							// test    i3,1 << translate_type
		UML_JMPc(block, COND_Z, tlbmiss = label++);											// jmp     tlbmiss,z
		UML_LABEL(block, tlbreturn = label++);											// tlbreturn:
		UML_ROLINS(block, I0, I3, 0, 0xfffff000);						// rolins  i0,i3,0,0xfffff000
	}
	else if (ppc->cap & PPCCAP_4XX)
		UML_AND(block, I0, I0, 0x7fffffff);									// and     i0,i0,0x7fffffff
	UML_XOR(block, I0, I0, (mode & MODE_LITTLE_ENDIAN) ? (8 - size) : 0);	// xor     i0,i0,8-size

	if ((ppc->device->machine().debug_flags & DEBUG_FLAG_ENABLED) != 0)
		for (ramnum = 0; ramnum < PPC_MAX_FASTRAM; ramnum++)
			if (ppc->impstate->fastram[ramnum].base != NULL && (!iswrite || !ppc->impstate->fastram[ramnum].readonly))
			{
				void *fastbase = (UINT8 *)ppc->impstate->fastram[ramnum].base - ppc->impstate->fastram[ramnum].start;
				UINT32 skip = label++;

				if (ppc->impstate->fastram[ramnum].end != 0xffffffff)
				{
					UML_CMP(block, I0, ppc->impstate->fastram[ramnum].end);			// cmp     i0,end
					UML_JMPc(block, COND_A, skip);												// ja      skip
				}
				if (ppc->impstate->fastram[ramnum].start != 0x00000000)
				{
					UML_CMP(block, I0, ppc->impstate->fastram[ramnum].start);			// cmp     i0,fastram_start
					UML_JMPc(block, COND_B, skip);												// jb      skip
				}

				if (!iswrite)
				{
					if (size == 1)
					{
						UML_XOR(block, I0, I0, fastxor & 7);						// xor     i0,i0,fastxor & 7
						UML_LOAD(block, I0, fastbase, I0, SIZE_BYTE, SCALE_x1);		// load    i0,fastbase,i0,byte
					}
					else if (size == 2)
					{
						UML_XOR(block, I0, I0, fastxor & 6);						// xor     i0,i0,fastxor & 6
						UML_LOAD(block, I0, fastbase, I0, SIZE_WORD, SCALE_x1);		// load    i0,fastbase,i0,word_x1
					}
					else if (size == 4)
					{
						UML_XOR(block, I0, I0, fastxor & 4);						// xor     i0,i0,fastxor & 4
						UML_LOAD(block, I0, fastbase, I0, SIZE_DWORD, SCALE_x1);		// load    i0,fastbase,i0,dword_x1
					}
					else if (size == 8)
					{
						UML_DLOAD(block, I0, fastbase, I0, SIZE_QWORD, SCALE_x1);		// dload   i0,fastbase,i0,qword
					}
					UML_RET(block);																// ret
				}
				else
				{
					if (size == 1)
					{
						UML_XOR(block, I0, I0, fastxor & 7);						// xor     i0,i0,fastxor & 7
						UML_STORE(block, fastbase, I0, I1, SIZE_BYTE, SCALE_x1);		// store   fastbase,i0,i1,byte
					}
					else if (size == 2)
					{
						UML_XOR(block, I0, I0, fastxor & 6);						// xor     i0,i0,fastxor & 6
						UML_STORE(block, fastbase, I0, I1, SIZE_WORD, SCALE_x1);		// store   fastbase,i0,i1,word_x1
					}
					else if (size == 4)
					{
						UML_XOR(block, I0, I0, fastxor & 4);						// xor     i0,i0,fastxor & 4
						if (ismasked)
						{
							UML_LOAD(block, I3, fastbase, I0, SIZE_DWORD, SCALE_x1);	// load    i3,fastbase,i0,dword_x1
							UML_AND(block, I1, I1, I2);							// and     i1,i1,i2
							UML_XOR(block, I2, I2, 0xffffffff);					// xor     i2,i2,0xfffffffff
							UML_AND(block, I3, I3, I2);							// and     i3,i3,i2
							UML_OR(block, I1, I1, I3);							// or      i1,i1,i3
						}
						UML_STORE(block, fastbase, I0, I1, SIZE_DWORD, SCALE_x1);		// store   fastbase,i0,i1,dword_x1
					}
					else if (size == 8)
					{
						if (ismasked)
						{
							UML_DLOAD(block, I3, fastbase, I0, SIZE_QWORD, SCALE_x1);	// dload   i3,fastbase,i0,qword_x1
							UML_DAND(block, I1, I1, I2);							// dand    i1,i1,i2
							UML_DXOR(block, I2, I2, U64(0xffffffffffffffff));	// dxor    i2,i2,0xfffffffffffffffff
							UML_DAND(block, I3, I3, I2);							// dand    i3,i3,i2
							UML_DOR(block, I1, I1, I3);							// dor     i1,i1,i3
						}
						UML_DSTORE(block, fastbase, I0, I1, SIZE_QWORD, SCALE_x1);	// dstore  fastbase,i0,i1,qword_x1
					}
					UML_RET(block);																// ret
				}

				UML_LABEL(block, skip);														// skip:
			}

	switch (size)
	{
		case 1:
			if (iswrite)
				UML_WRITE(block, I0, I1, SIZE_BYTE, SPACE_PROGRAM);							// write   i0,i1,program_byte
			else
				UML_READ(block, I0, I0, SIZE_BYTE, SPACE_PROGRAM);							// read    i0,i0,program_byte
			break;

		case 2:
			if (iswrite)
			{
				if (!ismasked)
					UML_WRITE(block, I0, I1, SIZE_WORD, SPACE_PROGRAM);						// write   i0,i1,program_word
				else
					UML_WRITEM(block, I0, I1, I2, SIZE_WORD, SPACE_PROGRAM);				// writem  i0,i2,i1,program_word
			}
			else
			{
				if (!ismasked)
					UML_READ(block, I0, I0, SIZE_WORD, SPACE_PROGRAM);						// read    i0,i0,program_word
				else
					UML_READM(block, I0, I0, I2, SIZE_WORD, SPACE_PROGRAM);				// readm   i0,i0,i2,program_word
			}
			break;

		case 4:
			if (iswrite)
			{
				if (!ismasked)
					UML_WRITE(block, I0, I1, SIZE_DWORD, SPACE_PROGRAM);						// write   i0,i1,program_dword
				else
					UML_WRITEM(block, I0, I1, I2, SIZE_DWORD, SPACE_PROGRAM);			// writem  i0,i2,i1,program_dword
			}
			else
			{
				if (!ismasked)
					UML_READ(block, I0, I0, SIZE_DWORD, SPACE_PROGRAM);						// read    i0,i0,program_dword
				else
					UML_READM(block, I0, I0, I2, SIZE_DWORD, SPACE_PROGRAM);				// readm   i0,i0,i2,program_dword
			}
			break;

		case 8:
			if (iswrite)
			{
				if (!ismasked)
					UML_DWRITE(block, I0, I1, SIZE_QWORD, SPACE_PROGRAM);						// dwrite  i0,i1,program_qword
				else
					UML_DWRITEM(block, I0, I1, I2, SIZE_QWORD, SPACE_PROGRAM);			// dwritem i0,i2,i1,program_qword
			}
			else
			{
				if (!ismasked)
					UML_DREAD(block, I0, I0, SIZE_QWORD, SPACE_PROGRAM);						// dread   i0,i0,program_qword
				else
					UML_DREADM(block, I0, I0, I2, SIZE_QWORD, SPACE_PROGRAM);			// dreadm  i0,i0,i2,program_qword
			}
			break;
	}
	UML_RET(block);																			// ret

	/* handle unaligned accesses */
	if (unaligned != 0)
	{
		UML_LABEL(block, unaligned);													// unaligned:
		if (size == 2)
		{
			if (iswrite)
			{
				UML_MOV(block, mem(&ppc->impstate->tempaddr), I0);						// mov     [tempaddr],i0
				UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I1);					// mov     [tempdata],i1
				UML_SUB(block, I0, I0, 1);									// sub     i0,i0,1
				UML_SHR(block, I1, I1, 8);									// shr     i1,i1,8
				UML_MOV(block, I2, 0x00ff);										// mov     i2,0x00ff
				UML_CALLH(block, *masked);													// callh   masked
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 1);				// add     i0,[tempaddr],1
				UML_SHL(block, I1, mem(&ppc->impstate->tempdata.w.l), 8);			// shl     i1,[tempdata],8
				UML_MOV(block, I2, 0xff00);										// mov     i2,0xff00
				UML_CALLH(block, *masked);													// callh   masked
			}
			else
			{
				UML_MOV(block, mem(&ppc->impstate->tempaddr), I0);						// mov     [tempaddr],i0
				UML_SUB(block, I0, I0, 1);									// sub     i0,i0,1
				UML_MOV(block, I2, 0x00ff);										// mov     i2,0x00ff
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHL(block, mem(&ppc->impstate->tempdata.w.l), I0, 8);			// shl     [tempdata],i0,8
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 1);				// add     i0,[tempaddr],1
				UML_MOV(block, I2, 0xff00);										// mov     i2,0xff00
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHR(block, I0, I0, 8);									// shr     i0,i0,8
				UML_OR(block, I0, I0, mem(&ppc->impstate->tempdata.w.l));			// or      i0,i0,[tempdata]
			}
		}
		else if (size == 4)
		{
			int offs2, offs3;
			if (iswrite)
			{
				UML_MOV(block, mem(&ppc->impstate->tempaddr), I0);						// mov     [tempaddr],i0
				UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I1);					// mov     [tempdata],i1
				UML_TEST(block, I0, 2);											// test    i0,i0,2
				UML_JMPc(block, COND_NZ, offs2 = label++);									// jnz     offs2
				UML_SUB(block, I0, I0, 1);									// sub     i0,i0,1
				UML_SHR(block, I1, I1, 8);									// shr     i1,i1,8
				UML_MOV(block, I2, 0x00ffffff);									// mov     i2,0x00ffffff
				UML_CALLH(block, *masked);													// callh   masked
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 3);				// add     i0,[tempaddr],3
				UML_SHL(block, I1, mem(&ppc->impstate->tempdata.w.l), 24);		// shl     i1,[tempdata],24
				UML_MOV(block, I2, 0xff000000);									// mov     i2,0xff000000
				UML_CALLH(block, *masked);													// callh   masked
				UML_RET(block);																// ret
				UML_LABEL(block, offs2);												// offs2:
				UML_TEST(block, I0, 1);											// test    i0,i0,1
				UML_JMPc(block, COND_NZ, offs3 = label++);									// jnz     offs3
				UML_SUB(block, I0, I0, 2);									// sub     i0,i0,2
				UML_SHR(block, I1, I1, 16);									// shr     i1,i1,16
				UML_MOV(block, I2, 0x0000ffff);									// mov     i2,0x0000ffff
				UML_CALLH(block, *masked);													// callh   masked
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 2);				// add     i0,[tempaddr],2
				UML_SHL(block, I1, mem(&ppc->impstate->tempdata.w.l), 16);		// shl     i1,[tempdata],16
				UML_MOV(block, I2, 0xffff0000);									// mov     i2,0xffff0000
				UML_CALLH(block, *masked);													// callh   masked
				UML_RET(block);																// ret
				UML_LABEL(block, offs3);												// offs3:
				UML_SUB(block, I0, I0, 3);									// sub     i0,i0,3
				UML_SHR(block, I1, I1, 24);									// shr     i1,i1,24
				UML_MOV(block, I2, 0x000000ff);									// mov     i2,0x000000ff
				UML_CALLH(block, *masked);													// callh   masked
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 1);				// add     i0,[tempaddr],1
				UML_SHL(block, I1, mem(&ppc->impstate->tempdata.w.l), 8);			// shl     i1,[tempdata],8
				UML_MOV(block, I2, 0xffffff00);									// mov     i2,0xffffff00
				UML_CALLH(block, *masked);													// callh   masked
			}
			else
			{
				UML_MOV(block, mem(&ppc->impstate->tempaddr), I0);						// mov     [tempaddr],i0
				UML_TEST(block, I0, 2);											// test    i0,i0,2
				UML_JMPc(block, COND_NZ, offs2 = label++);									// jnz     offs2
				UML_SUB(block, I0, I0, 1);									// sub     i0,i0,1
				UML_MOV(block, I2, 0x00ffffff);									// mov     i2,0x00ffffff
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHL(block, mem(&ppc->impstate->tempdata.w.l), I0, 8);			// shl     [tempdata],i0,8
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 3);				// add     i0,[tempaddr],3
				UML_MOV(block, I2, 0xff000000);									// mov     i2,0xff000000
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHR(block, I0, I0, 24);									// shr     i0,i0,24
				UML_OR(block, I0, I0, mem(&ppc->impstate->tempdata.w.l));			// or      i0,i0,[tempdata]
				UML_RET(block);																// ret
				UML_LABEL(block, offs2);												// offs2:
				UML_TEST(block, I0, 1);											// test    i0,i0,1
				UML_JMPc(block, COND_NZ, offs3 = label++);									// jnz     offs3
				UML_SUB(block, I0, I0, 2);									// sub     i0,i0,2
				UML_MOV(block, I2, 0x0000ffff);									// mov     i2,0x0000ffff
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHL(block, mem(&ppc->impstate->tempdata.w.l), I0, 16);		// shl     [tempdata],i0,16
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 2);				// add     i0,[tempaddr],2
				UML_MOV(block, I2, 0xffff0000);									// mov     i2,0xffff0000
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHR(block, I0, I0, 16);									// shr     i0,i0,16
				UML_OR(block, I0, I0, mem(&ppc->impstate->tempdata.w.l));			// or      i0,i0,[tempdata]
				UML_RET(block);																// ret
				UML_LABEL(block, offs3);												// offs3:
				UML_SUB(block, I0, I0, 3);									// sub     i0,i0,3
				UML_MOV(block, I2, 0x000000ff);									// mov     i2,0x000000ff
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHL(block, mem(&ppc->impstate->tempdata.w.l), I0, 24);		// shl     [tempdata],i0,24
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 1);				// add     i0,[tempaddr],1
				UML_MOV(block, I2, 0xffffff00);									// mov     i2,0xffffff00
				UML_CALLH(block, *masked);													// callh   masked
				UML_SHR(block, I0, I0, 8);									// shr     i0,i0,8
				UML_OR(block, I0, I0, mem(&ppc->impstate->tempdata.w.l));			// or      i0,i0,[tempdata]
			}
		}
		UML_RET(block);																		// ret
	}

	/* handle an alignment exception */
	if (alignex != 0)
	{
		UML_LABEL(block, alignex);														// alignex:
		UML_RECOVER(block, SPR32(SPROEA_DSISR), MAPVAR_DSISR);								// recover [dsisr],DSISR
		UML_EXH(block, *ppc->impstate->exception[EXCEPTION_ALIGN], I0);					// exh     align,i0
	}

	/* handle a TLB miss */
	if (tlbmiss != 0)
	{
		UML_LABEL(block, tlbmiss);														// tlbmiss:
		UML_MOV(block, mem(&ppc->param0), I0);											// mov     [param0],i0
		UML_MOV(block, mem(&ppc->param1), translate_type);								// mov     [param1],translate_type
		UML_CALLC(block, (c_function)ppccom_tlb_fill, ppc);												// callc   tlbfill,ppc
		UML_SHR(block, I3, I0, 12);											// shr     i3,i0,12
		UML_LOAD(block, I3, (void *)vtlb_table(ppc->vtlb), I3, SIZE_DWORD, SCALE_x4);// load    i3,[vtlb],i3,dword
		UML_TEST(block, I3, (UINT64)1 << translate_type);							// test    i3,1 << translate_type
		UML_JMPc(block, COND_NZ, tlbreturn);													// jmp     tlbreturn,nz

		/* 4XX case: protection exception */
		if (ppc->cap & PPCCAP_4XX)
		{
			UML_MOV(block, SPR32(SPR4XX_DEAR), I0);									// mov     [dear],i0
			UML_EXH(block, *ppc->impstate->exception[EXCEPTION_DSI], I0);				// exh     dsi,i0
		}

		/* 603 case: TLBMISS exception */
		else if (ppc->cap & PPCCAP_603_MMU)
		{
			UML_MOV(block, SPR32(SPR603_DMISS), I0);									// mov     [dmiss],i0
			UML_MOV(block, SPR32(SPR603_DCMP), mem(&ppc->mmu603_cmp));						// mov     [dcmp],[mmu603_cmp]
			UML_MOV(block, SPR32(SPR603_HASH1), mem(&ppc->mmu603_hash[0]));					// mov     [hash1],[mmu603_hash][0]
			UML_MOV(block, SPR32(SPR603_HASH2), mem(&ppc->mmu603_hash[1]));					// mov     [hash2],[mmu603_hash][1]
			if (iswrite)
				UML_EXH(block, *ppc->impstate->exception[EXCEPTION_DTLBMISSS], I0);		// exh     dtlbmisss,i0
			else
				UML_EXH(block, *ppc->impstate->exception[EXCEPTION_DTLBMISSL], I0);		// exh     dtlbmissl,i0
		}

		/* general case: DSI exception */
		else
		{
			UML_MOV(block, SPR32(SPROEA_DSISR), mem(&ppc->param0));							// mov     [dsisr],[param0]
			UML_EXH(block, *ppc->impstate->exception[EXCEPTION_DSI], I0);				// exh     dsi,i0
		}
	}

	block->end();
}


/*-------------------------------------------------
    static_generate_swap_tgpr - generate a
    subroutine to swap GPR0-3 with TGPR0-3
-------------------------------------------------*/

static void static_generate_swap_tgpr(powerpc_state *ppc)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;
	int regnum;

	/* begin generating */
	block = drcuml->begin_block(30);

	/* generate a hash jump via the current mode and PC */
	alloc_handle(drcuml, &ppc->impstate->swap_tgpr, "swap_tgpr");
	UML_HANDLE(block, *ppc->impstate->swap_tgpr);											// handle  swap_tgpr
	for (regnum = 0; regnum < 4; regnum++)
	{
		UML_MOV(block, I1, R32(regnum));												// mov     i1,r[regnum]
		UML_MOV(block, R32(regnum), mem(&ppc->mmu603_r[regnum]));							// mov     r[regnum],mmu603_r[regnum]
		UML_MOV(block, mem(&ppc->mmu603_r[regnum]), I1);								// mov     mmu603_r[regnum],i1
	}
	UML_RET(block);																			// ret

	block->end();
}


/*-------------------------------------------------
    static_generate_lsw_entries - generate a
    subroutine to perform LSWI/LSWX; one handle
    for each possible register
-------------------------------------------------*/

static void static_generate_lsw_entries(powerpc_state *ppc, int mode)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;
	int regnum;

	/* begin generating */
	block = drcuml->begin_block(32 * 30);

	/* iterate over all possible registers */
	for (regnum = 0; regnum < 32; regnum++)
	{
		char temp[20];

		/* allocate a handle */
		sprintf(temp, "lsw%d", regnum);
		alloc_handle(drcuml, &ppc->impstate->lsw[mode][regnum], temp);
		UML_HANDLE(block, *ppc->impstate->lsw[mode][regnum]);								// handle  lsw<regnum>
		UML_LABEL(block, regnum);														// regnum:
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 0);					// add     i0,[updateaddr],0
		UML_CALLH(block, *ppc->impstate->read8[mode]);										// callh   read8
		UML_ROLAND(block, R32(regnum), I0, 24, 0xff000000);					// roland  reg,i0,24,0xff000000
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 1);					// add     i0,[updateaddr],1
		UML_CALLH(block, *ppc->impstate->read8[mode]);										// callh   read8
		UML_ROLAND(block, I0, I0, 16, 0x00ff0000);						// roland  i0,i0,16,0x00ff0000
		UML_OR(block, R32(regnum), R32(regnum), I0);									// or      reg,i0
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 2);					// add     i0,[updateaddr],2
		UML_CALLH(block, *ppc->impstate->read8[mode]);										// callh   read8
		UML_ROLAND(block, I0, I0, 8, 0x0000ff00);						// roland  i0,i0,8,0x0000ff00
		UML_OR(block, R32(regnum), R32(regnum), I0);									// or      reg,i0
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 3);					// add     i0,[updateaddr],3
		UML_ADD(block, mem(&ppc->impstate->updateaddr), I0, 1);					// add     [updateaddr],i0,1
		UML_CALLH(block, *ppc->impstate->read8[mode]);										// callh   read8
		UML_ROLAND(block, I0, I0, 0, 0x000000ff);						// roland  i0,i0,0,0x000000ff
		UML_OR(block, R32(regnum), R32(regnum), I0);									// or      reg,i0
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_JMP(block, (regnum + 1) % 32);													// jmp     nextreg
	}

	block->end();
}


/*-------------------------------------------------
    static_generate_stsw_entries - generate a
    subroutine to perform LSWI/LSWX; one handle
    for each possible register
-------------------------------------------------*/

static void static_generate_stsw_entries(powerpc_state *ppc, int mode)
{
	drcuml_state *drcuml = ppc->impstate->drcuml;
	drcuml_block *block;
	int regnum;

	/* begin generating */
	block = drcuml->begin_block(32 * 30);

	/* iterate over all possible registers */
	for (regnum = 0; regnum < 32; regnum++)
	{
		char temp[20];

		/* allocate a handle */
		sprintf(temp, "stsw%d", regnum);
		alloc_handle(drcuml, &ppc->impstate->stsw[mode][regnum], temp);
		UML_HANDLE(block, *ppc->impstate->stsw[mode][regnum]);								// handle  stsw<regnum>
		UML_LABEL(block, regnum);														// regnum:
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 0);					// add     i0,[updateaddr],0
		UML_ROLAND(block, I1, R32(regnum), 8, 0xff);							// roland  i1,regnum,8,0xff
		UML_CALLH(block, *ppc->impstate->write8[mode]);										// callh   write8
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 1);					// add     i0,[updateaddr],1
		UML_ROLAND(block, I1, R32(regnum), 16, 0xff);						// roland  i1,regnum,16,0xff
		UML_CALLH(block, *ppc->impstate->write8[mode]);										// callh   write8
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 2);					// add     i0,[updateaddr],2
		UML_ROLAND(block, I1, R32(regnum), 24, 0xff);						// roland  i1,regnum,24,0xff
		UML_CALLH(block, *ppc->impstate->write8[mode]);										// callh   write8
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_ADD(block, I0, mem(&ppc->impstate->updateaddr), 3);					// add     i0,[updateaddr],3
		UML_ADD(block, mem(&ppc->impstate->updateaddr), I0, 1);					// add     [updateaddr],i0,1
		UML_ROLAND(block, I1, R32(regnum), 0, 0xff);							// roland  i1,regnum,0,0xff
		UML_CALLH(block, *ppc->impstate->write8[mode]);										// callh   write8
		UML_SUB(block, mem(&ppc->impstate->swcount), mem(&ppc->impstate->swcount), 1);	// sub     [swcount],[swcount],1
		UML_RETc(block, COND_Z);																// ret     z
		UML_JMP(block, (regnum + 1) % 32);													// jmp     nextreg
	}

	block->end();
}



/***************************************************************************
    CODE GENERATION
***************************************************************************/

/*-------------------------------------------------
    generate_update_mode - update the mode based
    on the MSR
-------------------------------------------------*/

static void generate_update_mode(powerpc_state *ppc, drcuml_block *block)
{
	/* LE in bit 0 of mode */
	UML_AND(block, I0, MSR32, MSR_LE);											// and     i0,msr,MSR_LE

	/* DR (OEA and 403GCX) in bit 1 of mode */
	if ((ppc->cap & PPCCAP_OEA) || ppc->flavor == PPC_MODEL_403GCX)
	{
		UML_ROLAND(block, I1, MSR32, 29, 0x02);								// roland  i1,[msr],29,0x02
		UML_OR(block, I0, I0, I1);											// or      i0,i0,i1
	}

	/* (4XX) in bit 1 of mode */
	if (ppc->cap & PPCCAP_4XX)
	{
		UML_ROLAND(block, I1, MSR32, 30, 0x02);								// roland  i1,[msr],30,0x02
		UML_OR(block, I0, I0, I1);											// or      i0,i0,i1
	}

	/* PR in bit 2 of mode */
	UML_ROLAND(block, I1, MSR32, 20, 0x04);									// roland  i1,[msr],20,0x04
	UML_OR(block, mem(&ppc->impstate->mode), I0, I1);								// or      [mode],i0,i1
}


/*-------------------------------------------------
    generate_update_cycles - generate code to
    subtract cycles from the icount and generate
    an exception if out
-------------------------------------------------*/

static void generate_update_cycles(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, parameter param, int allow_exception)
{
	/* check full interrupts if pending */
	if (compiler->checkints)
	{
		code_label skip;

		compiler->checkints = FALSE;
		UML_TEST(block, mem(&ppc->irq_pending), ~0);									// test    [irq_pending],0
		UML_JMPc(block, COND_Z, skip = compiler->labelnum++);									// jmp     skip,Z
		UML_TEST(block, MSR32, MSR_EE);												// test    [msr],MSR_EE
		UML_JMPc(block, COND_Z, skip);														// jmp     skip,Z
		UML_MOV(block, I0, param);										// mov     i0,nextpc
		UML_MOV(block, I1, compiler->cycles);										// mov     i1,cycles
		UML_CALLH(block, *ppc->impstate->exception_norecover[EXCEPTION_EI]);					// callh   interrupt_norecover
		UML_LABEL(block, skip);															// skip:
	}

	/* account for cycles */
	if (compiler->cycles > 0)
	{
		UML_SUB(block, mem(&ppc->icount), mem(&ppc->icount), MAPVAR_CYCLES);				// sub     icount,icount,cycles
		UML_MAPVAR(block, MAPVAR_CYCLES, 0);												// mapvar  cycles,0
		if (allow_exception)
			UML_EXHc(block, COND_S, *ppc->impstate->out_of_cycles, param);		// exh     out_of_cycles,nextpc
	}
	compiler->cycles = 0;
}


/*-------------------------------------------------
    generate_checksum_block - generate code to
    validate a sequence of opcodes
-------------------------------------------------*/

static void generate_checksum_block(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *seqhead, const opcode_desc *seqlast)
{
	const opcode_desc *curdesc;
	if (LOG_UML)
		block->append_comment("[Validation for %08X]", seqhead->pc);						// comment

	/* loose verify or single instruction: just compare and fail */
	if (!(ppc->impstate->drcoptions & PPCDRC_STRICT_VERIFY) || seqhead->next() == NULL)
	{
		if (!(seqhead->flags & OPFLAG_VIRTUAL_NOOP))
		{
			void *base = ppc->direct->read_decrypted_ptr(seqhead->physpc, ppc->codexor);
			UML_LOAD(block, I0, base, 0, SIZE_DWORD, SCALE_x4);					// load    i0,base,dword
			UML_CMP(block, I0, seqhead->opptr.l[0]);								// cmp     i0,*opptr
			UML_EXHc(block, COND_NE, *ppc->impstate->nocode, seqhead->pc);				// exne    nocode,seqhead->pc
		}
	}

	/* full verification; sum up everything */
	else
	{
#if 0
		for (curdesc = seqhead->next(); curdesc != seqlast->next(); curdesc = curdesc->next())
			if (!(curdesc->flags & OPFLAG_VIRTUAL_NOOP))
			{
				void *base = ppc->direct->read_decrypted_ptr(seqhead->physpc, ppc->codexor);
				UML_LOAD(block, I0, base, 0, SIZE_DWORD, SCALE_x4);				// load    i0,base,dword
				UML_CMP(block, I0, curdesc->opptr.l[0]);							// cmp     i0,*opptr
				UML_EXHc(block, COND_NE, *ppc->impstate->nocode, seqhead->pc);			// exne    nocode,seqhead->pc
			}
#else
		UINT32 sum = 0;
		void *base = ppc->direct->read_decrypted_ptr(seqhead->physpc, ppc->codexor);
		UML_LOAD(block, I0, base, 0, SIZE_DWORD, SCALE_x4);						// load    i0,base,dword
		sum += seqhead->opptr.l[0];
		for (curdesc = seqhead->next(); curdesc != seqlast->next(); curdesc = curdesc->next())
			if (!(curdesc->flags & OPFLAG_VIRTUAL_NOOP))
			{
				base = ppc->direct->read_decrypted_ptr(curdesc->physpc, ppc->codexor);
				UML_LOAD(block, I1, base, 0, SIZE_DWORD, SCALE_x4);				// load    i1,base,dword
				UML_ADD(block, I0, I0, I1);									// add     i0,i0,i1
				sum += curdesc->opptr.l[0];
			}
		UML_CMP(block, I0, sum);													// cmp     i0,sum
		UML_EXHc(block, COND_NE, *ppc->impstate->nocode, seqhead->pc);					// exne    nocode,seqhead->pc
#endif
	}
}


/*-------------------------------------------------
    generate_sequence_instruction - generate code
    for a single instruction in a sequence
-------------------------------------------------*/

static void generate_sequence_instruction(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	int hotnum;

	/* add an entry for the log */
	if (LOG_UML && !(desc->flags & OPFLAG_VIRTUAL_NOOP))
		log_add_disasm_comment(block, desc->pc, desc->opptr.l[0]);

	/* set the PC map variable */
	UML_MAPVAR(block, MAPVAR_PC, desc->pc);													// mapvar  PC,desc->pc

	/* accumulate total cycles */
	compiler->cycles += desc->cycles;

	/* is this a hotspot? */
	for (hotnum = 0; hotnum < PPC_MAX_HOTSPOTS; hotnum++)
		if (ppc->impstate->hotspot[hotnum].pc != 0 && desc->pc == ppc->impstate->hotspot[hotnum].pc && desc->opptr.l[0] == ppc->impstate->hotspot[hotnum].opcode)
		{
			compiler->cycles += ppc->impstate->hotspot[hotnum].cycles;
			break;
		}

	/* update the icount map variable */
	UML_MAPVAR(block, MAPVAR_CYCLES, compiler->cycles);										// mapvar  CYCLES,compiler->cycles

	/* if we want a probe, add it here */
	if (desc->pc == PROBE_ADDRESS)
	{
		UML_MOV(block, mem(&ppc->pc), desc->pc);										// mov     [pc],desc->pc
		UML_CALLC(block, cfunc_printf_probe, (void *)(FPTR)desc->pc);										// callc   cfunc_printf_probe,desc->pc
	}

	/* if we are debugging, call the debugger */
	if ((ppc->device->machine().debug_flags & DEBUG_FLAG_ENABLED) != 0)
	{
		UML_MOV(block, mem(&ppc->pc), desc->pc);										// mov     [pc],desc->pc
		save_fast_iregs(ppc, block);														// <save fastregs>
		UML_DEBUG(block, desc->pc);													// debug   desc->pc
	}

	/* if we hit an unmapped address, fatal error */
	if (desc->flags & OPFLAG_COMPILER_UNMAPPED)
	{
		UML_MOV(block, mem(&ppc->pc), desc->pc);										// mov     [pc],desc->pc
		save_fast_iregs(ppc, block);														// <save fastregs>
		UML_EXIT(block, EXECUTE_UNMAPPED_CODE);										// exit    EXECUTE_UNMAPPED_CODE
	}

	/* if we hit a compiler page fault, it's just like a TLB mismatch */
	if (desc->flags & OPFLAG_COMPILER_PAGE_FAULT)
	{
		if (PRINTF_MMU)
		{
			const char *text = "Compiler page fault @ %08X\n";
			UML_MOV(block, mem(&ppc->impstate->format), (FPTR)text);					// mov     [format],text
			UML_MOV(block, mem(&ppc->impstate->arg0), desc->pc);						// mov     [arg0],desc->pc
			UML_CALLC(block, cfunc_printf_debug, ppc);										// callc   printf_debug
		}
		UML_EXH(block, *ppc->impstate->tlb_mismatch, 0);								// exh     tlb_mismatch,0
	}

	/* validate our TLB entry at this PC; if we fail, we need to handle it */
	if ((desc->flags & OPFLAG_VALIDATE_TLB) && (ppc->impstate->mode & MODE_DATA_TRANSLATION))
	{
		const vtlb_entry *tlbtable = vtlb_table(ppc->vtlb);

		/* if we currently have a valid TLB read entry, we just verify */
		if (tlbtable[desc->pc >> 12] != 0)
		{
			if (PRINTF_MMU)
			{
				const char *text = "Checking TLB at @ %08X\n";
				UML_MOV(block, mem(&ppc->impstate->format), (FPTR)text);				// mov     [format],text
				UML_MOV(block, mem(&ppc->impstate->arg0), desc->pc);					// mov     [arg0],desc->pc
				UML_CALLC(block, cfunc_printf_debug, ppc);									// callc   printf_debug
			}
			UML_LOAD(block, I0, &tlbtable[desc->pc >> 12], 0, SIZE_DWORD, SCALE_x4);// load    i0,tlbtable[desc->pc >> 12],dword
			UML_CMP(block, I0, tlbtable[desc->pc >> 12]);							// cmp     i0,*tlbentry
			UML_EXHc(block, COND_NE, *ppc->impstate->tlb_mismatch, 0);					// exh     tlb_mismatch,0,NE
		}

		/* otherwise, we generate an unconditional exception */
		else
		{
			if (PRINTF_MMU)
			{
				const char *text = "No valid TLB @ %08X\n";
				UML_MOV(block, mem(&ppc->impstate->format), (FPTR)text);				// mov     [format],text
				UML_MOV(block, mem(&ppc->impstate->arg0), desc->pc);					// mov     [arg0],desc->pc
				UML_CALLC(block, cfunc_printf_debug, ppc);									// callc   printf_debug
			}
			UML_EXH(block, *ppc->impstate->tlb_mismatch, 0);							// exh     tlb_mismatch,0
		}
	}

	/* if this is an invalid opcode, generate the exception now */
	if (desc->flags & OPFLAG_INVALID_OPCODE)
		UML_EXH(block, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x80000);			// exh    exception_program,0x80000

	/* if this is a privileged opcode in user mode, generate the exception */
	else if ((desc->flags & OPFLAG_PRIVILEGED) && (ppc->impstate->mode & MODE_USER))
		UML_EXH(block, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x40000);			// exh    exception_program,0x40000

	/* otherwise, unless this is a virtual no-op, it's a regular instruction */
	else if (!(desc->flags & OPFLAG_VIRTUAL_NOOP))
	{
		/* compile the instruction */
		if (!generate_opcode(ppc, block, compiler, desc))
		{
			UML_MOV(block, mem(&ppc->pc), desc->pc);									// mov     [pc],desc->pc
			UML_MOV(block, mem(&ppc->impstate->arg0), desc->opptr.l[0]);				// mov     [arg0],*desc->opptr.l
			UML_CALLC(block, cfunc_unimplemented, ppc);										// callc   cfunc_unimplemented,ppc
		}
	}
}


/*------------------------------------------------------------------
    generate_compute_flags - compute CR0 and/or XER flags
------------------------------------------------------------------*/

static void generate_compute_flags(powerpc_state *ppc, drcuml_block *block, const opcode_desc *desc, int updatecr, UINT32 xermask, int invertcarry)
{
	UINT32 xerflags;

	/* modify inputs based on required flags */
	if (!DISABLE_FLAG_OPTIMIZATIONS)
	{
		if (!(desc->regreq[3] & REGFLAG_XER_CA))
			xermask &= ~XER_CA;
		if (!(desc->regreq[2] & REGFLAG_CR(0)))
			updatecr = 0;
	}
	xerflags = ((xermask & XER_OV) ? FLAG_V : 0) | ((xermask & XER_CA) ? FLAG_C : 0);

	/* easy case: nothing to do */
	if (!updatecr && xermask == 0)
		return;

	/* semi-easy case: crfield only */
	if (xermask == 0)
	{
		UML_GETFLGS(block, I0, FLAG_S | FLAG_Z);										// getflgs i0,sz
		UML_LOAD(block, I0, ppc->impstate->sz_cr_table, I0, SIZE_BYTE, SCALE_x1);	// load    i0,sz_cr_table,i0,byte
        UML_OR(block, CR32(0), I0, XERSO32);											// or      [cr0],i0,[xerso]
		return;
	}

	/* semi-easy case: xer only */
	if (!updatecr)
	{
		if (xermask & XER_OV)
		{
			UML_GETFLGS(block, I0, xerflags);											// getflgs i0,xerflags
			if (invertcarry && (xermask & XER_CA))
				UML_XOR(block, I0, I0, FLAG_C);								// xor     i0,i0,FLAG_C
			UML_ROLINS(block, SPR32(SPR_XER), I0, 29, xermask);				// rolins  [xer],i0,29,xermask
			UML_SHR(block, I0, I0, 1);										// shr     i0,i0,1
			UML_OR(block, XERSO32, XERSO32, I0);										// or      [xerso],i0
		}
		else
		{
			UML_SETc(block, invertcarry ? COND_NC : COND_C, I0);							// setc    i0,nc/c
			UML_ROLINS(block, SPR32(SPR_XER), I0, 29, XER_CA);				// rolins  [xer],i0,29,XER_CA
		}
		return;
	}

	/* tricky case: both */
	UML_GETFLGS(block, I0, FLAG_S | FLAG_Z | xerflags);								// getflgs i0,SZ | xerflags
	UML_LOAD(block, I1, ppc->impstate->sz_cr_table, I0, SIZE_BYTE, SCALE_x1);		// load    i1,sz_cr_table,i0,byte
	if (invertcarry && (xermask & XER_CA))
		UML_XOR(block, I0, I0, FLAG_C);										// xor     i0,i0,FLAG_C
	UML_ROLINS(block, SPR32(SPR_XER), I0, 29, xermask);						// rolins  [xer],i0,29,xermask
	if (xermask & XER_OV)
	{
		UML_ROLAND(block, I0, I0, 31, 1);								// roland  i0,i0,31,0x0001
		UML_OR(block, XERSO32, XERSO32, I0);							// or      [xerso],i0
        UML_AND(block, CR32(0), CR32(0), 0xfffffffe);                   // and  [cr0], [cr0], 0xfffffffe (clear SO copy in CR32)
		UML_OR(block, CR32(0), I1, XERSO32);							// or      [cr0],i1,[xerso]
	}
	else
    {
        UML_AND(block, CR32(0), CR32(0), 0xfffffffe);                   // and  [cr0], [cr0], 0xfffffffe (clear SO copy in CR32)
		UML_OR(block, CR32(0), I1, XERSO32);							// or      [cr0],i1,[xerso] (OR in new value from XERSO)
    }
}

/*-------------------------------------------------
    generate_fp_flags - compute FPSCR floating
    point status flags
-------------------------------------------------*/

static void generate_fp_flags(powerpc_state *ppc, drcuml_block *block, const opcode_desc *desc, int updatefprf)
{
	/* for now, only handle the FPRF field */
	if (updatefprf)
	{
		UML_MOV(block, mem(&ppc->param0), G_RD(desc->opptr.l[0]));
		UML_CALLC(block, (c_function)ppccom_update_fprf, ppc);
	}
}

/*-------------------------------------------------
    generate_branch - generate an unconditional
    branch
-------------------------------------------------*/

static void generate_branch(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc, int source, UINT8 link)
{
	compiler_state compiler_temp = *compiler;
	UINT32 *srcptr = &ppc->spr[source];

	/* set the link if needed */
	if (link)
	{
		if (desc->targetpc == BRANCH_TARGET_DYNAMIC && source == SPR_LR)
		{
			UML_MOV(block, mem(&ppc->impstate->tempaddr), mem(srcptr));						// mov     [tempaddr],[lr]
			srcptr = &ppc->impstate->tempaddr;
		}
		UML_MOV(block, SPR32(SPR_LR), desc->pc + 4);									// mov     [lr],desc->pc + 4
	}

	/* update the cycles and jump through the hash table to the target */
	if (desc->targetpc != BRANCH_TARGET_DYNAMIC)
	{
		generate_update_cycles(ppc, block, &compiler_temp, desc->targetpc, TRUE);		// <subtract cycles>
		if (desc->flags & OPFLAG_INTRABLOCK_BRANCH)
			UML_JMP(block, desc->targetpc | 0x80000000);									// jmp     desc->targetpc | 0x80000000
		else
			UML_HASHJMP(block, ppc->impstate->mode, desc->targetpc, *ppc->impstate->nocode);
																							// hashjmp <mode>,desc->targetpc,nocode
	}
	else
	{
		generate_update_cycles(ppc, block, &compiler_temp, mem(srcptr), TRUE);				// <subtract cycles>
		UML_HASHJMP(block, ppc->impstate->mode, mem(srcptr), *ppc->impstate->nocode);	// hashjmp <mode>,<rsreg>,nocode
	}

	/* update the label */
	compiler->labelnum = compiler_temp.labelnum;

	/* reset the mapvar to the current cycles */
	UML_MAPVAR(block, MAPVAR_CYCLES, compiler->cycles);										// mapvar  CYCLES,compiler->cycles
}


/*-------------------------------------------------
    generate_branch_bo - generate a conditional
    branch based on the BO and BI fields
-------------------------------------------------*/

static void generate_branch_bo(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc, UINT32 bo, UINT32 bi, int source, int link)
{
	int skip = compiler->labelnum++;

	if (!(bo & 0x04))
	{
		UML_SUB(block, SPR32(SPR_CTR), SPR32(SPR_CTR), 1);								// sub   [ctr],[ctr],1
		UML_JMPc(block, (bo & 0x02) ? COND_NZ : COND_Z, skip);									// jmp   skip,nz/z
	}
	if (!(bo & 0x10))
	{
		UML_TEST(block, CR32(bi / 4), 8 >> (bi % 4));									// test  cr32(bi/4),8 >> (bi % 4)
		UML_JMPc(block, (bo & 0x08) ? COND_Z : COND_NZ, skip);									// jmp   skip,z/nz
	}
	generate_branch(ppc, block, compiler, desc, source, link);								// <branch>
	UML_LABEL(block, skip);																// skip:
}


/*-------------------------------------------------
    generate_opcode - generate code for a specific
    opcode
-------------------------------------------------*/

static int generate_opcode(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	UINT32 op = desc->opptr.l[0];
	UINT32 opswitch = op >> 26;
	int regnum;

	switch (opswitch)
	{
		case 0x02:	/* TDI - 64-bit only */
		case 0x1e:	/* 0x1e group - 64-bit only */
		case 0x3a:	/* 0x3a group - 64-bit only */
		case 0x3e:	/* 0x3e group - 64-bit only */
			return FALSE;

		case 0x03:	/* TWI */
			UML_CMP(block, R32(G_RA(op)), (INT16)G_SIMM(op));							// cmp     ra,simm
			if (G_TO(op) & 0x10)
				UML_EXHc(block, COND_L, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,l
			if (G_TO(op) & 0x08)
				UML_EXHc(block, COND_G, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,g
			if (G_TO(op) & 0x04)
				UML_EXHc(block, COND_E, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,e
			if (G_TO(op) & 0x02)
				UML_EXHc(block, COND_B,  *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,b
			if (G_TO(op) & 0x01)
				UML_EXHc(block, COND_A,  *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,a
			return TRUE;

		case 0x07:	/* MULLI */
			UML_MULS(block, R32(G_RD(op)), R32(G_RD(op)), R32(G_RA(op)), (INT16)G_SIMM(op));
																							// muls    rd,rd,ra,simm
			return TRUE;

		case 0x0e:	/* ADDI */
			UML_ADD(block, R32(G_RD(op)), R32Z(G_RA(op)), (INT16)G_SIMM(op));			// add     rd,ra,simm
			return TRUE;

		case 0x0f:	/* ADDIS */
			UML_ADD(block, R32(G_RD(op)), R32Z(G_RA(op)), G_SIMM(op) << 16);			// add     rd,ra,simm << 16
			return TRUE;

		case 0x0a:	/* CMPLI */
			UML_CMP(block, R32(G_RA(op)), G_UIMM(op));									// cmp     ra,uimm
			UML_GETFLGS(block, I0, FLAG_Z | FLAG_C);									// getflgs i0,zc
			UML_LOAD(block, I0, ppc->impstate->cmpl_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,cmpl_cr_table,i0,byte
			UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);								// or      [crn],i0,[xerso]
			return TRUE;

		case 0x0b:	/* CMPI */
			UML_CMP(block, R32(G_RA(op)), (INT16)G_SIMM(op));							// cmp     ra,uimm
			UML_GETFLGS(block, I0, FLAG_Z | FLAG_C | FLAG_S);							// getflgs i0,zcs
			UML_LOAD(block, I0, ppc->impstate->cmp_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,cmp_cr_table,i0,byte
			UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);								// or      [crn],i0,[xerso]
			return TRUE;

		case 0x08:	/* SUBFIC */
			UML_SUB(block, R32(G_RD(op)), (INT16)G_SIMM(op), R32(G_RA(op)));			// sub     rd,simm,ra
			generate_compute_flags(ppc, block, desc, FALSE, XER_CA, TRUE);					// <update flags>
			return TRUE;

		case 0x0c:	/* ADDIC */
			UML_ADD(block, R32(G_RD(op)), R32(G_RA(op)), (INT16)G_SIMM(op));			// add     rd,ra,simm
			generate_compute_flags(ppc, block, desc, FALSE, XER_CA, FALSE);					// <update flags>
			return TRUE;

		case 0x0d:	/* ADDIC. */
			UML_ADD(block, R32(G_RD(op)), R32(G_RA(op)), (INT16)G_SIMM(op));			// add     rd,ra,simm
			generate_compute_flags(ppc, block, desc, TRUE, XER_CA, FALSE);					// <update flags>
			return TRUE;

		case 0x10:	/* BCx */
			generate_branch_bo(ppc, block, compiler, desc, G_BO(op), G_BI(op), 0, op & M_LK);// <branch conditional>
			return TRUE;

		case 0x11:	/* SC */
			UML_MAPVAR(block, MAPVAR_PC, desc->pc + 4);										// mapvar  PC,desc->pc+4
			UML_EXH(block, *ppc->impstate->exception[EXCEPTION_SYSCALL], 0);			// exh     syscall,0
			return TRUE;

		case 0x12:	/* Bx */
			generate_branch(ppc, block, compiler, desc, 0, op & M_LK);						// <branch>
			return TRUE;

		case 0x13:	/* 0x13 group */
			return generate_instruction_13(ppc, block, compiler, desc);						// <group13>

		case 0x14:	/* RLWIMIx */
			UML_ROLINS(block, R32(G_RA(op)), R32(G_RS(op)), G_SH(op), compute_rlw_mask(G_MB(op), G_ME(op)));
																							// rolins ra,rs,sh,mask
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x15:	/* RLWINMx */
			UML_ROLAND(block, R32(G_RA(op)), R32(G_RS(op)), G_SH(op), compute_rlw_mask(G_MB(op), G_ME(op)));
																							// roland ra,rs,sh,mask
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x17:	/* RLWNMx */
			UML_ROLAND(block, R32(G_RA(op)), R32(G_RS(op)), R32(G_RB(op)), compute_rlw_mask(G_MB(op), G_ME(op)));
																							// roland ra,rs,rb,mask
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x18:	/* ORI */
			UML_OR(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op));					// or      ra,rs,uimm
			return TRUE;

		case 0x19:	/* ORIS */
			UML_OR(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op) << 16);				// or      ra,rs,uimm << 16
			return TRUE;

		case 0x1a:	/* XORI */
			UML_XOR(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op));					// xor     ra,rs,uimm
			return TRUE;

		case 0x1b:	/* XORIS */
			UML_XOR(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op) << 16);			// xor     ra,rs,uimm << 16
			return TRUE;

		case 0x1c:	/* ANDI. */
			UML_AND(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op));					// and     ra,rs,uimm
			generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);						// <update flags>
			return TRUE;

		case 0x1d:	/* ANDIS. */
			UML_AND(block, R32(G_RA(op)), R32(G_RS(op)), G_UIMM(op) << 16);			// and  ra,rs,uimm << 16
			generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);						// <update flags>
			return TRUE;

		case 0x1f:	/* 0x1f group */
			return generate_instruction_1f(ppc, block, compiler, desc);						// <group1f>

		case 0x22:	/* LBZ */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read8[ppc->impstate->mode]);					// callh   read8
			UML_AND(block, R32(G_RD(op)), I0, 0xff);								// and     rd,i0,0xff
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x28:	/* LHZ */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_AND(block, R32(G_RD(op)), I0, 0xffff);							// and     rd,i0,0xffff
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2a:	/* LHA */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_SEXT(block, R32(G_RD(op)), I0, SIZE_WORD);									// sext    rd,i0,word
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x20:	/* LWZ */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, R32(G_RD(op)), I0);											// mov     rd,i0
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x23:	/* LBZU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read8[ppc->impstate->mode]);					// callh   read8
			UML_AND(block, R32(G_RD(op)), I0, 0xff);								// and     rd,i0,0xff
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x29:	/* LHZU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_AND(block, R32(G_RD(op)), I0, 0xffff);							// and     rd,i0,0xffff
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2b:	/* LHAU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_SEXT(block, R32(G_RD(op)), I0, SIZE_WORD);									// sext    rd,i0,word
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x21:	/* LWZU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, R32(G_RD(op)), I0);											// mov     rd,i0
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x26:	/* STB */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_AND(block, I1, R32(G_RS(op)), 0xff);								// and     i1,rs,0xff
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->write8[ppc->impstate->mode]);					// callh   write8
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2c:	/* STH */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_AND(block, I1, R32(G_RS(op)), 0xffff);							// and     i1,rs,0xffff
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->write16[ppc->impstate->mode]);					// callh   write16
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x24:	/* STW */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MOV(block, I1, R32(G_RS(op)));											// mov     i1,rs
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x27:	/* STBU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_AND(block, I1, R32(G_RS(op)), 0xff);								// and     i1,rs,0xff
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->write8[ppc->impstate->mode]);					// callh   write8
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2d:	/* STHU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_AND(block, I1, R32(G_RS(op)), 0xffff);							// and     i1,rs,0xffff
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->write16[ppc->impstate->mode]);					// callh   write16
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x25:	/* STWU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, I1, R32(G_RS(op)));											// mov     i1,rs
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2e:	/* LMW */
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_MOV(block, mem(&ppc->impstate->tempaddr), R32Z(G_RA(op)));					// mov     [tempaddr],ra
			for (regnum = G_RD(op); regnum < 32; regnum++)
			{
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), (INT16)G_SIMM(op) + 4 * (regnum - G_RD(op)));
																							// add     i0,[tempaddr],simm + 4*(regnum-rd)
				UML_CALLH(block, *ppc->impstate->read32align[ppc->impstate->mode]);			// callh   read32align
				UML_MOV(block, R32(regnum), I0);										// mov     regnum,i0
			}
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2f:	/* STMW */
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_MOV(block, mem(&ppc->impstate->tempaddr), R32Z(G_RA(op)));					// mov     [tempaddr],ra
			for (regnum = G_RS(op); regnum < 32; regnum++)
			{
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), (INT16)G_SIMM(op) + 4 * (regnum - G_RS(op)));
																							// add     i0,[tempaddr],simm + 4*(regnum-rs)
				UML_MOV(block, I1, R32(regnum));										// mov     i1,regnum
				UML_CALLH(block, *ppc->impstate->write32align[ppc->impstate->mode]);			// callh   write32align
			}
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x30:	/* LFS */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I0);						// mov     [tempdata],i0
			UML_FDFRFLT(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l), SIZE_DWORD);	// fdfrflt fd,[tempdata],dword
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x32:	/* LFD */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->read64[ppc->impstate->mode]);					// callh   read64
			UML_DMOV(block, mem(&ppc->impstate->tempdata.d), I0);						// dmov    [tempdata],i0
			UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.d));				// fdmov   fd,[tempdata]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x31:	/* LFSU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I0);						// mov     [tempdata],i0
			UML_FDFRFLT(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l), SIZE_DWORD);	// fdfrflt fd,[tempdata],dword
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x33:	/* LFDU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->read64[ppc->impstate->mode]);					// callh   read64
			UML_DMOV(block, mem(&ppc->impstate->tempdata.d), I0);						// dmov    [tempdata],i0
			UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.d));				// fdmov   fd,[tempdata]
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x34:	/* STFS */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_FSFRFLT(block, mem(&ppc->impstate->tempdata.w.l), F64(G_RS(op)), SIZE_QWORD);	// fsfrflt [tempdata],rs,qword
			UML_MOV(block, I1, mem(&ppc->impstate->tempdata.w.l));						// mov     i1,[tempdata]
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x36:	/* STFD */
			UML_ADD(block, I0, R32Z(G_RA(op)), (INT16)G_SIMM(op));				// add     i0,ra,simm
			UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RS(op)));				// fdmov   [tempdata],rs
			UML_DMOV(block, I1, mem(&ppc->impstate->tempdata.d));						// dmov    i1,[tempdata]
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMM(op));									// mapvar  dsisr,DSISR_IMM(op)
			UML_CALLH(block, *ppc->impstate->write64[ppc->impstate->mode]);					// callh   write64
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x35:	/* STFSU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_FSFRFLT(block, mem(&ppc->impstate->tempdata.w.l), F64(G_RS(op)), SIZE_QWORD);	// fsfrflt [tempdata],rs,qword
			UML_MOV(block, I1, mem(&ppc->impstate->tempdata.w.l));						// mov     i1,[tempdata]
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x37:	/* STFDU */
			UML_ADD(block, I0, R32(G_RA(op)), (INT16)G_SIMM(op));					// add     i0,ra,simm
			UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RS(op)));				// fdmov   [tempdata],rs
			UML_DMOV(block, I1, mem(&ppc->impstate->tempdata.d));						// dmov    i1,[tempdata]
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IMMU(op));								// mapvar  dsisr,DSISR_IMMU(op)
			UML_CALLH(block, *ppc->impstate->write64[ppc->impstate->mode]);					// callh   write64
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x3b:	/* 0x3b group */
			return generate_instruction_3b(ppc, block, compiler, desc);						// <group3b>

		case 0x3f:	/* 0x3f group */
			return generate_instruction_3f(ppc, block, compiler, desc);						// <group3f>
	}

	return FALSE;
}


/*-------------------------------------------------
    generate_instruction_13 - compile opcodes in
    the 0x13 group
-------------------------------------------------*/

static int generate_instruction_13(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	UINT32 op = desc->opptr.l[0];
	UINT32 opswitch = (op >> 1) & 0x3ff;

	switch (opswitch)
	{
		case 0x010:	/* BCLRx */
			generate_branch_bo(ppc, block, compiler, desc, G_BO(op), G_BI(op), SPR_LR, op & M_LK);// <branch conditional>
			return TRUE;

		case 0x210:	/* BCCTRx */
			generate_branch_bo(ppc, block, compiler, desc, G_BO(op), G_BI(op), SPR_CTR, op & M_LK);
																							// <branch conditional>
			return TRUE;

		case 0x000:	/* MCRF */
			UML_MOV(block, CR32(G_CRFD(op)), CR32(G_CRFS(op)));								// mov     [crd],[crs]
			return TRUE;

		case 0x101:	/* CRAND */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_AND(block, I0, I0, I1);										// and     i0,i1
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x081:	/* CRANDC */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_XOR(block, I1, I1, ~0);										// xor     i1,~0
			UML_AND(block, I0, I0, I1);										// and     i0,i1
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x0e1:	/* CRNAND */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_AND(block, I0, I0, I1);										// and     i0,i1
			UML_XOR(block, I0, I0, ~0);										// xor     i0,~0
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x1c1:	/* CROR */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_OR(block, I0, I0, I1);										// or      i0,i1
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x1a1:	/* CRORC */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_XOR(block, I1, I1, ~0);										// xor     i1,~0
			UML_OR(block, I0, I0, I1);										// or      i0,i1
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x021:	/* CRNOR */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_OR(block, I0, I0, I1);										// or      i0,i1
			UML_XOR(block, I0, I0, ~0);										// xor     i0,~0
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x0c1:	/* CRXOR */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_XOR(block, I0, I0, I1);										// xor     i0,i1
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x121:	/* CREQV */
			UML_SHL(block, I0, CR32(G_CRBA(op) / 4), G_CRBA(op) % 4);				// shl     i0,cr(a / 4),a % 4
			UML_SHL(block, I1, CR32(G_CRBB(op) / 4), G_CRBB(op) % 4);				// shl     i1,cr(b / 4),b % 4
			UML_XOR(block, I0, I0, I1);										// xor     i0,i1
			UML_XOR(block, I0, I0, ~0);										// xor     i0,~0
			UML_ROLINS(block, CR32(G_CRBD(op) / 4), I0, 32 - G_CRBD(op) % 4, 8 >> (G_CRBD(op) % 4));
																							// rolins  cr(d / 4),i0,32-(d % 4),8 >> (d % 4)
			return TRUE;

		case 0x032:	/* RFI */
			if (ppc->cap & PPCCAP_OEA)
			{
				if (!(ppc->cap & PPCCAP_603_MMU))
					UML_ROLINS(block, MSR32, SPR32(SPROEA_SRR1), 0, 0x87c0ffff);	// rolins  [msr],[srr1],0,0x87c0ffff
				else
				{
					UML_MOV(block, I0, MSR32);											// mov     i0,[msr]
					UML_ROLINS(block, MSR32, SPR32(SPROEA_SRR1), 0, 0x87c0ffff |  MSR603_TGPR);
																							// rolins  [msr],[srr1],0,0x87c0ffff | MSR603_TGPR
					UML_XOR(block, I0, I0, MSR32);								// xor     i0,i0,[msr]
					UML_TEST(block, I0, MSR603_TGPR);								// test    i0,tgpr
					UML_CALLHc(block, COND_NZ, *ppc->impstate->swap_tgpr);						// callh   swap_tgpr,nz
				}
			}
			else if (ppc->cap & PPCCAP_4XX)
				UML_MOV(block, MSR32, SPR32(SPR4XX_SRR1));									// mov     [msr],[srr1]
			generate_update_mode(ppc, block);												// <update mode>
			compiler->checkints = TRUE;
			generate_update_cycles(ppc, block, compiler, SPR32(SPROEA_SRR0), TRUE);			// <subtract cycles>
			UML_HASHJMP(block, mem(&ppc->impstate->mode), SPR32(SPROEA_SRR0), *ppc->impstate->nocode);
																							// hashjmp mode,[srr0],nocode
			return TRUE;

		case 0x033:	/* RFCI */
			assert(ppc->cap & PPCCAP_4XX);
			UML_MOV(block, MSR32, SPR32(SPR4XX_SRR3));										// mov     [msr],[srr3]
			generate_update_mode(ppc, block);												// <update mode>
			compiler->checkints = TRUE;
			generate_update_cycles(ppc, block, compiler, SPR32(SPR4XX_SRR2), TRUE);			// <subtract cycles>
			UML_HASHJMP(block, mem(&ppc->impstate->mode), SPR32(SPR4XX_SRR2), *ppc->impstate->nocode);
																							// hashjmp mode,[srr2],nocode
			return TRUE;

		case 0x096:	/* ISYNC */
			/* effective no-op */
			return TRUE;
	}

	return FALSE;
}


/*-------------------------------------------------
    generate_instruction_1f - compile opcodes in
    the 0x1f group
-------------------------------------------------*/

static int generate_instruction_1f(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	UINT32 op = desc->opptr.l[0];
	UINT32 opswitch = (op >> 1) & 0x3ff;
	int item;

	switch (opswitch)
	{
		case 0x009:	/* MULHDUx - 64-bit only */
		case 0x015:	/* LDX - 64-bit only */
		case 0x01b:	/* SLDx - 64-bit only */
		case 0x035:	/* LDUX - 64-bit only */
		case 0x03a:	/* CNTLZDx - 64-bit only */
		case 0x044:	/* TD - 64-bit only */
		case 0x049:	/* MULHDx - 64-bit only */
		case 0x054:	/* LDARX - 64-bit only */
		case 0x095:	/* STDX - 64-bit only */
		case 0x0b5:	/* STDUX - 64-bit only */
		case 0x0d6:	/* STDCX. - 64-bit only */
		case 0x0e9:	/* MULLD - 64-bit only */
		case 0x2e9:	/* MULLDO - 64-bit only */
		case 0x155:	/* LWAX - 64-bit only */
		case 0x175:	/* LWAUX - 64-bit only */
		case 0x33a: /* SRADIx - 64-bit only */
		case 0x33b: /* SRADIx - 64-bit only */
		case 0x1b2:	/* SLBIE - 64-bit only */
		case 0x1c9:	/* DIVDUx - 64-bit only */
		case 0x3c9:	/* DIVDUOx - 64-bit only */
		case 0x1e9:	/* DIVDx - 64-bit only */
		case 0x3e9:	/* DIVDOx - 64-bit only */
		case 0x1f2:	/* SLBIA - 64-bit only */
		case 0x21b:	/* SRDx - 64-bit only */
		case 0x31a:	/* SRADx - 64-bit only */
		case 0x3da:	/* EXTSW - 64-bit only */
			return FALSE;

		case 0x004:	/* TW */
			UML_CMP(block, R32(G_RA(op)), R32(G_RB(op)));									// cmp     ra,rb
			if (G_TO(op) & 0x10)
				UML_EXHc(block, COND_L, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,l
			if (G_TO(op) & 0x08)
				UML_EXHc(block, COND_G, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,g
			if (G_TO(op) & 0x04)
				UML_EXHc(block, COND_E, *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,e
			if (G_TO(op) & 0x02)
				UML_EXHc(block, COND_B,  *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,b
			if (G_TO(op) & 0x01)
				UML_EXHc(block, COND_A,  *ppc->impstate->exception[EXCEPTION_PROGRAM], 0x20000);// exh program,0x20000,a
			return TRUE;

		case 0x10a:	/* ADDx */
		case 0x30a:	/* ADDOx */
			UML_ADD(block, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));					// add     rd,ra,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, ((op & M_OE) ? XER_OV : 0), FALSE);
																							// <update flags>
            return TRUE;

		case 0x00a:	/* ADDCx */
		case 0x20a:	/* ADDCOx */
			UML_ADD(block, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));					// add     rd,ra,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), FALSE);
																							// <update flags>
			return TRUE;

		case 0x08a:	/* ADDEx */
		case 0x28a:	/* ADDEOx */
			UML_CARRY(block, SPR32(SPR_XER), 29);										// carry   [xer],XER_CA
			UML_ADDC(block, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));					// addc    rd,ra,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), FALSE);
																							// <update flags>
			return TRUE;

		case 0x0ca:	/* ADDZEx */
		case 0x2ca:	/* ADDZEOx */
			UML_CARRY(block, SPR32(SPR_XER), 29);										// carry   [xer],XER_CA
			UML_ADDC(block, R32(G_RD(op)), R32(G_RA(op)), 0);							// addc    rd,ra,0
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), FALSE);
																							// <update flags>
			return TRUE;

		case 0x0ea:	/* ADDMEx */
		case 0x2ea:	/* ADDMEOx */
			UML_CARRY(block, SPR32(SPR_XER), 29);										// carry   [xer],XER_CA
			UML_ADDC(block, R32(G_RD(op)), R32(G_RA(op)), (UINT32)-1);							// addc    rd,ra,-1
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), FALSE);
																							// <update flags>
			return TRUE;

		case 0x028:	/* SUBFx */
		case 0x228:	/* SUBFOx */
			UML_SUB(block, R32(G_RD(op)), R32(G_RB(op)), R32(G_RA(op)));					// sub     rd,rb,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, (op & M_OE) ? XER_OV : 0, TRUE);
																							// <update flags>
			return TRUE;

		case 0x008:	/* SUBFCx */
		case 0x208:	/* SUBFCOx */
			UML_SUB(block, R32(G_RD(op)), R32(G_RB(op)), R32(G_RA(op)));					// sub     rd,rb,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), TRUE);
																							// <update flags>
			return TRUE;

		case 0x088:	/* SUBFEx */
		case 0x288:	/* SUBFEOx */
			UML_XOR(block, I0, SPR32(SPR_XER), XER_CA);							// xor     i0,[xer],XER_CA
			UML_CARRY(block, I0, 29);												// carry   i0,XER_CA
			UML_SUBB(block, R32(G_RD(op)), R32(G_RB(op)), R32(G_RA(op)));					// subc    rd,rb,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), TRUE);
																							// <update flags>
			return TRUE;

		case 0x0c8:	/* SUBFZEx */
		case 0x2c8:	/* SUBFZEOx */
			UML_XOR(block, I0, SPR32(SPR_XER), XER_CA);							// xor     i0,[xer],XER_CA
			UML_CARRY(block, I0, 29);												// carry   i0,XER_CA
			UML_SUBB(block, R32(G_RD(op)), 0, R32(G_RA(op)));							// subc    rd,0,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), TRUE);
																							// <update flags>
			return TRUE;

		case 0x0e8:	/* SUBFMEx */
		case 0x2e8:	/* SUBFMEOx */
			UML_XOR(block, I0, SPR32(SPR_XER), XER_CA);							// xor     i0,[xer],XER_CA
			UML_CARRY(block, I0, 29);												// carry   i0,XER_CA
			UML_SUBB(block, R32(G_RD(op)), (UINT32)-1, R32(G_RA(op)));							// subc    rd,-1,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, XER_CA | ((op & M_OE) ? XER_OV : 0), TRUE);
																							// <update flags>
			return TRUE;

		case 0x068:	/* NEGx */
		case 0x268:	/* NEGOx */
			UML_SUB(block, R32(G_RD(op)), 0, R32(G_RA(op)));							// sub     rd,0,ra
			generate_compute_flags(ppc, block, desc, op & M_RC, (op & M_OE) ? XER_OV : 0, TRUE);
																							// <update flags>
			return TRUE;

		case 0x000:	/* CMP */
			UML_CMP(block, R32(G_RA(op)), R32(G_RB(op)));									// cmp     ra,rb
			UML_GETFLGS(block, I0, FLAG_Z | FLAG_C | FLAG_S);							// getflgs i0,zcs
			UML_LOAD(block, I0, ppc->impstate->cmp_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,cmp_cr_table,i0,byte
			UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);								// or      [crn],i0,[xerso]
			return TRUE;

		case 0x020:	/* CMPL */
			UML_CMP(block, R32(G_RA(op)), R32(G_RB(op)));									// cmp     ra,rb
			UML_GETFLGS(block, I0, FLAG_Z | FLAG_C);									// getflgs i0,zc
			UML_LOAD(block, I0, ppc->impstate->cmpl_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,cmpl_cr_table,i0,byte
			UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);								// or      [crn],i0,[xerso]
			return TRUE;

		case 0x00b:	/* MULHWUx */
			UML_MULU(block, I0, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));			// mulu    i0,rd,ra,rb
			if (op & M_RC)
			{
				UML_TEST(block, R32(G_RD(op)), ~0);									// test    rd,~0
				generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);				// <update flags>
			}
			return TRUE;

		case 0x04b:	/* MULHWx */
			UML_MULS(block, I0, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));			// muls    i0,rd,ra,rb
			if (op & M_RC)
			{
				UML_TEST(block, R32(G_RD(op)), ~0);									// test    rd,~0
				generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);				// <update flags>
			}
			return TRUE;

		case 0x0eb:	/* MULLWx */
		case 0x2eb:	/* MULLWOx */
			UML_MULS(block, R32(G_RD(op)), R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));	// muls    rd,rd,ra,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, ((op & M_OE) ? XER_OV : 0), FALSE);// <update flags>
			return TRUE;

		case 0x1cb:	/* DIVWUx */
		case 0x3cb:	/* DIVWUOx */
			UML_DIVU(block, R32(G_RD(op)), R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));	// divu    rd,rd,ra,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, ((op & M_OE) ? XER_OV : 0), FALSE);// <update flags>
			return TRUE;

		case 0x1eb:	/* DIVWx */
        case 0x3eb:	/* DIVWOx */
            UML_CMP(block, R32(G_RB(op)), 0x0);                 // cmp rb, #0
            UML_JMPc(block, COND_NZ, compiler->labelnum);       // bne 0:
            UML_CMP(block, R32(G_RA(op)), 0x80000000);          // cmp rb, #80000000
            UML_JMPc(block, COND_AE, compiler->labelnum);       // bae 0:

            UML_MOV(block, I0, 0x0);                            // move i0, #0
            if (op & M_OE)
            {
                UML_OR(block, XERSO32, XERSO32, 0x1);           // SO |= 1
            }
            UML_CMP(block, I0, 0x0);                            // force a flags update
            UML_JMP(block, compiler->labelnum+3);               // jmp 3:

            UML_LABEL(block, compiler->labelnum++);             // 0:

            UML_CMP(block, R32(G_RB(op)), 0x0);                 // cmp rb, #0
            UML_JMPc(block, COND_Z, compiler->labelnum);        // beq 1:

            UML_CMP(block, R32(G_RB(op)), 0xffffffff);          // cmp rb, #ffffffff
            UML_JMPc(block, COND_NZ, compiler->labelnum+1);     // bne 2:
            UML_CMP(block, R32(G_RA(op)), 0x80000000);          // cmp ra, #80000000
            UML_JMPc(block, COND_NZ, compiler->labelnum+1);     // bne 2:

            UML_LABEL(block, compiler->labelnum++);             // 1:
            // do second branch
            UML_MOV(block, I0, 0xffffffff);                     // move i0, #ffffffff
            if (op & M_OE)
            {
                UML_OR(block, XERSO32, XERSO32, 0x1);           // SO |= 1
            }
            UML_CMP(block, I0, 0x0);                            // force a flags update
            UML_JMP(block, compiler->labelnum+1);               // jmp 3:

            UML_LABEL(block, compiler->labelnum++);             // 2:
			UML_DIVS(block, I0, R32(G_RD(op)), R32(G_RA(op)), R32(G_RB(op)));	// divs    i0,rd,ra,rb

            UML_LABEL(block, compiler->labelnum++);             // 3:
            UML_MOV(block, R32(G_RD(op)), I0);                  // mov rd, I0
            generate_compute_flags(ppc, block, desc, op & M_RC, ((op & M_OE) ? XER_OV : 0), FALSE);// <update flags> 
            return TRUE;

		case 0x01c:	/* ANDx */
			UML_AND(block, R32(G_RA(op)), R32(G_RS(op)), R32(G_RB(op)));					// and     ra,rs,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x03c:	/* ANDCx */
			UML_XOR(block, I0, R32(G_RB(op)), ~0);								// xor     i0,rb,~0
			UML_AND(block, R32(G_RA(op)), R32(G_RS(op)), I0);							// and     ra,rs,i0
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x1dc:	/* NANDx */
			UML_AND(block, I0, R32(G_RS(op)), R32(G_RB(op)));							// and     i0,rs,rb
			UML_XOR(block, R32(G_RA(op)), I0, ~0);								// xor     ra,i0,~0
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x1bc:	/* ORx */
			UML_OR(block, R32(G_RA(op)), R32(G_RS(op)), R32(G_RB(op)));						// or      ra,rs,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x19c:	/* ORCx */
			UML_XOR(block, I0, R32(G_RB(op)), ~0);								// xor     i0,rb,~0
			UML_OR(block, R32(G_RA(op)), R32(G_RS(op)), I0);							// or      ra,rs,i0
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x07c:	/* NORx */
			UML_OR(block, I0, R32(G_RS(op)), R32(G_RB(op)));							// or      i0,rs,rb
			UML_XOR(block, R32(G_RA(op)), I0, ~0);								// xor     ra,i0,~0
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x13c:	/* XORx */
			UML_XOR(block, R32(G_RA(op)), R32(G_RS(op)), R32(G_RB(op)));					// xor     ra,rs,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x11c:	/* EQVx */
			UML_XOR(block, I0, R32(G_RS(op)), R32(G_RB(op)));							// xor     i0,rs,rb
			UML_XOR(block, R32(G_RA(op)), I0, ~0);								// xor     ra,i0,~0
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x018:	/* SLWx */
			UML_SHL(block, R32(G_RA(op)), R32(G_RS(op)), R32(G_RB(op)));					// shl     ra,rs,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x218:	/* SRWx */
			UML_MOV(block, I0, R32(G_RS(op)));											// mov     i0,rs
			UML_TEST(block, R32(G_RB(op)), 0x20);										// test    rb,0x20
			UML_MOVc(block, COND_NZ, I0, 0);										// mov     i0,0,nz
			UML_SHR(block, R32(G_RA(op)), I0, R32(G_RB(op)));							// shr     ra,i0,rb
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x318:	/* SRAWx */
			UML_MOV(block, I2, R32(G_RB(op)));											// mov     i2,rb
			UML_TEST(block, I2, 0x20);											// test    i2,0x20
			UML_MOVc(block, COND_NZ, I2, 31);										// mov     i2,31,nz
			if (DISABLE_FLAG_OPTIMIZATIONS || (desc->regreq[3] & REGFLAG_XER_CA))
			{
				UML_SHL(block, I1, 0xffffffff, I2);							// shl     i1,0xffffffff,i2
				UML_XOR(block, I1, I1, ~0);									// xor     i1,i1,~0
				UML_AND(block, I0, R32(G_RS(op)), I1);							// and     i0,rs,i1
				UML_SAR(block, I1, R32(G_RS(op)), 31);							// sar     i1,rs,31
				UML_TEST(block, I0, I1);											// test    i0,i1
				UML_SETc(block, COND_NZ, I0);											// set     i0,nz
				UML_ROLINS(block, SPR32(SPR_XER), I0, 29, XER_CA);			// rolins  [xer],i0,29,XER_CA
			}
			UML_SAR(block, R32(G_RA(op)), R32(G_RS(op)), I2);							// sar     ra,rs,i2
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x338:	/* SRAWIx */
			if (DISABLE_FLAG_OPTIMIZATIONS || (desc->regreq[3] & REGFLAG_XER_CA))
			{
				UML_AND(block, I0, R32(G_RS(op)), ~(0xffffffff << (G_SH(op) & 31)));// and   i0,rs,~(0xffffffff << (sh & 31))
				UML_SAR(block, I1, R32(G_RS(op)), 31);							// sar     i1,rs,31
				UML_TEST(block, I0, I1);											// test    i0,i1
				UML_SETc(block, COND_NZ, I0);											// set     i0,nz
				UML_ROLINS(block, SPR32(SPR_XER), I0, 29, XER_CA);			// rolins  [xer],i0,29,XER_CA
			}
			UML_SAR(block, R32(G_RA(op)), R32(G_RS(op)), G_SH(op));					// sar     ra,rs,sh
			generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);					// <update flags>
			return TRUE;

		case 0x01a:	/* CNTLZWx */
			UML_LZCNT(block, R32(G_RA(op)), R32(G_RS(op)));									// lzcnt   ra,rs
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);				// <update flags>
			return TRUE;

		case 0x3ba:	/* EXTSBx */
			UML_SEXT(block, R32(G_RA(op)), R32(G_RS(op)), SIZE_BYTE);							// sext    ra,rs,byte
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);				// <update flags>
			return TRUE;

		case 0x39a:	/* EXTSHx */
			UML_SEXT(block, R32(G_RA(op)), R32(G_RS(op)), SIZE_WORD);							// sext    ra,rs,word
			if (op & M_RC)
				generate_compute_flags(ppc, block, desc, op & M_RC, 0, FALSE);				// <update flags>
			return TRUE;

		case 0x057:	/* LBZX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read8[ppc->impstate->mode]);					// callh   read8
			UML_AND(block, R32(G_RD(op)), I0, 0xff);								// and     rd,i0,0xff
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x117:	/* LHZX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_AND(block, R32(G_RD(op)), I0, 0xffff);							// and     rd,i0,0xffff
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x157:	/* LHAX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_SEXT(block, R32(G_RD(op)), I0, SIZE_WORD);									// sext    rd,i0,word
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x017:	/* LWZX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, R32(G_RD(op)), I0);											// mov     rd,i0
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x217:	/* LFSX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I0);						// mov     [tempdata],i0
			UML_FDFRFLT(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l), SIZE_DWORD);	// fdfrflt fd,[tempdata],dword
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x257:	/* LFDX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read64[ppc->impstate->mode]);					// callh   read64
			UML_DMOV(block, mem(&ppc->impstate->tempdata.d), I0);						// dmov    [tempdata],i0
			UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.d));				// fdmov   fd,[tempdata]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x316:	/* LHBRX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_BSWAP(block, I0, I0);												// bswap   i0,i0
			UML_SHR(block, R32(G_RD(op)), I0, 16);								// shr     rd,i0,16
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x216:	/* LWBRX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read32align[ppc->impstate->mode]);				// callh   read32align
			UML_BSWAP(block, R32(G_RD(op)), I0);										// bswap   rd,i0
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x077:	/* LBZUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDXU(op));								// mapvar  dsisr,DSISR_IDXU(op)
			UML_CALLH(block, *ppc->impstate->read8[ppc->impstate->mode]);					// callh   read8
			UML_AND(block, R32(G_RD(op)), I0, 0xff);								// and     rd,i0,0xff
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x137:	/* LHZUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDXU(op));								// mapvar  dsisr,DSISR_IDXU(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_AND(block, R32(G_RD(op)), I0, 0xffff);							// and     rd,i0,0xffff
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x177:	/* LHAUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDXU(op));								// mapvar  dsisr,DSISR_IDXU(op)
			UML_CALLH(block, *ppc->impstate->read16[ppc->impstate->mode]);					// callh   read16
			UML_SEXT(block, R32(G_RD(op)), I0, SIZE_WORD);									// sext    rd,i0,word
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x037:	/* LWZUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDXU(op));								// mapvar  dsisr,DSISR_IDXU(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, R32(G_RD(op)), I0);											// mov     rd,i0
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x237:	/* LFSUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read32[ppc->impstate->mode]);					// callh   read32
			UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), I0);						// mov     [tempdata],i0
			UML_FDFRFLT(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l), SIZE_DWORD);	// fdfrflt fd,[tempdata],dword
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x277:	/* LFDUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read64[ppc->impstate->mode]);					// callh   read64
			UML_DMOV(block, mem(&ppc->impstate->tempdata.d), I0);						// dmov    [tempdata],i0
			UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.d));				// fdmov   fd,[tempdata]
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x014:	/* LWARX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->read32align[ppc->impstate->mode]);				// callh   read32align
			UML_MOV(block, R32(G_RD(op)), I0);											// mov     rd,i0
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x255:	/* LSWI */
			UML_MOV(block, mem(&ppc->impstate->updateaddr), R32Z(G_RA(op)));				// mov     [updateaddr],ra
			UML_MOV(block, mem(&ppc->impstate->swcount), ((G_NB(op) - 1) & 0x1f) + 1);	// mov     [swcount],G_NB
			UML_CALLH(block, *ppc->impstate->lsw[ppc->impstate->mode][G_RD(op)]);			// call    lsw[rd]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x215:	/* LSWX */
			UML_ADD(block, mem(&ppc->impstate->updateaddr), R32Z(G_RA(op)), R32(G_RB(op)));	// add     [updateaddr],ra,rb
			UML_AND(block, mem(&ppc->impstate->swcount), SPR32(SPR_XER), 0x7f);		// and     [swcount],[xer],0x7f
			UML_SUB(block, mem(&ppc->icount), mem(&ppc->icount), mem(&ppc->impstate->swcount));// sub  icount,icount,[swcount]
			UML_CALLHc(block, COND_NZ, *ppc->impstate->lsw[ppc->impstate->mode][G_RD(op)]);	// call    lsw[rd],nz
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x136:	/* ECIWX */
			/* not implemented */
			return FALSE;

		case 0x0d7:	/* STBX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_AND(block, I1, R32(G_RS(op)), 0xff);								// and     i1,rs,0xff
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write8[ppc->impstate->mode]);					// callh   write8
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x197:	/* STHX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_AND(block, I1, R32(G_RS(op)), 0xffff);							// and     i1,rs
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write16[ppc->impstate->mode]);					// callh   write16
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x097:	/* STWX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, I1, R32(G_RS(op)));											// mov     i1,rs
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x297:	/* STFSX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_FSFRFLT(block, mem(&ppc->impstate->tempdata.w.l), F64(G_RS(op)), SIZE_QWORD);	// fsfrflt [tempdata],rs,qword
			UML_MOV(block, I1, mem(&ppc->impstate->tempdata.w.l));						// mov     i1,[tempdata]
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x3d7:	/* STFIWX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RS(op)));				// fdmov   [tempdata],rs
			UML_MOV(block, I1, mem(&ppc->impstate->tempdata.w.l));						// mov     i1,[tempdata.lo]
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2d7:	/* STFDX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RS(op)));				// fdmov   [tempdata],rs
			UML_DMOV(block, I1, mem(&ppc->impstate->tempdata.d));						// dmov    i1,[tempdata]
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write64[ppc->impstate->mode]);					// callh   write64
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x396:	/* STHBRX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_BSWAP(block, I1, R32(G_RS(op)));										// bswap   i1,rs
			UML_SHR(block, I1, I1, 16);										// shr     i1,i1,16
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write16[ppc->impstate->mode]);					// callh   write16
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x296:	/* STWBRX */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_BSWAP(block, I1, R32(G_RS(op)));										// bswap   i1,rs
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x0f7:	/* STBUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_AND(block, I1, R32(G_RS(op)), 0xff);								// and     i1,rs,0xff
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write8[ppc->impstate->mode]);					// callh   write8
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x1b7:	/* STHUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_AND(block, I1, R32(G_RS(op)), 0xffff);							// and     i1,rs,0xffff
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write16[ppc->impstate->mode]);					// callh   write16
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x0b7:	/* STWUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, I1, R32(G_RS(op)));											// mov     i1,rs
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDXU(op));								// mapvar  dsisr,DSISR_IDXU(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2b7:	/* STFSUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_FSFRFLT(block, mem(&ppc->impstate->tempdata.w.l), F64(G_RS(op)), SIZE_QWORD);	// fsfrflt [tempdata],rs,qword
			UML_MOV(block, I1, mem(&ppc->impstate->tempdata.w.l));						// mov     i1,[tempdata]
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32[ppc->impstate->mode]);					// callh   write32
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x2f7:	/* STFDUX */
			UML_ADD(block, I0, R32(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RS(op)));				// fdmov   [tempdata],rs
			UML_DMOV(block, I1, mem(&ppc->impstate->tempdata.d));						// dmov    i1,[tempdata]
			UML_MOV(block, mem(&ppc->impstate->updateaddr), I0);						// mov     [updateaddr],i0
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write64[ppc->impstate->mode]);					// callh   write64
			UML_MOV(block, R32(G_RA(op)), mem(&ppc->impstate->updateaddr));					// mov     ra,[updateaddr]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x096:	/* STWCX. */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_MOV(block, I1, R32(G_RS(op)));											// mov     i1,rs
			UML_MAPVAR(block, MAPVAR_DSISR, DSISR_IDX(op));									// mapvar  dsisr,DSISR_IDX(op)
			UML_CALLH(block, *ppc->impstate->write32align[ppc->impstate->mode]);				// callh   write32align
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>

			UML_CMP(block, I0, I0);												// cmp     i0,i0
			UML_GETFLGS(block, I0, FLAG_Z | FLAG_C | FLAG_S);							// getflgs i0,zcs
			UML_LOAD(block, I0, ppc->impstate->cmp_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,cmp_cr_table,i0,byte
			UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);								// or      [crn],i0,[xerso]

			generate_compute_flags(ppc, block, desc, TRUE, 0, FALSE);						// <update flags>
			return TRUE;

		case 0x2d5:	/* STSWI */
			UML_MOV(block, mem(&ppc->impstate->updateaddr), R32Z(G_RA(op)));				// mov     [updateaddr],ra
			UML_MOV(block, mem(&ppc->impstate->swcount), ((G_NB(op) - 1) & 0x1f) + 1);	// mov     [swcount],G_NB
			UML_CALLH(block, *ppc->impstate->stsw[ppc->impstate->mode][G_RD(op)]);			// call    stsw[rd]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x295:	/* STSWX */
			UML_ADD(block, mem(&ppc->impstate->updateaddr), R32Z(G_RA(op)), R32(G_RB(op)));	// add     [updateaddr],ra,rb
			UML_AND(block, mem(&ppc->impstate->swcount), SPR32(SPR_XER), 0x7f);		// and     [swcount],[xer],0x7f
			UML_SUB(block, mem(&ppc->icount), mem(&ppc->icount), mem(&ppc->impstate->swcount));// sub  icount,icount,[swcount]
			UML_CALLHc(block, COND_NZ, *ppc->impstate->stsw[ppc->impstate->mode][G_RD(op)]);	// call   stsw[rd]
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x1b6:	/* ECOWX */
			/* not implemented */
			return FALSE;

		case 0x036:	/* DCBST */
		case 0x056:	/* DCBF */
		case 0x0f6:	/* DCBTST */
		case 0x116:	/* DCBT */
		case 0x3d6:	/* ICBI */
		case 0x256:	/* SYNC */
		case 0x356:	/* EIEIO */
		case 0x1d6:	/* DCBI */
		case 0x236:	/* TLBSYNC */
			/* effective no-ops */
			return TRUE;

		case 0x3f6:	/* DCBZ */
			UML_ADD(block, I0, R32Z(G_RA(op)), R32(G_RB(op)));							// add     i0,ra,rb
			UML_AND(block, mem(&ppc->impstate->tempaddr), I0, ~(ppc->cache_line_size - 1));
																							// and     [tempaddr],i0,~(cache_line_size - 1)
			for (item = 0; item < ppc->cache_line_size / 8; item++)
			{
				UML_ADD(block, I0, mem(&ppc->impstate->tempaddr), 8 * item);		// add     i0,[tempaddr],8*item
				UML_DMOV(block, I1, 0);											// dmov    i1,0
				UML_CALLH(block, *ppc->impstate->write64[ppc->impstate->mode]);				// callh   write64
			}
			return TRUE;

		case 0x132:	/* TLBIE */
			UML_MOV(block, mem(&ppc->param0), R32(G_RB(op)));								// mov     [param0],rb
			UML_CALLC(block, (c_function)ppccom_execute_tlbie, ppc);									// callc   ppccom_execute_tlbie,ppc
			return TRUE;

		case 0x172:	/* TLBIA */
			UML_CALLC(block, (c_function)ppccom_execute_tlbia, ppc);									// callc   ppccom_execute_tlbia,ppc
			return TRUE;

		case 0x3d2:	/* TLBLD */
			assert(ppc->cap & PPCCAP_603_MMU);
			UML_MOV(block, mem(&ppc->param0), R32(G_RB(op)));								// mov     [param0],rb
			UML_MOV(block, mem(&ppc->param1), 0);										// mov     [param1],0
			UML_CALLC(block, (c_function)ppccom_execute_tlbl, ppc);										// callc   ppccom_execute_tlbl,ppc
			return TRUE;

		case 0x3f2:	/* TLBLI */
			assert(ppc->cap & PPCCAP_603_MMU);
			UML_MOV(block, mem(&ppc->param0), R32(G_RB(op)));								// mov     [param0],rb
			UML_MOV(block, mem(&ppc->param1), 1);										// mov     [param1],1
			UML_CALLC(block, (c_function)ppccom_execute_tlbl, ppc);										// callc   ppccom_execute_tlbl,ppc
			return TRUE;

		case 0x013:	/* MFCR */
			UML_SHL(block, I0, CR32(0), 28);										// shl     i0,cr(0),28
			UML_ROLAND(block, I1, CR32(1), 24, 0x0f000000);					// roland  i1,cr(1),24,0x0f000000
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(2), 20, 0x00f00000);					// roland  i1,cr(2),20,0x00f00000
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(3), 16, 0x000f0000);					// roland  i1,cr(3),16,0x000f0000
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(4), 12, 0x0000f000);					// roland  i1,cr(4),12,0x0000f000
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(5), 8, 0x00000f00);					// roland  i1,cr(5),8,0x00000f00
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(6), 4, 0x000000f0);					// roland  i1,cr(6),4,0x000000f0
			UML_OR(block, I0, I0, I1);										// or      i0,i0,i1
			UML_ROLAND(block, I1, CR32(7), 0, 0x0000000f);					// roland  i1,cr(7),0,0x0000000f
			UML_OR(block, R32(G_RD(op)), I0, I1);									// or      rd,i0,i1
			return TRUE;

		case 0x053:	/* MFMSR */
			UML_MOV(block, R32(G_RD(op)), MSR32);											// mov     rd,msr
			return TRUE;

		case 0x153:	/* MFSPR */
		{
			UINT32 spr = compute_spr(G_SPR(op));
			if (spr == SPR_LR || spr == SPR_CTR || (spr >= SPROEA_SPRG0 && spr <= SPROEA_SPRG3))
				UML_MOV(block, R32(G_RD(op)), SPR32(spr));									// mov     rd,spr
			else if (spr == SPR_XER)
			{
				UML_SHL(block, I0, XERSO32, 31);									// shl     i0,[xerso],31
				UML_OR(block, R32(G_RD(op)), SPR32(spr), I0);							// or      [rd],[xer],i0
			}
			else if (spr == SPROEA_PVR)
				UML_MOV(block, R32(G_RD(op)), ppc->flavor);							// mov     rd,flavor
			else
			{
				generate_update_cycles(ppc, block, compiler, desc->pc, TRUE);			// <update cycles>
				UML_MOV(block, mem(&ppc->param0), spr);								// mov     [param0],spr
				UML_CALLC(block, (c_function)ppccom_execute_mfspr, ppc);								// callc   ppccom_execute_mfspr,ppc
				UML_MOV(block, R32(G_RD(op)), mem(&ppc->param1));							// mov     rd,[param1]
			}
			return TRUE;
		}

		case 0x253:	/* MFSR */
			UML_MOV(block, R32(G_RD(op)), SR32(G_SR(op)));									// mov     rd,sr
			return TRUE;

		case 0x293:	/* MFSRIN */
			UML_SHR(block, I0, R32(G_RB(op)), 28);								// shr     i0,G_RB,28
			UML_LOAD(block, R32(G_RD(op)), &ppc->sr[0], I0, SIZE_DWORD, SCALE_x4);		// load    rd,sr,i0,dword
			return TRUE;

		case 0x173:	/* MFTB */
		{
			UINT32 tbr = compute_spr(G_SPR(op));
			if (tbr != SPRVEA_TBL_R && tbr != SPRVEA_TBU_R)
				return FALSE;
			generate_update_cycles(ppc, block, compiler, desc->pc, TRUE);				// <update cycles>
			UML_MOV(block, mem(&ppc->param0), tbr);									// mov     [param0],tbr
			UML_CALLC(block, (c_function)ppccom_execute_mftb, ppc);										// callc   ppccom_execute_mftb,ppc
			UML_MOV(block, R32(G_RD(op)), mem(&ppc->param1));								// mov     rd,[param1]
			return TRUE;
		}

		case 0x090:	/* MTCRF */
			UML_MOV(block, I0, R32(G_RS(op)));											// mov     i0,rs
			if (G_CRM(op) & 0x80) UML_ROLAND(block, CR32(0), I0, 4, 0xf);	// roland  cr(0),i0,4,0x0f
			if (G_CRM(op) & 0x40) UML_ROLAND(block, CR32(1), I0, 8, 0xf);	// roland  cr(1),i0,8,0x0f
			if (G_CRM(op) & 0x20) UML_ROLAND(block, CR32(2), I0, 12, 0xf);	// roland  cr(2),i0,12,0x0f
			if (G_CRM(op) & 0x10) UML_ROLAND(block, CR32(3), I0, 16, 0xf);	// roland  cr(3),i0,16,0x0f
			if (G_CRM(op) & 0x08) UML_ROLAND(block, CR32(4), I0, 20, 0xf);	// roland  cr(4),i0,20,0x0f
			if (G_CRM(op) & 0x04) UML_ROLAND(block, CR32(5), I0, 24, 0xf);	// roland  cr(5),i0,24,0x0f
			if (G_CRM(op) & 0x02) UML_ROLAND(block, CR32(6), I0, 28, 0xf);	// roland  cr(6),i0,28,0x0f
			if (G_CRM(op) & 0x01) UML_ROLAND(block, CR32(7), I0, 0, 0xf);	// roland  cr(7),i0,0,0x0f
			return TRUE;

		case 0x092:	/* MTMSR */
			if (ppc->cap & PPCCAP_603_MMU)
				UML_XOR(block, I0, MSR32, R32(G_RS(op)));								// xor     i0,msr32,rs
			UML_MOV(block, MSR32, R32(G_RS(op)));											// mov     msr,rs
			if (ppc->cap & PPCCAP_603_MMU)
			{
				UML_TEST(block, I0, MSR603_TGPR);									// test    i0,tgpr
				UML_CALLHc(block, COND_NZ, *ppc->impstate->swap_tgpr);							// callh   swap_tgpr,nz
			}
			generate_update_mode(ppc, block);												// <update mode>
			return TRUE;

		case 0x1d3:	/* MTSPR */
		{
			UINT32 spr = compute_spr(G_SPR(op));
			if (spr == SPR_LR || spr == SPR_CTR || (spr >= SPROEA_SPRG0 && spr <= SPROEA_SPRG3))
				UML_MOV(block, SPR32(spr), R32(G_RS(op)));									// mov     spr,rs
			else if (spr == SPR_XER)
			{
				UML_AND(block, SPR32(spr), R32(G_RS(op)), ~XER_SO);					// and     spr,rs,~XER_SO
				UML_SHR(block, XERSO32, R32(G_RS(op)), 31);							// shr     [xerso],rs,31
			}
			else if (spr == SPROEA_PVR)
				;																			// read only
			else
			{
				generate_update_cycles(ppc, block, compiler, desc->pc, TRUE);			// <update cycles>
				UML_MOV(block, mem(&ppc->param0), spr);								// mov     [param0],spr
				UML_MOV(block, mem(&ppc->param1), R32(G_RS(op)));							// mov     [param1],rs
				UML_CALLC(block, (c_function)ppccom_execute_mtspr, ppc);								// callc   ppccom_execute_mtspr,ppc
				compiler->checkints = TRUE;
				generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);		// <update cycles>
			}
			return TRUE;
		}

		case 0x0d2:	/* MTSR */
			UML_MOV(block, SR32(G_SR(op)), R32(G_RS(op)));									// mov     sr[G_SR],rs
			UML_CALLC(block, (c_function)ppccom_tlb_flush, ppc);										// callc   ppccom_tlb_flush,ppc
			return TRUE;

		case 0x0f2:	/* MTSRIN */
			UML_SHR(block, I0, R32(G_RB(op)), 28);								// shr     i0,G_RB,28
			UML_STORE(block, &ppc->sr[0], I0, R32(G_RS(op)), SIZE_DWORD, SCALE_x4);	// store   sr,i0,rs,dword
			UML_CALLC(block, (c_function)ppccom_tlb_flush, ppc);							// callc   ppccom_tlb_flush,ppc
			return TRUE;

		case 0x200:	/* MCRXR */
			UML_ROLAND(block, I0, SPR32(SPR_XER), 28, 0x0f);					// roland  i0,[xer],28,0x0f
			UML_SHL(block, I1, XERSO32, 3);										// shl     i1,[xerso],3
			UML_OR(block, CR32(G_CRFD(op)), I0, I1);								// or      [crd],i0,i1
			UML_AND(block, SPR32(SPR_XER), SPR32(SPR_XER), ~0xf0000000);				// and     [xer],[xer],~0xf0000000
			UML_MOV(block, XERSO32, 0);												// mov     [xerso],0
			return TRUE;

		case 0x106:	/* ICBT */
		case 0x1c6:	/* DCCCI */
		case 0x3c6:	/* ICCCI */
			assert(ppc->cap & PPCCAP_4XX);
			/* effective no-nop */
			return TRUE;

		case 0x1e6:	/* DCREAD */
		case 0x3e6:	/* ICREAD */
			assert(ppc->cap & PPCCAP_4XX);
			UML_MOV(block, R32(G_RT(op)), 0);											// mov     rt,0
			return TRUE;

		case 0x143:	/* MFDCR */
		{
			UINT32 spr = compute_spr(G_SPR(op));
			assert(ppc->cap & PPCCAP_4XX);
			generate_update_cycles(ppc, block, compiler, desc->pc, TRUE);				// <update cycles>
			UML_MOV(block, mem(&ppc->param0), spr);									// mov     [param0],spr
			UML_CALLC(block, (c_function)ppccom_execute_mfdcr, ppc);									// callc   ppccom_execute_mfdcr,ppc
			UML_MOV(block, R32(G_RD(op)), mem(&ppc->param1));								// mov     rd,[param1]
			return TRUE;
		}

		case 0x1c3:	/* MTDCR */
		{
			UINT32 spr = compute_spr(G_SPR(op));
			assert(ppc->cap & PPCCAP_4XX);
			generate_update_cycles(ppc, block, compiler, desc->pc, TRUE);				// <update cycles>
			UML_MOV(block, mem(&ppc->param0), spr);									// mov     [param0],spr
			UML_MOV(block, mem(&ppc->param1), R32(G_RS(op)));								// mov     [param1],rs
			UML_CALLC(block, (c_function)ppccom_execute_mtdcr, ppc);									// callc   ppccom_execute_mtdcr,ppc
			compiler->checkints = TRUE;
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;
		}

		case 0x083:	/* WRTEE */
			assert(ppc->cap & PPCCAP_4XX);
			UML_ROLINS(block, MSR32, R32(G_RS(op)), 0, MSR_EE);					// rolins  msr,rs,0,MSR_EE
			compiler->checkints = TRUE;
			generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);			// <update cycles>
			return TRUE;

		case 0x0a3:	/* WRTEEI */
			assert(ppc->cap & PPCCAP_4XX);
			if (op & MSR_EE)
			{
				UML_OR(block, MSR32, MSR32, MSR_EE);									// or      msr,msr,MSR_EE
				compiler->checkints = TRUE;
				generate_update_cycles(ppc, block, compiler, desc->pc + 4, TRUE);		// <update cycles>
			}
			else
				UML_AND(block, MSR32, MSR32, ~MSR_EE);									// and     msr,msr,~MSR_EE
			return TRUE;

		case 0x254: /* ESA */
		case 0x274: /* DSA */
			/* no-op for now */
			return TRUE;
	}

	return FALSE;
}


/*-------------------------------------------------
    generate_instruction_3b - compile opcodes in
    the 0x3b group
-------------------------------------------------*/

static int generate_instruction_3b(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	UINT32 op = desc->opptr.l[0];
	UINT32 opswitch = (op >> 1) & 0x1f;

	switch (opswitch)
	{
		case 0x15:	/* FADDSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDADD(block, F0, F64(G_RA(op)), F64(G_RB(op)));						// fdadd   f0,ra,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x14:	/* FSUBSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDSUB(block, F0, F64(G_RA(op)), F64(G_RB(op)));						// fdsub   f0,ra,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x19:	/* FMULSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));						// fdmul   f0,ra,rc
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x12:	/* FDIVSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDDIV(block, F0, F64(G_RA(op)), F64(G_RB(op)));						// fddiv   f0,ra,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x16:	/* FSQRTSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDSQRT(block, F0, F64(G_RB(op)));										// fdsqrt  f0,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x18:	/* FRESx */
			UML_FSFRFLT(block, F0, F64(G_RB(op)), SIZE_QWORD);								// fsfrlt  f0,rb,qword
			UML_FSRECIP(block, F0, F0);											// fsrecip f0,f0
			UML_FDFRFLT(block, F64(G_RD(op)), F0, SIZE_DWORD);								// fdfrflt rd,f0,dword
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x1d:	/* FMADDSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));						// fdmul   f0,ra,rc
			UML_FDADD(block, F0, F0, F64(G_RB(op)));								// fdadd   f0,f0,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x1c:	/* FMSUBSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));						// fdmul   f0,ra,rc
			UML_FDSUB(block, F0, F0, F64(G_RB(op)));								// fdsub   f0,f0,rb
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x1f:	/* FNMADDSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));						// fdmul   f0,ra,rc
			UML_FDADD(block, F0, F0, F64(G_RB(op)));								// fdadd   f0,f0,rb
			UML_FDNEG(block, F0, F0);												// fdneg   f0,f0
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;

		case 0x1e:	/* FNMSUBSx */
			if (!(ppc->impstate->drcoptions & PPCDRC_ACCURATE_SINGLES))
				return generate_instruction_3f(ppc, block, compiler, desc);
			UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));						// fdmul   f0,ra,rc
			UML_FDSUB(block, F0, F64(G_RB(op)), F0);								// fdsub   f0,rb,f0
			UML_FDRNDS(block, F64(G_RD(op)), F0);										// fdrnds  rd,f0
			generate_fp_flags(ppc, block, desc, TRUE);
			return TRUE;
	}

	return FALSE;
}



/*-------------------------------------------------
    generate_instruction_3f - compile opcodes in
    the 0x3f group
-------------------------------------------------*/

static int generate_instruction_3f(powerpc_state *ppc, drcuml_block *block, compiler_state *compiler, const opcode_desc *desc)
{
	UINT32 op = desc->opptr.l[0];
	UINT32 opswitch = (op >> 1) & 0x3ff;

	if (opswitch & 0x10)
	{
		opswitch &= 0x1f;
		switch (opswitch)
		{
			case 0x15:	/* FADDx */
				UML_FDADD(block, F64(G_RD(op)), F64(G_RA(op)), F64(G_RB(op)));				// fdadd   rd,ra,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x14:	/* FSUBx */
				UML_FDSUB(block, F64(G_RD(op)), F64(G_RA(op)), F64(G_RB(op)));				// fdsub   rd,ra,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x19:	/* FMULx */
				UML_FDMUL(block, F64(G_RD(op)), F64(G_RA(op)), F64(G_REGC(op)));			// fdmul   rd,ra,rc
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x12:	/* FDIVx */
				UML_FDDIV(block, F64(G_RD(op)), F64(G_RA(op)), F64(G_RB(op)));				// fddiv   rd,ra,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x16:	/* FSQRTx */
				UML_FDSQRT(block, F64(G_RD(op)), F64(G_RB(op)));							// fdsqrt  rd,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x1a:	/* FRSQRTEx */
				UML_FDRSQRT(block, F64(G_RD(op)), F64(G_RB(op)));							// fdrsqrt rd,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x17:	/* FSELx */
				UML_FDCMP(block, F64(G_RA(op)), mem(&ppc->impstate->fp0));					// fdcmp   f0,ra,[fp0]
				UML_FDMOVc(block, COND_AE, F64(G_RD(op)), F64(G_REGC(op)));					// fdmov   rd,rc,AE
				UML_FDMOVc(block, COND_B, F64(G_RD(op)), F64(G_RB(op)));						// fdmov   rd,rb,B
				return TRUE;

			case 0x1d:	/* FMADDx */
				UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));					// fdmul   f0,ra,rc
				UML_FDADD(block, F64(G_RD(op)), F0, F64(G_RB(op)));					// fdadd   rd,f0,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x1f:	/* FNMADDx */
				UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));					// fdmul   f0,ra,rc
				UML_FDADD(block, F0, F0, F64(G_RB(op)));							// fdadd   f0,f0,rb
				UML_FDNEG(block, F64(G_RD(op)), F0);									// fdneg   rd,f0
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x1c:	/* FMSUBx */
				UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));					// fdmul   f0,ra,rc
				UML_FDSUB(block, F64(G_RD(op)), F0, F64(G_RB(op)));					// fdsub   rd,f0,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x1e:	/* FNMSUBx */
				UML_FDMUL(block, F0, F64(G_RA(op)), F64(G_REGC(op)));					// fdmul   f0,ra,rc
				UML_FDSUB(block, F64(G_RD(op)), F64(G_RB(op)), F0);					// fdsub   rd,rb,f0
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;
		}
	}
	else
	{
		switch (opswitch)
		{
			case 0x32e:	/* FCTIDx - 64-bit only */
			case 0x32f:	/* FCTIDZx - 64-bit only */
			case 0x34e:	/* FCFIDx - 64-bit only */
				return FALSE;

			case 0x000:	/* FCMPU */
			case 0x020:	/* FCMPO */
				UML_FDCMP(block, F64(G_RA(op)), F64(G_RB(op)));								// fdcmp   ra,rb
				UML_GETFLGS(block, I0, FLAG_C | FLAG_Z | FLAG_U);						// getflgs i0,czu
				UML_LOAD(block, I0, ppc->impstate->fcmp_cr_table, I0, SIZE_BYTE, SCALE_x1);// load    i0,fcmp_cr_table,i0,byte
				UML_OR(block, CR32(G_CRFD(op)), I0, XERSO32);							// or      [crn],i0,[xerso]
				return TRUE;

			case 0x00c:	/* FRSPx */
				UML_FDRNDS(block, F64(G_RD(op)), F64(G_RB(op)));							// fdrnds  rd,rb
				generate_fp_flags(ppc, block, desc, TRUE);
				return TRUE;

			case 0x00e:	/* FCTIWx */
				UML_FDTOINT(block, I0, F64(G_RB(op)), SIZE_DWORD, ROUND_DEFAULT);					// fdtoint i0,rb,dword,default
				UML_DAND(block, mem(&ppc->impstate->tempdata.w.l), I0, 0xffffffff);// dand    i0,i0,0xffffffff
				UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l));			// fdmovr  rd,i0
				return TRUE;

			case 0x00f:	/* FCTIWZx */
				UML_FDTOINT(block, I0, F64(G_RB(op)), SIZE_DWORD, ROUND_TRUNC);					// fdtoint i0,rb,dword,default
				UML_DAND(block, mem(&ppc->impstate->tempdata.w.l), I0, 0xffffffff);// dand    i0,i0,0xffffffff
				UML_FDMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.w.l));			// fdmovr  rd,i0
				return TRUE;

			case 0x028:	/* FNEGx */
				UML_FDNEG(block, F64(G_RD(op)), F64(G_RB(op)));								// fdneg   rd,rb
				return TRUE;

			case 0x048:	/* FMRx */
				UML_FDMOV(block, F64(G_RD(op)), F64(G_RB(op)));								// fdmov   rd,rb
				return TRUE;

			case 0x088:	/* FNABSx */
				UML_FDABS(block, F0, F64(G_RB(op)));									// fdabs   f0,rb
				UML_FDNEG(block, F64(G_RD(op)), F0);									// fdneg   rd,f0
				return TRUE;

			case 0x108:	/* FABSx */
				UML_FDABS(block, F64(G_RD(op)), F64(G_RB(op)));								// fdabs   rd,rb
				return TRUE;

			case 0x046:	/* MTFSB0x */
				UML_AND(block, FPSCR32, FPSCR32, ~(0x80000000 >> G_CRBD(op)));			// and     fpscr32,fpscr32,~(0x80000000 >> G_CRBD)
				return TRUE;

			case 0x026:	/* MTFSB1x */
				UML_OR(block, FPSCR32, FPSCR32, 0x80000000 >> G_CRBD(op));				// or      fpscr32,fpscr32,(0x80000000 >> G_CRBD)
				return TRUE;

			case 0x040:	/* MCRFS */
				UML_ROLAND(block, CR32(G_CRFD(op)), FPSCR32, ((G_CRFS(op) - 7) & 7) * 4, 0x0f);
																							// roland  [crd],[fpscr],shift,0x0f
				UML_AND(block, FPSCR32, FPSCR32, ~CRMASK(G_CRFS(op)));					// and     fpscr,fpscr,~crmask[crfs]
				return TRUE;

			case 0x247:	/* MFFSx */
				UML_MOV(block, mem(&ppc->impstate->tempdata.w.l), FPSCR32);					// mov     [tempdata],fpscr
				UML_FSMOV(block, F64(G_RD(op)), mem(&ppc->impstate->tempdata.d));			// fsmov   rd,fpscr
				return TRUE;

			case 0x2c7:	/* MTFSFx */
				UML_FDMOV(block, mem(&ppc->impstate->tempdata.d), F64(G_RB(op)));			// fdmov   [tempdata],fb
				UML_ROLINS(block, FPSCR32, mem(&ppc->impstate->tempdata.w.l), 0, compute_crf_mask(G_FM(op)));
																							// rolins  fpscr,rb,0,crf_mask
				return TRUE;

			case 0x086:	/* MTFSFIx */
				UML_ROLINS(block, FPSCR32, G_IMM(op), 28 - 4 * G_CRFD(op), CRMASK(G_CRFD(op)));
																							// rolins  fpscr,rb,0,crf_mask
				return TRUE;
		}
	}

	return FALSE;
}



/***************************************************************************
    CODE LOGGING HELPERS
***************************************************************************/

/*-------------------------------------------------
    log_add_disasm_comment - add a comment
    including disassembly of a PowerPC instruction
-------------------------------------------------*/

static void log_add_disasm_comment(drcuml_block *block, UINT32 pc, UINT32 op)
{
	char buffer[100];
	if (LOG_UML)
	{
		ppc_dasm_one(buffer, pc, op);
		block->append_comment("%08X: %s", pc, buffer);									// comment
	}
}


/*-------------------------------------------------
    log_desc_flags_to_string - generate a string
    representing the instruction description
    flags
-------------------------------------------------*/

static const char *log_desc_flags_to_string(UINT32 flags)
{
	static char tempbuf[30];
	char *dest = tempbuf;

	/* branches */
	if (flags & OPFLAG_IS_UNCONDITIONAL_BRANCH)
		*dest++ = 'U';
	else if (flags & OPFLAG_IS_CONDITIONAL_BRANCH)
		*dest++ = 'C';
	else
		*dest++ = '.';

	/* intrablock branches */
	*dest++ = (flags & OPFLAG_INTRABLOCK_BRANCH) ? 'i' : '.';

	/* branch targets */
	*dest++ = (flags & OPFLAG_IS_BRANCH_TARGET) ? 'B' : '.';

	/* delay slots */
	*dest++ = (flags & OPFLAG_IN_DELAY_SLOT) ? 'D' : '.';

	/* exceptions */
	if (flags & OPFLAG_WILL_CAUSE_EXCEPTION)
		*dest++ = 'E';
	else if (flags & OPFLAG_CAN_CAUSE_EXCEPTION)
		*dest++ = 'e';
	else
		*dest++ = '.';

	/* read/write */
	if (flags & OPFLAG_READS_MEMORY)
		*dest++ = 'R';
	else if (flags & OPFLAG_WRITES_MEMORY)
		*dest++ = 'W';
	else
		*dest++ = '.';

	/* TLB validation */
	*dest++ = (flags & OPFLAG_VALIDATE_TLB) ? 'V' : '.';

	/* TLB modification */
	*dest++ = (flags & OPFLAG_MODIFIES_TRANSLATION) ? 'T' : '.';

	/* redispatch */
	*dest++ = (flags & OPFLAG_REDISPATCH) ? 'R' : '.';
	return tempbuf;
}


/*-------------------------------------------------
    log_register_list - log a list of GPR registers
-------------------------------------------------*/

static void log_register_list(drcuml_state *drcuml, const char *string, const UINT32 *reglist, const UINT32 *regnostarlist)
{
	static const char *const crtext[4] = { "lt", "gt", "eq", "so" };
	int count = 0;
	int regnum;
	int crnum;

	/* skip if nothing */
	if (reglist[0] == 0 && reglist[1] == 0 && reglist[2] == 0 && reglist[3] == 0)
		return;

	drcuml->log_printf("[%s:", string);

	for (regnum = 0; regnum < 32; regnum++)
		if (reglist[0] & REGFLAG_R(regnum))
		{
			drcuml->log_printf("%sr%d", (count++ == 0) ? "" : ",", regnum);
			if (regnostarlist != NULL && !(regnostarlist[0] & REGFLAG_R(regnum)))
				drcuml->log_printf("*");
		}

	for (regnum = 0; regnum < 32; regnum++)
		if (reglist[1] & REGFLAG_FR(regnum))
		{
			drcuml->log_printf("%sfr%d", (count++ == 0) ? "" : ",", regnum);
			if (regnostarlist != NULL && !(regnostarlist[1] & REGFLAG_FR(regnum)))
				drcuml->log_printf("*");
		}

	for (regnum = 0; regnum < 8; regnum++)
		if (reglist[2] & REGFLAG_CR(regnum))
		{
			if ((reglist[2] & REGFLAG_CR(regnum)) == REGFLAG_CR(regnum) && (regnostarlist == NULL || (regnostarlist[2] & REGFLAG_CR(regnum)) == REGFLAG_CR(regnum)))
			{
				drcuml->log_printf("%scr%d", (count++ == 0) ? "" : ",", regnum);
				if (regnostarlist != NULL && !(regnostarlist[2] & REGFLAG_CR(regnum)))
					drcuml->log_printf("*");
			}
			else
			{
				for (crnum = 0; crnum < 4; crnum++)
					if (reglist[2] & REGFLAG_CR_BIT(regnum * 4 + crnum))
					{
						drcuml->log_printf("%scr%d[%s]", (count++ == 0) ? "" : ",", regnum, crtext[crnum]);
						if (regnostarlist != NULL && !(regnostarlist[2] & REGFLAG_CR_BIT(regnum * 4 + crnum)))
							drcuml->log_printf("*");
					}
			}
		}

	if (reglist[3] & REGFLAG_XER_CA)
	{
		drcuml->log_printf("%sxer_ca", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_XER_CA))
			drcuml->log_printf("*");
	}
	if (reglist[3] & REGFLAG_XER_OV)
	{
		drcuml->log_printf("%sxer_ov", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_XER_OV))
			drcuml->log_printf("*");
	}
	if (reglist[3] & REGFLAG_XER_SO)
	{
		drcuml->log_printf("%sxer_so", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_XER_SO))
			drcuml->log_printf("*");
	}
	if (reglist[3] & REGFLAG_XER_COUNT)
	{
		drcuml->log_printf("%sxer_count", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_XER_COUNT))
			drcuml->log_printf("*");
	}
	if (reglist[3] & REGFLAG_CTR)
	{
		drcuml->log_printf("%sctr", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_CTR))
			drcuml->log_printf("*");
	}
	if (reglist[3] & REGFLAG_LR)
	{
		drcuml->log_printf("%slr", (count++ == 0) ? "" : ",");
		if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_LR))
			drcuml->log_printf("*");
	}

	for (regnum = 0; regnum < 8; regnum++)
		if (reglist[3] & REGFLAG_FPSCR(regnum))
		{
			drcuml->log_printf("%sfpscr%d", (count++ == 0) ? "" : ",", regnum);
			if (regnostarlist != NULL && !(regnostarlist[3] & REGFLAG_FPSCR(regnum)))
				drcuml->log_printf("*");
		}

	drcuml->log_printf("] ");
}


/*-------------------------------------------------
    log_opcode_desc - log a list of descriptions
-------------------------------------------------*/

static void log_opcode_desc(drcuml_state *drcuml, const opcode_desc *desclist, int indent)
{
	/* open the file, creating it if necessary */
	if (indent == 0)
		drcuml->log_printf("\nDescriptor list @ %08X\n", desclist->pc);

	/* output each descriptor */
	for ( ; desclist != NULL; desclist = desclist->next())
	{
		char buffer[100];

		/* disassemle the current instruction and output it to the log */
		if (LOG_UML || LOG_NATIVE)
		{
			if (desclist->flags & OPFLAG_VIRTUAL_NOOP)
				strcpy(buffer, "<virtual nop>");
			else
				ppc_dasm_one(buffer, desclist->pc, desclist->opptr.l[0]);
		}
		else
			strcpy(buffer, "???");

		drcuml->log_printf("%08X [%08X] t:%08X f:%s: %-30s", desclist->pc, desclist->physpc, desclist->targetpc, log_desc_flags_to_string(desclist->flags), buffer);

		/* output register states */
		log_register_list(drcuml, "use", desclist->regin, NULL);
		log_register_list(drcuml, "mod", desclist->regout, desclist->regreq);
		drcuml->log_printf("\n");

		/* if we have a delay slot, output it recursively */
		if (desclist->delay.first() != NULL)
			log_opcode_desc(drcuml, desclist->delay.first(), indent + 1);

		/* at the end of a sequence add a dividing line */
		if (desclist->flags & OPFLAG_END_SEQUENCE)
			drcuml->log_printf("-----\n");
	}
}



/***************************************************************************
    PPC 4XX VARIANTS
***************************************************************************/

/*-------------------------------------------------
    ppcdrc4xx_get_info - PowerPC 4XX-specific
    information getter
-------------------------------------------------*/

static CPU_GET_INFO( ppcdrc4xx )
{
	powerpc_state *ppc = (device != NULL && device->token() != NULL) ? get_safe_token(device) : NULL;
	CPU_GET_INFO_CALL(ppcdrc);
	ppc4xx_get_info(ppc, state, info);
}


/*-------------------------------------------------
    ppcdrc4xx_set_info - PowerPC 4XX-specific
    information setter
-------------------------------------------------*/

static CPU_SET_INFO( ppcdrc4xx )
{
	powerpc_state *ppc = get_safe_token(device);
	CPU_SET_INFO_CALL(ppcdrc);
	ppc4xx_set_info(ppc, state, info);
}


/*-------------------------------------------------
    ppc403ga_init - PowerPC 403GA-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc403ga )
{
	ppcdrc_init(PPC_MODEL_403GA, PPCCAP_4XX, 1, device, irqcallback);
}


/*-------------------------------------------------
    ppc403ga_get_info - PowerPC 403GA-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc403ga )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc403ga);				break;
		case CPUINFO_FCT_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(ppcdrc4xx);		break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 403GA");		break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc4xx);		break;
	}
}


/*-------------------------------------------------
    ppc403gcx_init - PowerPC 403GCX-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc403gcx )
{
	ppcdrc_init(PPC_MODEL_403GCX, PPCCAP_4XX, 1, device, irqcallback);
}


/*-------------------------------------------------
    ppc403gcx_get_info - PowerPC 403GCX-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc403gcx )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc403gcx);			break;
		case CPUINFO_FCT_SET_INFO:						info->setinfo = CPU_SET_INFO_NAME(ppcdrc4xx);		break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 403GCX");		break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc4xx);		break;
	}
}



/***************************************************************************
    PPC 6XX VARIANTS
***************************************************************************/

/*-------------------------------------------------
    ppc601_init - PowerPC 601-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc601 )
{
	ppcdrc_init(PPC_MODEL_601, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_MFIOC | PPCCAP_601BAT, 0/* no TB */, device, irqcallback);
}


/*-------------------------------------------------
    ppc601_get_info - PowerPC 601-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc601 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc601);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 601");			break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}


/*-------------------------------------------------
    ppc602_init - PowerPC 602-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc602 )
{
	ppcdrc_init(PPC_MODEL_602, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_603_MMU, 4, device, irqcallback);
}


/*-------------------------------------------------
    ppc602_get_info - PowerPC 602-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc602 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc602);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 602");			break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}


/*-------------------------------------------------
    ppc603_init - PowerPC 603-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc603 )
{
	ppcdrc_init(PPC_MODEL_603, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_603_MMU, 4, device, irqcallback);
}


/*-------------------------------------------------
    ppc603_get_info - PowerPC 603-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc603 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc603);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 603");			break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}


/*-------------------------------------------------
    ppc603e_init - PowerPC 603e-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc603e )
{
	ppcdrc_init(PPC_MODEL_603E, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_603_MMU, 4, device, irqcallback);
}


/*-------------------------------------------------
    ppc603e_get_info - PowerPC 603e-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc603e )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc603e);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 603e");		break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}


/*-------------------------------------------------
    ppc603r_init - PowerPC 603r-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc603r )
{
	ppcdrc_init(PPC_MODEL_603R, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_603_MMU, 4, device, irqcallback);
}


/*-------------------------------------------------
    ppc603r_get_info - PowerPC 603r-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc603r )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc603r);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 603R");		break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}


/*-------------------------------------------------
    ppc604_init - PowerPC 604-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( ppc604 )
{
	ppcdrc_init(PPC_MODEL_604, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED, 4, device, irqcallback);
}


/*-------------------------------------------------
    ppc604_get_info - PowerPC 604-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( ppc604 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(ppc604);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC 604");			break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}



/***************************************************************************
    MPC VARIANTS
***************************************************************************/

/*-------------------------------------------------
    mpc8240_init - PowerPC MPC8240-specific
    initialization
-------------------------------------------------*/

static CPU_INIT( mpc8240 )
{
	ppcdrc_init(PPC_MODEL_MPC8240, PPCCAP_OEA | PPCCAP_VEA | PPCCAP_FPU | PPCCAP_MISALIGNED | PPCCAP_603_MMU, 4/* unknown */, device, irqcallback);
}


/*-------------------------------------------------
    mpc8240_get_info - PowerPC MPC8240-specific
    information getter
-------------------------------------------------*/

CPU_GET_INFO( mpc8240 )
{
	switch (state)
	{
		/* --- the following bits of info are returned as 64-bit signed integers --- */

		/* --- the following bits of info are returned as pointers to data or functions --- */
		case CPUINFO_FCT_INIT:							info->init = CPU_INIT_NAME(mpc8240);				break;

		/* --- the following bits of info are returned as NULL-terminated strings --- */
		case DEVINFO_STR_NAME:							strcpy(info->s, "PowerPC MPC8240");		break;

		/* --- everything else is handled generically --- */
		default:										CPU_GET_INFO_CALL(ppcdrc);			break;
	}
}

DEFINE_LEGACY_CPU_DEVICE(PPC403GA, ppc403ga);
DEFINE_LEGACY_CPU_DEVICE(PPC403GCX, ppc403gcx);

DEFINE_LEGACY_CPU_DEVICE(PPC601, ppc601);
DEFINE_LEGACY_CPU_DEVICE(PPC602, ppc602);
DEFINE_LEGACY_CPU_DEVICE(PPC603, ppc603);
DEFINE_LEGACY_CPU_DEVICE(PPC603E, ppc603e);
DEFINE_LEGACY_CPU_DEVICE(PPC603R, ppc603r);
DEFINE_LEGACY_CPU_DEVICE(PPC604, ppc604);
DEFINE_LEGACY_CPU_DEVICE(MPC8240, mpc8240);
