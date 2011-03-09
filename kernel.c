/*
 * Deblur kernel for FPGA implementation
 */

#include <autopilot_tech.h>
#include <math.h>

#define M 60
#define N 60
#define P 60
#define GAUSSIAN_NUMSTEPS 3
#define MAX_ITERATIONS 10
#define DT 0.0001
#define EPSILON 1.0E-10
#define EPSILON2 1.0E-5

#define SQR(x) ((x)*(x))

#define U_CENTER u[i][j][k]
#define U_LEFT u[i][j-1][k]
#define U_RIGHT u[i][j+1][k]
#define U_UP u[i-1][j][k]
#define U_DOWN u[i+1][j][k]
#define U_IN u[i][j][k-1]
#define U_OUT u[i][j][k+1]

#define G_CENTER g[i][j][k]
#define G_LEFT g[i][j-1][k]
#define G_RIGHT g[i][j+1][k]
#define G_UP g[i-1][j][k]
#define G_DOWN g[i+1][j][k]
#define G_IN g[i][j][k-1]
#define G_OUT g[i][j][k+1]

void gaussian_blur(double u[M][N][P], double Ksigma)
{
	double lambda = (Ksigma * Ksigma) / (2.0 * GAUSSIAN_NUMSTEPS);
	double nu =
	    (1.0 + 2.0 * lambda - sqrt(1.0 + 4.0 * lambda)) / (2.0 * lambda);
	double BoundaryScale = 1.0 / (1.0 - nu);
	double PostScale = 1;
	int steps, i, j, k;
	
	for (steps = 0; steps < 3 * GAUSSIAN_NUMSTEPS; steps++) {
#pragma AP unroll

		/* PostScale = (nu / lambda) ^ (3*GAUSSIAN_NUMSTEPS) */
		PostScale *= nu / lambda;
	}

	for (steps = 0; steps < GAUSSIAN_NUMSTEPS; steps++) {
        /* all of these loops have data dependencies
         * due to u[i][j][k] writebacks */
		/* move up by one plane, ie k++ */
		for (k = 0; k < P; k++) {
			/* move up by one col, ie j++ */
			for (j = 0; j < N; j++) {
				/* Filter downwards */
				/* i = 0, moving right in i direction */
				u[0][j][k] *= BoundaryScale;
				for (i = 1; i < M; i++) {
					u[i][j][k] += nu * u[i - 1][j][k];
				}

				/* Filter upwards */
				/* i = M-1, moving left in i direction */
				u[M - 1][j][k] *= BoundaryScale;
				for (i = M - 2; i >= 0; i--) {
					u[i][j][k] += u[i + 1][j][k];
				}
			}

			/* Filter right */
			/* j = 0, moving up in j direction */
			for (i = 0; i < M; i++) {
				u[i][0][k] *= BoundaryScale;
			}
			for (j = 1; j < N; j++) {
				for (i = 0; i < M; i++) {
					u[i][j][k] += nu * u[i][j - 1][k];
				}
			}

			/* Filter left */
			/* j = N-1, moving down in j direction */
			for (i = 0; i < M; i++) {
				u[i][N - 1][k] *= BoundaryScale;
			}
			for (j = N - 2; j >= 0; j--) {
				for (i = 0; i < M; i++) {
					u[i][j][k] += nu * u[i][j + 1][k];
				}
			}
		}

		/* Filter out */
		/* k = 0, moving out in k direction */
		for (j = 0; j < N; j++) {
			for (i = 0; i < M; i++) {
				u[i][j][0] *= BoundaryScale;
			}
		}
		for (k = 1; k < P; k++) {
			for (j = 0; j < N; j++) {
				for (i = 0; i < M; i++) {
					u[i][j][k] += nu * u[i][j][k - 1];
				}
			}
		}

		/* Filter in */
		/* k = P-1, moving in in k direction */
		for (j = 0; j < N; j++) {
			for (i = 0; i < M; i++) {
				u[i][j][P - 1] *= BoundaryScale;
			}
		}
		for (k = P - 2; k >= 0; k--) {
			for (j = 0; j < N; j++) {
				for (i = 0; i < M; i++) {
					u[i][j][k] += nu * u[i][j][k + 1];
				}
			}
		}
	}

	for (k = 0; k < P; k++) {
		for (j = 0; j < N; j++) {
			for (i = 0; i < M; i++) {
#pragma AP pipeline

#pragma AP unroll factor=2
				u[i][j][k] *= PostScale;
			}
		}
	}
}

