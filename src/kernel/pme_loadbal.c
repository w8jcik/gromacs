/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 4.6.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2011, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gallium Rubidium Oxygen Manganese Argon Carbon Silicon
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "smalloc.h"
#include "network.h"
#include "calcgrid.h"
#include "pme.h"
#include "vec.h"
#include "domdec.h"
#include "nbnxn_cuda_data_mgmt.h"
#include "force.h"
#include "pme_loadbal.h"

/* Parameters and setting for one PP-PME setup */
typedef struct {
    real rcut;            /* Coulomb cut-off                              */
    real rlist;           /* pair-list cut-off                            */
    real spacing;         /* (largest) PME grid spacing                   */
    ivec grid;            /* the PME grid dimensions                      */
    real grid_efficiency; /* ineffiency factor for non-uniform grids <= 1 */
    real ewaldcoeff;      /* the Ewald coefficient                        */
    gmx_pme_t pmedata;    /* the data structure used in the PME code      */

    int  count;           /* number of times this setup has been timed    */
    double cycles;        /* the fastest time for this setup in cycles    */
} pme_setup_t;

/* In the initial scan, step by grids that are at least a factor 0.8 coarser */
#define PME_LB_GRID_SCALE_FAC  0.8
/* In the initial scan, try to skip grids with uneven x/y/z spacing,
 * checking if the "efficiency" is more than 5% worse than the previous grid.
 */
#define PME_LB_GRID_EFFICIENCY_REL_FAC  1.05
/* Rerun up till 12% slower setups than the fastest up till now */
#define PME_LB_SLOW_FAC  1.12
/* If setups get more than 2% faster, do another round to avoid
 * choosing a slower setup due to acceleration or fluctuations.
 */
#define PME_LB_ACCEL_TOL 1.02

enum { epmelblimNO, epmelblimBOX, epmelblimDD, epmelblimNR };

const char *pmelblim_str[epmelblimNR] =
{ "no", "box size", "domain decompostion" };

struct pme_load_balancing {
    int  nstage;        /* the current maximum number of stages */

    real cut_spacing;   /* the minimum cutoff / PME grid spacing ratio */
    real rbuf;          /* the pairlist buffer size */
    matrix box_start;   /* the initial simulation box */
    int n;              /* the count of setup as well as the allocation size */
    pme_setup_t *setup; /* the PME+cutoff setups */
    int cur;            /* the current setup */
    int fastest;        /* fastest setup up till now */
    int start;          /* start of setup range to consider in stage>0 */
    int end;            /* end   of setup range to consider in stage>0 */
    int elimited;       /* was the balancing limited, uses enum above */

    int stage;          /* the current stage */
};

void pme_loadbal_init(pme_load_balancing_t *pme_lb_p,
                      const t_inputrec *ir,matrix box,
                      const interaction_const_t *ic,
                      gmx_pme_t pmedata)
{
    pme_load_balancing_t pme_lb;
    real spm,sp;
    int  d;

    snew(pme_lb,1);

    /* Any number of stages >= 2 is supported */
    pme_lb->nstage   = 2;

    pme_lb->rbuf = ic->rlist - ic->rcoulomb;

    copy_mat(box,pme_lb->box_start);
    if (ir->ePBC==epbcXY && ir->nwall==2)
    {
        svmul(ir->wall_ewald_zfac,pme_lb->box_start[ZZ],pme_lb->box_start[ZZ]);
    }

    pme_lb->n = 1;
    snew(pme_lb->setup,pme_lb->n);

    pme_lb->cur = 0;
    pme_lb->setup[0].rcut       = ic->rcoulomb;
    pme_lb->setup[0].rlist      = ic->rlist;
    pme_lb->setup[0].grid[XX]   = ir->nkx;
    pme_lb->setup[0].grid[YY]   = ir->nky;
    pme_lb->setup[0].grid[ZZ]   = ir->nkz;
    pme_lb->setup[0].ewaldcoeff = ic->ewaldcoeff;

    pme_lb->setup[0].pmedata  = pmedata;
    
    spm = 0;
    for(d=0; d<DIM; d++)
    {
        sp = norm(pme_lb->box_start[d])/pme_lb->setup[0].grid[d];
        if (sp > spm)
        {
            spm = sp;
        }
    }
    pme_lb->setup[0].spacing = spm;

    if (ir->fourier_spacing > 0)
    {
        pme_lb->cut_spacing = ir->rcoulomb/ir->fourier_spacing;
    }
    else
    {
        pme_lb->cut_spacing = ir->rcoulomb/pme_lb->setup[0].spacing;
    }

    pme_lb->stage = 0;

    pme_lb->fastest  = 0;
    pme_lb->start    = 0;
    pme_lb->end      = 0;
    pme_lb->elimited = epmelblimNO;

    *pme_lb_p = pme_lb;
}

