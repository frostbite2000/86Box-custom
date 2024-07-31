/*
 * 86Box     A hypervisor and IBM PC system emulator that specializes in
 *           running old operating systems and software designed for IBM
 *           PC systems and compatibles from 1981 through fairly recent
 *           system designs based on the PCI bus.
 *
 *           This file is part of the 86Box distribution.
 *
 *           Roland MPU-401 emulation.
 *
 *
 *
 * Authors:  DOSBox Team,
 *           Miran Grca, <mgrca8@gmail.com>
 *           TheCollector1995, <mariogplayer@gmail.com>
 *
 *           Copyright 2008-2020 DOSBox Team.
 *           Copyright 2016-2020 Miran Grca.
 *           Copyright 2016-2020 TheCollector1995.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/machine.h>
#include <86box/mca.h>
#include <86box/midi.h>
#include <86box/pic.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/snd_mpu401.h>
#include <86box/sound.h>
#include <86box/plat_unused.h>

static uint32_t MPUClockBase[8] = { 48, 72, 96, 120, 144, 168, 192 };
static uint8_t cth_data[16] = { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0 };

enum {
    STATUS_OUTPUT_NOT_READY = 0x40,
    STATUS_INPUT_NOT_READY  = 0x80
};

int mpu401_standalone_enable = 0;

static void MPU401_WriteCommand(mpu_t *mpu, uint8_t val);
static void MPU401_WriteData(mpu_t *mpu, uint8_t val);
static void MPU401_IntelligentOut(mpu_t *mpu, uint8_t track);
static void MPU401_EOIHandler(void *priv);
static void MPU401_EOIHandlerDispatch(void *priv);
static __inline void MPU401_NotesOff(mpu_t *mpu, unsigned int i);

static void MPU401_Reset(mpu_t *mpu);

#ifdef ENABLE_MPU401_LOG
int mpu401_do_log = ENABLE_MPU401_LOG;

static void
mpu401_log(const char *fmt, ...)
{
    va_list ap;

    if (mpu401_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define mpu401_log(fmt, ...)
#endif

static void
MPU401_UpdateIRQ(mpu_t *mpu, int set)
{
    /* Clear IRQ. */
    if (mpu->ext_irq_update)
        mpu->ext_irq_update(mpu->priv, 1);
    else {
        if (set)
            picint(1 << mpu->irq);
        else
            picintc(1 << mpu->irq);
    }
}

static int
MPU401_IRQPending(mpu_t *mpu)
{
    int irq_pending;

    if (mpu->ext_irq_pending)
        irq_pending = mpu->ext_irq_pending(mpu->priv);
    else
        irq_pending = mpu->state.irq_pending;

    return irq_pending;
}

static uint8_t
clamp(uint8_t val, uint8_t min, uint8_t max)
{
    if (val < min)
        val = min;
    if (val > max)
        val = max;
    return val;
}

static __inline void
MPU401_StartClock(mpu_t *mpu)
{
    if (!mpu->state.playing && !mpu->state.clock_to_host && (mpu->state.rec != M_RECON) && !mpu->clock.active) {
        mpu->clock.active = 1;
        timer_set_delay_u64(&mpu->mpu401_event_callback, (MPU401_TIMECONSTANT / (mpu->clock.tempo * mpu->clock.timebase)) * 1000 * TIMER_USEC);
    }
}

static __inline void
MPU401_StopClock(mpu_t *mpu)
{
    if (mpu->state.playing && !mpu->state.clock_to_host && (mpu->state.rec == M_RECON)) {
        mpu->clock.active = 0;
        timer_disable(&mpu->mpu401_event_callback);
    }
}

static void
MPU401_QueueByte(mpu_t *mpu, uint8_t data)
{
    if (mpu->state.block_ack) {
        mpu->state.block_ack = 0;
        return;
    }
    if (!mpu->queue_used && mpu->intelligent) {
        mpu->state.irq_pending = 1;
        MPU401_UpdateIRQ(mpu, 1);
    }
    if (mpu->queue_used < MPU401_QUEUE) {
        if (mpu->queue_pos > MPU401_QUEUE)  fatal("MPU queue overflow\n");
        uint8_t pos = mpu->queue_used + mpu->queue_pos;
        pos -= (pos >= MPU401_QUEUE) ? MPU401_QUEUE : 0;
        mpu->queue_pos -= (mpu->queue_pos >= MPU401_QUEUE) ? MPU401_QUEUE : 0;
        mpu->queue_used++;
        if (pos >= MPU401_QUEUE)  fatal("MPU position overflow\n");
        mpu->queue[pos] = data;
    } else
        mpu401_log("MPU401:Data queue full\n");
}

static void
MPU401_RecQueueBuffer(mpu_t *mpu, uint8_t *buf, unsigned int len)
{
	unsigned int cnt = 0;

	while (cnt<len) {
		if (mpu->rec_queue_used < MPU401_INPUT_QUEUE) {
			unsigned int pos = mpu->rec_queue_used + mpu->rec_queue_pos;
			if (pos >= MPU401_INPUT_QUEUE)
			    pos -= MPU401_INPUT_QUEUE;
			mpu->rec_queue[pos] = buf[cnt];
			mpu->rec_queue_used++;
			if (!mpu->state.sysex_in_finished && (buf[cnt] == MSG_EOX)) { /*finish sysex*/
				mpu->state.sysex_in_finished = 1;
				break;
			}
			cnt++;
		}
	}
	if (!mpu->queue_used) {
		if (mpu->state.rec_copy || MPU401_IRQPending(mpu))
			return;

		mpu->state.rec_copy = 1;
		if (mpu->rec_queue_pos >= MPU401_INPUT_QUEUE)
            mpu->rec_queue_pos -= MPU401_INPUT_QUEUE;

		MPU401_QueueByte(mpu, mpu->rec_queue[mpu->rec_queue_pos]);
		mpu->rec_queue_used--;
		mpu->rec_queue_pos++;
	}
}

static void
MPU401_ClrQueue(mpu_t *mpu)
{
    mpu->queue_used = 0;
    mpu->queue_pos  = 0;
	mpu->rec_queue_used = 0;
	mpu->rec_queue_pos = 0;
	mpu->state.sysex_in_finished = 1;
}

static uint8_t
MPU401_ReadStatus(mpu_t *mpu)
{
    uint8_t ret = 0x3f;
    if (mpu->state.cmd_pending)
        ret |= STATUS_OUTPUT_NOT_READY;
    if (!mpu->queue_used)
        ret |= STATUS_INPUT_NOT_READY;
    return ret;
}

