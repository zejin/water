#include "stepper.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdbool.h>

#include <omp.h>

//#define nbatch 10
//#define NUM_THREAD nthread

//extern int nthread;

//ldoc on
/**
 * ## Implementation
 *
 * ### Structure allocation
 */

central2d_t* __attribute__((target(mic))) central2d_init(float w, float h, int nx, int ny,
                            int nfield, flux_t flux, speed_t speed,
                            float cfl, int nbatch)
{
    // We extend to a four cell buffer to avoid BC comm on odd time steps
    int ng = nbatch;

    central2d_t* sim = (central2d_t*) malloc(sizeof(central2d_t));
    sim->nx = nx;
    sim->ny = ny;
    sim->ng = ng;
    sim->nfield = nfield;
    sim->dx = w/nx;
    sim->dy = h/ny;
    sim->flux = flux;
    sim->speed = speed;
    sim->cfl = cfl;

    int nx_all = nx + 2*ng;
    int ny_all = ny + 2*ng;
    int nc = nx_all * ny_all;
    int N  = nfield * nc;
    sim->u  = (float*) malloc((4*N + 6*nx_all)* sizeof(float));
    sim->v  = sim->u +   N;
    sim->f  = sim->u + 2*N;
    sim->g  = sim->u + 3*N;
    sim->scratch = sim->u + 4*N;

    return sim;
}


void __attribute__((target(mic))) central2d_free(central2d_t* sim)
{
    free(sim->u);
    free(sim);
}


int __attribute__((target(mic))) central2d_offset(central2d_t* sim, int k, int ix, int iy)
{
    int nx = sim->nx, ny = sim->ny, ng = sim->ng;
    int nx_all = nx + 2*ng;
    int ny_all = ny + 2*ng;
    return (k*ny_all+(ng+iy))*nx_all+(ng+ix);
}


/**
 * ### Boundary conditions
 *
 * In finite volume methods, boundary conditions are typically applied by
 * setting appropriate values in ghost cells.  For our framework, we will
 * apply periodic boundary conditions; that is, waves that exit one side
 * of the domain will enter from the other side.
 *
 * We apply the conditions by assuming that the cells with coordinates
 * `nghost <= ix <= nx+nghost` and `nghost <= iy <= ny+nghost` are
 * "canonical", and setting the values for all other cells `(ix,iy)`
 * to the corresponding canonical values `(ix+p*nx,iy+q*ny)` for some
 * integers `p` and `q`.
 */

static inline
void __attribute__((target(mic))) copy_subgrid(float* restrict dst,
                  const float* restrict src,
                  int nx, int ny, int stride)
{
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix)
            dst[iy*stride+ix] = src[iy*stride+ix];
}

void __attribute__((target(mic))) central2d_periodic(float* restrict u,
                        int nx, int ny, int ng, int nfield)
{
    // Stride and number per field
    int s = nx + 2*ng;
    int field_stride = (ny+2*ng)*s;

    // Offsets of left, right, top, and bottom data blocks and ghost blocks
    int l = nx,   lg = 0;
    int r = ng,   rg = nx+ng;
    int b = ny*s, bg = 0;
    int t = ng*s, tg = (nx+ng)*s;

    // Copy data into ghost cells on each side
    for (int k = 0; k < nfield; ++k) {
        float* uk = u + k*field_stride;
        copy_subgrid(uk+lg, uk+l, ng, ny+2*ng, s);
        copy_subgrid(uk+rg, uk+r, ng, ny+2*ng, s);
        copy_subgrid(uk+tg, uk+t, nx+2*ng, ng, s);
        copy_subgrid(uk+bg, uk+b, nx+2*ng, ng, s);
    }
}


/**
 * ### Derivatives with limiters
 *
 * In order to advance the time step, we also need to estimate
 * derivatives of the fluxes and the solution values at each cell.
 * In order to maintain stability, we apply a limiter here.
 *
 * The minmod limiter *looks* like it should be expensive to computer,
 * since superficially it seems to require a number of branches.
 * We do something a little tricky, getting rid of the condition
 * on the sign of the arguments using the `copysign` instruction.
 * If the compiler does the "right" thing with `max` and `min`
 * for floating point arguments (translating them to branch-free
 * intrinsic operations), this implementation should be relatively fast.
 */


