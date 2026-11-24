#include "gb.h"
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500
#endif
#include <windows.h>
#else
#include <sys/time.h>
#endif

static const unsigned TAC_TRIGGER_BITS[] = {512, 8, 32, 128};

#ifndef GB_DISABLE_TIMEKEEPING
static int64_t get_nanoseconds(void)
{
#ifndef _WIN32
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_usec) * 1000 + now.tv_sec * 1000000000L;
#else
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    return (((int64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) * 100L;
#endif
}

static void nsleep(uint64_t nanoseconds)
{
#ifndef _WIN32
    struct timespec sleep = {0, nanoseconds};
    nanosleep(&sleep, NULL);
#else
    HANDLE timer;
    LARGE_INTEGER time;
    timer = CreateWaitableTimer(NULL, true, NULL);
    time.QuadPart = -(nanoseconds / 100L);
    SetWaitableTimer(timer, &time, 0, NULL, NULL, false);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
#endif
}

bool GB_timing_sync_turbo(GB_gameboy_t *gb)
{
#ifndef GB_DISABLE_DEBUGGER
    if (unlikely(gb->backstep_instructions)) return false;
#endif
    if (!gb->turbo_dont_skip) {
        int64_t nanoseconds = get_nanoseconds();
        if (nanoseconds <= gb->last_render + 1000000000 / 60) {
            return true;
        }
        gb->last_render = nanoseconds;
    }
    return false;
}

void GB_timing_sync(GB_gameboy_t *gb)
{
#ifndef GB_DISABLE_DEBUGGER
    if (unlikely(gb->backstep_instructions)) return;
#endif
    /* Prevent syncing if not enough time has passed.*/
    if (gb->cycles_since_last_sync < LCDC_PERIOD / 3) return;

    unsigned target_clock_rate;
    if (gb->turbo) {
        if (gb->turbo_cap_multiplier) {
            target_clock_rate = GB_get_clock_rate(gb) * gb->turbo_cap_multiplier;
        }
        else {
            gb->cycles_since_last_sync = 0;
            if (gb->update_input_hint_callback) {
                gb->update_input_hint_callback(gb);
            }
            return;
        }
    }
    else {
        target_clock_rate = GB_get_clock_rate(gb);
    }
    
    uint64_t target_nanoseconds = gb->cycles_since_last_sync * 1000000000LL / 2 / target_clock_rate; /* / 2 because we use 8MHz units */
    int64_t nanoseconds = get_nanoseconds();
    int64_t time_to_sleep = target_nanoseconds + gb->last_sync - nanoseconds;
    if (time_to_sleep > 0 && time_to_sleep < LCDC_PERIOD * 1200000000LL / target_clock_rate) { // +20% to be more forgiving
        nsleep(time_to_sleep);
        gb->last_sync += target_nanoseconds;
    }
    else {
        if (time_to_sleep < 0 && -time_to_sleep < LCDC_PERIOD * 1200000000LL / target_clock_rate) {
            // We're running a bit too slow, but the difference is small enough,
            // just skip this sync and let it even out
            return;
        }
        gb->last_sync = nanoseconds;
    }

    gb->cycles_since_last_sync = 0;
    if (gb->update_input_hint_callback) {
        gb->update_input_hint_callback(gb);
    }
}
#else

bool GB_timing_sync_turbo(GB_gameboy_t *gb)
{
    return false;
}

void GB_timing_sync(GB_gameboy_t *gb)
{
#ifndef GB_DISABLE_DEBUGGER
    if (unlikely(gb->backstep_instructions)) return;
#endif
    if (gb->cycles_since_last_sync < LCDC_PERIOD / 3) return;
    gb->cycles_since_last_sync = 0;

    gb->cycles_since_last_sync = 0;
    if (gb->update_input_hint_callback) {
        gb->update_input_hint_callback(gb);
    }
    return;
}

#endif

#define IR_DECAY 23000
#define IR_NOISE 2048
#define IR_WARMUP 32000
#define IR_DECAY_MAX (IR_DECAY + IR_NOISE + IR_DECAY)
#define IR_WARMUP_MAX (IR_WARMUP + IR_NOISE)

static uint16_t ir_noise(GB_gameboy_t *gb)
{
    uint16_t noise = (((9 + gb->div_counter) * 127) ^ ((gb->display_cycles) * 137));
    noise >>= 3;
    noise %= IR_NOISE;
    return noise;
}

static void ir_run(GB_gameboy_t *gb, uint32_t cycles)
{
    /* TODO: the way this thing works makes the CGB IR port behave inaccurately when used together with HUC1/3 IR ports*/
    if ((gb->model > GB_MODEL_CGB_E || !gb->cgb_mode) && gb->cartridge_type->mbc_type != GB_HUC1 && gb->cartridge_type->mbc_type != GB_HUC3) return;
    bool is_sensing = (gb->io_registers[GB_IO_RP] & 0xC0) == 0xC0 ||
                       (gb->cartridge_type->mbc_type == GB_HUC1 && gb->huc1.ir_mode) ||
                       (gb->cartridge_type->mbc_type == GB_HUC3 && gb->huc3.mode == 0xE);
    
    if (likely(!is_sensing && gb->ir_warmup == 0 && gb->ir_decay == 0)) {
        /* Steady state: both counters would stay clamped at 0 and the input is forced low.
           Equivalent to the full computation below, without the work. */
        gb->effective_ir_input = false;
        return;
    }
    
    bool input = (gb->infrared_input || gb->cart_ir || (gb->io_registers[GB_IO_RP] & 1));
    if (is_sensing) {
        gb->ir_warmup += cycles;
        if (gb->ir_warmup > IR_WARMUP_MAX) {
            gb->ir_warmup = IR_WARMUP_MAX;
        }
    }
    else {
        gb->ir_warmup -= cycles;
        if (gb->ir_warmup < 0) {
            gb->ir_warmup = 0;
        }
        input = false;
    }
    
    if (gb->ir_warmup < IR_WARMUP) {
        input = false; // Not ready to sense yet
    }
    else if (gb->ir_warmup != IR_WARMUP_MAX) {
        input &= ir_noise(gb) < gb->ir_warmup - IR_WARMUP;
    }
    
    if (input) {
        gb->ir_decay += cycles;
        if (gb->ir_decay > IR_DECAY_MAX) {
            gb->ir_decay = IR_DECAY_MAX;
        }
    }
    else {
        gb->ir_decay -= cycles;
        if (gb->ir_decay < 0) {
            gb->ir_decay = 0;
        }
    }
    
    if (gb->ir_decay >= IR_DECAY && gb->ir_decay < IR_DECAY + IR_NOISE) {
        input &= ir_noise(gb) > gb->ir_decay - IR_DECAY;
    }
    else if (gb->ir_decay >= IR_DECAY + IR_NOISE) {
        input = false;
    }
    
    gb->effective_ir_input = input;
}

/* Whether ir_run over many cycles at once gives the same state as over the same cycles
   one by one. The noise is only consulted while the raw input is high, so with it low
   (or forced low when not sensing) both counters move linearly and the output stays low;
   with it high, warmup and decay both at their maximum are a fixed point (output low). */
static __attribute__((__noinline__, __cold__)) bool ir_batch_safe(GB_gameboy_t *gb)
{
    const GB_cartridge_t *cart = gb->cartridge_type;
    if ((gb->model > GB_MODEL_CGB_E || !gb->cgb_mode) && cart->mbc_type != GB_HUC1 && cart->mbc_type != GB_HUC3) return true;
    bool is_sensing = (gb->io_registers[GB_IO_RP] & 0xC0) == 0xC0 ||
                      (cart->mbc_type == GB_HUC1 && gb->huc1.ir_mode) ||
                      (cart->mbc_type == GB_HUC3 && gb->huc3.mode == 0xE);
    if (!is_sensing) return true;
    if (!(gb->infrared_input || gb->cart_ir || (gb->io_registers[GB_IO_RP] & 1))) return true;
    return gb->ir_warmup == IR_WARMUP_MAX && gb->ir_decay == IR_DECAY_MAX;
}

static void advance_tima_state_machine(GB_gameboy_t *gb)
{
    if (gb->tima_reload_state == GB_TIMA_RELOADED) {
        gb->tima_reload_state = GB_TIMA_RUNNING;
    }
    else if (gb->tima_reload_state == GB_TIMA_RELOADING) {
        gb->io_registers[GB_IO_IF] |= 4;
        gb->tima_reload_state = GB_TIMA_RELOADED;
    }
}

static void increase_tima(GB_gameboy_t *gb)
{
    gb->io_registers[GB_IO_TIMA]++;
    if (gb->io_registers[GB_IO_TIMA] == 0) {
        gb->io_registers[GB_IO_TIMA] = gb->io_registers[GB_IO_TMA];
        gb->tima_reload_state = GB_TIMA_RELOADING;
    }
}

static void printer_run(GB_gameboy_t *gb, unsigned cycles)
{
    if (gb->printer_callback || gb->printer_done_callback || gb->accessory == GB_ACCESSORY_PRINTER) {
        gb->printer.cycle_counter += cycles;
        unsigned ticks = gb->printer.cycle_counter / 512;
        gb->printer.cycle_counter %= 512;
        if (unlikely((gb->printer.command_state || gb->printer.bits_received))) {
            gb->printer.idle_time += ticks;
        }
        if (unlikely(gb->printer.time_remaining)) {
            if (gb->printer.time_remaining <= ticks) {
                gb->printer.time_remaining = 0;
                if (gb->printer_done_callback) {
                    gb->printer_done_callback(gb);
                }
            }
            else {
                gb->printer.time_remaining -= ticks;
            }
        }
    }
}

void GB_serial_master_edge(GB_gameboy_t *gb)
{
    
    gb->serial_master_clock ^= true;
    
    if (!gb->serial_master_clock && (gb->io_registers[GB_IO_SC] & 0x81) == 0x81) {
        gb->serial_count++;
        if (gb->serial_count == 8) {
            gb->serial_count = 0;
            gb->io_registers[GB_IO_SC] &= ~0x80;
            gb->io_registers[GB_IO_IF] |= 8;
        }
        
        gb->io_registers[GB_IO_SB] <<= 1;
        
        if (gb->serial_transfer_bit_end_callback) {
            gb->io_registers[GB_IO_SB] |= gb->serial_transfer_bit_end_callback(gb);
        }
        else {
            gb->io_registers[GB_IO_SB] |= 1;
        }
        
        if (gb->serial_count) {
            /* Still more bits to send */
            if (gb->serial_transfer_bit_start_callback) {
                gb->serial_transfer_bit_start_callback(gb, gb->io_registers[GB_IO_SB] & 0x80);
            }
        }
        
    }
}


static inline void set_internal_div_counter(GB_gameboy_t *gb, uint16_t value)
{
    /* TIMA increases when a specific high-bit becomes a low-bit. */
    uint16_t triggers = gb->div_counter & ~value;
    if ((gb->io_registers[GB_IO_TAC] & 4) && unlikely(triggers & TAC_TRIGGER_BITS[gb->io_registers[GB_IO_TAC] & 3])) {
        increase_tima(gb);
    }
    
    if (unlikely(triggers & gb->serial_mask)) {
        GB_serial_master_edge(gb);
    }
    
    /* TODO: Can switching to double speed mode trigger an event? */
    uint16_t apu_bit = gb->cgb_double_speed? 0x2000 : 0x1000;
    if (unlikely(triggers & apu_bit)) {
        GB_apu_div_event(gb);
    }
    else {
        uint16_t secondary_triggers = ~gb->div_counter & value;
        if (unlikely(secondary_triggers & apu_bit)) {
            GB_apu_div_secondary_event(gb);
        }
    }
    gb->div_counter = value;
}

void GB_set_internal_div_counter(GB_gameboy_t *gb, uint16_t value)
{
    set_internal_div_counter(gb, value);
}

/* Number of DIV counts before the counter crosses a multiple of period (a power of 2) */
static inline unsigned div_edge_distance(uint16_t counter, unsigned period)
{
    return period - (counter & (period - 1));
}

static void timers_run(GB_gameboy_t *gb, unsigned cycles)
{
    if (gb->stopped) {
        if (GB_is_cgb(gb)) {
            gb->apu.apu_cycles += 1 << !gb->cgb_double_speed;
        }
        gb->apu_output.sample_cycles += (gb->apu_output.sample_rate << !gb->cgb_double_speed) << 1;
        return;
    }
    
    /* Multi-M-cycle advance in the steady loop: the M-cycles before the next DIV edge that
       matters (a TIMA or serial clock falling edge, either edge of the DIV-APU bit) only
       advance counters, so perform them in closed form. The loop below then runs the
       remaining M-cycles (at least one) exactly as before. */
    if (cycles > 4 && gb->div_state == 2 &&
        gb->tima_reload_state == GB_TIMA_RUNNING && !gb->apu.pending_envelope_tick) {
        int32_t total = gb->div_cycles + (int32_t)cycles;
        if (total > 4) {
            unsigned iterations = (total + 3) / 4;
            unsigned distance = div_edge_distance(gb->div_counter, gb->cgb_double_speed? 0x2000 : 0x1000);
            if (gb->io_registers[GB_IO_TAC] & 4) {
                distance = MIN(distance, div_edge_distance(gb->div_counter, TAC_TRIGGER_BITS[gb->io_registers[GB_IO_TAC] & 3] * 2));
            }
            if (gb->serial_mask) {
                distance = MIN(distance, div_edge_distance(gb->div_counter, (gb->serial_mask & -gb->serial_mask) * 2));
            }
            unsigned skip = MIN(MIN(iterations - 1, (cycles - 1) / 4), (distance - 1) / 4);
            if (skip) {
                gb->div_counter += skip * 4;
                gb->apu.apu_cycles += skip << !gb->cgb_double_speed;
                gb->apu_output.sample_cycles += skip * ((gb->apu_output.sample_rate << !gb->cgb_double_speed) << 1);
                cycles -= skip * 4;
            }
        }
    }
    
    GB_STATE_MACHINE(gb, div, cycles, 1) {
        GB_STATE(gb, div, 1);
        GB_STATE(gb, div, 2);
    }
    
    GB_SLEEP(gb, div, 1, 3);
    while (true) {
        advance_tima_state_machine(gb);
        set_internal_div_counter(gb, gb->div_counter + 4);
        gb->apu.apu_cycles += 1 << !gb->cgb_double_speed;
        gb->apu_output.sample_cycles += (gb->apu_output.sample_rate << !gb->cgb_double_speed) << 1;
        GB_SLEEP(gb, div, 2, 4);
        if (unlikely(gb->apu.pending_envelope_tick)) {
            GB_apu_delayed_envelope_tick(gb);
        }
    }
}

void GB_set_rtc_mode(GB_gameboy_t *gb, GB_rtc_mode_t mode)
{
    if (gb->rtc_mode != mode) {
        gb->rtc_mode = mode;
        gb->rtc_cycles = 0;
        gb->last_rtc_second = time(NULL);
    }
}


void GB_set_rtc_multiplier(GB_gameboy_t *gb, double multiplier)
{
    if (multiplier == 1) {
        gb->rtc_second_length = 0;
        return;
    }
    
    gb->rtc_second_length = GB_get_unmultiplied_clock_rate(gb) * 2 * multiplier;
}

void GB_rtc_set_time(GB_gameboy_t *gb, uint64_t current_time)
{
    if (gb->cartridge_type->mbc_type == GB_HUC3) {
        while (gb->last_rtc_second / 60 < current_time / 60) {
            gb->last_rtc_second += 60;
            gb->huc3.minutes++;
            if (gb->huc3.minutes == 60 * 24) {
                gb->huc3.days++;
                gb->huc3.minutes = 0;
            }
        }
        return;
    }
    
    bool running = false;
    if (gb->cartridge_type->mbc_type == GB_TPP1) {
        running = gb->tpp1_mr4 & 0x4;
    }
    else {
        running = (gb->rtc_real.high & 0x40) == 0;
    }
    
    if (!running) return;
    
    while (gb->last_rtc_second + 60 * 60 * 24 < current_time) {
        gb->last_rtc_second += 60 * 60 * 24;
        if (gb->cartridge_type->mbc_type == GB_TPP1) {
            if (++gb->rtc_real.tpp1.weekday == 7) {
                gb->rtc_real.tpp1.weekday = 0;
                if (++gb->rtc_real.tpp1.weeks == 0) {
                    gb->tpp1_mr4 |= 8; /* Overflow bit */
                }
            }
        }
        else if (++gb->rtc_real.days == 0) {
            if (gb->rtc_real.high & 1) { /* Bit 8 of days*/
                gb->rtc_real.high |= 0x80; /* Overflow bit */
            }
            
            gb->rtc_real.high ^= 1;
        }
    }
    
    while (gb->last_rtc_second < current_time) {
        gb->last_rtc_second++;
        if (++gb->rtc_real.seconds != 60) continue;
        gb->rtc_real.seconds = 0;
        
        if (++gb->rtc_real.minutes != 60) continue;
        gb->rtc_real.minutes = 0;
        
        if (gb->cartridge_type->mbc_type == GB_TPP1) {
            if (++gb->rtc_real.tpp1.hours != 24) continue;
            gb->rtc_real.tpp1.hours = 0;
            if (++gb->rtc_real.tpp1.weekday != 7) continue;
            gb->rtc_real.tpp1.weekday = 0;
            if (++gb->rtc_real.tpp1.weeks == 0) {
                gb->tpp1_mr4 |= 8; /* Overflow bit */
            }
        }
        else {
            if (++gb->rtc_real.hours != 24) continue;
            gb->rtc_real.hours = 0;
            
            if (++gb->rtc_real.days != 0) continue;
            
            if (gb->rtc_real.high & 1) { /* Bit 8 of days*/
                gb->rtc_real.high |= 0x80; /* Overflow bit */
            }
            
            gb->rtc_real.high ^= 1;
        }
    }
}

static void rtc_run(GB_gameboy_t *gb, unsigned cycles)
{
    if (likely(gb->cartridge_type->mbc_type != GB_HUC3 && !gb->cartridge_type->has_rtc)) return;
    gb->rtc_cycles += cycles;
    time_t current_time = 0;
    uint32_t rtc_second_length = unlikely(gb->rtc_second_length)? gb->rtc_second_length : GB_get_unmultiplied_clock_rate(gb) * 2;
    
    switch (gb->rtc_mode) {
        case GB_RTC_MODE_SYNC_TO_HOST:
            // Sync in a 1/32s resolution
            if (gb->rtc_cycles < GB_get_unmultiplied_clock_rate(gb) / 16) return;
            gb->rtc_cycles -= GB_get_unmultiplied_clock_rate(gb) / 16;
            current_time = time(NULL);
            break;
        case GB_RTC_MODE_ACCURATE:
            if (gb->cartridge_type->mbc_type != GB_HUC3 && (gb->rtc_real.high & 0x40)) {
                gb->rtc_cycles -= cycles;
                return;
            }
            if (gb->rtc_cycles < rtc_second_length) return;
            gb->rtc_cycles -= rtc_second_length;
            current_time = gb->last_rtc_second + 1;
            break;
    }

    GB_rtc_set_time(gb, current_time);
}

static void camera_run(GB_gameboy_t *gb, unsigned cycles)
{
    /* Do we have a camera? */
    if (likely(gb->cartridge_type->mbc_type != GB_CAMERA)) return;

    /* The camera mapper uses the PHI pin to clock itself */

    /* PHI does not run in halt nor stop mode */
    if (unlikely(gb->halted || gb->stopped)) return;

    /* Only every other PHI is used (as the camera wants a 512KiHz clock) */
    gb->camera_alignment += cycles;

    /* Is the camera processing an image? */
    if (likely(gb->camera_countdown == 0)) return;

    gb->camera_countdown -= cycles;
    if (gb->camera_countdown <= 0) {
        gb->camera_countdown = 0;
        GB_camera_updated(gb);
    }
}


/* Inline guards replicating the callees' own early-return conditions, so the common
   "nothing to do" case doesn't pay for a call and a full prologue on every M-cycle. */
static inline void joypad_run(GB_gameboy_t *gb, unsigned cycles)
{
    if (likely(gb->joypad_is_stable && !gb->inputs_changed)) return;
    GB_joypad_run(gb, cycles);
}

static inline void dma_run(GB_gameboy_t *gb)
{
    /* Mirrors GB_dma_run's own early returns */
    if (likely(gb->dma_current_dest == 0xA1 || gb->halted)) return;
    GB_dma_run(gb);
}

static inline void apu_vgm_advance(GB_gameboy_t *gb, unsigned cycles)
{
    if (likely(!gb->apu_output.output_file)) return;
    GB_apu_vgm_advance(gb, cycles);
}

static inline void apu_run(GB_gameboy_t *gb)
{
    /* Same condition as the non-forced early return at the top of GB_apu_run */
    if (GB_apu_needs_run(gb, gb->apu.apu_cycles, gb->apu_output.sample_cycles)) {
        GB_apu_run(gb, false);
    }
}

void GB_advance_cycles(GB_gameboy_t *gb, unsigned cycles)
{
    if (unlikely(gb->speed_switch_countdown)) {
        if (gb->speed_switch_countdown == cycles) {
            gb->cgb_double_speed ^= true;
            gb->speed_switch_countdown = 0;
        }
        else if (gb->speed_switch_countdown > cycles) {
            gb->speed_switch_countdown -= cycles;
        }
        else {
            uint8_t old_cycles = gb->speed_switch_countdown;
            cycles -= old_cycles;
            gb->speed_switch_countdown = 0;
            GB_advance_cycles(gb, old_cycles);
            gb->cgb_double_speed ^= true;
        }
    }
    gb->apu.pcm_mask[0] = gb->apu.pcm_mask[1] = 0xFF; // Sort of hacky, but too many cross-component interactions to do it right
    // Affected by speed boost
    gb->dma_cycles = cycles;

    timers_run(gb, cycles);
    camera_run(gb, cycles);

    if (unlikely(gb->speed_switch_halt_countdown)) {
        gb->speed_switch_halt_countdown -= cycles;
        if (gb->speed_switch_halt_countdown <= 0) {
            gb->speed_switch_halt_countdown = 0;
            gb->halted = false;
        }
    }
        
#ifndef GB_DISABLE_DEBUGGER
    gb->debugger_ticks += cycles;
#endif
    
    if (gb->speed_switch_freeze) {
        if (gb->speed_switch_freeze >= cycles) {
            gb->speed_switch_freeze -= cycles;
            return;
        }
        cycles -= gb->speed_switch_freeze;
        gb->speed_switch_freeze = 0;
    }

    if (unlikely(!gb->cgb_double_speed)) {
        cycles <<= 1;
    }

    if (unlikely(gb->printer_callback || gb->printer_done_callback || gb->accessory == GB_ACCESSORY_PRINTER)) {
        printer_run(gb, cycles);
    }
    
#ifndef GB_DISABLE_DEBUGGER
    gb->absolute_debugger_ticks += cycles;
    *((gb->halted || gb->stopped)? &gb->current_frame_idle_cycles : &gb->current_frame_busy_cycles) += cycles;
    *((gb->halted || gb->stopped)? &gb->current_second_idle_cycles : &gb->current_second_busy_cycles) += cycles;
#endif
    
    // Not affected by speed boost
    if (likely(gb->io_registers[GB_IO_LCDC] & GB_LCDC_ENABLE)) {
        gb->double_speed_alignment += cycles;
    }
    gb->cycles_since_last_sync += cycles;
    gb->cycles_since_run += cycles;
    apu_vgm_advance(gb, cycles);
    
    gb->rumble_on_cycles += gb->rumble_strength & 3;
    gb->rumble_off_cycles += (gb->rumble_strength & 3) ^ 3;
    
    if (unlikely(gb->data_bus_decay_countdown)) {
        if (gb->data_bus_decay_countdown <= cycles) {
            gb->data_bus_decay_countdown = 0;
            gb->data_bus = 0xFF;
        }
        else {
            gb->data_bus_decay_countdown -= cycles;
        }
    }
    
    joypad_run(gb, cycles);
    apu_run(gb);
    GB_display_run(gb, cycles, false);
    if (unlikely(!gb->stopped)) { // TODO: Verify what happens in STOP mode
        dma_run(gb);
    }
    ir_run(gb, cycles);
    rtc_run(gb, cycles);
}

/* HALT batching
   While the CPU is halted, every M-cycle runs the same loop iteration in GB_cpu_run:
   two GB_advance_cycles(2) with an interrupt check in between on pre-CGB models, a single
   GB_advance_cycles(4) on CGB. As long as no enabled interrupt can be raised and no unit
   needs per-step work, a run of such iterations only advances counters, and a single
   GB_advance_cycles call with the total count produces the same state (every unit handles
   multi-cycle advances: the timers loop per M-cycle, the PPU takes its trivial path, the
   APU guard does not fire). GB_halt_batch_iterations returns how many iterations are
   guaranteed to be like that, every bound being conservative; GB_halt_batch performs them.
   Builds with GB_HALT_VERIFY replay the iterations one by one on a copy and compare. */

#define GB_HALT_BATCH_MAX 60

/* Whether advancing n M-cycles at once would skip an APU run that must happen at its exact
   cycle. Halted, the sample and render cadence does not need one: a single GB_apu_run over
   the whole batch renders every due sample at its own position (bit-identical over the
   regression set); only the per-cycle channel conditions (sweep, channel 1 restart, wave
   channel reads) still do. Running (lazy CPU windows), keep the full condition. */
static inline bool batch_needs_apu_run(GB_gameboy_t *gb, bool halted, unsigned apu_step, unsigned sample_step, unsigned n)
{
    if (halted) {
        return GB_apu_needs_run(gb, 0, 0);
    }
    return GB_apu_needs_run(gb, gb->apu.apu_cycles + apu_step * n, gb->apu_output.sample_cycles + sample_step * n);
}

/* Number of upcoming M-cycles during which no unit needs per-step work and no enabled
   interrupt can be raised, so that advancing them in a single GB_advance_cycles call is
   equivalent. Used for HALT batching (halted) and for the lazy CPU (running). */
static unsigned event_budget(GB_gameboy_t *gb, bool halted)
{
#ifndef GB_DISABLE_DEBUGGER
    return 0; /* The debugger hooks every GB_run call */
#endif
    if (!gb->halt_batching_allowed) return 0;
    if (gb->stopped) return 0;
    if (halted) {
        if (gb->just_halted) return 0;
    }
    else {
        /* The CPU shares the bus with OAM DMA and HDMA; keep them in lockstep */
        if (GB_is_dma_active(gb) || gb->hdma_on || gb->hdma_in_progress) return 0;
    }
    /* Most frequent blockers first: an APU run due within one M-cycle (always the case
       while the wave channel forces per-cycle APU runs), or a non-trivial PPU step */
    const unsigned apu_step = 1 << !gb->cgb_double_speed;
    const unsigned sample_step = (gb->apu_output.sample_rate << !gb->cgb_double_speed) << 1;
    if (batch_needs_apu_run(gb, halted, apu_step, sample_step, 1)) return 0;
    /* GB_advance_cycles units per iteration (4 CPU cycles, doubled when not in double speed) */
    const unsigned units = gb->cgb_double_speed? 4 : 8;
    int32_t display_budget = GB_display_trivial_budget(gb);
    if (display_budget < (int32_t)(2 * units)) return 0; /* Not worth it below 2 iterations */

    if (halted) {
        if (gb->ime_toggle || gb->pending_cycles) return 0;
        if (gb->interrupt_enable & gb->io_registers[GB_IO_IF] & 0x1F) return 0;
    }
    if (gb->speed_switch_countdown || gb->speed_switch_halt_countdown || gb->speed_switch_freeze) return 0;
    if (gb->printer_callback || gb->printer_done_callback || gb->accessory == GB_ACCESSORY_PRINTER) return 0;
    if (gb->apu_output.output_file) return 0;
    if (!gb->joypad_is_stable || gb->inputs_changed) return 0;
    if (gb->div_state != 2 || gb->tima_reload_state != GB_TIMA_RUNNING) return 0;
    if (gb->apu.pending_envelope_tick) return 0;
    /* Serial: an internally clocked transfer shifts on DIV edges and raises an interrupt.
       An externally clocked one only advances on edges from a link partner (callbacks). */
    if ((gb->io_registers[GB_IO_SC] & 0x81) == 0x81) return 0;
    if ((gb->io_registers[GB_IO_SC] & 0x80) && (gb->serial_transfer_bit_start_callback || gb->serial_transfer_bit_end_callback)) return 0;
    
    const GB_cartridge_t *cart = gb->cartridge_type;
    /* IR: its steady state (see ir_run) is a no-op; otherwise see ir_batch_safe */
    if ((gb->model <= GB_MODEL_CGB_E && gb->cgb_mode) || cart->mbc_type == GB_HUC1 || cart->mbc_type == GB_HUC3) {
        bool is_sensing = (gb->io_registers[GB_IO_RP] & 0xC0) == 0xC0 ||
                          (cart->mbc_type == GB_HUC1 && gb->huc1.ir_mode) ||
                          (cart->mbc_type == GB_HUC3 && gb->huc3.mode == 0xE);
        if ((is_sensing || gb->ir_warmup || gb->ir_decay) && !ir_batch_safe(gb)) return 0;
    }
    
    /* PPU: stay within the trivial (counters only) window; VBlank and STAT interrupts
       can only be raised outside of it */
    unsigned n = MIN(GB_HALT_BATCH_MAX, (unsigned)display_budget / units);
    
    /* DIV-APU events (both edges of the APU bit) force APU runs: stop before the next one.
       The DIV counter advances by 4 per M-cycle. */
    uint16_t apu_bit = gb->cgb_double_speed? 0x2000 : 0x1000;
    unsigned to_apu_edge = (apu_bit - (gb->div_counter & (apu_bit - 1)) + 3) / 4; /* M-cycle of the edge */
    n = MIN(n, to_apu_edge - 1);
    if (!n) return 0;
    
    /* Timer interrupt: stop before the M-cycle TIMA overflows (IF is set one M-cycle later) */
    if ((gb->interrupt_enable & 4) && (gb->io_registers[GB_IO_TAC] & 4)) {
        unsigned period = TAC_TRIGGER_BITS[gb->io_registers[GB_IO_TAC] & 3] * 2;
        unsigned first_increment = (period - (gb->div_counter & (period - 1)) + 3) / 4;
        unsigned overflow = first_increment + (0xFF - gb->io_registers[GB_IO_TIMA]) * (period / 4);
        n = MIN(n, overflow - 1);
        if (!n) return 0;
    }
    
    /* GB_timing_sync runs every iteration in this state; it must keep returning early */
    if ((gb->interrupt_enable & 0x10) && (gb->ime || halted)) {
        if (gb->cycles_since_last_sync + units >= LCDC_PERIOD / 3) return 0;
        n = MIN(n, (unsigned)((LCDC_PERIOD / 3 - 1 - gb->cycles_since_last_sync) / units));
        if (!n) return 0;
    }
    
    /* RTC: stay below the next second (or host sync) boundary */
    if (cart->mbc_type == GB_HUC3 || cart->has_rtc) {
        uint32_t limit = 0;
        if (gb->rtc_mode == GB_RTC_MODE_SYNC_TO_HOST) {
            limit = GB_get_unmultiplied_clock_rate(gb) / 16;
        }
        else if (cart->mbc_type == GB_HUC3 || !(gb->rtc_real.high & 0x40)) {
            limit = unlikely(gb->rtc_second_length)? gb->rtc_second_length : GB_get_unmultiplied_clock_rate(gb) * 2;
        }
        if (limit) {
            if (gb->rtc_cycles + units >= limit) return 0;
            n = MIN(n, (limit - 1 - gb->rtc_cycles) / units);
            if (!n) return 0;
        }
    }
    
    /* APU: no run may be triggered during the batch (see batch_needs_apu_run). The timers
       loop adds these per M-cycle; the condition is monotonic in n, so search the largest n
       that keeps it false. */
    if (batch_needs_apu_run(gb, halted, apu_step, sample_step, n)) {
        unsigned low = 1, high = n; /* needs_run(low) is false, needs_run(high) is true */
        while (high - low > 1) {
            unsigned mid = (low + high) / 2;
            if (batch_needs_apu_run(gb, halted, apu_step, sample_step, mid)) {
                high = mid;
            }
            else {
                low = mid;
            }
        }
        n = low;
    }
    return n;
}

unsigned GB_halt_batch_iterations(GB_gameboy_t *gb)
{
    if (!gb->halted) return 0;
    return event_budget(gb, true);
}

/* Lazy CPU (relaxed): whether the CPU may run ahead of the rest of the machine at all */
bool GB_lazy_cpu_allowed(GB_gameboy_t *gb)
{
#ifndef GB_DISABLE_DEBUGGER
    return false;
#endif
    if (!gb->lazy_cpu_enabled || !gb->halt_batching_allowed) return false;
    if (gb->stopped || gb->halted) return false;
    if (GB_is_dma_active(gb) || gb->hdma_on || gb->hdma_in_progress) return false;
    if (gb->speed_switch_countdown || gb->speed_switch_halt_countdown || gb->speed_switch_freeze) return false;
    if (gb->printer_callback || gb->printer_done_callback || gb->accessory == GB_ACCESSORY_PRINTER) return false;
    if (gb->apu_output.output_file) return false;
    if (!gb->joypad_is_stable || gb->inputs_changed) return false;
    return true;
}

/* How many CPU cycles (max_lag at most) the CPU may run ahead of the rest of the machine.
   Deferred cycles are applied exactly before any observable access, so the lag is only
   visible as interrupt latency. VBlank and timer handlers tolerate it (a few dozen
   microseconds), but STAT interrupts drive HBlank VRAM copies and raster effects that must
   fit in their mode: while one is armed, the window ends before it can be raised. */
unsigned GB_lazy_cpu_budget(GB_gameboy_t *gb, unsigned max_lag)
{
    if (!GB_lazy_cpu_allowed(gb)) return 0;
    unsigned budget = max_lag;
    uint8_t ie = gb->interrupt_enable;
    uint8_t stat = gb->io_registers[GB_IO_STAT];
    if (gb->io_registers[GB_IO_LCDC] & GB_LCDC_ENABLE) {
        /* Mode 0 / mode 2 STAT sources fire on every line, but only right after mode 3 /
           mode 0 (or a late mode 3): from any other mode they are more than GB_LAZY_MAX_LAG
           cycles away.
           HBlank starts in steps after mode 3: STAT reads mode 0 one dot before the mode 0
           interrupt is raised (display state 22), and an armed HBlank HDMA starts two dots
           later (state 33). Both must happen at their exact cycle. */
        bool hblank_ahead = (stat & 3) == 3 || gb->display_state == 22;
        /* The mode 2 source fires at the next line start: from a late mode 3 (a long one, or
           the relaxed 300-dot hold, display state 47) that can be less than GB_LAZY_MAX_LAG away */
        bool near = ((ie & 2) && (((stat & 0x08) && hblank_ahead) || ((stat & 0x20) && ((stat & 3) == 0 || hblank_ahead)))) ||
                    (gb->hdma_on_hblank && (hblank_ahead || gb->display_state == 33));
        if (!near) {
            /* VBlank, STAT mode 1 and LYC fire at the start of a given line: only bound the
               window when that line is close */
            unsigned ly = gb->current_line;
            /* Line 144 also ends GB_run_frame, where the frontend changes the inputs: the CPU
               must not be ahead there even without a VBlank interrupt */
            unsigned lines = (144 + 154 - ly) % 154;
            if ((ie & 2) && (stat & 0x40) && gb->io_registers[GB_IO_LYC] < 154) {
                /* LY reads 0 from early in line 153 on, so LYC = 0 matches there (and at line 0
                   when the LCD was just turned on) */
                unsigned lyc = gb->io_registers[GB_IO_LYC];
                lines = MIN(lines, (lyc + 154 - ly) % 154);
                if (lyc == 0) {
                    lines = MIN(lines, (153 + 154 - ly) % 154);
                }
            }
            if ((ie & 2) && (stat & 0x20)) {
                /* Mode 2 source from VBlank: line 0 (and line 144, as VBlank starts) */
                lines = MIN(lines, (154 - ly) % 154);
                lines = MIN(lines, (144 + 154 - ly) % 154);
            }
            if (lines <= 1) {
                near = true;
            }
            else {
                budget = MIN(budget, ((lines - 1) * 456) << gb->cgb_double_speed);
            }
        }
        if (near) {
            int32_t display = GB_display_trivial_budget(gb); /* GB_advance_cycles units */
            if (display <= 0) return 0;
            budget = MIN(budget, (unsigned)display >> !gb->cgb_double_speed);
        }
    }
    else {
        /* LCD off: the display still ends frames (GB_run_frame returns, inputs change) on
           its own schedule; stop before that */
        int32_t display = GB_display_trivial_budget(gb);
        if (display <= 0) return 0;
        budget = MIN(budget, (unsigned)display >> !gb->cgb_double_speed);
    }
    /* Timer: TIMA overflows on a known DIV edge; stop before it (taking the timer interrupt
       late changes how far a busy loop got, which games use as a random seed or counter) */
    /* A pending reload raises the interrupt on its own, even with the timer just disabled
       (a TAC write can increment TIMA into an overflow) */
    if ((ie & 4) && gb->tima_reload_state != GB_TIMA_RUNNING) return 0;
    if ((ie & 4) && (gb->io_registers[GB_IO_TAC] & 4)) {
        unsigned period = TAC_TRIGGER_BITS[gb->io_registers[GB_IO_TAC] & 3] * 2;
        unsigned distance = div_edge_distance(gb->div_counter, period) + (0xFF - gb->io_registers[GB_IO_TIMA]) * period;
        if (distance <= 8) return 0;
        budget = MIN(budget, distance - 8);
    }
    /* Serial: only an internally clocked transfer can complete (and interrupt) on its own */
    if ((ie & 8) &&(gb->io_registers[GB_IO_SC] & 0x81) == 0x81) {
        /* The serial interrupt can only be raised on a serial clock edge (a DIV bit falling):
           stop before the next one */
        unsigned distance = div_edge_distance(gb->div_counter, (gb->serial_mask & -gb->serial_mask) * 2);
        if (distance <= 8) return 0;
        budget = MIN(budget, distance - 8);
    }
    return budget & ~3;
}

/* Number of M-cycles that can be advanced in one GB_advance_cycles call while keeping the
   units exact relative to each other: never more than GB_LAZY_CHUNK (so the PPU crosses at
   most one line boundary per call; display_run_full cannot take more, 64 overflows its
   stack), and ending at the next DIV-APU edge so APU events keep their exact cycle.
   With GB_LAZY_APU_BATCH, chunks run through audio samples like HALT batches do: the
   picture and emulated state are unchanged, the band-limited output may shift slightly
   (fast mode only; measured 84/388 real games, median 0.36% RMS, mostly closer to exact
   mode). Set it to 0 for chunks cut at every sample render as before. */
#ifndef GB_LAZY_CHUNK
#define GB_LAZY_CHUNK 16
#endif
#ifndef GB_LAZY_APU_BATCH
#define GB_LAZY_APU_BATCH 1
#endif
static unsigned lazy_chunk_limit(GB_gameboy_t *gb)
{
    unsigned limit = GB_LAZY_CHUNK;
#if GB_LAZY_APU_BATCH
    /* Same as HALT batches (see batch_needs_apu_run): one GB_apu_run at the end of the
       chunk renders every sample due within it at its own position; only the per-cycle
       channel conditions need the APU stepped one M-cycle at a time */
    if (GB_apu_needs_run(gb, 0, 0)) return 1;
#else
    const unsigned apu_step = 1 << !gb->cgb_double_speed;
    const unsigned sample_step = (gb->apu_output.sample_rate << !gb->cgb_double_speed) << 1;
    if (GB_apu_needs_run(gb, gb->apu.apu_cycles + apu_step, gb->apu_output.sample_cycles + sample_step)) return 1;
    if (sample_step) {
        limit = MIN(limit, (gb->clock_rate - gb->apu_output.sample_cycles + sample_step - 1) / sample_step);
    }
    unsigned used = gb->apu.apu_cycles + gb->apu_output.cycles_since_render;
    if (used < gb->apu_output.max_cycles_per_sample) {
        limit = MIN(limit, (gb->apu_output.max_cycles_per_sample - used + apu_step - 1) / apu_step);
    }
#endif
    uint16_t apu_bit = gb->cgb_double_speed? 0x2000 : 0x1000;
    limit = MIN(limit, (apu_bit - (gb->div_counter & (apu_bit - 1)) + 3) / 4);
    /* Units that are not batch-safe in their current state */
    const GB_cartridge_t *cart = gb->cartridge_type;
    if ((gb->model <= GB_MODEL_CGB_E && gb->cgb_mode) || cart->mbc_type == GB_HUC1 || cart->mbc_type == GB_HUC3) {
        bool is_sensing = (gb->io_registers[GB_IO_RP] & 0xC0) == 0xC0 ||
                          (cart->mbc_type == GB_HUC1 && gb->huc1.ir_mode) ||
                          (cart->mbc_type == GB_HUC3 && gb->huc3.mode == 0xE);
        if ((is_sensing || gb->ir_warmup || gb->ir_decay) && !ir_batch_safe(gb)) return 1;
    }
    if (gb->div_state != 2 || gb->apu.pending_envelope_tick) return 1;
    /* The display call that performs a scheduled WY check runs its remaining cycles without
       batching (display_run_full). Stepping, that is the rest of one M-cycle; a chunk would
       extend it and could make mode 3 take the per-dot path where the exact run batches. */
    if (!gb->wy_triggered && gb->wy_check_scheduled) return 1;
    /* display_run_full treats a call that crosses the line end, as measured from
       cycles_for_line before HBlank resets it, as a mode 3 that never ended (display9).
       Stepping cannot trigger it there; keep chunks short of that point. */
    if (gb->io_registers[GB_IO_LCDC] & GB_LCDC_ENABLE) {
        const unsigned units = gb->cgb_double_speed? 4 : 8; /* display cycles per M-cycle */
        int32_t room = 456 * 2 /* LINE_LENGTH */ - (int32_t)gb->cycles_for_line * 2 - gb->display_cycles;
        if (room < (int32_t)(limit * units)) {
            limit = room > (int32_t)units? room / units : 1;
        }
    }
    return MAX(limit, 1);
}

/* Applies `cycles` CPU cycles that were deferred over `steps` CPU accesses */
void GB_advance_cycles_chunked(GB_gameboy_t *gb, unsigned cycles, unsigned steps)
{
    unsigned calls = 0;
    while (cycles) {
        unsigned chunk = MIN(cycles, lazy_chunk_limit(gb) * 4);
#if GB_LAZY_APU_BATCH
        gb->lazy_apu_replay = chunk > 4 && !gb->speed_switch_countdown && !gb->speed_switch_freeze && !gb->stopped;
        GB_advance_cycles(gb, chunk);
        gb->lazy_apu_replay = false;
#else
        GB_advance_cycles(gb, chunk);
#endif
        cycles -= chunk;
        calls++;
    }
    /* Per-call side effects of GB_advance_cycles, as if called once per deferred access */
    if (steps > calls) {
        gb->rumble_on_cycles += (gb->rumble_strength & 3) * (steps - calls);
        gb->rumble_off_cycles += ((gb->rumble_strength & 3) ^ 3) * (steps - calls);
    }
    gb->dma_cycles = 4;
}


static void halt_batch_apply(GB_gameboy_t *gb, unsigned iterations)
{
    bool pre_cgb = !GB_is_cgb(gb);
    unsigned calls = pre_cgb? iterations * 2 : iterations;
    GB_advance_cycles(gb, iterations * 4);
    /* Per-call side effects of GB_advance_cycles */
    gb->dma_cycles = pre_cgb? 2 : 4;
    gb->rumble_on_cycles += (gb->rumble_strength & 3) * (calls - 1);
    gb->rumble_off_cycles += ((gb->rumble_strength & 3) ^ 3) * (calls - 1);
}

#ifdef GB_HALT_VERIFY
#include <stdlib.h>
#include <string.h>
static void halt_verify_trap(void)
{
    fprintf(stderr, "HALT batch verifier: a callback fired during a batch\n");
    abort();
}

static void halt_verify_ignore_sample(GB_gameboy_t *gb, GB_sample_t *sample)
{
    (void)gb; (void)sample;
}

static void halt_batch_reference(GB_gameboy_t *gb, unsigned iterations)
{
    /* Exactly what GB_cpu_run does for each halted iteration that doesn't wake up */
    for (unsigned i = 0; i < iterations; i++) {
        if ((gb->interrupt_enable & 0x10) && (gb->ime || gb->halted)) {
            GB_timing_sync(gb);
        }
        if (!GB_is_cgb(gb)) {
            GB_advance_cycles(gb, 2);
        }
        if (gb->interrupt_enable & gb->io_registers[GB_IO_IF] & 0x1F) {
            fprintf(stderr, "HALT batch verifier: interrupt pending inside the batch (iteration %u/%u)\n", i, iterations);
            abort();
        }
        GB_advance_cycles(gb, GB_is_cgb(gb)? 4 : 2);
        if (gb->interrupt_enable & gb->io_registers[GB_IO_IF] & 0x1F) {
            fprintf(stderr, "HALT batch verifier: interrupt pending at the end of iteration %u/%u\n", i, iterations);
            abort();
        }
    }
}
#endif

void GB_halt_batch(GB_gameboy_t *gb, unsigned iterations)
{
#ifdef GB_HALT_VERIFY
    GB_gameboy_t *reference = malloc(sizeof(*gb));
    memcpy(reference, gb, sizeof(*gb));
    /* Any callback firing during a batch is a failure */
    reference->vblank_callback = (void *)halt_verify_trap;
    reference->apu_output.sample_callback = halt_verify_ignore_sample; /* Batches may render samples */
    reference->lcd_line_callback = (void *)halt_verify_trap;
    reference->rumble_callback = (void *)halt_verify_trap;
    reference->infrared_callback = (void *)halt_verify_trap;
    reference->serial_transfer_bit_start_callback = (void *)halt_verify_trap;
    reference->serial_transfer_bit_end_callback = (void *)halt_verify_trap;
    halt_batch_reference(reference, iterations);
    reference->vblank_callback = gb->vblank_callback;
    reference->apu_output.sample_callback = gb->apu_output.sample_callback;
    reference->lcd_line_callback = gb->lcd_line_callback;
    reference->rumble_callback = gb->rumble_callback;
    reference->infrared_callback = gb->infrared_callback;
    reference->serial_transfer_bit_start_callback = gb->serial_transfer_bit_start_callback;
    reference->serial_transfer_bit_end_callback = gb->serial_transfer_bit_end_callback;
#endif
    halt_batch_apply(gb, iterations);
#ifdef GB_HALT_VERIFY
    if (memcmp(reference, gb, sizeof(*gb))) {
        const uint8_t *a = (const void *)reference, *b = (const void *)gb;
        for (size_t i = 0; i < sizeof(*gb); i++) {
            if (a[i] != b[i]) {
                fprintf(stderr, "HALT batch verifier: state differs at offset %zu (reference %02x, batched %02x), %u iterations, %s, double speed %d\n",
                        i, a[i], b[i], iterations, GB_is_cgb(gb)? "CGB" : "pre-CGB", gb->cgb_double_speed);
                break;
            }
        }
        abort();
    }
    free(reference);
#endif
}

/* 
   This glitch is based on the expected results of mooneye-gb rapid_toggle test.
   This glitch happens because how TIMA is increased, see GB_set_internal_div_counter.
   According to GiiBiiAdvance, GBC's behavior is different, but this was not tested or implemented.
*/
void GB_emulate_timer_glitch(GB_gameboy_t *gb, uint8_t old_tac, uint8_t new_tac)
{
    /* Glitch only happens when old_tac is enabled. */
    if (!(old_tac & 4)) return;

    unsigned old_clocks = TAC_TRIGGER_BITS[old_tac & 3];
    unsigned new_clocks = TAC_TRIGGER_BITS[new_tac & 3];

    /* The bit used for overflow testing must have been 1 */
    if (gb->div_counter & old_clocks) {
        /* And now either the timer must be disabled, or the new bit used for overflow testing be 0. */
        if (!(new_tac & 4) || !(gb->div_counter & new_clocks)) {
            increase_tima(gb);
        }
    }
}
