/* Copyright (c) 2007-2009 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 */

#include <stdlib.h>
#include <math.h>
#include <float.h>

#include "nlopt-internal.h"

/*********************************************************************/

#ifndef HAVE_ISNAN
static int my_isnan(double x) { return x != x; }
#  define isnan my_isnan
#endif

/*********************************************************************/

#include "praxis.h"
#include "direct.h"

#ifdef WITH_CXX
#  include "stogo.h"
#endif

#include "cdirect.h"

#ifdef WITH_NOCEDAL
#  include "l-bfgs-b.h"
#endif

#include "luksan.h"

#include "crs.h"

#include "mlsl.h"
#include "mma.h"
#include "cobyla.h"
#include "newuoa.h"
#include "neldermead.h"
#include "auglag.h"
#include "bobyqa.h"
#include "isres.h"

/*********************************************************************/

typedef struct {
     nlopt_func f;
     void *f_data;
     const double *lb, *ub;
} nlopt_data;

#include "praxis.h"

static double f_bound(int n, const double *x, void *data_)
{
     int i;
     nlopt_data *data = (nlopt_data *) data_;
     double f;

     /* some methods do not support bound constraints, but support
	discontinuous objectives so we can just return Inf for invalid x */
     for (i = 0; i < n; ++i)
	  if (x[i] < data->lb[i] || x[i] > data->ub[i])
	       return HUGE_VAL;

     f = data->f(n, x, NULL, data->f_data);
     return (isnan(f) || nlopt_isinf(f) ? HUGE_VAL : f);
}

static double f_noderiv(int n, const double *x, void *data_)
{
     nlopt_data *data = (nlopt_data *) data_;
     return data->f(n, x, NULL, data->f_data);
}

static double f_direct(int n, const double *x, int *undefined, void *data_)
{
     nlopt_data *data = (nlopt_data *) data_;
     double f;
     f = data->f(n, x, NULL, data->f_data);
     *undefined = isnan(f) || nlopt_isinf(f);
     return f;
}

/*********************************************************************/

/* get min(dx) for algorithms requiring a scalar initial step size */
static nlopt_result initial_step(nlopt_opt opt, const double *x, double *step)
{
     int freedx = 0, i;

     if (!opt->dx) {
	  freedx = 1;
	  if (nlopt_set_default_initial_step(opt, x) != NLOPT_SUCCESS)
	       return NLOPT_OUT_OF_MEMORY;
     }

     *step = HUGE_VAL;
     for (i = 0; i < opt->n; ++i)
	  if (*step > fabs(opt->dx[i]))
	       *step = fabs(opt->dx[i]);

     if (freedx) { free(opt->dx); opt->dx = NULL; }
     return NLOPT_SUCCESS;
}

/*********************************************************************/

#define POP(defaultpop) (opt->stochastic_population > 0 ?		\
                         opt->stochastic_population :			\
                         (nlopt_stochastic_population > 0 ?		\
			  nlopt_stochastic_population : (defaultpop)))

