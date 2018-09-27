// Copyright (C) 2016, Jérémy Anger <jeremy.anger@cmla.ens-cachan.fr>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "iio.h"
#include "fba.h"
#include "consistent_registration.h"
#include "vpp.h"

void for_vpp(const char* input, const char* output, int n, float p, int W)
{
    int w, h, d;
    FILE* in = vpp_init_input(input, &w, &h, &d);
    assert(in);
    FILE* out = vpp_init_output(output, w, h, d);
    assert(out);

    image_float_t frame = new_image_float(w, h, d);
    image_float_t result = new_image_float(w, h, d);
    image_float_t buffer[n];
    image_float_t registered[n];
    for (int i = 0; i < n; i++) {
        buffer[i] = new_image_float(w, h, d);
        registered[i] = new_image_float(w, h, d);
    }

    int c = 0;
    int C = 0;
    while (vpp_read_frame(in, frame.data, w, h, d)) {
        if (C != n) C++;
        c = (c + 1) % C;
        copyf(buffer[c], frame);

        consistent_registration(registered, buffer, C, c, 1.f, 3);
        fba(result, registered, W, p, C);

        if (!vpp_write_frame(out, result.data, w, h, d))
            break;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3) {
        fprintf(stderr, "launching fba for vpp, '%s' -> '%s'\n", argv[1], argv[2]);
        return for_vpp(argv[1], argv[2], 5, 11, 128), 0;
    }

    if (argc < 9) {
        fprintf(stderr, "usage:\n\t%s M W p downsampling register iterations inputs... registered_fmt output_fmt\n", *argv);
        return EXIT_FAILURE;
    }

    int arg = 1;
    int M = atoi(argv[arg++]);
    int W = atoi(argv[arg++]);
    int p = atof(argv[arg++]);
    int downsampling = atoi(argv[arg++]);
    int doregister = atoi(argv[arg++]);
    int iterations = atoi(argv[arg++]);
    int n = argc - arg - 2;

    if (M < 1 || M > 10)
    {
        fprintf(stderr, "invalid 'M', should be a number between 1 and 10\n");
        return EXIT_FAILURE;
    }

    if (W < 2 || W % 2 != 0)
    {
        fprintf(stderr, "invalid 'W', should be an even number equal or greater than 2\n");
        return EXIT_FAILURE;
    }

    if (p > 40.f)
    {
        p = 40.f;
        fprintf(stderr, "'p' clamped to 40\n");
    }

    if (p < 0.f)
    {
        fprintf(stderr, "invalid 'p', should be a number between 0 and 40\n");
        return EXIT_FAILURE;
    }

    if (downsampling < 1 || downsampling > 10)
    {
        fprintf(stderr, "invalid 'downsampling', should be a number between 1 and 10\n");
        return EXIT_FAILURE;
    }

    if (iterations < 1)
    {
        fprintf(stderr, "invalid 'iterations', should be a positive number\n");
        return EXIT_FAILURE;
    }

    if (n <= 0)
    {
        fprintf(stderr, "usage:\n\t%s M W p downsampling register iterations inputs... registered_fmt output_fmt\n", *argv);
        return EXIT_FAILURE;
    }

    image_float_t* inputs = (image_float_t*) malloc(n * sizeof(image_float_t));
    image_float_t* outputs = (image_float_t*) malloc(n * sizeof(image_float_t));

    // load images and allocate outputs
    int w=0, h=0, d=0;
    for (int i = 0; i < n; i++)
    {
        int ww, hh, dd;
        float* data = iio_read_image_float_vec(argv[arg++], &ww, &hh, &dd);
        if (!data) {
            fprintf(stderr, "%s: cannot open file", argv[arg-1]);
            return EXIT_FAILURE;
        }
        if (w && (ww != w || hh != h || dd != d)) {
            fprintf(stderr, "%s: invalid image size", argv[arg-1]);
            return EXIT_FAILURE;
        }
        w = ww; h = hh; d = dd;

        inputs[i] = new_image_float_data(w, h, d, data);
        outputs[i] = new_image_float(w, h, d);
    }

    char* registered_fmt = argv[arg++];
    char* output_fmt = argv[arg++];

    for (int j = 0; j < iterations; j++)
    {
        printf("Iteration: %d/%d\n", j+1, iterations);
#ifdef _OPENMP
#pragma omp parallel
#endif
        {
            image_float_t* registered = (image_float_t*) malloc((2*M+1) * sizeof(image_float_t));
            for (int m = 0; m < 2*M+1; m++)
                registered[m] = new_image_float(w, h, d);

#ifdef _OPENMP
#pragma omp for
#endif
            for (int i = 0; i < n; i++)
            {
                clock_t clock1 = clock();
                clock_t clock2, clock3;

                // compute the start and end of the temporal window
                image_float_t* start = inputs + (i - M < 0 ? 0 : i - M);
                image_float_t* end = inputs + (i + M >= n ? n - 1 : i + M);
                int len = end - start + 1;
                int ref = i - (start - inputs);

                // run the registration
                if (doregister)
                {
                    consistent_registration(registered, start, len, ref, 1.f /* epsilon */, downsampling);
                }
                else
                {
                    for (int m = 0; m < len; m++)
                    for (int i = 0; i < w*h*d; i++)
                        registered[m].data[i] = start[m].data[i];
                }

                // run the burst accumulation
                clock2 = clock();
                fba(outputs[i], registered, W, p, len);
                clock3 = clock();

                printf("Registration CPU time: %g sec\n", ((double)clock2 - clock1) / CLOCKS_PER_SEC);
                printf("FBA CPU time: %g sec\n", ((double)clock3 - clock2) / CLOCKS_PER_SEC);

                // if this is the last iteration, output the results
                if (j == iterations - 1) {
                    if (doregister)
                    {
                        for (int m = 0; m < len; m++)
                        {
                            char filename[1024] = {0};
                            snprintf(filename, sizeof(filename), registered_fmt, i, (start - inputs) + m);
                            iio_save_image_float_vec(filename, registered[m].data, w, h, d);
                        }
                    }
                    char filename[1024] = {0};
                    snprintf(filename, sizeof(filename), output_fmt, i);
                    iio_save_image_float_vec(filename, outputs[i].data, w, h, d);
                }
            }

            for (int m = 0; m < 2*M+1; m++)
                free(registered[m].data);
        }

        for (int i = 0; i < n; i++)
        for (int x = 0; x < w*h*d; x++)
            inputs[i].data[x] = outputs[i].data[x];
    }

    for (int i = 0; i < n; i++)
    {
        free(inputs[i].data);
        free(outputs[i].data);
    }

    return EXIT_SUCCESS;
}

