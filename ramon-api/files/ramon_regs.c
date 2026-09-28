// SPDX-License-Identifier: MIT
/* libramon register windows, RS-TOP and SYSMON helpers */
#include "ramon_priv.h"

static int reg_io(ramon_ctx *c, const char *name, uint32_t index, uint32_t op, uint32_t off,
		  uint32_t *val, struct ramon_status *st)
{
	struct ramon_reg_io r;
	int ret;

	memset(&r, 0, sizeof(r));
	if (name) {
		if (!name[0] || strlen(name) >= sizeof(r.name))
			return ramon__fail(st, RAMON_EL_INVAL, 0, 0, 0,
					   "register window name \"%.40s\" is empty or too long", name);
		memcpy(r.name, name, strlen(name));
	}
	r.index = index;
	r.op = op;
	r.offset = off;
	r.value = *val;
	ret = ramon__ioctl(c, RAMON_IOC_REG_IO, &r, &r.st, st);
	if (!ret)
		*val = r.value;
	return ret;
}

int ramon_reg_read(ramon_ctx *c, const char *win, uint32_t off, uint32_t *val,
		   struct ramon_status *st)
{
	*val = 0;
	return reg_io(c, win, 0, RAMON_REG_OP_READ, off, val, st);
}

int ramon_reg_write(ramon_ctx *c, const char *win, uint32_t off, uint32_t val,
		    struct ramon_status *st)
{
	return reg_io(c, win, 0, RAMON_REG_OP_WRITE, off, &val, st);
}

int ramon_reg_read_idx(ramon_ctx *c, uint32_t index, uint32_t off, uint32_t *val,
		       struct ramon_status *st)
{
	*val = 0;
	return reg_io(c, NULL, index, RAMON_REG_OP_READ, off, val, st);
}

int ramon_reg_write_idx(ramon_ctx *c, uint32_t index, uint32_t off, uint32_t val,
			struct ramon_status *st)
{
	return reg_io(c, NULL, index, RAMON_REG_OP_WRITE, off, &val, st);
}

int ramon_regwin_find(ramon_ctx *c, const char *name, uint32_t *index, struct ramon_status *st)
{
	struct ramon_regwin_info ri;
	size_t n = strlen(name);
	uint32_t i;
	int ret;

	for (i = 0; i < c->info.n_regwin; i++) {
		ret = ramon_regwin_info(c, i, &ri, st);
		if (ret)
			return ret;
		ri.name[sizeof(ri.name) - 1] = 0;
		if (!strcmp(ri.name, name) ||
		    (!strncmp(ri.name, name, n) && ri.name[n] == '@')) {
			*index = i;
			return 0;
		}
	}
	return ramon__fail(st, RAMON_EL_INVAL, ENOENT, 0, 0, "no register window \"%.40s\"", name);
}

int ramon_rstop_version(ramon_ctx *c, uint32_t v[3], struct ramon_status *st)
{
	int ret;

	ret = ramon_reg_read(c, "rs_top", RAMON_RSTOP_MAJOR, &v[0], st);
	if (!ret)
		ret = ramon_reg_read(c, "rs_top", RAMON_RSTOP_MINOR, &v[1], st);
	if (!ret)
		ret = ramon_reg_read(c, "rs_top", RAMON_RSTOP_MINOR_MINOR, &v[2], st);
	return ret;
}

void ramon_rstop_decode_time(uint32_t v, struct ramon_rstop_time *t)
{
	t->sec = v & 0x3F;
	t->min = (v >> 6) & 0x3F;
	t->hour = (v >> 12) & 0x1F;
	t->year = (v >> 17) & 0x3F;
	t->month = (v >> 23) & 0x0F;
	t->day = (v >> 27) & 0x1F;
}