static void
MPU401_WriteCommand(mpu_t *mpu, uint8_t val)
{
    int send_prchg;

    /* The only command recognized in UART mode is 0xFF: Reset and return to Intelligent mode. */
    if (mpu->mode == M_UART && (val != 0xff))
        return;

    if (mpu->state.reset) {
        if (mpu->state.cmd_pending || (val != 0xff)) {
            mpu->state.cmd_pending = val + 1;
            return;
        }
        timer_disable(&mpu->mpu401_reset_callback);
        mpu->state.reset = 0;
    }

	/*hack:enable midi through after the first mpu401 command is written*/
	mpu->midi_thru = 1;

    if (val <= 0x2f) {
        send_prchg = 0;
        /* MIDI stop, start, continue */
        switch (val & 3) {
            case 1:
                mpu->state.last_rtcmd = 0xfc;
                if (mpu->filter.rt_out)
                    midi_raw_out_rt_byte(0xfc);
                else
                    midi_raw_out_byte(0xfc);

                mpu->clock.meas_old = mpu->clock.measure_counter;
                mpu->clock.cth_old = mpu->clock.cth_counter;
                break;
            case 2:
                mpu->state.last_rtcmd = 0xfa;
                if (mpu->filter.rt_out)
                    midi_raw_out_rt_byte(0xfb);
                else
                    midi_raw_out_byte(0xfa);

                mpu->clock.measure_counter = mpu->clock.meas_old = 0;
                mpu->clock.cth_counter = mpu->clock.cth_old = 0;
                break;
            case 3:
                mpu->state.last_rtcmd = 0xfc;
                if (mpu->filter.rt_out)
                    midi_raw_out_rt_byte(0xfa);
                else
                    midi_raw_out_byte(0xfb);

                mpu->clock.measure_counter = mpu->clock.meas_old;
                mpu->clock.cth_counter = mpu->clock.cth_old;
                break;
        }

        switch (val & 0x30) { /* Recording */
            case 0: /*check if it waited for MIDI RT command*/
                if (((val & 3) < 2) || !mpu->filter.rt_affection || (mpu->state.rec != M_RECSTB))
                    break;

                MPU401_StartClock(mpu);
                mpu->state.rec = M_RECON;
                if (mpu->filter.prchg_mask)
                    send_prchg = 1;
                break;
            case 0x10:  /* Stop */
                MPU401_StopClock(mpu);
                mpu->state.rec = M_RECOFF;
                MPU401_QueueByte(mpu, MSG_MPU_ACK);
                MPU401_QueueByte(mpu, mpu->clock.rec_counter);
                MPU401_QueueByte(mpu, MSG_MPU_END);
                mpu->filter.prchg_mask = 0;
                mpu->clock.rec_counter = 0;
                return;
            case 0x20:  /* Start */
                pclog("MPU-401: Intelligent mode recording\n");
                if (mpu->state.rec != M_RECON) {
                    mpu->clock.rec_counter = 0;
                    mpu->state.rec = M_RECSTB;
                }
                if ((mpu->state.last_rtcmd == 0xfa) || (mpu->state.last_rtcmd == 0xfb)) {
                    mpu->clock.rec_counter = 0;
                    MPU401_StartClock(mpu);
                    mpu->state.rec = M_RECON;
                    if (mpu->filter.prchg_mask)
                        send_prchg = 1;
                }
                break;
            default:
                break;
        }

        switch (val & 0xc) { /* Playing */
            case 0x4:        /* Stop */
                MPU401_StopClock(mpu);
                mpu->state.playing = 0;
                for (uint8_t i = 0; i < 16; i++)
                    MPU401_NotesOff(mpu, i);

                mpu->filter.prchg_mask =0;
                break;
            case 0x8: /* Start */
                pclog("MPU-401: Intelligent mode playback started\n");
                MPU401_StartClock(mpu);
                mpu->state.playing = 1;
                MPU401_ClrQueue(mpu);
                break;

            default:
                break;
        }
        MPU401_QueueByte(mpu, MSG_MPU_ACK);
		/*record counter hack: needed by Prism, but sent only on cmd 0x20/0x26 (or breaks Ballade)*/
		uint8_t rec_cnt = mpu->clock.rec_counter;
		if (((val==0x20) || (val==0x26)) && (mpu->state.rec == M_RECON))
			MPU401_RecQueueBuffer(mpu, &rec_cnt, 1);

		if (send_prchg) {
            for (uint8_t i = 0; i < 16; i++) {
                if (mpu->filter.prchg_mask&(1<<i)) {
                    uint8_t recmsg[3] = { mpu->clock.rec_counter, 0xc0 | i, mpu->filter.prchg_buf[i] };
                    MPU401_RecQueueBuffer(mpu, recmsg, 3);
                    mpu->filter.prchg_mask &= ~(1 << i);
                }
            }
		}
        return;
    } else if ((val >= 0xa0) && (val <= 0xa7)) { /* Request play counter */
        if (mpu->state.cmask & (1 << (val & 7)))
            MPU401_QueueByte(mpu, mpu->playbuf[val & 7].counter);
    } else if ((val >= 0xd0) && (val <= 0xd7)) { /* Send data */
        mpu->state.old_track = mpu->state.track;
        mpu->state.track     = val & 7;
        mpu->state.wsd       = 1;
        mpu->state.wsm       = 0;
        mpu->state.wsd_start = 1;
    } else if ((val < 0x80) && (val >= 0x40)) { /* Set reference table channel */
		mpu->chanref[(val >> 4) - 4].on = 1;
		mpu->chanref[(val >> 4) - 4].chan = val & 0x0f;
		mpu->chanref[(val >> 4) - 4].trmask = 0;
		for (uint8_t i = 0; i < 4; i++)
            mpu->chanref[(val >> 4) - 4].key[i] = 0;

		for (uint8_t i = 0; i < 16; i++) {
            if (mpu->ch_toref[i] == ((val >> 4) - 4))
                mpu->ch_toref[i] = 4;
		}
		mpu->ch_toref[val&0x0f]=(val>>4)-4;
    } else
        switch (val) {
            case 0x30: /* Configuration 0x30 - 0x39 */
                mpu->filter.allnotesoff_out = 0;
                break;
            case 0x32:
                mpu->filter.rt_out = 0;
                break;
            case 0x33:
                mpu->filter.all_thru = 0;
                mpu->filter.commonmsgs_thru = 0;
                mpu->filter.midi_thru = 0;
                for (uint8_t i = 0; i < 16; i++) {
                    mpu->inputref[i].on = 0;
                    for (uint8_t j = 0; j < 4; j++)
                        mpu->inputref[i].key[j] = 0;
                }
                break;
            case 0x34:
                mpu->filter.timing_in_stop = 1;
                break;
            case 0x35:
                mpu->filter.modemsgs_in = 1;
                break;
            case 0x37:
                mpu->filter.sysex_thru = 1;
                break;
            case 0x38:
                mpu->filter.commonmsgs_in = 1;
                break;
            case 0x39:
                mpu->filter.rt_in = 1;
                break;

            case 0x80:  /* Internal clock */
                if (mpu->clock.active)
                    timer_set_delay_u64(&mpu->mpu401_event_callback, (MPU401_TIMECONSTANT / (mpu->clock.tempo * mpu->clock.timebase)) * 1000 * TIMER_USEC);
                break;

            case 0x82:  /* Sync to MIDI */
                break;

            case 0x86: /* Bender */
            case 0x87:
                mpu->filter.bender_in = (val & 0x01);
                break;
            case 0x88: /* MIDI through */
            case 0x89:
                mpu->filter.midi_thru = (val & 0x01);
                for (uint8_t i = 0; i < 16; i++) {
                    mpu->inputref[i].on = mpu->filter.midi_thru;
                    if (!(val & 0x01)) {
                        for (uint8_t j = 0; j < 4; j++)
                            mpu->inputref[i].key[j] = 0;
                    }
                }
                break;
            case 0x8a: /* Data in stop */
            case 0x8b:
                mpu->filter.data_in_stop = (val & 0x01);
                break;
            case 0x8c: /* Send measure end */
            case 0x8d:
                mpu->filter.rec_measure_end = (val & 0x01);
                break;

            case 0xdf: /* Send system message */
                mpu->state.wsd       = 0;
                mpu->state.wsm       = 1;
                mpu->state.wsd_start = 1;
                break;

            case 0x8e: /* Conductor */
            case 0x8f:
                mpu->state.cond_set  = (val & 0x01);
                break;

            case 0x90: /* Realtime affection */
            case 0x91:
                mpu->filter.rt_affection = (val & 0x01);
                break;

            case 0x94: /* Clock to host */
                MPU401_StopClock(mpu);
                mpu->state.clock_to_host = 0;
                break;
            case 0x95:
                MPU401_StartClock(mpu);
                mpu->state.clock_to_host = 1;
                break;
            case 0x96: /* Sysex input allow */
            case 0x97:
                mpu->filter.sysex_in = (val & 0x01);
                if (val & 0x01)
                    mpu->filter.sysex_thru = 0;
                break;
            case 0x98:
            case 0x99:
            case 0x9a:
            case 0x9b: /* Reference tables on/off */
            case 0x9c:
            case 0x9d:
            case 0x9e:
            case 0x9f:
                mpu->chanref[(val - 0x98) >> 1].on = (val & 0x01);
                break;
            /* Internal timebase */
            case 0xc2:
            case 0xc3:
            case 0xc4:
            case 0xc5:
            case 0xc6:
            case 0xc7:
            case 0xc8:
                mpu->clock.timebase = MPUClockBase[val - 0xc2];
                break;
            /* Commands with data byte */
            case 0xe0:
            case 0xe1:
            case 0xe2:
            case 0xe4:
            case 0xe6:
            case 0xe7:
            case 0xec:
            case 0xed:
            case 0xee:
            case 0xef:
                mpu->state.command_byte = val;
                break;
            /* Commands 0xa# returning data */
            case 0xab: /* Request and clear recording counter */
                MPU401_QueueByte(mpu, MSG_MPU_ACK);
                MPU401_QueueByte(mpu, 0);
                return;
            case 0xac: /* Request version */
                MPU401_QueueByte(mpu, MSG_MPU_ACK);
                MPU401_QueueByte(mpu, MPU401_VERSION);
                return;
            case 0xad: /* Request revision */
                MPU401_QueueByte(mpu, MSG_MPU_ACK);
                MPU401_QueueByte(mpu, MPU401_REVISION);
                return;
            case 0xaf: /* Request tempo */
                MPU401_QueueByte(mpu, MSG_MPU_ACK);
                MPU401_QueueByte(mpu, mpu->clock.tempo);
                return;
            case 0xb1: /* Reset relative tempo */
                mpu->clock.tempo_rel     = 0x40;
                break;
            case 0xb8: /* Clear play counters */
            case 0xb9: /* Clear play map */
                for (uint8_t i = 0; i < 16; i++)
                    MPU401_NotesOff(mpu, i);

                mpu->state.last_rtcmd = 0;
                for (uint8_t i = 0; i < 8; i++) {
                    mpu->playbuf[i].counter = 0;
                    mpu->playbuf[i].type    = T_OVERFLOW;
                }
                mpu->condbuf.counter   = 0;
                mpu->condbuf.type      = T_OVERFLOW;
                if (mpu->state.conductor != mpu->state.cond_set)
                    mpu->state.cond_req = 0;
                mpu->state.amask       = mpu->state.tmask;
                mpu->state.conductor = mpu->state.cond_set;
                mpu->clock.cth_counter = mpu->clock.cth_old = 0;
                mpu->clock.measure_counter = mpu->clock.meas_old = 0;
                mpu->state.req_mask    = 0;
                mpu->state.irq_pending = 1;
                break;
            case 0xba: /* Clear record counter */
                mpu->clock.rec_counter = 0;
                break;
            case 0xff: /* Reset MPU-401 */
                pclog("MPU-401:Reset %u\n", val);
                timer_set_delay_u64(&mpu->mpu401_reset_callback, MPU401_RESETBUSY * TIMER_USEC);
                mpu->state.reset = 1;
                if (mpu->mode == M_UART) {
                    MPU401_Reset(mpu);
                    return;
                }
                MPU401_Reset(mpu);
                break;
            case 0x3f: /* UART mode */
                pclog("MPU-401:Set UART mode %u\n", val);
                mpu->mode = M_UART;
                break;
            default:;
                // pclog("MPU-401:Unhandled command %X",val);
       }

    MPU401_QueueByte(mpu, MSG_MPU_ACK);
}

