/*
 * vGT EDID virtualization module
 *
 * Copyright(c) 2011-2013 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/delay.h>
#include <linux/slab.h>
#include <drm/drmP.h>

#include "vgt.h"

static const u8 edid_default_data[EDID_SIZE] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	0x10, 0xac, 0x7b, 0xa0, 0x4c, 0x39, 0x56, 0x31,
	0x03, 0x16, 0x01, 0x04,	0xa5, 0x34, 0x20, 0x78,
	0x3a, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
	0x0f, 0x50, 0x54, 0xa1, 0x08, 0x00, 0x81, 0x40,
	0x81, 0x80, 0xa9, 0x40, 0xb3, 0x00, 0xd1, 0xc0,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x28, 0x3c,
	0x80, 0xa0, 0x70, 0xb0, 0x23, 0x40, 0x30, 0x20,
	0x36, 0x00, 0x06, 0x44, 0x21, 0x00, 0x00, 0x1a,
	0x00, 0x00, 0x00, 0xff, 0x00, 0x59, 0x52, 0x34,
	0x38, 0x56, 0x32, 0x31, 0x48, 0x31, 0x56, 0x39,
	0x4c, 0x0a, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44,
	0x45, 0x4c, 0x4c, 0x20, 0x55, 0x32, 0x34, 0x31,
	0x32, 0x4d, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
	0x00, 0x32, 0x3d, 0x1e, 0x53, 0x11, 0x00, 0x0a,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xbb
};

static const u8 edid_header[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

int vgt_edid_header_is_valid(const u8 *raw_edid)
{
	int i, score = 0;
	for (i = 0; i < sizeof(edid_header); i++)
		if (raw_edid[i] == edid_header[i])
			score++;
	return score;
}

bool vgt_is_edid_valid(u8 *raw_edid)
{
	bool is_valid = false;
	int score, i;
	u8 check_sum = 0;

	score = vgt_edid_header_is_valid(raw_edid);

	check_sum = 0;
	for (i = 0; i < EDID_SIZE; ++i) {
		check_sum += raw_edid[i];
	}
	if (check_sum) {
		vgt_err("EDID check sum is invalid\n");
	}

	if ((score == 8) && (check_sum == 0)) {
		is_valid = true;
	}
	return is_valid;
}

static inline void vgt_clear_edid(struct gt_port *port)
{
	if (port && port->edid && port->edid->data_valid) {
		port->edid->data_valid = false;
	}
}

static inline void vgt_clear_dpcd(struct gt_port *port)
{
	if (port && port->dpcd && port->dpcd->data_valid) {
		port->dpcd->data_valid = false;
	}
}

void vgt_clear_port(struct vgt_device *vgt, int index)
{
	struct gt_port *port;

	if (!dpy_is_valid_port(index)) {
		vgt_warn("Wrong port index input! Will do nothing!\n");
		return;
	}

	port = &vgt->ports[index];
	vgt_clear_edid(port);
	vgt_clear_dpcd(port);
	
	port->type = VGT_PORT_MAX;
}

static unsigned char edid_get_byte(struct vgt_device *vgt)
{
	unsigned char chr = 0;
	struct vgt_i2c_edid_t *edid = &vgt->vgt_i2c_edid;

	if (edid->state == I2C_NOT_SPECIFIED || !edid->slave_selected) {
		vgt_warn("Driver tries to read EDID without proper sequence!\n");
		return 0;
	}
	if (edid->current_edid_read >= EDID_SIZE) {
		vgt_warn("edid_get_byte() exceeds the size of EDID!\n");
		return 0;
	}

	if (!edid->edid_available) {
		vgt_warn("Reading EDID but EDID is not available!"
			" Will return 0.\n");
		return 0;
	}

	if (dpy_has_monitor_on_port(vgt, edid->port)) {
		struct vgt_edid_data_t *edid_data = vgt->ports[edid->port].edid;
		chr = edid_data->edid_block[edid->current_edid_read];
		vgt_dbg(VGT_DBG_EDID,
			"edid_get_byte with offset %d and value %d\n",
			edid->current_edid_read, chr);
		edid->current_edid_read ++;
	} else {
		vgt_warn("No EDID available during the reading?\n");
	}

	return chr;
}

/**************************************************************************
 *
 * GMBUS interface for I2C access
 *
 *************************************************************************/