void rician_deconv3(double u[M][N][P], const double f[M][N][P], 
		double g[M][N][P], double conv[M][N][P],
		double Ksigma, double sigma, double lambda)
{
#pragma AP interface ap_bus port=f pipeline
#pragma AP interface ap_bus port=u pipeline
#pragma AP interface ap_memory port=g pipeline
#pragma AP interface ap_memory port=conv pipeline

	double sigma2, gamma, r;
	double numer, denom;
	double u_stencil_up, u_stencil_center, u_stencil_down;
	double g_stencil_up, g_stencil_center, g_stencil_down;
	int i, j, k;
	int iteration;

	/* Initializations */
	sigma2 = SQR(sigma);
	gamma = lambda / sigma2;

    /*** Main gradient descent loop ***/
	for (iteration = 1; iteration <= MAX_ITERATIONS; iteration++) {
		/* parallelize/pipeline this, no data deps */
		/* Approximate g = 1/|grad u| */
		for (k = 1; k < P - 1; k++) {
			for (j = 1; j < N - 1; j++) {
				u_stencil_center = u[0][j][k];
				u_stencil_down = u[1][j][k];
				for (i = 1; i < M - 1; i++) {
					u_stencil_up = u_stencil_center;
					u_stencil_center = u_stencil_down;
					u_stencil_down = U_DOWN;
					denom =
					    sqrt(EPSILON +
						 SQR(u_stencil_center -
						     U_RIGHT) +
						 SQR(u_stencil_center -
						     U_LEFT) +
						 SQR(u_stencil_center -
						     u_stencil_up) +
						 SQR(u_stencil_center -
						     u_stencil_down) +
						 SQR(u_stencil_center - U_IN) +
						 SQR(u_stencil_center - U_OUT));
					G_CENTER = 1.0 / denom;
				}
			}
		}
		for (i = 0; i < M; i++) {
			for (j = 0; j < N; j++) {
				for (k = 0; k < P; k++) {
#pragma AP pipeline
#pragma AP unroll skip_exit_check factor=2
					conv[i][j][k] = u[i][j][k];
				}
			}
		}
		gaussian_blur(conv, Ksigma);
		/* parallelize/pipeline this, no data deps */
		for (k = 0; k < P; k++) {
			for (j = 0; j < N; j++) {
				for (i = 0; i < M; i++) {
#pragma AP pipeline
#pragma AP unroll skip_exit_check factor=2
					r = conv[i][j][k] * f[i][j][k] / sigma2;
					numer =
					    r * 2.38944 + r * (0.950037 + r);
					denom =
					    4.65314 + r * (2.57541 +
							   r * (2.57541 +
								r * (1.48937 +
								     r)));
					conv[i][j][k] -= f[i][j][k] * r;
				}
			}
		}
		gaussian_blur(conv, Ksigma);
		/* Update u by a semi-implict step */
        /* pipeline? data deps due to u[i][j][k] writeback */
		for (k = 1; k < P - 1; k++) {
			for (j = 1; j < N - 1; j++) {
				u_stencil_center = u[0][j][k];
				g_stencil_center = g[0][j][k];
				u_stencil_down = u[1][j][k];
				g_stencil_down = g[1][j][k];
				for (i = 1; i < M - 1; i++) {
					u_stencil_up = u_stencil_center;
					g_stencil_up = g_stencil_center;
					u_stencil_center = u_stencil_down;
					g_stencil_center = g_stencil_down;
					u_stencil_down = U_DOWN;
					g_stencil_down = G_DOWN;

					numer =
					    u_stencil_center +
					    DT * (U_RIGHT * G_RIGHT +
						  U_LEFT * G_LEFT +
						  U_RIGHT * G_RIGHT +
						  u_stencil_up * g_stencil_up +
						  u_stencil_down *
						  g_stencil_down + U_IN * G_IN +
						  U_OUT * G_OUT -
						  gamma * conv[i][j][k]);
					denom =
					    1.0 + DT * (G_RIGHT + G_LEFT +
							g_stencil_down +
							g_stencil_up + G_IN +
							G_OUT);
					U_CENTER = numer / denom;
				}
			}
		}
	}
}
