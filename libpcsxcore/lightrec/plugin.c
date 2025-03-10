#include <lightrec.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "../cdrom.h"
#include "../gpu.h"
#include "../gte.h"
#include "../mdec.h"
#include "../psxdma.h"
#include "../psxhw.h"
#include "../psxmem.h"
#include "../r3000a.h"

#include "../frontend/main.h"

#define ARRAY_SIZE(x) (sizeof(x) ? sizeof(x) / sizeof((x)[0]) : 0)

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#	define LE32TOH(x)	__builtin_bswap32(x)
#	define HTOLE32(x)	__builtin_bswap32(x)
#	define LE16TOH(x)	__builtin_bswap16(x)
#	define HTOLE16(x)	__builtin_bswap16(x)
#else
#	define LE32TOH(x)	(x)
#	define HTOLE32(x)	(x)
#	define LE16TOH(x)	(x)
#	define HTOLE16(x)	(x)
#endif

#ifdef __GNUC__
#	define likely(x)       __builtin_expect(!!(x),1)
#	define unlikely(x)     __builtin_expect(!!(x),0)
#else
#	define likely(x)       (x)
#	define unlikely(x)     (x)
#endif

psxRegisters psxRegs;
Rcnt rcnts[4];

static struct lightrec_state *lightrec_state;

static char *name = "retroarch.exe";

static bool use_lightrec_interpreter;
static bool use_pcsx_interpreter;
static bool lightrec_debug;
static bool lightrec_very_debug;
static bool booting;
static u32 lightrec_begin_cycles;

enum my_cp2_opcodes {
	OP_CP2_RTPS		= 0x01,
	OP_CP2_NCLIP		= 0x06,
	OP_CP2_OP		= 0x0c,
	OP_CP2_DPCS		= 0x10,
	OP_CP2_INTPL		= 0x11,
	OP_CP2_MVMVA		= 0x12,
	OP_CP2_NCDS		= 0x13,
	OP_CP2_CDP		= 0x14,
	OP_CP2_NCDT		= 0x16,
	OP_CP2_NCCS		= 0x1b,
	OP_CP2_CC		= 0x1c,
	OP_CP2_NCS		= 0x1e,
	OP_CP2_NCT		= 0x20,
	OP_CP2_SQR		= 0x28,
	OP_CP2_DCPL		= 0x29,
	OP_CP2_DPCT		= 0x2a,
	OP_CP2_AVSZ3		= 0x2d,
	OP_CP2_AVSZ4		= 0x2e,
	OP_CP2_RTPT		= 0x30,
	OP_CP2_GPF		= 0x3d,
	OP_CP2_GPL		= 0x3e,
	OP_CP2_NCCT		= 0x3f,
};

static void (*cp2_ops[])(struct psxCP2Regs *) = {
	[OP_CP2_RTPS] = gteRTPS,
	[OP_CP2_RTPS] = gteRTPS,
	[OP_CP2_NCLIP] = gteNCLIP,
	[OP_CP2_OP] = gteOP,
	[OP_CP2_DPCS] = gteDPCS,
	[OP_CP2_INTPL] = gteINTPL,
	[OP_CP2_MVMVA] = gteMVMVA,
	[OP_CP2_NCDS] = gteNCDS,
	[OP_CP2_CDP] = gteCDP,
	[OP_CP2_NCDT] = gteNCDT,
	[OP_CP2_NCCS] = gteNCCS,
	[OP_CP2_CC] = gteCC,
	[OP_CP2_NCS] = gteNCS,
	[OP_CP2_NCT] = gteNCT,
	[OP_CP2_SQR] = gteSQR,
	[OP_CP2_DCPL] = gteDCPL,
	[OP_CP2_DPCT] = gteDPCT,
	[OP_CP2_AVSZ3] = gteAVSZ3,
	[OP_CP2_AVSZ4] = gteAVSZ4,
	[OP_CP2_RTPT] = gteRTPT,
	[OP_CP2_GPF] = gteGPF,
	[OP_CP2_GPL] = gteGPL,
	[OP_CP2_NCCT] = gteNCCT,
};

static char cache_buf[64 * 1024];

static void cop2_op(struct lightrec_state *state, u32 func)
{
	struct lightrec_registers *regs = lightrec_get_registers(state);

	psxRegs.code = func;

	if (unlikely(!cp2_ops[func & 0x3f])) {
		fprintf(stderr, "Invalid CP2 function %u\n", func);
	} else {
		/* This works because regs->cp2c comes right after regs->cp2d,
		 * so it can be cast to a pcsxCP2Regs pointer. */
		cp2_ops[func & 0x3f]((psxCP2Regs *) regs->cp2d);
	}
}