static inline enum port vgt_get_port_from_gmbus0(vgt_reg_t gmbus0)
{
	enum port port = I915_MAX_PORTS;
	int port_select = gmbus0 & _GMBUS_PIN_SEL_MASK;

	if (port_select == 2)
		port = PORT_E;
	else if (port_select == 4)
		port = PORT_C;
	else if (port_select == 5)
		port = PORT_B;
	else if (port_select == 6)
		port = PORT_D;

	return port;
}

/* GMBUS0 */
static bool vgt_gmbus0_mmio_write(struct vgt_device *vgt,
			unsigned int offset, void *p_data, unsigned int bytes)
{
	vgt_reg_t wvalue = *(vgt_reg_t *)p_data;
	enum port port = I915_MAX_PORTS;
	int pin_select = wvalue & _GMBUS_PIN_SEL_MASK;

	vgt_init_i2c_edid(vgt);

	if (pin_select == 0)
		return true;

	vgt->vgt_i2c_edid.state = I2C_GMBUS;
	port = vgt_get_port_from_gmbus0(pin_select);
	if (!dpy_is_valid_port(port)) {
		vgt_dbg(VGT_DBG_EDID,
			"VM(%d): Driver tries GMBUS write not on valid port!\n"
			"gmbus write value is: 0x%x\n", vgt->vgt_id, wvalue);
		return true;
	}

	vgt->vgt_i2c_edid.gmbus.phase = GMBUS_IDLE_PHASE;

	/* FIXME: never clear GMBUS_HW_WAIT_PHASE */
	__vreg(vgt, PCH_GMBUS2) &= ~ GMBUS_ACTIVE;
	__vreg(vgt, PCH_GMBUS2) |= GMBUS_HW_RDY | GMBUS_HW_WAIT_PHASE;

	if (dpy_has_monitor_on_port(vgt, port) && !dpy_port_is_dp(vgt, port)) {
		vgt->vgt_i2c_edid.port = port;
		vgt->vgt_i2c_edid.edid_available = true;
		__vreg(vgt, PCH_GMBUS2) &= ~GMBUS_SATOER;
	} else {
		__vreg(vgt, PCH_GMBUS2) |= GMBUS_SATOER;
	}

	memcpy(p_data, (char *)vgt->state.vReg + offset, bytes);
	return true;
}

/* TODO: */
void vgt_reset_gmbus_controller(struct vgt_device *vgt)
{
	/* TODO: clear gmbus0 ? */
	//__vreg(vgt, PCH_GMBUS0) = 0;
	//__vreg(vgt, PCH_GMBUS1) = 0;
	__vreg(vgt, PCH_GMBUS2) = GMBUS_HW_RDY;
	if (!vgt->vgt_i2c_edid.edid_available) {
		__vreg(vgt, PCH_GMBUS2) |= GMBUS_SATOER;
	}
	//__vreg(vgt, PCH_GMBUS3) = 0;
	//__vreg(vgt, PCH_GMBUS4) = 0;
	//__vreg(vgt, PCH_GMBUS5) = 0;
	vgt->vgt_i2c_edid.gmbus.phase = GMBUS_IDLE_PHASE;
}