static gmx_bool pme_loadbal_increase_cutoff(pme_load_balancing_t pme_lb,
                                            int pme_order)
{
    pme_setup_t *set;
    real fac,sp;
    int d;

    /* Try to add a new setup with next larger cut-off to the list */
    pme_lb->n++;
    srenew(pme_lb->setup,pme_lb->n);
    set = &pme_lb->setup[pme_lb->n-1];
    set->pmedata = NULL;

    fac = 1;
    do
    {
        fac *= 1.01;
        clear_ivec(set->grid);
        sp = calc_grid(NULL,pme_lb->box_start,
                       fac*pme_lb->setup[pme_lb->cur].spacing,
                       &set->grid[XX],
                       &set->grid[YY],
                       &set->grid[ZZ]);

        /* In parallel we can't have grids smaller than 2*pme_order,
         * and we would anyhow not gain much speed at these grid sizes.
         */
        for(d=0; d<DIM; d++)
        {
            if (set->grid[d] <= 2*pme_order)
            {
                pme_lb->n--;

                return FALSE;
            }
        }
    }
    while (sp <= 1.001*pme_lb->setup[pme_lb->cur].spacing);

    set->rcut    = pme_lb->cut_spacing*sp;
    set->rlist   = set->rcut + pme_lb->rbuf;
    set->spacing = sp;
    /* The grid efficiency is the size wrt a grid with uniform x/y/z spacing */
    set->grid_efficiency = 1;
    for(d=0; d<DIM; d++)
    {
        set->grid_efficiency *= (set->grid[d]*sp)/norm(pme_lb->box_start[d]);
    }
    /* The Ewald coefficient is inversly proportional to the cut-off */
    set->ewaldcoeff =
        pme_lb->setup[0].ewaldcoeff*pme_lb->setup[0].rcut/set->rcut;

    set->count   = 0;
    set->cycles  = 0;

    if (debug)
    {
        fprintf(debug,"PME loadbal: grid %d %d %d, cutoff %f\n",
                set->grid[XX],set->grid[YY],set->grid[ZZ],set->rcut);
    }

    return TRUE;
}

static void print_grid(FILE *fp_err,FILE *fp_log,
                       const char *pre,
                       const char *desc,
                       const pme_setup_t *set,
                       double cycles)
{
    char buf[STRLEN],buft[STRLEN];
    
    if (cycles >= 0)
    {
        sprintf(buft,": %.1f M-cycles",cycles*1e-6);
    }
    else
    {
        buft[0] = '\0';
    }
    sprintf(buf,"%-11s%10s pme grid %d %d %d, cutoff %.3f%s",
            pre,
            desc,set->grid[XX],set->grid[YY],set->grid[ZZ],set->rcut,
            buft);
    if (fp_err != NULL)
    {
        fprintf(fp_err,"\r%s\n",buf);
    }
    if (fp_log != NULL)
    {
        fprintf(fp_log,"%s\n",buf);
    }
}

static int pme_loadbal_end(pme_load_balancing_t pme_lb)
{
    /* In the initial stage only n is set; end is not set yet */
    if (pme_lb->end > 0)
    {
        return pme_lb->end;
    }
    else
    {
        return pme_lb->n;
    }
}

static void print_loadbal_limited(FILE *fp_err,FILE *fp_log,
                                  gmx_large_int_t step,
                                  pme_load_balancing_t pme_lb)
{
    char buf[STRLEN],sbuf[22];

    sprintf(buf,"step %4s: the %s limited the PME load balancing to a cut-off of %.3f",
            gmx_step_str(step,sbuf),
            pmelblim_str[pme_lb->elimited],
            pme_lb->setup[pme_loadbal_end(pme_lb)-1].rcut);
    if (fp_err != NULL)
    {
        fprintf(fp_err,"\r%s\n",buf);
    }
    if (fp_log != NULL)
    {
        fprintf(fp_log,"%s\n",buf);
    }
}

