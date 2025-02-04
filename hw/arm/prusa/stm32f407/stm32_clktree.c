/*
 * Basic Clock Tree Building Blocks
 *
 * Copyright (C) 2012 Andre Beckus
 * Adapted for QEMU 5.2 in 2020 by VintagePC <http://github.com/vintagepc>
 *
 * Source code roughly based on omap_clk.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "stm32_clktree.h"
#include "qemu/host-utils.h"

/* DEFINITIONS*/

/* See README for DEBUG documentation */
//#define DEBUG_CLKTREE


/* Helper to:
 *  1) Add a value to an array.
 *  2) Increment the array size count.
 *  3) Make sure the array does not overflow.
 */
#define CLKTREE_ADD_LINK(array, count, value, array_size) \
            (array)[(count)++] = (value); \
            assert((count) <= (array_size));


#include "stm32_clk_type.h"

static void clktree_recalc_output_freq(Clk_p clk);

/* HELPER FUNCTIONS */

static Clk_p clktree_get_input_clk(Clk_p clk)
{
    return clk->input[clk->selected_input + 1];
}

#ifdef DEBUG_CLKTREE

static void clktree_print_state(Clk clk)
{
    Clk input_clk = clktree_get_input_clk(clk);

    printf("CLKTREE: %s Output Change (SrcClk:%s InFreq:%lu OutFreq:%lu Mul:%u Div:%u Enabled:%c)\n",
            clk->name,
            input_clk ? input_clk->name : "None",
            (unsigned long)clk->input_freq,
            (unsigned long)clk->output_freq,
            (unsigned)clk->multiplier,
            (unsigned)clk->divisor,
            clk->enabled ? '1' : '0');
}
#endif

static void clktree_set_input_freq(Clk_p clk, uint32_t input_freq)
{
    clk->input_freq = input_freq;

    clktree_recalc_output_freq(clk);
}

/* Recalculates the output frequency based on the clock's input_freq variable.
 */
static void clktree_recalc_output_freq(Clk_p clk) {
    int i;
    Clk_p next_clk, next_clk_input;
    uint32_t new_output_freq;

    /* Get the output frequency, or 0 if the output is disabled. */
    new_output_freq = clk->enabled ?
                            muldiv64(clk->input_freq,
                                     clk->multiplier,
                                     clk->divisor)
                            : 0;

    /* if the frequency has changed. */
   if(new_output_freq != clk->output_freq) {
       clk->output_freq = new_output_freq;

#ifdef DEBUG_CLKTREE
        clktree_print_state(clk);
#endif

        /* Check the new frequency against the max frequency. */
        if(new_output_freq > clk->max_output_freq) {
            fprintf(stderr, "%s: Clock %s output frequency (%u Hz) exceeds max frequency (%u Hz).\n",
                    __FUNCTION__,
                    clk->name,
                    new_output_freq,
                    clk->max_output_freq);
        }

        /* Notify users of change. */
        for(i=0; i < clk->user_count; i++) {
            qemu_set_irq(clk->user[i], 1);
        }

        /* Propagate the frequency change to the child clocks */
        for(i=0; i < clk->output_count; i++) {
            next_clk = clk->output[i];
            assert(next_clk != NULL);

            /* Only propagate the change if the child has selected the current
             * clock as input.
             */
            next_clk_input = clktree_get_input_clk(next_clk);
            if(next_clk_input == clk) {
                /* Recursively propagate changes.  The clock tree should not be
                 * too deep, so we shouldn't have to recurse too many times.
                 */
                clktree_set_input_freq(next_clk, new_output_freq);
            }
        }
    }
}


/* Generic create routine used by the public create routines. */
static void clktree_create_generic(Clk_p clk,
                    const char *name,
                    uint16_t multiplier,
                    uint16_t divisor,
                    bool enabled)
{
    clk->name = name;

    clk->input_freq = 0;
    clk->output_freq = 0;
    clk->max_output_freq = CLKTREE_NO_MAX_FREQ;

    clk->multiplier = multiplier;
    clk->divisor = divisor;

    clk->enabled = enabled;

    clk->user_count = 0;

    clk->output_count = 0;

    clk->input_count = 1;
    clk->input[0] = NULL;
    clk->selected_input = CLKTREE_NO_INPUT;

    clk->is_initialized = true;
}

/* PUBLIC FUNCTIONS */
bool clktree_is_enabled(Clk_p clk)
{
    return clk->enabled && clk->is_initialized;
}

uint32_t clktree_get_output_freq(Clk_p clk)
{
    return clk->output_freq;
}

void clktree_adduser(Clk_p clk, qemu_irq user)
{
    CLKTREE_ADD_LINK(
            clk->user,
            clk->user_count,
            user,
            CLKTREE_MAX_IRQ);
}


void clktree_create_src_clk(Clk_p clk,
                    const char *name,
                    uint32_t src_freq,
                    bool enabled)
{

    clktree_create_generic(clk, name, 1, 1, enabled);

    clktree_set_input_freq(clk, src_freq);
}


void clktree_create_clk( Clk_p clk,
                    const char *name,
                    uint16_t multiplier,
                    uint16_t divisor,
                    bool enabled,
                    uint32_t max_output_freq,
                    int selected_input,
                    ...)
{
    va_list input_clks;
    Clk_p input_clk;

    clktree_create_generic(clk, name, multiplier, divisor, enabled);

    /* Add the input clock connections. */
    va_start(input_clks, selected_input);
    while((input_clk = va_arg(input_clks, Clk_p)) != NULL) {
        CLKTREE_ADD_LINK(
                clk->input,
                clk->input_count,
                input_clk,
                CLKTREE_MAX_INPUT);

        CLKTREE_ADD_LINK(
                input_clk->output,
                input_clk->output_count,
                clk,
                CLKTREE_MAX_OUTPUT);
    }
    va_end(input_clks);
    clktree_set_selected_input(clk, selected_input);
}


void clktree_set_scale(Clk_p clk, uint16_t multiplier, uint16_t divisor)
{
    clk->multiplier = multiplier;
    clk->divisor = divisor;

    clktree_recalc_output_freq(clk);
}


void clktree_set_enabled(Clk_p clk, bool enabled)
{
    clk->enabled = enabled;

    clktree_recalc_output_freq(clk);
}


void clktree_set_selected_input(Clk_p clk, int selected_input)
{
    uint32_t input_freq;

    assert((selected_input + 1) < clk->input_count);

    clk->selected_input = selected_input;

    /* Get the input clock frequency.  If there is no input, this should be 0.
     */
    if(selected_input > -1) {
        input_freq = clktree_get_input_clk(clk)->output_freq;
    } else {
        input_freq = 0;
    }

    clktree_set_input_freq(clk, input_freq);
}
