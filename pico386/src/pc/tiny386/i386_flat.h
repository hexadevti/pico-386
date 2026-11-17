/* i386_flat.h -- pico-386: a fast path for flat 32-bit protected-mode code (FastDoom's DOS/32A).
 * Included only by i386.c, before cpu_exec1.
 *
 * cpu_exec1 spends about 130 host instructions on each guest instruction on the P4: the prefix
 * dispatch, fetches through the ifetch cache, segment and paging checks in translate, and the
 * reloads the compiler must do after every store into guest RAM (a u8/u32 store may alias any
 * field of the CPU struct). Here, while CS, DS and SS have base 0, paging is off and the code is
 * 32-bit, the hottest instructions run with host pointers into guest RAM and the registers in a
 * local array. The lazy flags stay in cpu->cc with tiny386's representation, so get_CF() and the
 * rest still read them. Anything else (prefixes, other opcodes, a memory operand outside plain
 * RAM) leaves the loop with nothing changed, and cpu_exec1 runs that instruction as before. No
 * instruction here can fault: segment limits are not checked by tiny386 either (segcheck), and
 * the only checks done there (a null DS/SS) are done once on entry. */

#define FL_ALL (CF | PF | AF | ZF | SF | OF)
#define FL_LOGIC (CF | PF | ZF | SF | OF)

#define FL_RD16(q) (*(const u16 *) (q))
#define FL_RD32(q) (*(const u32 *) (q))
#define FL_R8(i) ((i) & 4 ? (u8) (R[(i) & 3] >> 8) : (u8) R[(i)])
#define FL_SET8(i, v) do { int _i = (i); u32 _s = (_i & 4) << 1; \
	R[_i & 3] = (R[_i & 3] & ~(0xffu << _s)) | ((u32) (u8) (v) << _s); } while (0)
#define FL_SET16(i, v) (R[i] = (R[i] & 0xffff0000u) | (u16) (v))
/* plain RAM, as PLAIN_RAM in i386.c: outside the VGA window, and the whole access in RAM */
#define FL_OK(a, n) ((a) <= memsz - (n) && (a) - 0xa0000 >= 0x20000)


#ifdef T386_FLATCHECK
/* desktop only: each instruction the fast path runs is run again by cpu_exec1 from the same
 * state (the hook in i386_exec.h), and the registers, EIP, the six flags and the bytes written
 * must match */
static uword fl_wa;
static int fl_wn, flat_off;
static u8 fl_wold[4], fl_wnew[4];
static u32 fa_R[8], fa_ip, fa_fl;
static unsigned long long flat_checked, flat_bad;
#define FLAT_WREC(a, n) (fl_wa = (a), fl_wn = (n), memcpy(fl_wold, mem + (a), (n)))
static u32 fl_flags(CPUI386 *cpu)
{
	return (cpu->flags & ~(u32) FL_ALL) | (get_CF(cpu) ? CF : 0) | (get_PF(cpu) ? PF : 0) |
	       (get_AF(cpu) ? AF : 0) | (get_ZF(cpu) ? ZF : 0) | (get_SF(cpu) ? SF : 0) | (get_OF(cpu) ? OF : 0);
}
static void flat_check_dump(void)
{
	fprintf(stderr, "flatcheck %llu checked, %llu bad\n", flat_checked, flat_bad);
}
static void flat_check_a(CPUI386 *cpu)
{
	for (int i = 0; i < 8; i++) fa_R[i] = REGi(i);
	fa_ip = cpu->next_ip;
	fa_fl = fl_flags(cpu);
	if (fl_wa != ~(uword) 0) {
		memcpy(fl_wnew, cpu->phys_mem + fl_wa, fl_wn);
		memcpy(cpu->phys_mem + fl_wa, fl_wold, fl_wn);
	}
}
static void flat_check_b(CPUI386 *cpu, const CPUI386 *before)
{
	static bool reg;
	if (!reg) { reg = true; atexit(flat_check_dump); }
	flat_checked++;
	const u8 *j = cpu->phys_mem + fa_ip;
	/* CMP and TEST in cpu_exec1 also take a JZ/JNZ right after them (try_jcc8) */
	bool jcc = (j[0] & ~1) == 0x74 && cpu->next_ip == fa_ip + 2 + (s8) j[1];
	bool bad = (fa_ip != cpu->next_ip && !jcc) || fa_fl != fl_flags(cpu);
	for (int i = 0; i < 8; i++) bad |= fa_R[i] != REGi(i);
	if (fl_wa != ~(uword) 0) bad |= memcmp(fl_wnew, cpu->phys_mem + fl_wa, fl_wn) != 0;
	if (bad && flat_bad++ < 30) {
		const u8 *c = cpu->phys_mem + before->next_ip;
		fprintf(stderr, "flatcheck BAD at %08x: %02x %02x %02x %02x %02x %02x | ip %08x/%08x fl %03x/%03x\n",
			before->next_ip, c[0], c[1], c[2], c[3], c[4], c[5], fa_ip, cpu->next_ip, fa_fl, fl_flags(cpu));
		for (int i = 0; i < 8; i++)
			if (fa_R[i] != REGi(i)) fprintf(stderr, "  r%d %08x/%08x\n", i, fa_R[i], REGi(i));
		if (fl_wa != ~(uword) 0 && memcmp(fl_wnew, cpu->phys_mem + fl_wa, fl_wn))
			fprintf(stderr, "  mem %08x differs\n", fl_wa);
	}
}
#else
#define FLAT_WREC(a, n)
#endif
#define FL_CHK(a, n) do { if (!FL_OK(a, n)) goto miss; FLAT_WREC(a, n); } while (0)