static void switch_to_stage1(pme_load_balancing_t pme_lb)
{
    pme_lb->start = 0;
    while (pme_lb->start+1 < pme_lb->n &&
           (pme_lb->setup[pme_lb->start].count == 0 ||
            pme_lb->setup[pme_lb->start].cycles >
            pme_lb->setup[pme_lb->fastest].cycles*PME_LB_SLOW_FAC))
    {
        pme_lb->start++;
    }
    while (pme_lb->start > 0 && pme_lb->setup[pme_lb->start-1].cycles == 0)
    {
        pme_lb->start--;
    }

    pme_lb->end = pme_lb->n;
    if (pme_lb->setup[pme_lb->end-1].count > 0 &&
        pme_lb->setup[pme_lb->end-1].cycles >
        pme_lb->setup[pme_lb->fastest].cycles*PME_LB_SLOW_FAC)
    {
        pme_lb->end--;
    }

    pme_lb->stage = 1;

    /* Next we want to choose setup pme_lb->start, but as we will increase
     * pme_ln->cur by one right after returning, we subtract 1 here.
     */
    pme_lb->cur = pme_lb->start - 1;
}

gmx_bool pme_load_balance(pme_load_balancing_t pme_lb,
                          t_commrec *cr,
                          FILE *fp_err,
                          FILE *fp_log,
                          t_inputrec *ir,
                          t_state *state,
                          double cycles,
                          interaction_const_t *ic,
                          nonbonded_verlet_t *nbv,
                          gmx_pme_t *pmedata,
                          gmx_large_int_t step)
{
    gmx_bool OK;
    pme_setup_t *set;
    double cycles_fast;
    char buf[STRLEN],sbuf[22];

    if (pme_lb->stage == pme_lb->nstage)
    {
        return FALSE;
    }

    if (PAR(cr))
    {
        gmx_sumd(1,&cycles,cr);
        cycles /= cr->nnodes;
    }

    set = &pme_lb->setup[pme_lb->cur];

    set->count++;
    if (set->count % 2 == 1)
    {
        /* Skip the first cycle, because the first step after a switch
         * is much slower due to allocation and/or caching effects.
         */
        return TRUE;
    }

    sprintf(buf, "step %4s: ", gmx_step_str(step,sbuf));
    print_grid(fp_err,fp_log,buf,"timed with",set,cycles);

    if (set->count <= 2)
    {
        set->cycles = cycles;
    }
    else
    {
        if (cycles*PME_LB_ACCEL_TOL < set->cycles &&
            pme_lb->stage == pme_lb->nstage - 1)
        {
            /* The performance went up a lot (due to e.g. DD load balancing).
             * Add a stage, keep the minima, but rescan all setups.
             */
            pme_lb->nstage++;

            if (debug)
            {
                fprintf(debug,"The performance for grid %d %d %d went from %.3f to %.1f M-cycles, this is more than %f\n"
                        "Increased the number stages to %d"
                        " and ignoring the previous performance\n",
                        set->grid[XX],set->grid[YY],set->grid[ZZ],
                        cycles*1e-6,set->cycles*1e-6,PME_LB_ACCEL_TOL,
                        pme_lb->nstage);
            }
        }
        set->cycles = min(set->cycles,cycles);
    }

    if (set->cycles < pme_lb->setup[pme_lb->fastest].cycles)
    {
        pme_lb->fastest = pme_lb->cur;
    }
    cycles_fast = pme_lb->setup[pme_lb->fastest].cycles;

    /* Check in stage 0 if we should stop scanning grids.
     * Stop when the time is more than SLOW_FAC longer than the fastest.
     */
    if (pme_lb->stage == 0 && pme_lb->cur > 0 &&
        cycles > pme_lb->setup[pme_lb->fastest].cycles*PME_LB_SLOW_FAC)
    {
        pme_lb->n = pme_lb->cur + 1;
        /* Done with scanning, go to stage 1 */
        switch_to_stage1(pme_lb);
    }

    if (pme_lb->stage == 0)
    {
        int gridsize_start;

        gridsize_start = set->grid[XX]*set->grid[YY]*set->grid[ZZ];

        do
        {
            if (pme_lb->cur+1 < pme_lb->n)
            {
                /* We had already generated the next setup */
                OK = TRUE;
            }
            else
            {
                /* Find the next setup */
                OK = pme_loadbal_increase_cutoff(pme_lb,ir->pme_order);
            }
                
            if (OK && ir->ePBC != epbcNONE)
            {
                OK = (sqr(pme_lb->setup[pme_lb->cur+1].rlist)
                      <= max_cutoff2(ir->ePBC,state->box));
                if (!OK)
                {
                    pme_lb->elimited = epmelblimBOX;
                }
            }

            if (OK)
            {
                pme_lb->cur++;

                if (DOMAINDECOMP(cr))
                {
                    OK = change_dd_cutoff(cr,state,ir,
                                          pme_lb->setup[pme_lb->cur].rlist);
                    if (!OK)
                    {
                        /* Failed: do not use this setup */
                        pme_lb->cur--;
                        pme_lb->elimited = epmelblimDD;
                    }
                }
            }
            if (!OK)
            {
                /* We hit the upper limit for the cut-off,
                 * the setup should not go further than cur.
                 */
                pme_lb->n = pme_lb->cur + 1;
                print_loadbal_limited(fp_err,fp_log,step,pme_lb);
                /* Switch to the next stage */
                switch_to_stage1(pme_lb);
            }
        }
        while (OK &&
               !(pme_lb->setup[pme_lb->cur].grid[XX]*
                 pme_lb->setup[pme_lb->cur].grid[YY]*
                 pme_lb->setup[pme_lb->cur].grid[ZZ] <
                 gridsize_start*PME_LB_GRID_SCALE_FAC
                 &&
                 pme_lb->setup[pme_lb->cur].grid_efficiency <
                 pme_lb->setup[pme_lb->cur-1].grid_efficiency*PME_LB_GRID_EFFICIENCY_REL_FAC));
    }

    if (pme_lb->stage > 0 && pme_lb->end == 1)
    {
        pme_lb->cur = 0;
        pme_lb->stage = pme_lb->nstage;
    }
    else if (pme_lb->stage > 0 && pme_lb->end > 1)
    {
        /* If stage = nstage-1:
         *   scan over all setups, rerunning only those setups
         *   which are not much slower than the fastest
         * else:
         *   use the next setup
         */
        do
        {
            pme_lb->cur++;
            if (pme_lb->cur == pme_lb->end)
            {
                pme_lb->stage++;
                pme_lb->cur = pme_lb->start;
            }
        }
        while (pme_lb->stage == pme_lb->nstage - 1 &&
               pme_lb->setup[pme_lb->cur].count > 0 &&
               pme_lb->setup[pme_lb->cur].cycles > cycles_fast*PME_LB_SLOW_FAC);

        if (pme_lb->stage == pme_lb->nstage)
        {
            /* We are done optimizing, use the fastest setup we found */
            pme_lb->cur = pme_lb->fastest;
        }
    }

    if (DOMAINDECOMP(cr) && pme_lb->stage > 0)
    {
        OK = change_dd_cutoff(cr,state,ir,pme_lb->setup[pme_lb->cur].rlist);
        if (!OK)
        {
            /* Failsafe solution */
            if (pme_lb->cur > 1 && pme_lb->stage == pme_lb->nstage)
            {
                pme_lb->stage--;
            }
            pme_lb->fastest  = 0;
            pme_lb->start    = 0;
            pme_lb->end      = pme_lb->cur;
            pme_lb->cur      = pme_lb->start;
            pme_lb->elimited = epmelblimDD;
            print_loadbal_limited(fp_err,fp_log,step,pme_lb);
        }
    }

    /* Change the Coulomb cut-off and the PME grid */

    set = &pme_lb->setup[pme_lb->cur];

    ic->rcoulomb   = set->rcut;
    ic->rlist      = set->rlist;
    ic->ewaldcoeff = set->ewaldcoeff;

    if (nbv->grp[0].kernel_type == nbk8x8x8_CUDA)
    {
        nbnxn_cuda_pme_loadbal_update_param(nbv->cu_nbv,ic);
    }
    else
    {
        init_interaction_const_tables(NULL,ic,nbv->grp[0].kernel_type);
    }

    if (nbv->ngrp > 1)
    {
        init_interaction_const_tables(NULL,ic,nbv->grp[1].kernel_type);
    }

    if (cr->duty & DUTY_PME)
    {
        if (pme_lb->setup[pme_lb->cur].pmedata == NULL)
        {
            /* Generate a new PME data structure,
             * copying part of the old pointers.
             */
            gmx_pme_reinit(&set->pmedata,
                           cr,pme_lb->setup[0].pmedata,ir,
                           set->grid);
        }
        *pmedata = set->pmedata;
    }
    else
    {
        /* Tell our PME-only node to switch grid */
        gmx_pme_send_switch(cr, set->grid, set->ewaldcoeff);
    }

    if (debug)
    {
        print_grid(NULL,debug,"","switched to",set,-1);
    }

    if (pme_lb->stage == pme_lb->nstage)
    {
        print_grid(fp_err,fp_log,"","optimal",set,-1);
    }

    return TRUE;
}