// Branch-free computation of minmod of two numbers times 2s
static inline
float __attribute__((target(mic))) xmin2s(float s, float a, float b) {
    float sa = copysignf(s, a);
    float sb = copysignf(s, b);
    float abs_a = fabsf(a);
    float abs_b = fabsf(b);
    float min_abs = (abs_a < abs_b ? abs_a : abs_b);
    return (sa+sb) * min_abs;
}


// Limited combined slope estimate
static inline
float __attribute__((target(mic))) limdiff(float um, float u0, float up) {
    const float theta = 2.0;
    const float quarter = 0.25;
    float du1 = u0-um;   // Difference to left
    float du2 = up-u0;   // Difference to right
    float duc = up-um;   // Twice centered difference
    return xmin2s( quarter, xmin2s(theta, du1, du2), duc );
}


// Compute limited derivs
static inline
void __attribute__((target(mic))) limited_deriv1(float* restrict du,
                    const float* restrict u,
                    int ncell)
{
    for (int i = 0; i < ncell; ++i)
        du[i] = limdiff(u[i-1], u[i], u[i+1]);
}


// Compute limited derivs across stride
static inline
void __attribute__((target(mic))) limited_derivk(float* restrict du,
                    const float* restrict u,
                    int ncell, int stride)
{
    assert(stride > 0);
    for (int i = 0; i < ncell; ++i)
        du[i] = limdiff(u[i-stride], u[i], u[i+stride]);
}


/**
 * ### Advancing a time step
 *
 * Take one step of the numerical scheme.  This consists of two pieces:
 * a first-order corrector computed at a half time step, which is used
 * to obtain new $F$ and $G$ values; and a corrector step that computes
 * the solution at the full step.  For full details, we refer to the
 * [Jiang and Tadmor paper][jt].
 *
 * The `compute_step` function takes two arguments: the `io` flag
 * which is the time step modulo 2 (0 if even, 1 if odd); and the `dt`
 * flag, which actually determines the time step length.  We need
 * to know the even-vs-odd distinction because the Jiang-Tadmor
 * scheme alternates between a primary grid (on even steps) and a
 * staggered grid (on odd steps).  This means that the data at $(i,j)$
 * in an even step and the data at $(i,j)$ in an odd step represent
 * values at different locations in space, offset by half a space step
 * in each direction.  Every other step, we shift things back by one
 * mesh cell in each direction, essentially resetting to the primary
 * indexing scheme.
 *
 * We're slightly tricky in the corrector in that we write
 * $$
 *   v(i,j) = (s(i+1,j) + s(i,j)) - (d(i+1,j)-d(i,j))
 * $$
 * where $s(i,j)$ comprises the $u$ and $x$-derivative terms in the
 * update formula, and $d(i,j)$ the $y$-derivative terms.  This cuts
 * the arithmetic cost a little (not that it's that big to start).
 * It also makes it more obvious that we only need four rows worth
 * of scratch space.
 */


// Predictor half-step
static
void __attribute__((target(mic))) central2d_predict(float* restrict v,
                       float* restrict scratch,
                       const float* restrict u,
                       const float* restrict f,
                       const float* restrict g,
                       float dtcdx2, float dtcdy2,
                       int nx, int ny, int nfield)
{
    float* restrict fx = scratch;
    float* restrict gy = scratch+nx;
    for (int k = 0; k < nfield; ++k) {
        for (int iy = 1; iy < ny-1; ++iy) {
            int offset = (k*ny+iy)*nx+1;
            limited_deriv1(fx+1, f+offset, nx-2);
            limited_derivk(gy+1, g+offset, nx-2, nx);
            for (int ix = 1; ix < nx-1; ++ix) {
                int offset = (k*ny+iy)*nx+ix;
                v[offset] = u[offset] - dtcdx2 * fx[ix] - dtcdy2 * gy[ix];
            }
        }
    }
}