static bool has_interrupt(void)
{
	return ((psxHu32(0x1070) & psxHu32(0x1074)) &&
	     (psxRegs.CP0.n.Status & 0x401) == 0x401) ||
	    (psxRegs.CP0.n.Status & psxRegs.CP0.n.Cause & 0x0300);
}

static void lightrec_restore_state(struct lightrec_state *state)
{
	lightrec_reset_cycle_count(state, psxRegs.cycle);

	if (booting || has_interrupt())
		lightrec_set_exit_flags(state, LIGHTREC_EXIT_CHECK_INTERRUPT);
	else
		lightrec_set_target_cycle_count(state, next_interupt);
}

static void hw_write_byte(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u8 val)
{
	psxRegs.cycle = lightrec_current_cycle_count(state);

	psxHwWrite8(mem, val);

	lightrec_restore_state(state);
}

static void hw_write_half(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u16 val)
{
	psxRegs.cycle = lightrec_current_cycle_count(state);

	psxHwWrite16(mem, val);

	lightrec_restore_state(state);
}

static void hw_write_word(struct lightrec_state *state,
			  u32 op, void *host, u32 mem, u32 val)
{
	psxRegs.cycle = lightrec_current_cycle_count(state);

	psxHwWrite32(mem, val);

	lightrec_restore_state(state);
}

static u8 hw_read_byte(struct lightrec_state *state, u32 op, void *host, u32 mem)
{
	u8 val;

	psxRegs.cycle = lightrec_current_cycle_count(state);

	val = psxHwRead8(mem);

	lightrec_restore_state(state);

	return val;
}

static u16 hw_read_half(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u16 val;

	psxRegs.cycle = lightrec_current_cycle_count(state);

	val = psxHwRead16(mem);

	lightrec_restore_state(state);

	return val;
}

static u32 hw_read_word(struct lightrec_state *state,
			u32 op, void *host, u32 mem)
{
	u32 val;

	psxRegs.cycle = lightrec_current_cycle_count(state);

	val = psxHwRead32(mem);

	lightrec_restore_state(state);

	return val;
}

static struct lightrec_mem_map_ops hw_regs_ops = {
	.sb = hw_write_byte,
	.sh = hw_write_half,
	.sw = hw_write_word,
	.lb = hw_read_byte,
	.lh = hw_read_half,
	.lw = hw_read_word,
};

static u32 cache_ctrl;

static void cache_ctrl_write_word(struct lightrec_state *state,
				  u32 op, void *host, u32 mem, u32 val)
{
	cache_ctrl = val;
}

static u32 cache_ctrl_read_word(struct lightrec_state *state,
				u32 op, void *host, u32 mem)
{
	return cache_ctrl;
}

static struct lightrec_mem_map_ops cache_ctrl_ops = {
	.sw = cache_ctrl_write_word,
	.lw = cache_ctrl_read_word,
};

static struct lightrec_mem_map lightrec_map[] = {
	[PSX_MAP_KERNEL_USER_RAM] = {
		/* Kernel and user memory */
		.pc = 0x00000000,
		.length = 0x200000,
	},
	[PSX_MAP_BIOS] = {
		/* BIOS */
		.pc = 0x1fc00000,
		.length = 0x80000,
	},
	[PSX_MAP_SCRATCH_PAD] = {
		/* Scratch pad */
		.pc = 0x1f800000,
		.length = 0x400,
	},
	[PSX_MAP_PARALLEL_PORT] = {
		/* Parallel port */
		.pc = 0x1f000000,
		.length = 0x10000,
	},
	[PSX_MAP_HW_REGISTERS] = {
		/* Hardware registers */
		.pc = 0x1f801000,
		.length = 0x2000,
		.ops = &hw_regs_ops,
	},
	[PSX_MAP_CACHE_CONTROL] = {
		/* Cache control */
		.pc = 0x5ffe0130,
		.length = 4,
		.ops = &cache_ctrl_ops,
	},