uint8_t
MPU401_ReadData(mpu_t *mpu)
{
    uint8_t ret = MSG_MPU_ACK;

    if (mpu->queue_used) {
        mpu->queue_pos -= (mpu->queue_pos >= MPU401_QUEUE) ? MPU401_QUEUE : 0;
        ret = mpu->queue[mpu->queue_pos];
        mpu->queue_pos++;
        mpu->queue_used--;
    }
    if (!mpu->intelligent)
        return ret;

    /*copy from recording buffer*/
	if (mpu->state.rec_copy && !mpu->rec_queue_used) {
		mpu->state.rec_copy = 0;
		MPU401_EOIHandler(mpu);
		return ret;
	}

    if (!mpu->queue_used)
        MPU401_UpdateIRQ(mpu, 0);

    if ((ret >= 0xf0) && (ret <= 0xf7)) { /* MIDI data request */
        mpu->state.track      = ret & 7;
        mpu->state.data_onoff = 0;
        mpu->state.cond_req   = 0;
    }

    if (ret == MSG_MPU_COMMAND_REQ) {
        mpu->state.data_onoff = 0;
        mpu->state.cond_req   = 1;
        if (mpu->condbuf.type != T_OVERFLOW) {
            mpu->state.block_ack = 1;
            MPU401_WriteCommand(mpu, mpu->condbuf.value[0]);
            if (mpu->state.command_byte)
                MPU401_WriteData(mpu, mpu->condbuf.value[1]);
        }
        mpu->condbuf.type = T_OVERFLOW;
    }

    if ((ret == MSG_MPU_END) || (ret == MSG_MPU_CLOCK) || (ret == MSG_MPU_ACK)) {
        mpu->state.data_onoff = -1;
        MPU401_EOIHandlerDispatch(mpu);
    }

    return ret;
}