// Corrector
static
void __attribute__((target(mic))) central2d_correct_sd(float* restrict s,
                          float* restrict d,
                          const float* restrict ux,
                          const float* restrict uy,
                          const float* restrict u,
                          const float* restrict f,
                          const float* restrict g,
                          float dtcdx2, float dtcdy2,
                          int xlo, int xhi)
{
    for (int ix = xlo; ix < xhi; ++ix)
        s[ix] =
            0.2500f * (u [ix] + u [ix+1]) +
            0.0625f * (ux[ix] - ux[ix+1]) +
            dtcdx2  * (f [ix] - f [ix+1]);
    for (int ix = xlo; ix < xhi; ++ix)
        d[ix] =
            0.0625f * (uy[ix] + uy[ix+1]) +
            dtcdy2  * (g [ix] + g [ix+1]);
}


// Corrector
static
void __attribute__((target(mic))) central2d_correct(float* restrict v,
                       float* restrict scratch,
                       const float* restrict u,
                       const float* restrict f,
                       const float* restrict g,
                       float dtcdx2, float dtcdy2,
                       int xlo, int xhi, int ylo, int yhi,
                       int nx, int ny, int nfield)
{
    assert(0 <= xlo && xlo < xhi && xhi <= nx);
    assert(0 <= ylo && ylo < yhi && yhi <= ny);

    float* restrict ux = scratch;
    float* restrict uy = scratch +   nx;
    float* restrict s0 = scratch + 2*nx;
    float* restrict d0 = scratch + 3*nx;
    float* restrict s1 = scratch + 4*nx;
    float* restrict d1 = scratch + 5*nx;

    for (int k = 0; k < nfield; ++k) {

        float*       restrict vk = v + k*ny*nx;
        const float* restrict uk = u + k*ny*nx;
        const float* restrict fk = f + k*ny*nx;
        const float* restrict gk = g + k*ny*nx;

        limited_deriv1(ux+1, uk+ylo*nx+1, nx-2);
        limited_derivk(uy+1, uk+ylo*nx+1, nx-2, nx);
        central2d_correct_sd(s1, d1, ux, uy,
                             uk + ylo*nx, fk + ylo*nx, gk + ylo*nx,
                             dtcdx2, dtcdy2, xlo, xhi);

        for (int iy = ylo; iy < yhi; ++iy) {

            float* tmp;
            tmp = s0; s0 = s1; s1 = tmp;
            tmp = d0; d0 = d1; d1 = tmp;

            limited_deriv1(ux+1, uk+(iy+1)*nx+1, nx-2);
            limited_derivk(uy+1, uk+(iy+1)*nx+1, nx-2, nx);
            central2d_correct_sd(s1, d1, ux, uy,
                                 uk + (iy+1)*nx, fk + (iy+1)*nx, gk + (iy+1)*nx,
                                 dtcdx2, dtcdy2, xlo, xhi);

            for (int ix = xlo; ix < xhi; ++ix)
                vk[iy*nx+ix] = (s1[ix]+s0[ix])-(d1[ix]-d0[ix]);
        }
    }
}