nlopt_result nlopt_optimize(nlopt_opt opt, double *x, double *minf)
{
     const double *lb, *ub;
     nlopt_algorithm algorithm;
     nlopt_func f; void *f_data;
     int n, i;
     nlopt_data d;
     nlopt_stopping stop;

     if (!opt || !x || !minf || !opt->f) return NLOPT_INVALID_ARGS;

     /* copy a few params to local vars for convenience */
     n = opt->n;
     lb = opt->lb; ub = opt->ub;
     algorithm = opt->algorithm;
     f = opt->f; f_data = opt->f_data;

     if (n == 0) { /* trivial case: no degrees of freedom */
	  *minf = f(n, x, NULL, f_data);
	  return NLOPT_SUCCESS;
     }

     *minf = HUGE_VAL;
     
     d.f = f;
     d.f_data = f_data;
     d.lb = lb;
     d.ub = ub;

     /* make sure rand generator is inited */
     if (!nlopt_srand_called)
	  nlopt_srand_time(); /* default is non-deterministic */

     /* check bound constraints */
     for (i = 0; i < n; ++i)
	  if (lb[i] > ub[i] || x[i] < lb[i] || x[i] > ub[i])
	       return NLOPT_INVALID_ARGS;

     stop.n = n;
     stop.minf_max = opt->minf_max;
     stop.ftol_rel = opt->ftol_rel;
     stop.ftol_abs = opt->ftol_abs;
     stop.xtol_rel = opt->xtol_rel;
     stop.xtol_abs = opt->xtol_abs;
     stop.nevals = 0;
     stop.maxeval = opt->maxeval;
     stop.maxtime = opt->maxtime;
     stop.start = nlopt_seconds();

     switch (algorithm) {
	 case NLOPT_GN_DIRECT:
	 case NLOPT_GN_DIRECT_L: 
	 case NLOPT_GN_DIRECT_L_RAND: 
	      return cdirect(n, f, f_data, 
			     lb, ub, x, minf, &stop, 0.0, 
			     (algorithm != NLOPT_GN_DIRECT)
			     + 3 * (algorithm == NLOPT_GN_DIRECT_L_RAND 
				    ? 2 : (algorithm != NLOPT_GN_DIRECT))
			     + 9 * (algorithm == NLOPT_GN_DIRECT_L_RAND 
				    ? 1 : (algorithm != NLOPT_GN_DIRECT)));
	      
	 case NLOPT_GN_DIRECT_NOSCAL:
	 case NLOPT_GN_DIRECT_L_NOSCAL: 
	 case NLOPT_GN_DIRECT_L_RAND_NOSCAL: 
	      return cdirect_unscaled(n, f, f_data, lb, ub, x, minf, 
				      &stop, 0.0, 
				      (algorithm != NLOPT_GN_DIRECT)
				      + 3 * (algorithm == NLOPT_GN_DIRECT_L_RAND ? 2 : (algorithm != NLOPT_GN_DIRECT))
				      + 9 * (algorithm == NLOPT_GN_DIRECT_L_RAND ? 1 : (algorithm != NLOPT_GN_DIRECT)));
	      
	 case NLOPT_GN_ORIG_DIRECT:
	 case NLOPT_GN_ORIG_DIRECT_L: 
	      switch (direct_optimize(f_direct, &d, n, lb, ub, x, minf,
				      stop.maxeval, -1, 0.0, 0.0,
				      pow(stop.xtol_rel, (double) n), -1.0,
				      stop.minf_max, 0.0,
				      NULL, 
				      algorithm == NLOPT_GN_ORIG_DIRECT
				      ? DIRECT_ORIGINAL
				      : DIRECT_GABLONSKY)) {
		  case DIRECT_INVALID_BOUNDS:
		  case DIRECT_MAXFEVAL_TOOBIG:
		  case DIRECT_INVALID_ARGS:
		       return NLOPT_INVALID_ARGS;
		  case DIRECT_INIT_FAILED:
		  case DIRECT_SAMPLEPOINTS_FAILED:
		  case DIRECT_SAMPLE_FAILED:
		       return NLOPT_FAILURE;
		  case DIRECT_MAXFEVAL_EXCEEDED:
		  case DIRECT_MAXITER_EXCEEDED:
		       return NLOPT_MAXEVAL_REACHED;
		  case DIRECT_GLOBAL_FOUND:
		       return NLOPT_MINF_MAX_REACHED;
		  case DIRECT_VOLTOL:
		  case DIRECT_SIGMATOL:
		       return NLOPT_XTOL_REACHED;
		  case DIRECT_OUT_OF_MEMORY:
		       return NLOPT_OUT_OF_MEMORY;
	      break;
	 }

	 case NLOPT_GD_STOGO:
	 case NLOPT_GD_STOGO_RAND:
#ifdef WITH_CXX
	      if (!stogo_minimize(n, f, f_data, x, minf, lb, ub, &stop,
				  algorithm == NLOPT_GD_STOGO
				  ? 0 : POP(2*n)))
		   return NLOPT_FAILURE;
	      break;
#else
	      return NLOPT_FAILURE;
#endif

#if 0
	      /* lacking a free/open-source license, we no longer use
		 Rowan's code, and instead use by "sbplx" re-implementation */
	 case NLOPT_LN_SUBPLEX: {
	      int iret, freedx = 0;
	      if (!opt->dx) {
		   freedx = 1;
		   if (nlopt_set_default_initial_step(opt, x) != NLOPT_SUCCESS)
			return NLOPT_OUT_OF_MEMORY;
	      }		       
	      iret = nlopt_subplex(f_bound, minf, x, n, &d, &stop, opt->dx);
	      if (freedx) { free(opt->dx); opt->dx = NULL; }
	      switch (iret) {
		  case -2: return NLOPT_INVALID_ARGS;
		  case -10: return NLOPT_MAXTIME_REACHED;
		  case -1: return NLOPT_MAXEVAL_REACHED;
		  case 0: return NLOPT_XTOL_REACHED;
		  case 1: return NLOPT_SUCCESS;
		  case 2: return NLOPT_MINF_MAX_REACHED;
		  case 20: return NLOPT_FTOL_REACHED;
		  case -200: return NLOPT_OUT_OF_MEMORY;
		  default: return NLOPT_FAILURE; /* unknown return code */
	      }
	      break;
	 }
#endif

	 case NLOPT_LN_PRAXIS: {
	      double step;
	      if (initial_step(opt, x, &step) != NLOPT_SUCCESS)
		   return NLOPT_OUT_OF_MEMORY;
	      return praxis_(0.0, DBL_EPSILON, 
			     step, n, x, f_bound, &d, &stop, minf);
	 }

#ifdef WITH_NOCEDAL
	 case NLOPT_LD_LBFGS_NOCEDAL: {
	      int iret, *nbd = (int *) malloc(sizeof(int) * n);
	      if (!nbd) return NLOPT_OUT_OF_MEMORY;
	      for (i = 0; i < n; ++i) {
		   int linf = nlopt_isinf(lb[i]) && lb[i] < 0;
		   int uinf = nlopt_isinf(ub[i]) && ub[i] > 0;
		   nbd[i] = linf && uinf ? 0 : (uinf ? 1 : (linf ? 3 : 2));
	      }
	      iret = lbfgsb_minimize(n, f, f_data, x, nbd, lb, ub,
				     MIN(n, 5), 0.0, stop.ftol_rel, 
				     stop.xtol_abs[0] > 0 ? stop.xtol_abs[0]
				     : stop.xtol_rel,
				     stop.maxeval);
	      free(nbd);
	      if (iret <= 0) {
		   switch (iret) {
		       case -1: return NLOPT_INVALID_ARGS;
		       case -2: default: return NLOPT_FAILURE;
		   }
	      }
	      else {
		   *minf = f(n, x, NULL, f_data);
		   switch (iret) {
		       case 5: return NLOPT_MAXEVAL_REACHED;
		       case 2: return NLOPT_XTOL_REACHED;
		       case 1: return NLOPT_FTOL_REACHED;
		       default: return NLOPT_SUCCESS;
		   }
	      }
	      break;
	 }
#endif

	 case NLOPT_LD_LBFGS: 
	      return luksan_plis(n, f, f_data, lb, ub, x, minf, &stop);

	 case NLOPT_LD_VAR1: 
	 case NLOPT_LD_VAR2: 
	      return luksan_plip(n, f, f_data, lb, ub, x, minf, &stop,
		   algorithm == NLOPT_LD_VAR1 ? 1 : 2);

	 case NLOPT_LD_TNEWTON: 
	 case NLOPT_LD_TNEWTON_RESTART: 
	 case NLOPT_LD_TNEWTON_PRECOND: 
	 case NLOPT_LD_TNEWTON_PRECOND_RESTART: 
	      return luksan_pnet(n, f, f_data, lb, ub, x, minf, &stop,
				 1 + (algorithm - NLOPT_LD_TNEWTON) % 2,
				 1 + (algorithm - NLOPT_LD_TNEWTON) / 2);

	 case NLOPT_GN_CRS2_LM:
	      return crs_minimize(n, f, f_data, lb, ub, x, minf, &stop, 
				  POP(0), 0);

	 case NLOPT_GN_MLSL:
	 case NLOPT_GD_MLSL:
	 case NLOPT_GN_MLSL_LDS:
	 case NLOPT_GD_MLSL_LDS: {
	      nlopt_opt local_opt = opt->local_opt;
	      nlopt_result ret;
	      if (!local_opt) { /* default */
		   local_opt = nlopt_create((algorithm == NLOPT_GN_MLSL ||
					     algorithm == NLOPT_GN_MLSL_LDS)
					    ? nlopt_local_search_alg_nonderiv
					    : nlopt_local_search_alg_deriv, n);
		   if (!local_opt) return NLOPT_FAILURE;
		   nlopt_set_ftol_rel(local_opt, opt->ftol_rel);
		   nlopt_set_ftol_abs(local_opt, opt->ftol_abs);
		   nlopt_set_xtol_rel(local_opt, opt->xtol_rel);
		   nlopt_set_xtol_abs(local_opt, opt->xtol_abs);
		   nlopt_set_maxeval(local_opt, nlopt_local_search_maxeval);
		   nlopt_set_initial_step(local_opt, opt->dx);
	      }
	      for (i = 0; i < n && stop.xtol_abs[i] > 0; ++i) ;
	      if (local_opt->ftol_rel <= 0 && local_opt->ftol_abs <= 0 &&
		  local_opt->xtol_rel <= 0 && i < n) {
		   /* it is not sensible to call MLSL without *some*
		      nonzero tolerance for the local search */
		   nlopt_set_ftol_rel(local_opt, 1e-15);
		   nlopt_set_xtol_rel(local_opt, 1e-7);
	      }
	      ret = mlsl_minimize(n, f, f_data, lb, ub, x, minf, &stop,
				  local_opt, POP(0),
				  algorithm >= NLOPT_GN_MLSL_LDS);
	      if (!opt->local_opt) nlopt_destroy(local_opt);
	      return ret;
	 }

	 case NLOPT_LD_MMA: {
	      nlopt_opt dual_opt;
	      nlopt_result ret;
#define LO(param, def) (opt->local_opt ? opt->local_opt->param : (def))
	      dual_opt = nlopt_create(LO(algorithm,
					 nlopt_local_search_alg_deriv),
				      opt->m);
	      if (!dual_opt) return NLOPT_FAILURE;
	      nlopt_set_ftol_rel(dual_opt, LO(ftol_rel, 1e-12));
	      nlopt_set_ftol_abs(dual_opt, LO(ftol_abs, 0.0));
	      nlopt_set_maxeval(dual_opt, LO(maxeval, 100000));
#undef LO

	      ret = mma_minimize(n, f, f_data, opt->m, opt->fc,
				 lb, ub, x, minf, &stop, dual_opt);
	      nlopt_destroy(dual_opt);
	      return ret;
	 }

	 case NLOPT_LN_COBYLA: {
	      double step;
	      if (initial_step(opt, x, &step) != NLOPT_SUCCESS)
		   return NLOPT_OUT_OF_MEMORY;
	      return cobyla_minimize(n, f, f_data, 
				     opt->m, opt->fc,
				     lb, ub, x, minf, &stop,
				     step);
	 }
				     
	 case NLOPT_LN_NEWUOA: {
	      double step;
	      if (initial_step(opt, x, &step) != NLOPT_SUCCESS)
		   return NLOPT_OUT_OF_MEMORY;
	      return newuoa(n, 2*n+1, x, 0, 0, step,
			    &stop, minf, f_noderiv, &d);
	 }
				     
	 case NLOPT_LN_NEWUOA_BOUND: {
	      double step;
	      if (initial_step(opt, x, &step) != NLOPT_SUCCESS)
		   return NLOPT_OUT_OF_MEMORY;
	      return newuoa(n, 2*n+1, x, lb, ub, step,
			    &stop, minf, f_noderiv, &d);
	 }

	 case NLOPT_LN_BOBYQA: {
	      double step;
	      if (initial_step(opt, x, &step) != NLOPT_SUCCESS)
		   return NLOPT_OUT_OF_MEMORY;
	      return bobyqa(n, 2*n+1, x, lb, ub, step,
			    &stop, minf, f_noderiv, &d);
	 }

	 case NLOPT_LN_NELDERMEAD: 
	 case NLOPT_LN_SBPLX: 
	 {
	      nlopt_result ret;
	      int freedx = 0;
	      if (!opt->dx) {
		   freedx = 1;
		   if (nlopt_set_default_initial_step(opt, x) != NLOPT_SUCCESS)
			return NLOPT_OUT_OF_MEMORY;
	      }
	      if (algorithm == NLOPT_LN_NELDERMEAD)
		   ret= nldrmd_minimize(n,f,f_data,lb,ub,x,minf,opt->dx,&stop);
	      else
		   ret= sbplx_minimize(n,f,f_data,lb,ub,x,minf,opt->dx,&stop);
	      if (freedx) { free(opt->dx); opt->dx = NULL; }
	      return ret;
	 }

	 case NLOPT_LN_AUGLAG:
	 case NLOPT_LN_AUGLAG_EQ:
	 case NLOPT_LD_AUGLAG:
	 case NLOPT_LD_AUGLAG_EQ: {
	      nlopt_opt local_opt = opt->local_opt;
	      nlopt_result ret;
	      if (!local_opt) { /* default */
		   local_opt = nlopt_create(
			algorithm == NLOPT_LN_AUGLAG || 
			algorithm == NLOPT_LN_AUGLAG_EQ
			? nlopt_local_search_alg_nonderiv
			: nlopt_local_search_alg_deriv, n);
		   if (!local_opt) return NLOPT_FAILURE;
		   nlopt_set_ftol_rel(local_opt, opt->ftol_rel);
		   nlopt_set_ftol_abs(local_opt, opt->ftol_abs);
		   nlopt_set_xtol_rel(local_opt, opt->xtol_rel);
		   nlopt_set_xtol_abs(local_opt, opt->xtol_abs);
		   nlopt_set_maxeval(local_opt, nlopt_local_search_maxeval);
		   nlopt_set_initial_step(local_opt, opt->dx);
	      }
	      ret = auglag_minimize(n, f, f_data, 
				    opt->m, opt->fc, 
				    opt->p, opt->h,
				    lb, ub, x, minf, &stop,
				    local_opt,
				    algorithm == NLOPT_LN_AUGLAG_EQ
				    || algorithm == NLOPT_LD_AUGLAG_EQ);
	      if (!opt->local_opt) nlopt_destroy(local_opt);
	      return ret;
	 }

	 case NLOPT_GN_ISRES:
	      return isres_minimize(n, f, f_data, 
				    opt->m, opt->fc,
				    opt->p, opt->h,
				    lb, ub, x, minf, &stop,
				    POP(0));

	 default:
	      return NLOPT_INVALID_ARGS;
     }

     return NLOPT_SUCCESS; /* never reached */
}

/*********************************************************************/

nlopt_result nlopt_optimize_limited(nlopt_opt opt, double *x, double *minf,
				    int maxeval, double maxtime)
{
     int save_maxeval;
     double save_maxtime;
     nlopt_result ret;

     if (!opt) return NLOPT_INVALID_ARGS;

     save_maxeval = nlopt_get_maxeval(opt);
     save_maxtime = nlopt_get_maxtime(opt);
     
     /* override opt limits if maxeval and/or maxtime are more stringent */
     if (save_maxeval <= 0 || (maxeval > 0 && maxeval < save_maxeval))
	  nlopt_set_maxeval(opt, maxeval);
     if (save_maxtime <= 0 || (maxtime > 0 && maxtime < save_maxtime))
	  nlopt_set_maxtime(opt, maxtime);

     ret = nlopt_optimize(opt, x, minf);

     nlopt_set_maxeval(opt, save_maxeval);
     nlopt_set_maxtime(opt, save_maxtime);

     return ret;
}

/*********************************************************************/