static bool vgt_gmbus1_mmio_write(struct vgt_device *vgt, unsigned int offset,
void *p_data, unsigned int bytes)
{
	u32 slave_addr;
	struct vgt_i2c_edid_t *i2c_edid = &vgt->vgt_i2c_edid;

	vgt_reg_t wvalue = *(vgt_reg_t *)p_data;
	if (__vreg(vgt, offset) & GMBUS_SW_CLR_INT) {
		if (!(wvalue & GMBUS_SW_CLR_INT)) {
			__vreg(vgt, offset) &= ~GMBUS_SW_CLR_INT;
			vgt_reset_gmbus_controller(vgt);
		}
		/* TODO: "This bit is cleared to zero when an event
		 * causes the HW_RDY bit transition to occur "*/
	} else {
		/* per bspec setting this bit can cause:
		 1) INT status bit cleared
		 2) HW_RDY bit asserted
		 */
		if (wvalue & GMBUS_SW_CLR_INT) {
			__vreg(vgt, PCH_GMBUS2) &= ~GMBUS_INT;
			__vreg(vgt, PCH_GMBUS2) |= GMBUS_HW_RDY;
		}

		/* For virtualization, we suppose that HW is always ready,
		 * so GMBUS_SW_RDY should always be cleared
		 */
		if (wvalue & GMBUS_SW_RDY)
			wvalue &= ~GMBUS_SW_RDY;

		i2c_edid->gmbus.total_byte_count =
			gmbus1_total_byte_count(wvalue);
		slave_addr = gmbus1_slave_addr(wvalue);

		/* vgt gmbus only support EDID */
		if (slave_addr == EDID_ADDR) {
			i2c_edid->slave_selected = true;
		} else if (slave_addr != 0) {
			vgt_dbg(VGT_DBG_DPY,
				"vGT(%d): unsupported gmbus slave addr(0x%x)\n"
				"	gmbus operations will be ignored.\n",
					vgt->vgt_id, slave_addr);
		}

		if (wvalue & GMBUS_CYCLE_INDEX) {
			i2c_edid->current_edid_read = gmbus1_slave_index(wvalue);
		}

		i2c_edid->gmbus.cycle_type = gmbus1_bus_cycle(wvalue);
		switch (gmbus1_bus_cycle(wvalue)) {
			case GMBUS_NOCYCLE:
				break;
			case GMBUS_STOP:
				/* From spec:
				This can only cause a STOP to be generated
				if a GMBUS cycle is generated, the GMBUS is
				currently in a data/wait/idle phase, or it is in a
				WAIT phase
				 */
				if (gmbus1_bus_cycle(__vreg(vgt, offset)) != GMBUS_NOCYCLE) {
					vgt_init_i2c_edid(vgt);
					/* After the 'stop' cycle, hw state would become
					 * 'stop phase' and then 'idle phase' after a few
					 * milliseconds. In emulation, we just set it as
					 * 'idle phase' ('stop phase' is not
					 * visible in gmbus interface)
					 */
					i2c_edid->gmbus.phase = GMBUS_IDLE_PHASE;
					/*
					FIXME: never clear GMBUS_WAIT
					__vreg(vgt, PCH_GMBUS2) &=
						~(GMBUS_ACTIVE | GMBUS_HW_WAIT_PHASE);
					*/
					__vreg(vgt, PCH_GMBUS2) &= ~GMBUS_ACTIVE;
				}
				break;
			case NIDX_NS_W:
			case IDX_NS_W:
			case NIDX_STOP:
			case IDX_STOP:
				/* From hw spec the GMBUS phase
				 * transition like this:
				 * START (-->INDEX) -->DATA
				 */
				i2c_edid->gmbus.phase = GMBUS_DATA_PHASE;
				__vreg(vgt, PCH_GMBUS2) |= GMBUS_ACTIVE;
				/* FIXME: never clear GMBUS_WAIT */
				//__vreg(vgt, PCH_GMBUS2) &= ~GMBUS_HW_WAIT_PHASE;
				break;
			default:
				vgt_err("Unknown/reserved GMBUS cycle detected!");
				break;
		}
		/* From hw spec the WAIT state will be
		 * cleared:
		 * (1) in a new GMBUS cycle
		 * (2) by generating a stop
		 */
		/* FIXME: never clear GMBUS_WAIT
		if (gmbus1_bus_cycle(wvalue) != GMBUS_NOCYCLE)
			__vreg(vgt, PCH_GMBUS2) &= ~GMBUS_HW_WAIT_PHASE;
		*/

		__vreg(vgt, offset) = wvalue;
	}
	return true;
}