static
void __attribute__((target(mic))) central2d_step(float* restrict u, float* restrict v,
                    float* restrict scratch,
                    float* restrict f,
                    float* restrict g,
                    int io, int nx, int ny, int ng,
                    int nfield, flux_t flux, speed_t speed,
                    float dt, float dx, float dy)
{
    int nx_all = nx + 2*ng;
    int ny_all = ny + 2*ng;

    float dtcdx2 = 0.5 * dt / dx;
    float dtcdy2 = 0.5 * dt / dy;

    flux(f, g, u, nx_all * ny_all, nx_all * ny_all);

    central2d_predict(v, scratch, u, f, g, dtcdx2, dtcdy2,
                      nx_all, ny_all, nfield);

    // Flux values of f and g at half step
    for (int iy = 1; iy < ny_all-1; ++iy) {
        int jj = iy*nx_all+1;
        flux(f+jj, g+jj, v+jj, nx_all-2, nx_all * ny_all);
    }

    central2d_correct(v+io*(nx_all+1), scratch, u, f, g, dtcdx2, dtcdy2,
                      ng-io, nx+ng-io,
                      ng-io, ny+ng-io,
                      nx_all, ny_all, nfield);

    for (int j = ng; j < ny+ng; ++j) {
        for (int i = ng; i < nx+ng; ++i) {
            u[j*nx_all + i] = v[(j-io)*nx_all+i-io];
            u[nx_all*ny_all+j*nx_all+i] = v[nx_all*ny_all+(j-io)*nx_all+i-io];
            u[nx_all*ny_all*2+j*nx_all+i] = v[nx_all*ny_all*2+(j-io)*nx_all+i-io];
        }
    }
}


/**
 * ### Advance a fixed time
 *
 * The `run` method advances from time 0 (initial conditions) to time
 * `tfinal`.  Note that `run` can be called repeatedly; for example,
 * we might want to advance for a period of time, write out a picture,
 * advance more, and write another picture.  In this sense, `tfinal`
 * should be interpreted as an offset from the time represented by
 * the simulator at the start of the call, rather than as an absolute time.
 *
 * We always take an even number of steps so that the solution
 * at the end lives on the main grid instead of the staggered grid.
 */