void restart_pme_loadbal(pme_load_balancing_t pme_lb, int n)
{
    pme_lb->nstage += n;
}

static int pme_grid_points(const pme_setup_t *setup)
{
    return setup->grid[XX]*setup->grid[YY]*setup->grid[ZZ];
}

static void print_pme_loadbal_setting(FILE *fplog,
                                     char *name,
                                     const pme_setup_t *setup)
{
    fprintf(fplog,
            "   %-7s %6.3f nm %6.3f nm     %3d %3d %3d   %5.3f nm  %5.3f nm\n",
            name,
            setup->rcut,setup->rlist,
            setup->grid[XX],setup->grid[YY],setup->grid[ZZ],
            setup->spacing,1/setup->ewaldcoeff);
}

static void print_pme_loadbal_settings(pme_load_balancing_t pme_lb,
                                       FILE *fplog)
{
    double pp_ratio,grid_ratio;

    pp_ratio   = pow(pme_lb->setup[pme_lb->cur].rlist/pme_lb->setup[0].rlist,3.0);
    grid_ratio = pme_grid_points(&pme_lb->setup[pme_lb->cur])/
        (double)pme_grid_points(&pme_lb->setup[0]);

    fprintf(fplog,"\n");
    fprintf(fplog,"       P P   -   P M E   L O A D   B A L A N C I N G\n");
    fprintf(fplog,"\n");
    /* Here we only warn when the optimal setting is the last one */
    if (pme_lb->elimited != epmelblimNO &&
        pme_lb->cur == pme_loadbal_end(pme_lb)-1)
    {
        fprintf(fplog," NOTE: The PP/PME load balancing was limited by the %s,\n",
                pmelblim_str[pme_lb->elimited]);
        fprintf(fplog,"       you might not have reached a good load balance.\n");
        if (pme_lb->elimited == epmelblimDD)
        {
            fprintf(fplog,"       Try different mdrun -dd settings or lower the -dds value.\n");
        }
        fprintf(fplog,"\n");
    }
    fprintf(fplog," PP/PME load balancing changed the cut-off and PME settings:\n");
    fprintf(fplog,"           particle-particle                    PME\n");
    fprintf(fplog,"            rcoulomb  rlist            grid      spacing   1/beta\n");
    print_pme_loadbal_setting(fplog,"initial",&pme_lb->setup[0]);
    print_pme_loadbal_setting(fplog,"final"  ,&pme_lb->setup[pme_lb->cur]);
    fprintf(fplog," cost-ratio           %4.2f             %4.2f\n",
            pp_ratio,grid_ratio);
    fprintf(fplog," (note that these numbers concern only part of the total PP and PME load)\n");
    fprintf(fplog,"\n");
}

void pme_loadbal_done(pme_load_balancing_t pme_lb, FILE *fplog)
{
    if (fplog != NULL && (pme_lb->cur > 0 || pme_lb->elimited != epmelblimNO))
    {
        print_pme_loadbal_settings(pme_lb,fplog);
    }

    /* TODO: Here we should free all pointers in pme_lb,
     * but as it contains pme data structures,
     * we need to first make pme.c free all data.
     */
}