bool vgt_gmbus3_mmio_write(struct vgt_device *vgt, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	vgt_err("vGT(%d): VM can not request gmbus3 mmio write\n", vgt->vgt_id);
	return false;
}

bool vgt_gmbus3_mmio_read(struct vgt_device *vgt, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	int i;
	unsigned char byte_data;
	struct vgt_i2c_edid_t *i2c_edid = &vgt->vgt_i2c_edid;
	int byte_left = i2c_edid->gmbus.total_byte_count -
				i2c_edid->current_edid_read;
	int byte_count = byte_left;
	vgt_reg_t reg_data = 0;

	/* Data can only be recevied if previous settings correct */
	if (__vreg(vgt, PCH_GMBUS1) & GMBUS_SLAVE_READ) {
		if (byte_left <= 0) {
			memcpy((char *)p_data, (char *)vgt->state.vReg + offset, bytes);
			return true;
		}

		if (byte_count > 4)
			byte_count = 4;
		for (i = 0; i< byte_count; i++) {
			byte_data = edid_get_byte(vgt);
			reg_data |= (byte_data << (i << 3));
		}

		memcpy((char *)p_data, (char *)&reg_data, byte_count);
		memcpy((char *)vgt->state.vReg + offset, (char *)&reg_data, byte_count);

		if (byte_left <= 4) {
			switch (i2c_edid->gmbus.cycle_type) {
				case NIDX_STOP:
				case IDX_STOP:
					i2c_edid->gmbus.phase = GMBUS_IDLE_PHASE;
					break;
				case NIDX_NS_W:
				case IDX_NS_W:
				default:
					i2c_edid->gmbus.phase = GMBUS_WAIT_PHASE;
					break;
			}
			//if (i2c_bus->gmbus.phase == GMBUS_WAIT_PHASE)
			//__vreg(vgt, PCH_GMBUS2) |= GMBUS_HW_WAIT_PHASE;

			vgt_init_i2c_edid(vgt);
		}

		/* Read GMBUS3 during send operation, return the latest written value */
	} else {
		memcpy((char *)p_data, (char *)vgt->state.vReg + offset, bytes);
		printk("vGT(%d): warning: gmbus3 read with nothing retuned\n",
				vgt->vgt_id);
	}

	return true;
}

static bool vgt_gmbus2_mmio_read(struct vgt_device *vgt, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	vgt_reg_t value = __vreg(vgt, offset);
	if (!(__vreg(vgt, offset) & GMBUS_INUSE)) {
		__vreg(vgt, offset) |= GMBUS_INUSE;
	}

	memcpy(p_data, (void *)&value, bytes);
	return true;
}

bool vgt_gmbus2_mmio_write(struct vgt_device *vgt, unsigned int offset,
		void *p_data, unsigned int bytes)
{
	vgt_reg_t wvalue = *(vgt_reg_t *)p_data;
	if (wvalue & GMBUS_INUSE)
		__vreg(vgt, offset) &= ~GMBUS_INUSE;
	/* All other bits are read-only */
	return true;
}

bool vgt_i2c_handle_gmbus_read(struct vgt_device *vgt, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	if (bytes > 8 || (offset & (bytes - 1))) {
		vgt_err("vGT(%d) %s: invalid offset(%x) or bytes(%d)\n",
				vgt->vgt_id, __func__, offset, bytes);
		return false;
	}

	switch (offset) {
		case PCH_GMBUS2:
			return vgt_gmbus2_mmio_read(vgt, offset, p_data, bytes);
		case PCH_GMBUS3:
			return vgt_gmbus3_mmio_read(vgt, offset, p_data, bytes);
		default:
			memcpy(p_data, (char *)vgt->state.vReg + offset, bytes);
	}
	return true;
}