	/* Mirrors of the kernel/user memory */
	[PSX_MAP_MIRROR1] = {
		.pc = 0x00200000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR2] = {
		.pc = 0x00400000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
	[PSX_MAP_MIRROR3] = {
		.pc = 0x00600000,
		.length = 0x200000,
		.mirror_of = &lightrec_map[PSX_MAP_KERNEL_USER_RAM],
	},
};

static void lightrec_enable_ram(struct lightrec_state *state, bool enable)
{
	if (enable)
		memcpy(psxM, cache_buf, sizeof(cache_buf));
	else
		memcpy(cache_buf, psxM, sizeof(cache_buf));
}

static const struct lightrec_ops lightrec_ops = {
	.cop2_op = cop2_op,
	.enable_ram = lightrec_enable_ram,
};

static int lightrec_plugin_init(void)
{
	lightrec_map[PSX_MAP_KERNEL_USER_RAM].address = psxM;
	lightrec_map[PSX_MAP_BIOS].address = psxR;
	lightrec_map[PSX_MAP_SCRATCH_PAD].address = psxH;
	lightrec_map[PSX_MAP_PARALLEL_PORT].address = psxP;

	lightrec_debug = !!getenv("LIGHTREC_DEBUG");
	lightrec_very_debug = !!getenv("LIGHTREC_VERY_DEBUG");
	use_lightrec_interpreter = !!getenv("LIGHTREC_INTERPRETER");
	if (getenv("LIGHTREC_BEGIN_CYCLES"))
	  lightrec_begin_cycles = (unsigned int) strtol(
				  getenv("LIGHTREC_BEGIN_CYCLES"), NULL, 0);

	lightrec_state = lightrec_init(name,
			lightrec_map, ARRAY_SIZE(lightrec_map),
			&lightrec_ops);

	// fprintf(stderr, "M=0x%lx, P=0x%lx, R=0x%lx, H=0x%lx\n",
	// 		(uintptr_t) psxM,
	// 		(uintptr_t) psxP,
	// 		(uintptr_t) psxR,
	// 		(uintptr_t) psxH);

#ifndef _WIN32
	signal(SIGPIPE, exit);
#endif
	return 0;
}

static u32 hash_calculate_le(const void *buffer, u32 count)
{
	unsigned int i;
	u32 *data = (u32 *) buffer;
	u32 hash = 0xffffffff;

	count /= 4;
	for(i = 0; i < count; ++i) {
		hash += LE32TOH(data[i]);
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);
	return hash;
}

static u32 hash_calculate(const void *buffer, u32 count)
{
	unsigned int i;
	u32 *data = (u32 *) buffer;
	u32 hash = 0xffffffff;

	count /= 4;
	for(i = 0; i < count; ++i) {
		hash += data[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);
	return hash;
}

static const char * const mips_regs[] = {
	"zero",
	"at",
	"v0", "v1",
	"a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"t8", "t9",
	"k0", "k1",
	"gp", "sp", "fp", "ra",
	"lo", "hi",
};

static void print_for_big_ass_debugger(void)
{
	unsigned int i;

	printf("CYCLE 0x%08x PC 0x%08x", psxRegs.cycle, psxRegs.pc);

	if (lightrec_very_debug)
		printf(" RAM 0x%08x SCRATCH 0x%08x HW 0x%08x",
				hash_calculate_le(psxM, 0x200000),
				hash_calculate_le(psxH, 0x400),
				hash_calculate_le(psxH + 0x1000, 0x2000));

	printf(" CP0 0x%08x CP2D 0x%08x CP2C 0x%08x INT 0x%04x INTCYCLE 0x%08x GPU 0x%08x",
			hash_calculate(&psxRegs.CP0.r,
				sizeof(psxRegs.CP0.r)),
			hash_calculate(&psxRegs.CP2D.r,
				sizeof(psxRegs.CP2D.r)),
			hash_calculate(&psxRegs.CP2C.r,
				sizeof(psxRegs.CP2C.r)),
			psxRegs.interrupt,
			hash_calculate(psxRegs.intCycle,
				sizeof(psxRegs.intCycle)),
			LE32TOH(HW_GPU_STATUS));

	if (lightrec_very_debug)
		for (i = 0; i < 34; i++)
			printf(" %s 0x%08x", mips_regs[i], psxRegs.GPR.r[i]);
	else
		printf(" GPR 0x%08x", hash_calculate(&psxRegs.GPR.r,
					sizeof(psxRegs.GPR.r)));
	printf("\n");
}

static void lightrec_dump_regs(struct lightrec_state *state)
{
	struct lightrec_registers *regs = lightrec_get_registers(state);

	if (unlikely(booting))
		memcpy(&psxRegs.GPR, regs->gpr, sizeof(regs->gpr));
	psxRegs.CP0.n.Status = regs->cp0[12];
	psxRegs.CP0.n.Cause = regs->cp0[13];
}

static void lightrec_restore_regs(struct lightrec_state *state)
{
	struct lightrec_registers *regs = lightrec_get_registers(state);

	if (unlikely(booting))
		memcpy(regs->gpr, &psxRegs.GPR, sizeof(regs->gpr));
	regs->cp0[12] = psxRegs.CP0.n.Status;
	regs->cp0[13] = psxRegs.CP0.n.Cause;
	regs->cp0[14] = psxRegs.CP0.n.EPC;
}

extern void intExecuteBlock();
extern void gen_interupt();

static void lightrec_plugin_execute_block(void)
{
	u32 old_pc = psxRegs.pc;
	u32 flags;

	gen_interupt();

	if (use_pcsx_interpreter) {
		intExecuteBlock();
	} else {
		lightrec_reset_cycle_count(lightrec_state, psxRegs.cycle);
		lightrec_restore_regs(lightrec_state);

		if (unlikely(use_lightrec_interpreter))
			psxRegs.pc = lightrec_run_interpreter(lightrec_state,
							      psxRegs.pc);
		// step during early boot so that 0x80030000 fastboot hack works
		else if (unlikely(booting || lightrec_debug))
			psxRegs.pc = lightrec_execute_one(lightrec_state,
							  psxRegs.pc);
		else
			psxRegs.pc = lightrec_execute(lightrec_state,
						      psxRegs.pc, next_interupt);

		psxRegs.cycle = lightrec_current_cycle_count(lightrec_state);

		lightrec_dump_regs(lightrec_state);
		flags = lightrec_exit_flags(lightrec_state);

		if (flags & LIGHTREC_EXIT_SEGFAULT) {
			fprintf(stderr, "Exiting at cycle 0x%08x\n",
				psxRegs.cycle);
			exit(1);
		}

		if (flags & LIGHTREC_EXIT_SYSCALL)
			psxException(0x20, 0);

		if (booting && (psxRegs.pc & 0xff800000) == 0x80000000)
			booting = false;
	}

	if (lightrec_debug && psxRegs.cycle >= lightrec_begin_cycles
			&& psxRegs.pc != old_pc)
		print_for_big_ass_debugger();

	if ((psxRegs.CP0.n.Cause & psxRegs.CP0.n.Status & 0x300) &&
			(psxRegs.CP0.n.Status & 0x1)) {
		/* Handle software interrupts */
		psxRegs.CP0.n.Cause &= ~0x7c;
		psxException(psxRegs.CP0.n.Cause, 0);
	}
}

static void lightrec_plugin_execute(void)
{
	extern int stop;

	while (!stop)
		lightrec_plugin_execute_block();
}

static void lightrec_plugin_clear(u32 addr, u32 size)
{
	if (addr == 0 && size == UINT32_MAX)
		lightrec_invalidate_all(lightrec_state);
	else
		/* size * 4: PCSX uses DMA units */
		lightrec_invalidate(lightrec_state, addr, size * 4);
}

static void lightrec_plugin_notify(int note, void *data)
{
	/*
	To change once proper icache emulation is emulated
	switch (note)
	{
		case R3000ACPU_NOTIFY_CACHE_UNISOLATED:
			lightrec_plugin_clear(0, 0x200000/4);
			break;
		case R3000ACPU_NOTIFY_CACHE_ISOLATED:
		// Sent from psxDma3().
		case R3000ACPU_NOTIFY_DMA3_EXE_LOAD:
		default:
			break;
	}*/
}

static void lightrec_plugin_apply_config()
{
}

static void lightrec_plugin_shutdown(void)
{
	lightrec_destroy(lightrec_state);
}

static void lightrec_plugin_reset(void)
{
	struct lightrec_registers *regs;

	lightrec_plugin_shutdown();
	lightrec_plugin_init();

	regs = lightrec_get_registers(lightrec_state);

	regs->cp0[12] = 0x10900000; // COP0 enabled | BEV = 1 | TS = 1
	regs->cp0[15] = 0x00000002; // PRevID = Revision ID, same as R3000A

	booting = true;
}

R3000Acpu psxRec =
{
	lightrec_plugin_init,
	lightrec_plugin_reset,
	lightrec_plugin_execute,
	lightrec_plugin_execute_block,
	lightrec_plugin_clear,
	lightrec_plugin_notify,
	lightrec_plugin_apply_config,
	lightrec_plugin_shutdown,
};