static void
MPU401_WriteData(mpu_t *mpu, uint8_t val)
{
    if (mpu->mode == M_UART) {
        midi_raw_out_byte(val);

        if (val == 0xff)
            midi_reset();
        return;
    }

    /* 0xe# command data */
    switch (mpu->state.command_byte) {
        case 0x00:
            break;
        case 0xe0: /* Set tempo */
            mpu->state.command_byte = 0;
            /* Range clamp of true MPU-401 (always between 4 and 250) */
            val = clamp(val, 4, 250);
            mpu->clock.tempo = val;
            return;
        case 0xe1: /* Set relative tempo */
            mpu->state.command_byte  = 0;
            mpu->clock.tempo_rel = val;
            return;
		case 0xe2:	/* Set gradation for relative tempo */
			mpu->clock.tempo_grad = val;
			mpu->state.command_byte = 0;
			return;
		case 0xe4:	/* Set MIDI clocks per metronome tick */
			mpu->state.command_byte = 0;
			mpu->clock.midimetro = val;
			return;
		case 0xe6:	/* Set metronome ticks per measure */
			mpu->state.command_byte = 0;
			mpu->clock.metromeas = val;
            return;
        case 0xe7: /* Set internal clock to host interval */
            mpu->state.command_byte = 0;
			if (!val)
                val = 64;

			for (uint8_t i = 0; i < 4; i++)
                mpu->clock.cth_rate[i] = (val >> 2) + cth_data[((val & 3) << 2) + i];

			mpu->clock.cth_mode = 0;
            return;
        case 0xec: /* Set active track mask */
            mpu->state.command_byte = 0;
            mpu->state.tmask        = val;
            return;
        case 0xed: /* Set play counter mask */
            mpu->state.command_byte = 0;
            mpu->state.cmask        = val;
            return;
        case 0xee: /* Set 1-8 MIDI channel mask */
            mpu->state.command_byte = 0;
            mpu->state.midi_mask &= 0xff00;
            mpu->state.midi_mask |= val;
            return;
        case 0xef: /* Set 9-16 MIDI channel mask */
            mpu->state.command_byte = 0;
            mpu->state.midi_mask &= 0x00ff;
            mpu->state.midi_mask |= ((uint16_t) val) << 8;
            return;
        default:
            mpu->state.command_byte = 0;
            return;
    }
    static unsigned int length, cnt, posd;
    if (mpu->state.wsd && !mpu->state.track_req && !mpu->state.cond_req) { /* Directly send MIDI message */
        if (mpu->state.wsd_start) {
            mpu->state.wsd_start = 0;
            cnt                  = 0;
            switch (val & 0xf0) {
                case 0xc0:
                case 0xd0:
					length = mpu->playbuf[mpu->state.track].length = 2;
					mpu->playbuf[mpu->state.track].type = T_MIDI_NORM;
                    break;
                case 0x80:
                case 0x90:
                case 0xa0:
                case 0xb0:
                case 0xe0:
					length = mpu->playbuf[mpu->state.track].length = 3;
					mpu->playbuf[mpu->state.track].type = T_MIDI_NORM;
                    break;
                case 0xf0:
                    pclog("MPU-401:Illegal WSD byte\n");
                    mpu->state.wsd   = 0;
                    mpu->state.track = mpu->state.old_track;
                    return;
                default: /* MIDI with running status */
                    cnt++;
					length = mpu->playbuf[mpu->state.track].length;
					mpu->playbuf[mpu->state.track].type = T_MIDI_NORM;
            }
        }
        if (cnt < length) {
            mpu->playbuf[mpu->state.track].value[cnt] = val;
            cnt++;
        }
        if (cnt == length) {
            MPU401_IntelligentOut(mpu, mpu->state.track);
            mpu->state.wsd   = 0;
            mpu->state.track = mpu->state.old_track;
        }
        return;
    }
    if (mpu->state.wsm && !mpu->state.track_req && !mpu->state.cond_req) { /* Send system message */
        if (mpu->state.wsd_start) {
            mpu->state.wsd_start = 0;
            cnt                  = 0;
            switch (val) {
                case 0xf2:
                    length = 3;
                    break;
                case 0xf3:
                    length = 2;
                    break;
                case 0xf6:
                    length = 1;
                    break;
                case 0xf0:
                    length = 0;
                    break;
                default:
                    length = 0;
                    mpu->state.wsm = 0;
                    break;
            }
        } else if (val & 0x80) {
            midi_raw_out_byte(MSG_EOX);
            mpu->state.wsm = 0;
            return;
        }
        if (!length || cnt < length) {
            midi_raw_out_byte(val);
            cnt++;
        }
        if (cnt == length)
            mpu->state.wsm = 0;
        return;
    }
    if (mpu->state.cond_req) { /* Command */
        switch (mpu->state.data_onoff) {
            case -1:
                return;
            case 0: /* Timing byte */
                mpu->condbuf.vlength = 0;
                if (val < 0xf0)
                    mpu->state.data_onoff++;
                else {
                    mpu->state.data_onoff = -1;
                    MPU401_EOIHandlerDispatch(mpu);
                    return;
                }
                /* A timing value of 0 means send it now! */
                mpu->state.send_now  = (val == 0);
                mpu->condbuf.counter = val;
                break;
            case 1: /* Command byte #1 */
                mpu->condbuf.type = T_COMMAND;
                if ((val == 0xf8) || (val == 0xf9) || (val == 0xfc))
                    mpu->condbuf.type = T_OVERFLOW;

                mpu->condbuf.value[mpu->condbuf.vlength] = val;
                mpu->condbuf.vlength++;

                if ((val & 0xf0) != 0xe0)
					MPU401_EOIHandlerDispatch(mpu);
                else
                    mpu->state.data_onoff++;
                break;
            case 2: /* Command byte #2 */
                mpu->condbuf.value[mpu->condbuf.vlength] = val;
                mpu->condbuf.vlength++;
                MPU401_EOIHandlerDispatch(mpu);
                break;
            default:
                break;
        }
        return;
    }
    /* Data */
    switch (mpu->state.data_onoff) {
        case -1:
            return;
        case 0: /* Timing byte */
            if (val < 0xf0)
                mpu->state.data_onoff = 1;
            else {
                mpu->state.data_onoff = -1;
                MPU401_EOIHandlerDispatch(mpu);
                mpu->state.track_req = 0;
                return;
            }
            mpu->state.send_now                    = (val == 0);
            mpu->playbuf[mpu->state.track].counter = val;
            break;
        case 1: /* MIDI */
            mpu->playbuf[mpu->state.track].vlength++;
            posd = mpu->playbuf[mpu->state.track].vlength;
            if (posd == 1) {
                switch (val & 0xf0) {
                    case 0xf0: /* System message or mark */
                        mpu->playbuf[mpu->state.track].sys_val = val;
                        if (val > 0xf7) {
                            mpu->playbuf[mpu->state.track].type = T_MARK;
                            if (val == 0xf9)
                                mpu->clock.measure_counter = 0;
                        } else
                            mpu->playbuf[mpu->state.track].type = T_OVERFLOW;

                        length = 1;
                        break;
                    case 0xc0:
                    case 0xd0: /* MIDI Message */
                        mpu->playbuf[mpu->state.track].type            = T_MIDI_NORM;
                        length = mpu->playbuf[mpu->state.track].length = 2;
                        break;
                    case 0x80:
                    case 0x90:
                    case 0xa0:
                    case 0xb0:
                    case 0xe0:
                        mpu->playbuf[mpu->state.track].type            = T_MIDI_NORM;
                        length = mpu->playbuf[mpu->state.track].length = 3;
                        break;
                    default: /* MIDI with running status */
                        posd++;
                        length = mpu->playbuf[mpu->state.track].vlength++;
                        mpu->playbuf[mpu->state.track].type = T_MIDI_NORM;
                        length                              = mpu->playbuf[mpu->state.track].length;
                        break;
                }
            }
            if (!((posd == 1) && (val >= 0xf0)))
                mpu->playbuf[mpu->state.track].value[posd - 1] = val;

            if (posd == length) {
                MPU401_EOIHandlerDispatch(mpu);
                mpu->state.track_req = 0;
            }
            break;
    }
}