/* the old init_sysmon_list(): 10 PS rails, then 27 PL rails */
#define PS	RAMON_WIN_SYSMON_PS
#define PL	RAMON_WIN_SYSMON_PL
const struct ramon_sysmon_rail ramon_sysmon_rails[RAMON_SYSMON_RAILS] = {
	{ "vcc_psintlp", "VCC PS LPD voltage (supply1)",		PS, 0x0004, 3 },
	{ "vcc_psintfp", "VCC PS FPD voltage (supply2)",		PS, 0x0008, 3 },
	{ "vcc_psaux",	 "PS aux voltage reference (supply3)",		PS, 0x0018, 3 },
	{ "vcco_psddr",	 "DDR I/O VCC voltage",				PS, 0x0034, 3 },
	{ "vcco_psio2",	 "PS IO bank 503 voltage (supply5)",		PS, 0x0038, 6 },
	{ "vcco_psio3",	 "PS IO bank 500 voltage",			PS, 0x003C, 6 },
	{ "vcco_psio0",	 "VCCO_PSIO1 voltage",				PS, 0x0200, 6 },
	{ "vcco_psio1",	 "VCCO_PSIO2 voltage",				PS, 0x0204, 6 },
	{ "ps_mgtravcc", "VCC_PS_GTR voltage (VPS_MGTRAVCC)",		PS, 0x0208, 3 },
	{ "ps_mgtravtt", "VTT_PS_GTR voltage (VPS_MGTRAVTT)",		PS, 0x020C, 3 },
	{ "vauxp00",	 "auxiliary channel 0 voltage",			PL, 0x0040, 1 },
	{ "vauxp01",	 "auxiliary channel 1 voltage",			PL, 0x0044, 1 },
	{ "vauxp02",	 "auxiliary channel 2 voltage",			PL, 0x0048, 1 },
	{ "vauxp03",	 "auxiliary channel 3 voltage",			PL, 0x004C, 1 },
	{ "vauxp04",	 "auxiliary channel 4 voltage",			PL, 0x0050, 1 },
	{ "vauxp05",	 "auxiliary channel 5 voltage",			PL, 0x0054, 1 },
	{ "vauxp06",	 "auxiliary channel 6 voltage",			PL, 0x0058, 1 },
	{ "vauxp07",	 "auxiliary channel 7 voltage",			PL, 0x005C, 1 },
	{ "vauxp08",	 "auxiliary channel 8 voltage",			PL, 0x0060, 1 },
	{ "vauxp09",	 "auxiliary channel 9 voltage",			PL, 0x0064, 1 },
	{ "vauxp10",	 "auxiliary channel 10 voltage",		PL, 0x0068, 1 },
	{ "vauxp11",	 "auxiliary channel 11 voltage",		PL, 0x006C, 1 },
	{ "vauxp12",	 "auxiliary channel 12 voltage",		PL, 0x0070, 1 },
	{ "vauxp13",	 "auxiliary channel 13 voltage",		PL, 0x0074, 1 },
	{ "vauxp14",	 "auxiliary channel 14 voltage",		PL, 0x0078, 1 },
	{ "vauxp15",	 "auxiliary channel 15 voltage",		PL, 0x007C, 1 },
	{ "vccint",	 "PL internal voltage",				PL, 0x0004, 3 },
	{ "vccaux",	 "PL auxiliary voltage (VCCAUX)",		PL, 0x0008, 3 },
	{ "vccbram",	 "PL block RAM voltage (VCCBRAM)",		PL, 0x0018, 3 },
	{ "vcc_psintlp", "LPD internal voltage (VCC_PSINTLP)",		PL, 0x0034, 3 },
	{ "vcc_psintfp", "FPD internal voltage (VCC_PSINTFP)",		PL, 0x0038, 3 },
	{ "vcc_psaux",	 "PS auxiliary voltage",			PL, 0x003C, 3 },
	{ "vp_vn",	 "differential analog input voltage",		PL, 0x000C, 1 },
	{ "vuser0",	 "VUser0 voltage",				PL, 0x0200, 6 },
	{ "vuser1",	 "VUser1 voltage",				PL, 0x0204, 6 },
	{ "vuser2",	 "VUser2 voltage",				PL, 0x0208, 6 },
	{ "vuser3",	 "VUser3 voltage",				PL, 0x020C, 6 },
};
#undef PS
#undef PL

double ramon_sysmon_temp_c(uint32_t raw)
{
	return raw / 65536.0 * 509.314 - 280.239;
}

double ramon_sysmon_volts(uint32_t raw, uint32_t mult)
{
	return raw / 65536.0 * mult;
}

int ramon_sysmon_read(ramon_ctx *c, struct ramon_sysmon *out, struct ramon_status *st)
{
	unsigned i;
	int ret;

	memset(out, 0, sizeof(*out));
	ret = ramon_reg_read_idx(c, RAMON_WIN_SYSMON_PS, 0, &out->ps_temp_raw, st);
	if (!ret)
		ret = ramon_reg_read_idx(c, RAMON_WIN_SYSMON_PL, 0, &out->pl_temp_raw, st);
	if (ret)
		return ret;
	out->ps_temp_c = ramon_sysmon_temp_c(out->ps_temp_raw);
	out->pl_temp_c = ramon_sysmon_temp_c(out->pl_temp_raw);
	for (i = 0; i < RAMON_SYSMON_RAILS; i++) {
		const struct ramon_sysmon_rail *r = &ramon_sysmon_rails[i];

		ret = ramon_reg_read_idx(c, r->win, r->offset, &out->raw[i], st);
		if (ret)
			return ret;
		out->volts[i] = ramon_sysmon_volts(out->raw[i], r->mult);
	}
	return 0;
}