static
int central2d_yrun(float* restrict u, float* restrict v,
                   float* restrict scratch,
                   float* restrict f,
                   float* restrict g,
                   int nx, int ny, int ng,
                   int nfield, flux_t flux, speed_t speed,
                   float tfinal, float dx, float dy, float cfl, int nthread, int nbatch)
{
    int nstep = 0;
    int nx_all = nx + 2*ng;
    int ny_all = ny + 2*ng;
    bool done = false;
    float t = 0;

    float** f_block = (float**) malloc(nthread*sizeof(float*));
    float** g_block = (float**) malloc(nthread*sizeof(float*));
    float** u_block = (float**) malloc(nthread*sizeof(float*));
    float** v_block = (float**) malloc(nthread*sizeof(float*));
    float** scr_block = (float**) malloc(nthread*sizeof(float*));
  
    int block_size = nx / nthread;
    
    for(int i = 0; i < nthread; ++i) {
        u_block[i] = (float*) malloc(nx_all*nfield*(2*nbatch+block_size)*sizeof(float));
        v_block[i] = (float*) malloc(nx_all*nfield*(2*nbatch+block_size)*sizeof(float));
        f_block[i] = (float*) malloc(nx_all*nfield*(2*nbatch+block_size)*sizeof(float));
        g_block[i] = (float*) malloc(nx_all*nfield*(2*nbatch+block_size)*sizeof(float));
        scr_block[i] = (float*) malloc(nx_all*6*sizeof(float));
    }

    while (!done) {
        float cxy[2] = {1.0e-15f, 1.0e-15f};
        central2d_periodic(u, nx, ny, ng, nfield);
        speed(cxy, u, nx_all * ny_all, nx_all * ny_all);
        float dt = cfl / fmaxf(cxy[0]/dx, cxy[1]/dy);
        if (t + nbatch*dt >= tfinal) {
            dt = (tfinal-t)/nbatch;
            done = true;
        }

        #pragma omp parallel num_threads(nthread) 
        {
            int thread_id = omp_get_thread_num();
            
            for(int k = 0; k < nfield; ++k) {
                memcpy(u_block[thread_id]+k*nx_all*(2*nbatch+block_size), 
		       u+k*nx_all*ny_all+nx_all*(block_size*thread_id), 
                       nx_all*(2*nbatch+block_size)*sizeof(float));
                memcpy(v_block[thread_id]+k*nx_all*(2*nbatch+block_size), 
		       v+k*nx_all*ny_all+nx_all*(block_size*thread_id), 
                       nx_all*(2*nbatch+block_size)*sizeof(float));
                memcpy(f_block[thread_id]+k*nx_all*(2*nbatch+block_size), 
		       f+k*nx_all*ny_all+nx_all*(block_size*thread_id), 
                       nx_all*(2*nbatch+block_size)*sizeof(float));
                memcpy(g_block[thread_id]+k*nx_all*(2*nbatch+block_size), 
		       g+k*nx_all*ny_all+nx_all*(block_size*thread_id), 
                       nx_all*(2*nbatch+block_size)*sizeof(float));
            }   
            #pragma omp barrier

            for(int j = 0; j < nbatch / 2; ++j) {
                central2d_step(u_block[thread_id], v_block[thread_id], scr_block[thread_id], 
			       f_block[thread_id], g_block[thread_id],
                               0, nx+2*(nbatch-1-j*2), block_size+2*(nbatch-1-j*2), 1+j*2,
                               nfield, flux, speed,
                               dt, dx, dy);
                central2d_step(u_block[thread_id], v_block[thread_id], scr_block[thread_id], 
			       f_block[thread_id], g_block[thread_id],
                               1, nx+2*(nbatch-2-j*2), block_size+2*(nbatch-2-j*2), 2+j*2,
                               nfield, flux, speed,
                               dt, dx, dy);
            }

            for(int k = 0;k<nfield;++k) {
                memcpy(u+k*nx_all*ny_all+nx_all*(ng+block_size*thread_id), 
		       u_block[thread_id]+k*nx_all*(2*nbatch+block_size)+nbatch*nx_all, 
                       nx_all*block_size*sizeof(float));
                memcpy(v+k*nx_all*ny_all+nx_all*(ng+block_size*thread_id), 
		       v_block[thread_id]+k*nx_all*(2*nbatch+block_size)+nbatch*nx_all, 
                       nx_all*block_size*sizeof(float));
                memcpy(f+k*nx_all*ny_all+nx_all*(ng+block_size*thread_id), 
		       f_block[thread_id]+k*nx_all*(2*nbatch+block_size)+nbatch*nx_all, 
                       nx_all*block_size*sizeof(float));
                memcpy(g+k*nx_all*ny_all+nx_all*(ng+block_size*thread_id), 
		       g_block[thread_id]+k*nx_all*(2*nbatch+block_size)+nbatch*nx_all, 
                       nx_all*block_size*sizeof(float));
            }
            #pragma omp barrier

        }

        t += nbatch * dt;
        nstep += nbatch;

    }
    
    for(int i = 0; i < nthread; ++i) {
        free(u_block[i]);
        free(v_block[i]);
        free(f_block[i]);
        free(g_block[i]);
        free(scr_block[i]);
    }
            
    return nstep;
}


static
int central2d_xrun(float* restrict u, 
                   int nx, int ny, int ng,
                   int nfield, flux_t flux, speed_t speed,
                   float tfinal, float dx, float dy, float cfl, int nthread, int nbatch)
{
    int nstep = 0;
    int nx_all = nx + 2*ng;
    int ny_all = ny + 2*ng;
    int nc = nx_all * ny_all;
    int N  = nfield * nc;

    //sim->u  = (float*) malloc((4*N + 6*nx_all)* sizeof(float));
    



        #pragma offload target(mic) inout(u : length(nx_all*ny_all*nfield)) \
        inout(nstep) \
        in(nx, ny, ng, nfield, tfinal, dx, dy, cfl, nthread, nbatch)
        {
            nstep = central2d_yrun(u, u + N, u + 4*N, u + 2*N, u + 3*N, nx, ny, ng, nfield, flux, speed, tfinal, dx, dy, cfl, nthread, nbatch);
        }



    return nstep;
}


int central2d_run(central2d_t* sim, float tfinal, int nthread, int nbatch)
{
    return central2d_xrun(sim->u,
                          sim->nx, sim->ny, sim->ng,
                          sim->nfield, sim->flux, sim->speed,
                          tfinal, sim->dx, sim->dy, sim->cfl, nthread, nbatch);
}