static void
MPU401_IntelligentOut(mpu_t *mpu, uint8_t track)
{
	unsigned int chan, chrefnum;
	uint8_t key, msg;
    uint8_t val;
	int send, retrigger;

    switch (mpu->playbuf[track].type) {
        case T_OVERFLOW:
            break;
        case T_MARK:
            val = mpu->playbuf[track].sys_val;
            if (val == 0xfc) {
                midi_raw_out_rt_byte(val);
                mpu->state.amask &= ~(1 << track);
                mpu->state.req_mask &= ~(1 << track);
            }
            break;
        case T_MIDI_NORM:
            chan = mpu->playbuf[track].value[0] & 0xf;
            key = mpu->playbuf[track].value[1] & 0x7f;
            chrefnum = mpu->ch_toref[chan];
            send = 1;
            retrigger = 0;
			switch (msg = mpu->playbuf[track].value[0] & 0xf0) {
				case 0x80: //note off
					if (mpu->inputref[chan].on && (mpu->inputref[chan].M_GETKEY))
                        send = 0;
					if (mpu->chanref[chrefnum].on && !(mpu->chanref[chrefnum].M_GETKEY))
                        send = 0;

					mpu->chanref[chrefnum].M_DELKEY;
					break;
				case 0x90: //note on
					if (mpu->inputref[chan].on && (mpu->inputref[chan].M_GETKEY))
                        retrigger = 1;
					if (mpu->chanref[chrefnum].on && (mpu->chanref[chrefnum].M_GETKEY))
                        retrigger = 1;

					mpu->chanref[chrefnum].M_SETKEY;
					break;
				case 0xb0:
					if (mpu->playbuf[track].value[1] == 123) {/* All notes off */
						MPU401_NotesOff(mpu, mpu->playbuf[track].value[0] & 0xf);
						return;
					}
					break;
                default:
                    break;
			}
			if (retrigger) {
                midi_raw_out_byte(0x80 | chan);
                midi_raw_out_byte(key);
                midi_raw_out_byte(0);
			}
			if (send) {
                for (uint8_t i = 0; i < mpu->playbuf[track].vlength; ++i)
                    midi_raw_out_byte(mpu->playbuf[track].value[i]);
			}
            break;

        default:
            break;
    }
}

static void
UpdateTrack(mpu_t *mpu, uint8_t track)
{
    MPU401_IntelligentOut(mpu, track);
    if (mpu->state.amask & (1 << track)) {
        mpu->playbuf[track].vlength = 0;
        mpu->playbuf[track].type    = T_OVERFLOW;
        mpu->playbuf[track].counter = 0xf0;
        mpu->state.req_mask |= (1 << track);
    } else {
        if ((mpu->state.amask == 0) && !mpu->state.conductor)
            mpu->state.req_mask |= (1 << 12);
    }
}

static void
MPU401_Event(void *priv)
{
    mpu_t  *mpu = (mpu_t *) priv;

    if (mpu->mode == M_UART)
        return;

    uint64_t event_delay = (MPU401_TIMECONSTANT / (mpu->clock.tempo * mpu->clock.timebase)) * 1000 * TIMER_USEC;
    if (mpu->state.irq_pending && mpu->clock.active) {
        timer_advance_u64(&mpu->mpu401_event_callback, event_delay);
        return;
    }

    if (mpu->state.playing) {
        /* Decrease counters. */
        for (uint8_t i = 0; i < 8; i++) {
            if (mpu->state.amask & (1 << i)) {
                if (mpu->playbuf[i].counter)
                    mpu->playbuf[i].counter--;
                if (!mpu->playbuf[i].counter)
                    UpdateTrack(mpu, i);
            }
        }
        if (mpu->state.conductor) {
            if (mpu->condbuf.counter)
                mpu->condbuf.counter--;
            if (!mpu->condbuf.counter) {
                mpu->condbuf.vlength = 0;
                mpu->condbuf.counter = 0xf0;
                mpu->state.req_mask |= (1 << 9);
            }
        }
    }

    if (mpu->state.clock_to_host) {
        mpu->clock.cth_counter++;
        if (mpu->clock.cth_counter >= mpu->clock.cth_rate[mpu->clock.cth_mode]) {
            mpu->clock.cth_counter = 0;
            ++mpu->clock.cth_mode;
            mpu->clock.cth_mode %= 4;
            mpu->state.req_mask |= (1 << 13);
        }
    }

    /*recording*/
	if (mpu->state.rec == M_RECON) {
		mpu->clock.rec_counter++;
		if (mpu->clock.rec_counter >= 240) {
			mpu->clock.rec_counter = 0;
			mpu->state.req_mask |= (1 << 8);
		}
	}

	if (mpu->state.playing || (mpu->state.rec == M_RECON)) {
		int max_meascnt = (mpu->clock.timebase * mpu->clock.midimetro * mpu->clock.metromeas) / 24;
		if (max_meascnt) { //measure end
			if (++mpu->clock.measure_counter >= max_meascnt) {
				if (mpu->filter.rt_out)
                    midi_raw_out_rt_byte(0xf8);

				mpu->clock.measure_counter=0;
				if (mpu->filter.rec_measure_end && (mpu->state.rec == M_RECON))
					mpu->state.req_mask |= (1 << 12);
			}
		}
	}

    if (!mpu->state.irq_pending && mpu->state.req_mask)
        MPU401_EOIHandler(mpu);

    timer_advance_u64(&mpu->mpu401_event_callback, event_delay);
}

static void
MPU401_EOIHandlerDispatch(void *priv)
{
    mpu_t *mpu = (mpu_t *) priv;

    mpu401_log("EOI handler dispatch\n");
    if (mpu->state.send_now) {
        mpu->state.eoi_scheduled = 1;
        timer_set_delay_u64(&mpu->mpu401_eoi_callback, 60LL * TIMER_USEC); /* Possibly a bit longer */
    } else if (!mpu->state.eoi_scheduled)
        MPU401_EOIHandler(mpu);
}

/* Updates counters and requests new data on "End of Input" */
static void
MPU401_EOIHandler(void *priv)
{
    mpu_t  *mpu = (mpu_t *) priv;

    mpu401_log("MPU-401 end of input callback\n");

    mpu->state.eoi_scheduled = 0;
    if (mpu->state.send_now) {
        mpu->state.send_now = 0;
        if (mpu->state.cond_req) {
            mpu->condbuf.counter = 0xf0;
            mpu->state.req_mask |= (1<<9);
        } else
            UpdateTrack(mpu, mpu->state.track);
    }
    if (mpu->state.rec_copy || !mpu->state.sysex_in_finished)
        return;

    mpu->state.irq_pending = 0;
    if (!mpu->state.req_mask)
        return;

    uint8_t i = 0;
    do {
        if (mpu->state.req_mask & (1 << i)) {
            MPU401_QueueByte(mpu, 0xf0 + i);
            mpu->state.req_mask &= ~(1 << i);
            break;
        }
    } while ((i++) < 16);
}

static __inline void
MPU401_NotesOff(mpu_t *mpu, unsigned int i)
{
	if (mpu->filter.allnotesoff_out && !(mpu->inputref[i].on &&
		(mpu->inputref[i].key[0] | mpu->inputref[i].key[1] |
        mpu->inputref[i].key[2] | mpu->inputref[i].key[3]))) {
        for (uint8_t j = 0;j < 4; j++)
            mpu->chanref[mpu->ch_toref[i]].key[j] = 0;

		midi_raw_out_byte(0xb0 | i);
		midi_raw_out_byte(123);
		midi_raw_out_byte(0);
	} else if (mpu->chanref[mpu->ch_toref[i]].on) {
        for (uint8_t key = 0; key < 128; key++) {
            if ((mpu->chanref[mpu->ch_toref[i]].M_GETKEY) &&
                !(mpu->inputref[i].on && (mpu->inputref[i].M_GETKEY))) {
                midi_raw_out_byte(0x80 | i);
                midi_raw_out_byte(key);
                midi_raw_out_byte(0);
            }
            mpu->chanref[mpu->ch_toref[i]].M_DELKEY;
        }
	}
}

static void
MPU401_ResetDone(void *priv)
{
    mpu_t *mpu = (mpu_t *) priv;

    mpu->state.reset = 0;

    if (mpu->state.cmd_pending) {
        MPU401_WriteCommand(mpu, mpu->state.cmd_pending - 1);
        mpu->state.cmd_pending = 0;
    }
}