#ifdef T386_OPSTATS
/* why the fast path stopped: the first byte (0x0f: 0x100 + the second) */
static unsigned long long flat_miss[768], flat_hit;
static void flat_dump(void)
{
	fprintf(stderr, "flatstat hit %llu\n", flat_hit);
	for (int i = 0; i < 768; i++)
		if (flat_miss[i]) fprintf(stderr, "flatstat %s%02x %llu\n", i >= 512 ? "66 " : i >= 256 ? "0f " : "", i & 255, flat_miss[i]);
}
#define FLAT_MISS(p) do { static bool _r; if (!_r) { _r = true; atexit(flat_dump); } \
	flat_miss[(p)[0] == 0x0f ? 256 + (p)[1] : (p)[0] == 0x66 ? 512 + (p)[1] : (p)[0]]++; } while (0)
#define FLAT_HIT(n) (flat_hit += (n))
#else
#define FLAT_MISS(p)
#define FLAT_HIT(n)
#endif

/* the effective address of a 32-bit ModRM operand (mod != 3); seg bases are all 0 here */
static inline __attribute__((always_inline)) uword fl_ea(const u8 **pp, int modrm, const u32 *R)
{
	const u8 *p = *pp;
	int mod = modrm >> 6, rm = modrm & 7;
	uword a;
	if (rm == 4) {
		u8 sib = *p++;
		int b = sib & 7;
		if (b == 5 && mod == 0) {
			a = FL_RD32(p);
			p += 4;
		} else {
			a = R[b];
		}
		int i = (sib >> 3) & 7;
		if (i != 4)
			a += R[i] << (sib >> 6);
	} else if (rm == 5 && mod == 0) {
		a = FL_RD32(p);
		p += 4;
	} else {
		a = R[rm];
	}
	if (mod == 1) {
		a += (s8) *p++;
	} else if (mod == 2) {
		a += (s32) FL_RD32(p);
		p += 4;
	}
	*pp = p;
	return a;
}

/* ADD OR AND SUB XOR CMP (kind = bits 3-5 of the opcode; ADC and SBB never get here), as
 * AOP0_helper and LOP0_helper set the flags */
static inline __attribute__((always_inline)) u32 fl_alu32(CPUI386 *cpu, int kind, u32 a, u32 b)
{
	switch (kind) {
	case 0:
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = a + b;
		cpu->cc.op = CC_ADD; cpu->cc.mask = FL_ALL;
		return a + b;
	case 5: case 7:
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = a - b;
		cpu->cc.op = CC_SUB; cpu->cc.mask = FL_ALL;
		return a - b;
	case 1:
		cpu->cc.dst = a | b; cpu->cc.op = CC_OR; cpu->cc.mask = FL_LOGIC;
		return a | b;
	case 4:
		cpu->cc.dst = a & b; cpu->cc.op = CC_AND; cpu->cc.mask = FL_LOGIC;
		return a & b;
	default: /* 6 */
		cpu->cc.dst = a ^ b; cpu->cc.op = CC_XOR; cpu->cc.mask = FL_LOGIC;
		return a ^ b;
	}
}

static inline __attribute__((always_inline)) u8 fl_alu8(CPUI386 *cpu, int kind, u8 a8, u8 b8)
{
	uword a = sext8(a8), b = sext8(b8), r;
	switch (kind) {
	case 0:
		r = sext8(a + b);
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = r;
		cpu->cc.op = CC_ADD; cpu->cc.mask = FL_ALL;
		return r;
	case 5: case 7:
		r = sext8(a - b);
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = r;
		cpu->cc.op = CC_SUB; cpu->cc.mask = FL_ALL;
		return r;
	case 1:
		r = sext8(a8 | b8); cpu->cc.op = CC_OR; break;
	case 4:
		r = sext8(a8 & b8); cpu->cc.op = CC_AND; break;
	default:
		r = sext8(a8 ^ b8); cpu->cc.op = CC_XOR; break;
	}
	cpu->cc.dst = r; cpu->cc.mask = FL_LOGIC;
	return r;
}