bool vgt_i2c_handle_gmbus_write(struct vgt_device *vgt, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	if (bytes > 8 || (offset & (bytes - 1))) {
		vgt_err("vGT(%d) %s: invalid offset(%x) or bytes(%d)\n",
				vgt->vgt_id, __func__, offset, bytes);
		return false;
	}

	switch (offset) {
		case PCH_GMBUS0:
			return vgt_gmbus0_mmio_write(vgt, offset, p_data, bytes);
		case PCH_GMBUS1:
			return vgt_gmbus1_mmio_write(vgt, offset, p_data, bytes);
		case PCH_GMBUS2:
			return vgt_gmbus2_mmio_write(vgt, offset, p_data, bytes);
		/* TODO: */
		case PCH_GMBUS3:
			BUG();
			return false;
		default:
			memcpy((char *)vgt->state.vReg + offset, p_data, bytes);
	}
	return true;
}


/**************************************************************************
 *
 * Aux CH interface for I2C access
 *
 *************************************************************************/

/* vgt_get_aux_ch_reg()
 *
 * return the AUX_CH register according to its lower 8 bits of the address
 */
static inline AUX_CH_REGISTERS vgt_get_aux_ch_reg(unsigned int offset)
{
	AUX_CH_REGISTERS reg;
	switch (offset & 0xff) {
	case 0x10:
		reg = AUX_CH_CTL;
		break;
	case 0x14:
		reg = AUX_CH_DATA1;
		break;
	case 0x18:
		reg = AUX_CH_DATA2;
		break;
	case 0x1c:
		reg = AUX_CH_DATA3;
		break;
	case 0x20:
		reg = AUX_CH_DATA4;
		break;
	case 0x24:
		reg = AUX_CH_DATA5;
		break;
	default:
		reg = AUX_CH_INV;
		break;
	}
	return reg;
}

#define AUX_CTL_MSG_LENGTH(reg) \
	((reg & _DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) >> \
		_DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT)

bool vgt_i2c_handle_aux_ch_write(struct vgt_device *vgt,
				enum port port_idx,
				unsigned int offset,
				void *p_data)
{
	struct vgt_i2c_edid_t *i2c_edid = &vgt->vgt_i2c_edid;
	int msg_length, ret_msg_size;
	int msg, addr, ctrl, op;
	int value = *(int *)p_data;
	int aux_data_for_write = 0;
	AUX_CH_REGISTERS reg = vgt_get_aux_ch_reg(offset);

	if (reg != AUX_CH_CTL) {
		__vreg(vgt, offset) = value;
		return true;
	}

	msg_length = AUX_CTL_MSG_LENGTH(value);
	// check the msg in DATA register.
	msg = __vreg(vgt, offset + 4);
	addr = (msg >> 8) & 0xffff;
	ctrl = (msg >> 24)& 0xff;
	op = ctrl >> 4;
	if (!(value & _REGBIT_DP_AUX_CH_CTL_SEND_BUSY)) {
		/* The ctl write to clear some states */
		return true;
	}

	/* Always set the wanted value for vms. */
	ret_msg_size = (((op & 0x1) == VGT_AUX_I2C_READ) ? 2 : 1);
	__vreg(vgt, offset) =
		_REGBIT_DP_AUX_CH_CTL_DONE |
		((ret_msg_size << _DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT) &
		_DP_AUX_CH_CTL_MESSAGE_SIZE_MASK);

	if (msg_length == 3) {
		if (!(op & VGT_AUX_I2C_MOT)) {
			/* stop */
			vgt_dbg(VGT_DBG_EDID,
				"AUX_CH: stop. reset I2C!\n");
			vgt_init_i2c_edid(vgt);
		} else {
			/* start or restart */
			vgt_dbg(VGT_DBG_EDID,
				"AUX_CH: start or restart I2C!\n");
			i2c_edid->aux_ch.i2c_over_aux_ch = true;
			i2c_edid->aux_ch.aux_ch_mot = true;
			if (addr == 0) {
				/* reset the address */
				vgt_dbg(VGT_DBG_EDID,
					"AUX_CH: reset I2C!\n");
				vgt_init_i2c_edid(vgt);
			} else if (addr == EDID_ADDR) {
				vgt_dbg(VGT_DBG_EDID,
					"AUX_CH: setting EDID_ADDR!\n");
				i2c_edid->state = I2C_AUX_CH;
				i2c_edid->port = port_idx;
				i2c_edid->slave_selected = true;
				if (dpy_has_monitor_on_port(vgt, port_idx) &&
					dpy_port_is_dp(vgt, port_idx))
					i2c_edid->edid_available = true;
			} else {
				vgt_dbg(VGT_DBG_EDID,
		"Not supported address access [0x%x]with I2C over AUX_CH!\n",
				addr);
			}
		}
	} else if ((op & 0x1) == VGT_AUX_I2C_WRITE) {
		/* TODO
		 * We only support EDID reading from I2C_over_AUX. And
		 * we do not expect the index mode to be used. Right now
		 * the WRITE operation is ignored. It is good enough to
		 * support the gfx driver to do EDID access.
		 */
	} else {
		if (((op & 0x1) != VGT_AUX_I2C_READ) || msg_length != 4) {
			vgt_err("vGT(%d) %s: Error operation(%d), or error message length(%d)\n",
				vgt->vgt_id, __func__, op, msg_length);
			return false;
		}
		if (i2c_edid->edid_available && i2c_edid->slave_selected) {
			unsigned char val = edid_get_byte(vgt);
			aux_data_for_write = (val << 16);
		} else
			aux_data_for_write = (0xff << 16);
	}

	/* write the return value in AUX_CH_DATA reg which includes:
	 * ACK of I2C_WRITE
	 * returned byte if it is READ
	 */
	aux_data_for_write |= (VGT_AUX_I2C_REPLY_ACK & 0xff) << 24;
	__vreg(vgt, offset + 4) = aux_data_for_write;

	return true;
}