static void
MPU401_Reset(mpu_t *mpu)
{
    uint8_t i;

    midi_reset();
    /* Clear MIDI buffers, terminate notes. */
    midi_clear_buffer();
    for (i = 0xb0; i <= 0xbf; i++) {
        midi_raw_out_byte(i);
        midi_raw_out_byte(0x7b);
        midi_raw_out_byte(0);
    }
    MPU401_UpdateIRQ(mpu, 0);
    mpu->mode                                       = M_INTELLIGENT;
    mpu->clock.active                               = 0;
    timer_disable(&mpu->mpu401_event_callback);
    timer_disable(&mpu->mpu401_eoi_callback);
    mpu->state.eoi_scheduled                        = 0;
    mpu->state.wsd                                  = 0;
    mpu->state.wsm                                  = 0;
    mpu->state.conductor                            = 0;
    mpu->state.cond_req                             = 0;
    mpu->state.cond_set                             = 0;
    mpu->state.playing                              = 0;
    mpu->state.run_irq                              = 0;
    mpu->state.irq_pending                          = 0;
	mpu->midi_thru                                  = 0;
	mpu->state.rec                                  = M_RECOFF;
    mpu->state.cmask                                = 0xff;
    mpu->state.amask = mpu->state.tmask             = 0;
    mpu->state.midi_mask                            = 0xffff;
    mpu->state.data_onoff                           = -1;
    mpu->state.command_byte                         = 0;
    mpu->state.block_ack                            = 0;
    mpu->clock.tempo = mpu->clock.old_tempo         = 100;
    mpu->clock.timebase = mpu->clock.old_timebase   = 120;
    mpu->clock.tempo_rel = mpu->clock.old_tempo_rel = 0x40;
    mpu->clock.tempo_grad                           = 0;
    mpu->state.clock_to_host                        = 0;
    for (i = 0; i < 4; i++)
        mpu->clock.cth_rate[i]                      = 60;

    mpu->clock.cth_counter                          = 0;
    mpu->clock.cth_old                              = 0;
    MPU401_ClrQueue(mpu);
    mpu->state.req_mask                             = 0;
    mpu->condbuf.counter                            = 0;
    mpu->condbuf.type                               = T_OVERFLOW;
    mpu->clock.midimetro                            = 12;
    mpu->clock.metromeas                            = 8;
	mpu->filter.rec_measure_end                     = 1;
	mpu->filter.rt_out                              = 1;
	mpu->filter.rt_affection                        = 1;
	mpu->filter.allnotesoff_out                     = 1;
	mpu->filter.all_thru                            = 1;
	mpu->filter.midi_thru                           = 1;
	mpu->filter.commonmsgs_thru                     = 1;
    for (i = 0; i < 8; i++) {
        mpu->playbuf[i].type                        = T_OVERFLOW;
        mpu->playbuf[i].counter                     = 0;
    }
    /* Reset channel reference and input tables. */
    for (i = 0; i < 4; i++) {
        mpu->chanref[i].on                          = 1;
        mpu->chanref[i].chan                        = i;
        mpu->ch_toref[i]                            = i;
    }
    for (i = 0; i < 16; i++) {
		mpu->inputref[i].on                         = 1;
		mpu->inputref[i].chan                       = i;
        if (i > 3)
            mpu->ch_toref[i]                        = 4; /* Dummy reftable. */
    }
}

void
mpu401_write(uint16_t addr, uint8_t val, void *priv)
{
    mpu_t *mpu = (mpu_t *) priv;

    /* mpu401_log("MPU401 Write Port %04X, val %x\n", addr, val); */
    switch (addr & 1) {
        case 0: /*Data*/
            MPU401_WriteData(mpu, val);
            mpu401_log("Write Data (0x330) %X\n", val);
            break;

        case 1: /*Command*/
            MPU401_WriteCommand(mpu, val);
            mpu401_log("Write Command (0x331) %x\n", val);
            break;

        default:
            break;
    }
}

uint8_t
mpu401_read(uint16_t addr, void *priv)
{
    mpu_t  *mpu = (mpu_t *) priv;
    uint8_t ret = 0;

    switch (addr & 1) {
        case 0: /* Read Data */
            ret = MPU401_ReadData(mpu);
            mpu401_log("Read Data (0x330) %X\n", ret);
            break;

        case 1: /* Read Status */
            ret = MPU401_ReadStatus(mpu);
            mpu401_log("Read Status (0x331) %x\n", ret);
            break;

        default:
            break;
    }

    /* mpu401_log("MPU401 Read Port %04X, ret %x\n", addr, ret); */
    return ret;
}

/*Input handler for SysEx */
int
MPU401_InputSysex(void *priv, uint8_t *buffer, uint32_t len, int abort)
{
    mpu_t  *mpu    = (mpu_t *) priv;
    uint8_t val_ff = 0xff;

    if (mpu->mode == M_UART) {
        /* UART mode input. */
        for (uint32_t i = 0; i < len; i++)
            MPU401_QueueByte(mpu, buffer[i]);
        return 0;
    }

    if (mpu->filter.sysex_in) {
        if (abort) {
            mpu->state.sysex_in_finished = 1;
            mpu->rec_queue_used          = 0; /*reset also the input queue*/
            return 0;
        }
        if (mpu->state.sysex_in_finished) {
            if (mpu->rec_queue_used >= MPU401_INPUT_QUEUE)
                return len;
            MPU401_RecQueueBuffer(mpu, &val_ff, 1);
            mpu->state.sysex_in_finished = 0;
            mpu->clock.rec_counter       = 0;
        }
        if (mpu->rec_queue_used >= MPU401_INPUT_QUEUE)
            return len;
        int available = MPU401_INPUT_QUEUE - mpu->rec_queue_used;

        if (available >= len) {
            MPU401_RecQueueBuffer(mpu, buffer, len);
            return 0;
        } else {
            MPU401_RecQueueBuffer(mpu, buffer, available);
            if (mpu->state.sysex_in_finished)
                return 0;
            return (len - available);
        }
    } else if (mpu->filter.sysex_thru && mpu->midi_thru) {
        midi_raw_out_byte(0xf0);
        for (uint32_t i = 0; i < len; i++)
            midi_raw_out_byte(*(buffer + i));
    }
    pclog("MPU401:Input Sysex.\n");
    return 0;
}