/* the same with an operand-size prefix: tiny386 keeps 16-bit flags sign-extended to 32 bits */
static inline __attribute__((always_inline)) u16 fl_alu16(CPUI386 *cpu, int kind, u16 a16, u16 b16)
{
	uword a = sext16(a16), b = sext16(b16), r;
	switch (kind) {
	case 0:
		r = sext16(a + b);
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = r;
		cpu->cc.op = CC_ADD; cpu->cc.mask = FL_ALL;
		return r;
	case 5: case 7:
		r = sext16(a - b);
		cpu->cc.src1 = a; cpu->cc.src2 = b; cpu->cc.dst = r;
		cpu->cc.op = CC_SUB; cpu->cc.mask = FL_ALL;
		return r;
	case 1:
		r = sext16(a16 | b16); cpu->cc.op = CC_OR; break;
	case 4:
		r = sext16(a16 & b16); cpu->cc.op = CC_AND; break;
	default:
		r = sext16(a16 ^ b16); cpu->cc.op = CC_XOR; break;
	}
	cpu->cc.dst = r; cpu->cc.mask = FL_LOGIC;
	return r;
}

/* COND() for condition c (the low nibble of a Jcc) */
static inline __attribute__((always_inline)) int fl_cond(CPUI386 *cpu, int c)
{
	/* the common case first: the flags of a SUB/CMP or a logic op, all lazy */
	if (cpu->cc.mask == FL_ALL && cpu->cc.op == CC_SUB) {
		uword s1 = cpu->cc.src1, s2 = cpu->cc.src2, d = cpu->cc.dst;
		switch (c) {
		case 0x2: return s1 < s2;
		case 0x3: return s1 >= s2;
		case 0x4: return d == 0;
		case 0x5: return d != 0;
		case 0x6: return s1 <= s2;
		case 0x7: return s1 > s2;
		case 0x8: return d >> 31;
		case 0x9: return !(d >> 31);
		}
	}
	int cond;
	switch (c) {
	case 0x0: cond =  get_OF(cpu); break;
	case 0x1: cond = !get_OF(cpu); break;
	case 0x2: cond =  get_CF(cpu); break;
	case 0x3: cond = !get_CF(cpu); break;
	case 0x4: cond =  get_ZF(cpu); break;
	case 0x5: cond = !get_ZF(cpu); break;
	case 0x6: cond =  get_ZF(cpu) ||  get_CF(cpu); break;
	case 0x7: cond = !get_ZF(cpu) && !get_CF(cpu); break;
	case 0x8: cond =  get_SF(cpu); break;
	case 0x9: cond = !get_SF(cpu); break;
	case 0xa: cond =  get_PF(cpu); break;
	case 0xb: cond = !get_PF(cpu); break;
	case 0xc: cond =  get_SF(cpu) != get_OF(cpu); break;
	case 0xd: cond =  get_SF(cpu) == get_OF(cpu); break;
	case 0xe: cond =  get_ZF(cpu) || get_SF(cpu) != get_OF(cpu); break;
	default:  cond = !get_ZF(cpu) && get_SF(cpu) == get_OF(cpu); break;
	}
	return cond;
}

/* Runs up to maxn instructions; returns how many. 0 when the state is not flat or the first
 * instruction is not one of these. */