void vgt_init_i2c_edid(struct vgt_device *vgt)
{
	struct vgt_i2c_edid_t *edid = &vgt->vgt_i2c_edid;

	edid->state = I2C_NOT_SPECIFIED;

	edid->port = I915_MAX_PORTS;
	edid->slave_selected = false;
	edid->edid_available = false;
	edid->current_edid_read = 0;
	
	memset(&edid->gmbus, 0, sizeof(struct vgt_i2c_gmbus_t));

	edid->aux_ch.i2c_over_aux_ch = false;
	edid->aux_ch.aux_ch_mot = false;
}

bool vgt_init_default_monitor(struct vgt_device *vgt)
{
	int i = 0;
	enum port port = 0;

	if (vgt->vm_id == 0)
		return true;

	for (i = 0; i < preallocated_monitor_to_guest; i++) {

		port = PORT_B + i;

		if (port >= I915_MAX_PORTS) {
			vgt_err("init monitor: port exceeds the maximum!\n");
			break;
		}

		vgt->ports[port].type = VGT_DP_B + i;

		if (vgt->ports[port].edid == NULL)
			vgt->ports[port].edid = kmalloc(
				sizeof(struct vgt_edid_data_t),	GFP_ATOMIC);

		if (vgt->ports[port].edid == NULL) {
			vgt_err("Memory allocation fail for EDID block!\n");
			return false;
		}

		memcpy(vgt->ports[port].edid->edid_block,
			edid_default_data, EDID_SIZE);
		vgt->ports[port].edid->data_valid = true;

		if (vgt->ports[port].dpcd == NULL)
			vgt->ports[port].dpcd = kmalloc(
				sizeof(struct vgt_dpcd_data), GFP_ATOMIC);

		if (vgt->ports[port].dpcd == NULL) {
			vgt_err("Memory allocation fail for dpcd block!\n");
			return false;
		}

		memset(vgt->ports[port].dpcd->data, 0, DPCD_SIZE);
		memcpy(vgt->ports[port].dpcd->data,
			dpcd_fix_data, DPCD_HEADER_SIZE);
		vgt->ports[port].dpcd->data_valid = true;
	}

	vgt_check_and_fix_port_mapping(vgt);
	vgt_update_monitor_status(vgt);

	return true;
}