/*Input handler for MIDI*/
void
MPU401_InputMsg(void *priv, uint8_t *msg, uint32_t len)
{
    mpu_t         *mpu = (mpu_t *) priv;
    static uint8_t old_msg = 0;
    uint8_t        key;
    uint8_t        recdata[2];
    uint8_t        recmsg[4];
    int            send = 1;
    int            send_thru = 0;
    int            retrigger_thru = 0;
    int            chan;
    int            chrefnum;

    /* Abort if sysex transfer is in progress. */
    if (!mpu->state.sysex_in_finished) {
        mpu401_log("SYSEX in progress\n");
        return;
    }

    if (mpu->mode == M_INTELLIGENT) {
        if (msg[0] < 0x80) {
            /* Expand running status */
            msg[2] = msg[1];
            msg[1] = msg[0];
            msg[0] = old_msg;
        }
        old_msg  = msg[0];
        chan     = msg[0] & 0xf;
        chrefnum = mpu->ch_toref[chan];
        key      = msg[1] & 0x7f;
        if (msg[0] < 0xf0) {
            /* If non-system msg. */
            if (!(mpu->state.midi_mask & (1 << chan)) && mpu->filter.all_thru)
                send_thru = 1;
            else if (mpu->filter.midi_thru)
                send_thru = 1;

            switch (msg[0] & 0xf0) {
                case 0x80: /* Note off. */
                    if (send_thru) {
                        if (mpu->chanref[chrefnum].on && (mpu->chanref[chrefnum].M_GETKEY))
                            send_thru = 0;
                        if (!mpu->filter.midi_thru)
                            break;
                        if (!(mpu->inputref[chan].M_GETKEY))
                            send_thru = 0;
                        mpu->inputref[chan].M_DELKEY;
                    }
                    break;
                case 0x90: /* Note on. */
                    if (send_thru) {
                        if (mpu->chanref[chrefnum].on && (mpu->chanref[chrefnum].M_GETKEY))
                            retrigger_thru = 1;
                        if (!mpu->filter.midi_thru)
                            break;
                        if (mpu->inputref[chan].M_GETKEY)
                            retrigger_thru = 1;
                        mpu->inputref[chan].M_SETKEY;
                    }
                    break;
                case 0xb0:
                    if (msg[1] >= 120) {
                        send_thru = 0;
                        if (msg[1] == 123) {
                            /* All notes off. */
                            for (key = 0; key < 128; key++) {
                                if (!(mpu->chanref[chrefnum].on && (mpu->chanref[chrefnum].M_GETKEY))) {
                                    if (mpu->inputref[chan].on && mpu->inputref[chan].M_GETKEY) {
                                        midi_raw_out_byte(0x80 | chan);
                                        midi_raw_out_byte(key);
                                        midi_raw_out_byte(0);
                                    }
                                    mpu->inputref[chan].M_DELKEY;
                                }
                            }
                        }
                    }
                    break;

                default:
                    break;
            }
        }
        if ((msg[0] >= 0xf0) || (mpu->state.midi_mask & (1 << chan))) {
            switch (msg[0] & 0xf0) {
                case 0xa0: /* Aftertouch. */
                    if (!mpu->filter.bender_in)
                        send = 0;
                    break;
                case 0xb0: /* Control change. */
                    if (!mpu->filter.bender_in && (msg[1] < 64))
                        send = 0;
                    if (msg[1] >= 120) {
                        if (mpu->filter.modemsgs_in)
                            send = 1;
                    }
                    break;
                case 0xc0: /* Program change. */
                    if ((mpu->state.rec != M_RECON) && !mpu->filter.data_in_stop) {
                        mpu->filter.prchg_buf[chan] = msg[1];
                        mpu->filter.prchg_mask |= 1 << chan;
                    }
                    break;
                case 0xd0: /* Ch pressure. */
                case 0xe0: /* Pitch wheel. */
                    if (!mpu->filter.bender_in)
                        send = 0;
                    break;
                case 0xf0: /* System message. */
                    if (msg[0] == 0xf8) {
                        send = 0;
                    } else if (msg[0] > 0xf8) { /* Realtime. */
                        if (!(mpu->filter.rt_in && (msg[0] <= 0xfc) && (msg[0] >= 0xfa))) {
                            recdata[0] = 0xff;
                            recdata[1] = msg[0];
                            MPU401_RecQueueBuffer(mpu, recdata, 2);
                            send = 0;
                        }
                    } else { /* Common or system. */
                        send = 0;
                        if ((msg[0] == 0xf2) || (msg[0] == 0xf3) || (msg[0] == 0xf6)) {
                            if (mpu->filter.commonmsgs_in)
                                send = 1;
                            if (mpu->filter.commonmsgs_thru)
                                for (uint32_t i = 0; i < len; i++)
                                    midi_raw_out_byte(msg[i]);
                        }
                    }
                    if (send) {
                        recmsg[0] = 0xff;
                        recmsg[1] = msg[0];
                        recmsg[2] = msg[1];
                        recmsg[3] = msg[2];
                        MPU401_RecQueueBuffer(mpu, recmsg, len + 1);
                    }
                    if (mpu->filter.rt_affection) {
                        switch (msg[0]) {
                            case 0xf2:
                            case 0xf3:
                                mpu->state.block_ack = 1;
                                MPU401_WriteCommand(mpu, 0xb8); /* Clear play counters. */
                                break;
                            case 0xfa:
                                mpu->state.block_ack = 1;
                                MPU401_WriteCommand(mpu, 0xa); /* Start, play. */
                                if (mpu->filter.rt_out)
                                    midi_raw_out_rt_byte(msg[0]);
                                break;
                            case 0xfb:
                                mpu->state.block_ack = 1;
                                MPU401_WriteCommand(mpu, 0xb); /* Continue, play. */
                                if (mpu->filter.rt_out)
                                    midi_raw_out_rt_byte(msg[0]);
                                break;
                            case 0xfc:
                                mpu->state.block_ack = 1;
                                MPU401_WriteCommand(mpu, 0xd); /* Stop: Play, rec, midi */
                                if (mpu->filter.rt_out)
                                    midi_raw_out_rt_byte(msg[0]);
                                break;

                            default:
                                break;
                        }
                        return;
                    }
                    break;

                default:
                    break;
            }
        }
        if (send_thru && mpu->midi_thru) {
            if (retrigger_thru) {
                midi_raw_out_byte(0x80 | (msg[0] & 0xf));
                midi_raw_out_byte(msg[1]);
                midi_raw_out_byte(msg[2]);
            }
            for (uint32_t i = 0; i < len; i++)
                midi_raw_out_byte(msg[i]);
        }
        if (send) {
            if (mpu->state.rec == M_RECON) {
                recmsg[0] = mpu->clock.rec_counter;
                recmsg[1] = msg[0];
                recmsg[2] = msg[1];
                recmsg[3] = msg[2];
                MPU401_RecQueueBuffer(mpu, recmsg, len + 1);
                mpu->clock.rec_counter = 0;
            } else if (mpu->filter.data_in_stop) {
                if (mpu->filter.timing_in_stop) {
                    recmsg[0] = 0;
                    recmsg[1] = msg[0];
                    recmsg[2] = msg[1];
                    recmsg[3] = msg[2];
                    MPU401_RecQueueBuffer(mpu, recmsg, len + 1);
                } else {
                    recmsg[0] = msg[0];
                    recmsg[1] = msg[1];
                    recmsg[2] = msg[2];
                    recmsg[3] = 0;
                    MPU401_RecQueueBuffer(mpu, recmsg, len);
                }
            }
        }
        return;
    }

    /* UART mode input. */
    for (uint32_t i = 0; i < len; i++)
        MPU401_QueueByte(mpu, msg[i]);

    pclog("MPU401:Input Msg.\n");
    return;
}

void
mpu401_setirq(mpu_t *mpu, int irq)
{
    mpu->irq = irq;
}

void
mpu401_change_addr(mpu_t *mpu, uint16_t addr)
{
    if (mpu == NULL)
        return;
    if (mpu->addr)
        io_removehandler(mpu->addr, 2,
                         mpu401_read, NULL, NULL, mpu401_write, NULL, NULL, mpu);
    mpu->addr = addr;
    if (mpu->addr)
        io_sethandler(mpu->addr, 2,
                      mpu401_read, NULL, NULL, mpu401_write, NULL, NULL, mpu);
}