static int IRAM_ATTR noinline cpu_exec_flat(CPUI386 *cpu, int maxn)
{
#ifndef ESP_PLATFORM /* TEMP: PC_NOFLAT=1 turns it off on the desktop */
	static int noflat = -1;
	if (noflat < 0) noflat = getenv("PC_NOFLAT") != NULL;
	if (noflat) return 0;
#endif
	if ((cpu->seg[SEG_CS].base | cpu->seg[SEG_DS].base | cpu->seg[SEG_SS].base) ||
	    cpu->sp_mask != 0xffffffff || (cpu->flags & VM) ||
	    !cpu->seg[SEG_DS].limit || !cpu->seg[SEG_SS].limit)
		return 0;
	u8 *const mem = cpu->phys_mem;
	const uword memsz = cpu->phys_mem_size;
	u32 R[8];
	for (int i = 0; i < 8; i++) R[i] = REGi(i);
	uword eip = cpu->next_ip;
	int n = 0;

	for (; n < maxn; n++) {
		if (unlikely(eip > memsz - 16 || eip - 0xa0000 < 0x20000))
			break;
		const u8 *p = mem + eip;
		const u8 op = *p++;
		switch (op) {
		/* ALU Eb,Gb / Ev,Gv / Gb,Eb / Gv,Ev / AL,Ib / eAX,Id: ADD OR AND SUB XOR CMP */
		case 0x00: case 0x08: case 0x20: case 0x28: case 0x30: case 0x38:
		case 0x02: case 0x0a: case 0x22: case 0x2a: case 0x32: case 0x3a: {
			int kind = op >> 3, modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				int rm = modrm & 7;
				if (op & 2) {
					u8 r = fl_alu8(cpu, kind, FL_R8(reg), FL_R8(rm));
					if (kind != 7) FL_SET8(reg, r);
				} else {
					u8 r = fl_alu8(cpu, kind, FL_R8(rm), FL_R8(reg));
					if (kind != 7) FL_SET8(rm, r);
				}
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				if (op & 2) {
					u8 r = fl_alu8(cpu, kind, FL_R8(reg), mem[a]);
					if (kind != 7) FL_SET8(reg, r);
				} else {
					u8 r = fl_alu8(cpu, kind, mem[a], FL_R8(reg));
					if (kind != 7) mem[a] = r;
				}
			}
			break;
		}
		case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39:
		case 0x03: case 0x0b: case 0x23: case 0x2b: case 0x33: case 0x3b: {
			int kind = op >> 3, modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				int rm = modrm & 7;
				if (op & 2) {
					u32 r = fl_alu32(cpu, kind, R[reg], R[rm]);
					if (kind != 7) R[reg] = r;
				} else {
					u32 r = fl_alu32(cpu, kind, R[rm], R[reg]);
					if (kind != 7) R[rm] = r;
				}
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				u32 *m = (u32 *) (mem + a);
				if (op & 2) {
					u32 r = fl_alu32(cpu, kind, R[reg], *m);
					if (kind != 7) R[reg] = r;
				} else {
					u32 r = fl_alu32(cpu, kind, *m, R[reg]);
					if (kind != 7) *m = r;
				}
			}
			break;
		}
		case 0x04: case 0x0c: case 0x24: case 0x2c: case 0x34: case 0x3c: {
			u8 r = fl_alu8(cpu, op >> 3, (u8) R[0], *p++);
			if (op != 0x3c) FL_SET8(0, r);
			break;
		}
		case 0x05: case 0x0d: case 0x25: case 0x2d: case 0x35: case 0x3d: {
			u32 r = fl_alu32(cpu, op >> 3, R[0], FL_RD32(p));
			p += 4;
			if (op != 0x3d) R[0] = r;
			break;
		}
		/* INC/DEC r32 */
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
			int cf = get_CF(cpu);
			u32 r = R[op & 7] + 1;
			cpu->cc.dst = r; cpu->cc.op = CC_INC32;
			SET_BIT(cpu->flags, cf, CF);
			cpu->cc.mask = PF | AF | ZF | SF | OF;
			R[op & 7] = r;
			break;
		}
		case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
			int cf = get_CF(cpu);
			u32 r = R[op & 7] - 1;
			cpu->cc.dst = r; cpu->cc.op = CC_DEC32;
			SET_BIT(cpu->flags, cf, CF);
			cpu->cc.mask = PF | AF | ZF | SF | OF;
			R[op & 7] = r;
			break;
		}
		/* PUSH/POP r32, PUSH imm */
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
			uword sp = R[4] - 4;
			FL_CHK(sp, 4);
			u32 v = R[op & 7];
			R[4] = sp;
			*(u32 *) (mem + sp) = v;
			break;
		}
		case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
			uword sp = R[4];
			FL_CHK(sp, 4);
			u32 v = *(u32 *) (mem + sp);
			R[4] = sp + 4;
			R[op & 7] = v;
			break;
		}
		case 0x68: case 0x6a: {
			u32 v;
			if (op == 0x68) { v = FL_RD32(p); p += 4; }
			else v = sext8(*p++);
			uword sp = R[4] - 4;
			FL_CHK(sp, 4);
			R[4] = sp;
			*(u32 *) (mem + sp) = v;
			break;
		}
		/* Jcc rel8 */
		case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
			s8 d = *p++;
			eip = p - mem;
			if (fl_cond(cpu, op & 0xf)) eip += d;
			continue;
		}
		/* G1: ADD OR AND SUB XOR CMP with an immediate */
		case 0x80: case 0x82: {
			int modrm = *p++, kind = (modrm >> 3) & 7;
			if (kind == 2 || kind == 3) goto miss;
			if ((modrm >> 6) == 3) {
				int rm = modrm & 7;
				u8 r = fl_alu8(cpu, kind, FL_R8(rm), *p++);
				if (kind != 7) FL_SET8(rm, r);
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				u8 r = fl_alu8(cpu, kind, mem[a], *p++);
				if (kind != 7) mem[a] = r;
			}
			break;
		}
		case 0x81: case 0x83: {
			int modrm = *p++, kind = (modrm >> 3) & 7;
			if (kind == 2 || kind == 3) goto miss;
			u32 *m;
			if ((modrm >> 6) == 3) {
				m = &R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				m = (u32 *) (mem + a);
			}
			u32 imm;
			if (op == 0x81) { imm = FL_RD32(p); p += 4; }
			else imm = sext8(*p++);
			u32 r = fl_alu32(cpu, kind, *m, imm);
			if (kind != 7) *m = r;
			break;
		}
		/* TEST */
		case 0x84: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			u8 v;
			if ((modrm >> 6) == 3) {
				v = FL_R8(modrm & 7);
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				v = mem[a];
			}
			fl_alu8(cpu, 4, v, FL_R8(reg));
			break;
		}
		case 0x85: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			u32 v;
			if ((modrm >> 6) == 3) {
				v = R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				v = *(u32 *) (mem + a);
			}
			fl_alu32(cpu, 4, v, R[reg]);
			break;
		}
		case 0xa8:
			fl_alu8(cpu, 4, (u8) R[0], *p++);
			break;
		case 0xa9:
			fl_alu32(cpu, 4, R[0], FL_RD32(p));
			p += 4;
			break;
		/* MOV */
		case 0x88: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				FL_SET8(modrm & 7, FL_R8(reg));
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				mem[a] = FL_R8(reg);
			}
			break;
		}
		case 0x89: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				R[modrm & 7] = R[reg];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				*(u32 *) (mem + a) = R[reg];
			}
			break;
		}
		case 0x8a: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				FL_SET8(reg, FL_R8(modrm & 7));
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				FL_SET8(reg, mem[a]);
			}
			break;
		}
		case 0x8b: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			if ((modrm >> 6) == 3) {
				R[reg] = R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				R[reg] = *(u32 *) (mem + a);
			}
			break;
		}
		case 0x8d: { /* LEA */
			int modrm = *p++;
			if ((modrm >> 6) == 3) goto miss;
			R[(modrm >> 3) & 7] = fl_ea(&p, modrm, R);
			break;
		}
		case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
			uword a = FL_RD32(p);
			p += 4;
			FL_CHK(a, 4);
			switch (op) {
			case 0xa0: FL_SET8(0, mem[a]); break;
			case 0xa1: R[0] = *(u32 *) (mem + a); break;
			case 0xa2: mem[a] = (u8) R[0]; break;
			default:   *(u32 *) (mem + a) = R[0]; break;
			}
			break;
		}
		case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
			FL_SET8(op & 7, *p++);
			break;
		case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
			R[op & 7] = FL_RD32(p);
			p += 4;
			break;
		case 0xc6: case 0xc7: {
			int modrm = *p++;
			if ((modrm >> 3) & 7) goto miss;
			if ((modrm >> 6) == 3) {
				if (op == 0xc6) FL_SET8(modrm & 7, *p++);
				else { R[modrm & 7] = FL_RD32(p); p += 4; }
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				if (op == 0xc6) mem[a] = *p++;
				else { *(u32 *) (mem + a) = FL_RD32(p); p += 4; }
			}
			break;
		}
		/* shifts: SHL SHR SAR by an immediate, 1 or CL, as SHL/SHR/SAR_helper */
		case 0xc1: case 0xd1: case 0xd3: {
			int modrm = *p++, kind = (modrm >> 3) & 7;
			if (kind < 4) goto miss;
			u32 *m;
			if ((modrm >> 6) == 3) {
				m = &R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				m = (u32 *) (mem + a);
			}
			uword y = (op == 0xc1 ? *p++ : op == 0xd1 ? 1 : R[1]) & 0x1f;
			if (y) {
				uword x = *m;
				if (kind == 5) {
					cpu->cc.src1 = x;
					cpu->cc.dst = x >> y;
					cpu->cc.dst2 = (x >> (y - 1)) & 1;
					cpu->cc.op = CC_SHR;
				} else if (kind == 7) {
					cpu->cc.dst = (sword) x >> y;
					cpu->cc.dst2 = ((sword) x >> (y - 1)) & 1;
					cpu->cc.op = CC_SAR;
				} else {
					cpu->cc.dst = x << y;
					cpu->cc.dst2 = (x >> (32 - y)) & 1;
					cpu->cc.op = CC_SHL;
				}
				cpu->cc.mask = FL_LOGIC;
				*m = cpu->cc.dst;
			}
			break;
		}
		/* JMP, CALL, RET */
		case 0xeb:
			eip = p + 1 - mem + (s8) *p;
			continue;
		case 0xe9:
			eip = p + 4 - mem + FL_RD32(p);
			continue;
		case 0xe8: {
			uword sp = R[4] - 4;
			FL_CHK(sp, 4);
			uword ret = p + 4 - mem;
			R[4] = sp;
			*(u32 *) (mem + sp) = ret;
			eip = ret + FL_RD32(p);
			continue;
		}
		case 0xc3: case 0xc2: {
			uword sp = R[4];
			FL_CHK(sp, 4);
			eip = *(u32 *) (mem + sp);
			R[4] = sp + 4 + (op == 0xc2 ? FL_RD16(p) : 0);
			continue;
		}
		case 0x0f: {
			const u8 op2 = *p++;
			if ((op2 & 0xf0) == 0x80) { /* Jcc rel32 */
				u32 d = FL_RD32(p);
				eip = p + 4 - mem;
				if (fl_cond(cpu, op2 & 0xf)) eip += d;
				continue;
			}
			switch (op2) {
			case 0xb6: case 0xbe: { /* MOVZX/MOVSX Gd,Eb */
				int modrm = *p++, reg = (modrm >> 3) & 7;
				u8 v;
				if ((modrm >> 6) == 3) {
					v = FL_R8(modrm & 7);
				} else {
					uword a = fl_ea(&p, modrm, R);
					FL_CHK(a, 1);
					v = mem[a];
				}
				R[reg] = op2 == 0xb6 ? v : sext8(v);
				break;
			}
			case 0xb7: case 0xbf: { /* MOVZX/MOVSX Gd,Ew */
				int modrm = *p++, reg = (modrm >> 3) & 7;
				u16 v;
				if ((modrm >> 6) == 3) {
					v = (u16) R[modrm & 7];
				} else {
					uword a = fl_ea(&p, modrm, R);
					FL_CHK(a, 2);
					v = *(u16 *) (mem + a);
				}
				R[reg] = op2 == 0xb7 ? v : sext16(v);
				break;
			}
			case 0xa4: case 0xa5: case 0xac: case 0xad: { /* SHLD/SHRD Ed,Gd,Ib/CL */
				int modrm = *p++, reg = (modrm >> 3) & 7;
				u32 *m;
				if ((modrm >> 6) == 3) {
					m = &R[modrm & 7];
				} else {
					uword a = fl_ea(&p, modrm, R);
					FL_CHK(a, 4);
					m = (u32 *) (mem + a);
				}
				int count = (op2 & 1 ? R[1] : *p++) & 0x1f;
				if (count) {
					uword x = *m, y = R[reg];
					if (op2 < 0xa8) {
						cpu->cc.src1 = x;
						cpu->cc.dst = (x << count) | (y >> (32 - count));
						cpu->cc.dst2 = count == 1 ? x : (x << (count - 1)) | (y >> (32 - (count - 1)));
						cpu->cc.op = CC_SHLD;
					} else {
						cpu->cc.src1 = x;
						cpu->cc.dst = (x >> count) | (y << (32 - count));
						cpu->cc.dst2 = count == 1 ? x : (x >> (count - 1)) | (y << (32 - (count - 1)));
						cpu->cc.op = CC_SHRD;
					}
					cpu->cc.mask = FL_LOGIC;
					*m = cpu->cc.dst;
				}
				break;
			}
			case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
			case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: {
				/* SETcc Eb */
				int modrm = *p++, c = fl_cond(cpu, op2 & 0xf);
				if ((modrm >> 6) == 3) {
					FL_SET8(modrm & 7, c);
				} else {
					uword a = fl_ea(&p, modrm, R);
					FL_CHK(a, 1);
					mem[a] = c;
				}
				break;
			}
			case 0xaf: { /* IMUL Gd,Ed */
				int modrm = *p++, reg = (modrm >> 3) & 7;
				u32 v;
				if ((modrm >> 6) == 3) {
					v = R[modrm & 7];
				} else {
					uword a = fl_ea(&p, modrm, R);
					FL_CHK(a, 4);
					v = *(u32 *) (mem + a);
				}
				int64_t res = (int64_t) (s32) R[reg] * (int64_t) (s32) v;
				cpu->cc.src1 = R[reg];
				cpu->cc.src2 = v;
				cpu->cc.dst = res;
				cpu->cc.dst2 = res >> 32;
				cpu->cc.op = CC_IMUL32;
				cpu->cc.mask = FL_ALL;
				R[reg] = res;
				break;
			}
			default:
				goto miss;
			}
			break;
		}
		/* operand-size prefix: the 16-bit forms FastDoom uses most */
		case 0x66: {
			const u8 op2 = *p++;
			switch (op2) {
			case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39:
			case 0x03: case 0x0b: case 0x23: case 0x2b: case 0x33: case 0x3b:
			case 0x85: case 0x89: case 0x8b: {
				int modrm = *p++, reg = (modrm >> 3) & 7, rm = modrm & 7;
				bool isreg = (modrm >> 6) == 3;
				uword a = 0;
				u16 v;
				if (isreg) {
					v = (u16) R[rm];
				} else {
					a = fl_ea(&p, modrm, R);
					FL_CHK(a, 2);
					v = FL_RD16(mem + a);
				}
				if (op2 == 0x8b) {
					FL_SET16(reg, v);
				} else if (op2 == 0x89) {
					if (isreg) FL_SET16(rm, R[reg]);
					else *(u16 *) (mem + a) = R[reg];
				} else if (op2 == 0x85) {
					fl_alu16(cpu, 4, v, (u16) R[reg]);
				} else if (op2 & 2) {
					u16 r = fl_alu16(cpu, op2 >> 3, (u16) R[reg], v);
					if (op2 != 0x3b) FL_SET16(reg, r);
				} else {
					u16 r = fl_alu16(cpu, op2 >> 3, v, (u16) R[reg]);
					if (op2 != 0x39) {
						if (isreg) FL_SET16(rm, r);
						else *(u16 *) (mem + a) = r;
					}
				}
				break;
			}
			case 0x81: case 0x83: case 0xc7: {
				int modrm = *p++, kind = (modrm >> 3) & 7, rm = modrm & 7;
				if (op2 == 0xc7 ? kind != 0 : (kind == 2 || kind == 3)) goto miss;
				bool isreg = (modrm >> 6) == 3;
				uword a = 0;
				if (!isreg) {
					a = fl_ea(&p, modrm, R);
					FL_CHK(a, 2);
				}
				u16 imm;
				if (op2 == 0x83) imm = sext8(*p++);
				else { imm = FL_RD16(p); p += 2; }
				u16 r = imm;
				if (op2 != 0xc7) {
					r = fl_alu16(cpu, kind, isreg ? (u16) R[rm] : FL_RD16(mem + a), imm);
					if (kind == 7) break;
				}
				if (isreg) FL_SET16(rm, r);
				else *(u16 *) (mem + a) = r;
				break;
			}
			default:
				goto miss;
			}
			break;
		}
		/* IMUL Gd,Ed,Ib/Id */
		case 0x69: case 0x6b: {
			int modrm = *p++, reg = (modrm >> 3) & 7;
			u32 v;
			if ((modrm >> 6) == 3) {
				v = R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				v = *(u32 *) (mem + a);
			}
			u32 imm;
			if (op == 0x6b) imm = sext8(*p++);
			else { imm = FL_RD32(p); p += 4; }
			int64_t res = (int64_t) (s32) v * (int64_t) (s32) imm;
			cpu->cc.src1 = v;
			cpu->cc.src2 = imm;
			cpu->cc.dst = res;
			cpu->cc.dst2 = res >> 32;
			cpu->cc.op = CC_IMUL32;
			cpu->cc.mask = FL_ALL;
			R[reg] = res;
			break;
		}
		case 0x98: /* CWDE */
			R[0] = sext16((u16) R[0]);
			break;
		case 0x99: /* CDQ */
			R[2] = (s32) R[0] >> 31;
			break;
		/* G3 byte: TEST Ib, NOT, NEG, MUL (DIV/IDIV stay in cpu_exec1) */
		case 0xf6: case 0xfe: {
			int modrm = *p++, sub = (modrm >> 3) & 7, rm = modrm & 7;
			if (op == 0xf6 ? (sub == 1 || sub > 4) : sub > 1) goto miss;
			bool isreg = (modrm >> 6) == 3;
			uword a = 0;
			u8 x;
			if (isreg) {
				x = FL_R8(rm);
			} else {
				a = fl_ea(&p, modrm, R);
				FL_CHK(a, 1);
				x = mem[a];
			}
			u8 r;
			if (op == 0xfe) { /* INC/DEC Eb */
				int cf = get_CF(cpu);
				r = sub ? x - 1 : x + 1;
				cpu->cc.dst = sext8(r);
				cpu->cc.op = sub ? CC_DEC8 : CC_INC8;
				SET_BIT(cpu->flags, cf, CF);
				cpu->cc.mask = PF | AF | ZF | SF | OF;
			} else if (sub == 0) {
				fl_alu8(cpu, 4, x, *p++);
				break;
			} else if (sub == 2) {
				r = ~x;
			} else if (sub == 3) {
				cpu->cc.src1 = sext8(x);
				cpu->cc.dst = sext8(-cpu->cc.src1);
				cpu->cc.op = CC_NEG8;
				cpu->cc.mask = FL_ALL;
				r = cpu->cc.dst;
			} else { /* MUL AL,Eb */
				cpu->cc.src1 = (u8) R[0];
				cpu->cc.src2 = x;
				cpu->cc.dst = sext16(cpu->cc.src1 * cpu->cc.src2);
				cpu->cc.op = CC_MUL8;
				cpu->cc.mask = FL_ALL;
				FL_SET16(0, cpu->cc.dst);
				break;
			}
			if (isreg) FL_SET8(rm, r);
			else mem[a] = r;
			break;
		}
		/* G3: TEST Id, NOT, NEG, MUL, IMUL, DIV, IDIV (a #DE goes to cpu_exec1) */
		case 0xf7: {
			int modrm = *p++, sub = (modrm >> 3) & 7;
			if (sub == 1) goto miss;
			u32 *m;
			if ((modrm >> 6) == 3) {
				m = &R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				m = (u32 *) (mem + a);
			}
			u32 x = *m;
			switch (sub) {
			case 0:
				fl_alu32(cpu, 4, x, FL_RD32(p));
				p += 4;
				break;
			case 2:
				*m = ~x;
				break;
			case 3:
				cpu->cc.src1 = x;
				cpu->cc.dst = -x;
				cpu->cc.op = CC_NEG32;
				cpu->cc.mask = FL_ALL;
				*m = -x;
				break;
			case 4: {
				uint64_t res = (uint64_t) R[0] * x;
				cpu->cc.src1 = R[0];
				cpu->cc.src2 = x;
				cpu->cc.dst = res;
				cpu->cc.dst2 = res >> 32;
				cpu->cc.op = CC_MUL32;
				cpu->cc.mask = FL_ALL;
				R[0] = res;
				R[2] = res >> 32;
				break;
			}
			case 5: {
				int64_t res = (int64_t) (s32) R[0] * (int64_t) (s32) x;
				cpu->cc.src1 = R[0];
				cpu->cc.src2 = x;
				cpu->cc.dst = res;
				cpu->cc.dst2 = res >> 32;
				cpu->cc.op = CC_IMUL32;
				cpu->cc.mask = FL_ALL;
				R[0] = res;
				R[2] = res >> 32;
				break;
			}
			case 6: {
				if (!x) goto miss;
				uint64_t s = ((uint64_t) R[2] << 32) | R[0], q = s / x;
				if (q >> 32) goto miss;
				R[0] = q;
				R[2] = s % x;
				break;
			}
			default: {
				int64_t s = (int64_t) (((uint64_t) R[2] << 32) | R[0]), d = (s32) x;
				if (!d || (d == -1 && s == INT64_MIN)) goto miss;
				int64_t q = s / d;
				if (q > INT32_MAX || q < INT32_MIN) goto miss;
				R[0] = q;
				R[2] = s % d;
				break;
			}
			}
			break;
		}
		/* G5: INC, DEC, CALL, JMP, PUSH Ed */
		case 0xff: {
			int modrm = *p++, sub = (modrm >> 3) & 7;
			if (sub == 3 || sub == 5 || sub == 7) goto miss;
			u32 *m;
			if ((modrm >> 6) == 3) {
				m = &R[modrm & 7];
			} else {
				uword a = fl_ea(&p, modrm, R);
				FL_CHK(a, 4);
				m = (u32 *) (mem + a);
			}
			u32 x = *m;
			if (sub < 2) {
				int cf = get_CF(cpu);
				u32 r = sub ? x - 1 : x + 1;
				cpu->cc.dst = r;
				cpu->cc.op = sub ? CC_DEC32 : CC_INC32;
				SET_BIT(cpu->flags, cf, CF);
				cpu->cc.mask = PF | AF | ZF | SF | OF;
				*m = r;
				break;
			}
			if (sub == 4) {
				eip = x;
				continue;
			}
			uword sp = R[4] - 4;
			FL_CHK(sp, 4);
			R[4] = sp;
			if (sub == 2) {
				*(u32 *) (mem + sp) = p - mem;
				eip = x;
				continue;
			}
			*(u32 *) (mem + sp) = x;
			break;
		}
		default:
			goto miss;
		}
		eip = p - mem;
	}
	if (0) {
miss:
		FLAT_MISS(mem + eip);
	}
	for (int i = 0; i < 8; i++) REGi(i) = R[i];
	cpu->next_ip = eip;
	cpu->cycle += n;
	FLAT_HIT(n);
	return n;
}