void
mpu401_init(mpu_t *mpu, uint16_t addr, int irq, int mode, int receive_input)
{
    mpu->status     = STATUS_INPUT_NOT_READY;
    mpu->irq        = irq;
    mpu->queue_used = 0;
    mpu->queue_pos  = 0;
    mpu->mode       = M_UART;
    mpu->addr       = addr;

    /* Expalantion:
    MPU-401 starting in intelligent mode = Full MPU-401 intelligent mode capability;
    MPU-401 starting in UART mode = Reduced MPU-401 intelligent mode capability seen on the Sound Blaster 16/AWE32,
                    only supporting commands 3F (set UART mode) and FF (reset). */
    mpu->intelligent = (mode == M_INTELLIGENT) ? 1 : 0;
    mpu401_log("Starting as %s (mode is %s)\n", mpu->intelligent ? "INTELLIGENT" : "UART", (mode == M_INTELLIGENT) ? "INTELLIGENT" : "UART");

    if (mpu->addr)
        io_sethandler(mpu->addr, 2,
                      mpu401_read, NULL, NULL, mpu401_write, NULL, NULL, mpu);
    timer_add(&mpu->mpu401_event_callback, MPU401_Event, mpu, 0);
    timer_add(&mpu->mpu401_eoi_callback, MPU401_EOIHandler, mpu, 0);
    timer_add(&mpu->mpu401_reset_callback, MPU401_ResetDone, mpu, 0);

    MPU401_Reset(mpu);

    if (receive_input)
        midi_in_handler(1, MPU401_InputMsg, MPU401_InputSysex, mpu);
}

void
mpu401_device_add(void)
{
    if (!mpu401_standalone_enable)
        return;

    if (machine_has_bus(machine, MACHINE_BUS_MCA))
        device_add(&mpu401_mca_device);
    else
        device_add(&mpu401_device);
}

static uint8_t
mpu401_mca_read(int port, void *priv)
{
    const mpu_t *mpu = (mpu_t *) priv;

    return mpu->pos_regs[port & 7];
}

static void
mpu401_mca_write(int port, uint8_t val, void *priv)
{
    mpu_t   *mpu = (mpu_t *) priv;
    uint16_t addr;

    if (port < 0x102)
        return;

    addr = (mpu->pos_regs[2] & 2) ? 0x0330 : 0x1330;

    port &= 7;

    mpu->pos_regs[port] = val;

    if (port == 2) {
        io_removehandler(addr, 2,
                         mpu401_read, NULL, NULL, mpu401_write, NULL, NULL, mpu);

        addr = (mpu->pos_regs[2] & 2) ? 0x1330 : 0x0330;

        io_sethandler(addr, 2,
                      mpu401_read, NULL, NULL, mpu401_write, NULL, NULL, mpu);
    }
}

static uint8_t
mpu401_mca_feedb(UNUSED(void *priv))
{
    return 1;
}

void
mpu401_irq_attach(mpu_t *mpu, void (*ext_irq_update)(void *priv, int set), int (*ext_irq_pending)(void *priv), void *priv)
{
    mpu->ext_irq_update  = ext_irq_update;
    mpu->ext_irq_pending = ext_irq_pending;
    mpu->priv            = priv;
}

static void *
mpu401_standalone_init(const device_t *info)
{
    mpu_t   *mpu;
    int      irq;
    uint16_t base;

    mpu = malloc(sizeof(mpu_t));
    memset(mpu, 0, sizeof(mpu_t));

    mpu401_log("mpu_init\n");

    if (info->flags & DEVICE_MCA) {
        mca_add(mpu401_mca_read, mpu401_mca_write, mpu401_mca_feedb, NULL, mpu);
        mpu->pos_regs[0] = 0x0F;
        mpu->pos_regs[1] = 0x6C;
        base             = 0; /* Tell mpu401_init() that this is the MCA variant. */
        /* According to @6c0f.adf, the IRQ is supposed to be fixed to 2.
           This is only true for earlier models. Later ones have selectable IRQ. */
        irq = device_get_config_int("irq");
    } else {
        base = device_get_config_hex16("base");
        irq  = device_get_config_int("irq");
        if (irq == 2)
            irq = 9;
    }

    mpu401_init(mpu, base, irq, M_INTELLIGENT, device_get_config_int("receive_input"));

    return mpu;
}

static void
mpu401_standalone_close(void *priv)
{
    mpu_t *mpu = (mpu_t *) priv;

    free(mpu);
}

static const device_config_t mpu401_standalone_config[] = {
  // clang-format off
    {
        .name = "base",
        .description = "MPU-401 Address",
        .type = CONFIG_HEX16,
        .default_string = "",
        .default_int = 0x330,
        .file_filter = "",
        .spinner = { 0 },
        .selection = {
            {
                .description = "0x220",
                .value = 0x220
            },
            {
                .description = "0x230",
                .value = 0x230
            },
            {
                .description = "0x240",
                .value = 0x240
            },
            {
                .description = "0x250",
                .value = 0x250
            },
            {
                .description = "0x300",
                .value = 0x300
            },
            {
                .description = "0x320",
                .value = 0x320
            },
            {
                .description = "0x330",
                .value = 0x330
            },
            {
                .description = "0x332",
                .value = 0x332
            },
            {
                .description = "0x334",
                .value = 0x334
            },
            {
                .description = "0x336",
                .value = 0x336
            },
            {
                .description = "0x340",
                .value = 0x340
            },
            {
                .description = "0x350",
                .value = 0x350
            },
            { .description = "" }
        }
    },
    {
        .name = "irq",
        .description = "MPU-401 IRQ",
        .type = CONFIG_SELECTION,
        .default_string = "",
        .default_int = 2,
        .file_filter = "",
        .spinner = { 0 },
        .selection = {
            {
                .description = "IRQ 2",
                .value = 2
            },
            {
                .description = "IRQ 3",
                .value = 3
            },
            {
                .description = "IRQ 4",
                .value = 4
            },
            {
                .description = "IRQ 5",
                .value = 5
            },
            {
                .description = "IRQ 6",
                .value = 6
            },
            {
                .description = "IRQ 7",
                .value = 7
            },
            { .description = "" }
        }
    },
    {
        .name = "receive_input",
        .description = "Receive input",
        .type = CONFIG_BINARY,
        .default_int = 1
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

static const device_config_t mpu401_standalone_mca_config[] = {
  // clang-format off
    {
        .name = "irq",
        .description = "MPU-401 IRQ",
        .type = CONFIG_SELECTION,
        .default_string = "",
        .default_int = 9,
        .file_filter = "",
        .spinner = { 0 },
        .selection = {
            {
                .description = "IRQ 3",
                .value = 3
            },
            {
                .description = "IRQ 4",
                .value = 4
            },
            {
                .description = "IRQ 5",
                .value = 5
            },
            {
                .description = "IRQ 6",
                .value = 6
            },
            {
                .description = "IRQ 7",
                .value = 7
            },
            {
                .description = "IRQ 9",
                .value = 9
            },
            { .description = "" }
        }
    },
    {
        .name = "receive_input",
        .description = "Receive input",
        .type = CONFIG_BINARY,
        .default_int = 1
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t mpu401_device = {
    .name          = "Roland MPU-IPC-T",
    .internal_name = "mpu401",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = mpu401_standalone_init,
    .close         = mpu401_standalone_close,
    .reset         = NULL,
    { .available = NULL },
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = mpu401_standalone_config
};

const device_t mpu401_mca_device = {
    .name          = "Roland MPU-IMC",
    .internal_name = "mpu401_mca",
    .flags         = DEVICE_MCA,
    .local         = 0,
    .init          = mpu401_standalone_init,
    .close         = mpu401_standalone_close,
    .reset         = NULL,
    { .available = NULL },
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = mpu401_standalone_mca_config
};
