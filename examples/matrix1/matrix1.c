/* matrix1 - SGEMM example with verification.

   Copyright (c) 2018 Michal Babej / Tampere University of Technology

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
   sell copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

// For srandom
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pocl_opencl.h"

#ifdef _WIN32
#  include "vccompat.hpp"
#endif

#ifndef min
#define min(X, Y) X < Y ? X : Y
#endif
#define WPT_L (local_wg / 4)

#define WPT_R 8

int
create_buf (cl_context context, cl_command_queue cmd_queue,
            cl_svm_mem_flags flags, size_t size, void *src, int use_svm,
            void **retval)
{
  void *r = NULL;
  int err = CL_SUCCESS;
  *retval = NULL;
  if (use_svm)
    {
      printf ("Using SVM\n");
      flags
          = flags & (CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);
      r = clSVMAlloc (context, flags, size, 0);
      TEST_ASSERT (r != NULL);
      if (src)
        {
          err = clEnqueueSVMMemcpy (cmd_queue, CL_TRUE, r, src, size, 0, NULL,
                                    NULL);
          CHECK_CL_ERROR2 (err);
        }
    }
  else
    {
      r = clCreateBuffer (context, flags, size, src, &err);
      CHECK_CL_ERROR2 (err);
    }
  *retval = r;

ERROR:
  return err;
}

int
read_buf (cl_command_queue cmd_queue, void *dst, void *src, size_t size,
          int use_svm)
{
  int err = CL_SUCCESS;
  if (use_svm)
    {
      err = clEnqueueSVMMemcpy (cmd_queue, CL_TRUE, dst, src, size, 0, NULL,
                                NULL);
      CHECK_CL_ERROR2 (err);
    }
  else
    {
      err = clEnqueueReadBuffer (cmd_queue, src, CL_TRUE, 0, size, dst, 0,
                                 NULL, NULL);
      CHECK_CL_ERROR2 (err);
    }
ERROR:
  return err;
}

#define ITERS 30

int
exec_matrix_kernel (cl_context context, cl_device_id device,
                    cl_command_queue cmd_queue, cl_program program, cl_uint n,
                    cl_float *srcA, cl_float *srcB, cl_float *dst,
                    const char *kernel_name, const size_t *global_work_size,
                    const size_t *local_work_size, int transpose, int use_svm)
{
  cl_kernel kernel = NULL;
  void *memobjs[3] = { 0, 0, 0 };
  void *temp = NULL;
  cl_int err = CL_SUCCESS;
  size_t buf_size = sizeof (cl_float) * n * n;

  poclu_bswap_cl_float_array (device, (cl_float *)srcA, n * n);
  poclu_bswap_cl_float_array (device, (cl_float *)srcB, n * n);

  err = create_buf (context, cmd_queue,
                    CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, buf_size, srcA,
                    use_svm, &memobjs[0]);
  CHECK_CL_ERROR2 (err);

  if (transpose)
    {
      err = create_buf (context, cmd_queue,
                        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, buf_size,
                        srcB, use_svm, &temp);
      CHECK_CL_ERROR2 (err);

      err = create_buf (context, cmd_queue, CL_MEM_READ_WRITE, buf_size, NULL,
                        use_svm, &memobjs[1]);
      CHECK_CL_ERROR2 (err);

      kernel = clCreateKernel (program, "transpose", NULL);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArg (kernel, 0, sizeof (cl_uint), (void *)&n);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArg (kernel, 1, sizeof (cl_uint), (void *)&n);
      CHECK_CL_ERROR2 (err);

      if (use_svm)
        {
          err = clSetKernelArgSVMPointer (kernel, 2, temp);
          CHECK_CL_ERROR2 (err);

          err = clSetKernelArgSVMPointer (kernel, 3, memobjs[1]);
          CHECK_CL_ERROR2 (err);
        }
      else
        {
          err = clSetKernelArg (kernel, 2, sizeof (cl_mem), (void *)&temp);
          CHECK_CL_ERROR2 (err);

          err = clSetKernelArg (kernel, 3, sizeof (cl_mem),
                                (void *)&memobjs[1]);
          CHECK_CL_ERROR2 (err);
        }

      size_t global[2] = { n, n };
      size_t local[2] = { 8, 8 };
      err = clEnqueueNDRangeKernel (cmd_queue, kernel, 2, NULL, global, local,
                                    0, NULL, NULL);
      CHECK_CL_ERROR2 (err);
    }
  else
    {
      err = create_buf (context, cmd_queue,
                        CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, buf_size,
                        srcB, use_svm, &memobjs[1]);
      CHECK_CL_ERROR2 (err);
    }

  err = create_buf (context, cmd_queue, CL_MEM_WRITE_ONLY, buf_size, NULL,
                    use_svm, &memobjs[2]);
  CHECK_CL_ERROR2 (err);

  kernel = clCreateKernel (program, kernel_name, &err);
  CHECK_CL_ERROR2 (err);

  if (use_svm)
    {
      err = clSetKernelArgSVMPointer (kernel, 0, memobjs[0]);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArgSVMPointer (kernel, 1, memobjs[1]);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArgSVMPointer (kernel, 2, memobjs[2]);
      CHECK_CL_ERROR2 (err);
    }
  else
    {
      err = clSetKernelArg (kernel, 0, sizeof (cl_mem), (void *)&memobjs[0]);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArg (kernel, 1, sizeof (cl_mem), (void *)&memobjs[1]);
      CHECK_CL_ERROR2 (err);

      err = clSetKernelArg (kernel, 2, sizeof (cl_mem), (void *)&memobjs[2]);
      CHECK_CL_ERROR2 (err);
    }
  err = clSetKernelArg (kernel, 3, sizeof (cl_uint), (void *)&n);
  CHECK_CL_ERROR2 (err);

  err = clSetKernelArg (kernel, 4, sizeof (cl_uint), (void *)&n);
  CHECK_CL_ERROR2 (err);

  err = clSetKernelArg (kernel, 5, sizeof (cl_uint), (void *)&n);
  CHECK_CL_ERROR2 (err);

  printf ("gws: %lu %lu lws: %lu %lu\n", global_work_size[0],
          global_work_size[1], local_work_size[0], local_work_size[1]);

  cl_event events[ITERS];
  for (size_t i = 0; i < ITERS; ++i)
    {
      err = clEnqueueNDRangeKernel (cmd_queue, kernel, 2, NULL,
                                    global_work_size, local_work_size, 0, NULL,
                                    &events[i]);
      CHECK_CL_ERROR2 (err);
    }

  err = read_buf (cmd_queue, dst, memobjs[2], buf_size, use_svm);

  cl_ulong startTime;
  cl_ulong endTime;
  cl_ulong minTime = 1UL << 30;

  // Get kernel profiling info
  for (size_t i = 0; i < ITERS; ++i)
    {
      err = clGetEventProfilingInfo (events[i], CL_PROFILING_COMMAND_START,
                                     sizeof (cl_ulong), &startTime, 0);
      CHECK_CL_ERROR2 (err);
      err = clGetEventProfilingInfo (events[i], CL_PROFILING_COMMAND_END,
                                     sizeof (cl_ulong), &endTime, 0);
      CHECK_CL_ERROR2 (err);
      if (endTime - startTime < minTime)
        {
          minTime = endTime - startTime;
        }
      clReleaseEvent (events[i]);
    }

  cl_ulong ev_nsec = minTime;
  double nsec = (double)ev_nsec;
  double msec = ev_nsec / 1000000.0;
  size_t f = 2UL * n * n * n;
  double flops = (double)f;
  double gflops = f / 1000000000.0;
  double perf = flops / nsec;

  printf ("Performance: %lf GFLOPS/s  | Time: %lf  msec  | Total Ops to "
          "execute: %lf G \n",
          perf, msec, gflops);

  poclu_bswap_cl_float_array (device, (cl_float *)dst, n * n);

ERROR:
  if (use_svm)
    {
      if (temp)
        clSVMFree (context, temp);
      clSVMFree (context, memobjs[0]);
      clSVMFree (context, memobjs[1]);
      clSVMFree (context, memobjs[2]);
    }
  else
    {
      if (temp)
        clReleaseMemObject (temp);
      clReleaseMemObject (memobjs[0]);
      clReleaseMemObject (memobjs[1]);
      clReleaseMemObject (memobjs[2]);
    }
  clReleaseKernel (kernel);
  return err;
}

int
main (int argc, char **argv)
{
  cl_float *srcA = NULL;
  cl_float *srcB = NULL;
  cl_float *dst = NULL;
  long *sums = NULL;
  long i, j, sum;
  int is_binary = 0;
  int spir = 0;
  int spirv = 0;
  int poclbin = 0;
  int use_locals = 0;
  int use_2d_reg_block = 0;
  int use_fma = 0;
  int use_svm = 0;
  cl_int err;

  cl_context context = NULL;
  cl_device_id device = NULL;
  cl_platform_id platform = NULL;
  cl_command_queue queue = NULL;
  cl_program program = NULL;
  size_t max_wg_size = 0;
  size_t TSM = 0;
  size_t TSK = 0;
  cl_ulong local_mem_size = 0;
  unsigned local_wg = 0;
  unsigned explicit_local_wg = 0;
  long matrix_size = 0;
  const char *explicit_binary_path = NULL;

  printf ("argc: %i \n", argc);

  err = poclu_get_any_device2 (&context, &device, &queue, &platform);
  CHECK_OPENCL_ERROR_IN ("clCreateContext");

  if (argc > 1)
    {
      errno = 0;
      matrix_size = strtol (argv[1], NULL, 10);
    }

  if (matrix_size < 64 || matrix_size > 65536 || errno)
    {
      printf ("USAGE: matrix1 MATRIX_SIZE [LOCAL_WG_SIZE] [options] "
              "[path-to-binary]\n"
              "Matrix width must be power-of-4, in [64, 65536] range\n");
      return EXIT_FAILURE;
    }

  /**************************************************************************/

  long matrix_2d_size = matrix_size * matrix_size;

  int arg_i = 2;
  if ((argc > 2) && argv[2][0] != '-')
    {
      errno = 0;
      explicit_local_wg = strtol (argv[2], NULL, 10);
      if (explicit_local_wg < 1 || explicit_local_wg > 1024 || errno)
        {
          printf (
              "USAGE: matrix1 MATRIX_SIZE [LOCAL_WG_SIZE] [options] "
              "[path-to-binary]\n"
              "Explicit local-WG-size must be power-of-2, in [1, 64] range\n");
          return EXIT_FAILURE;
        }
      else
        {
          ++arg_i;
        }
    }

  while ((argc > arg_i) && (argv[arg_i][0] == '-'))
    {
      if (argv[arg_i][1] == 's')
        spir = 1;
      if (argv[arg_i][1] == 'v')
        spirv = 1;
      if (argv[arg_i][1] == 'b')
        poclbin = 1;
      if (argv[arg_i][1] == 'l')
        use_locals = 1;
      if (argv[arg_i][1] == 'r')
        use_2d_reg_block = 1;
      if (argv[arg_i][1] == 'f')
        use_fma = 1;
      if (argv[arg_i][1] == 'M')
        use_svm = 1;
      ++arg_i;
    }

  if ((spir + spirv + poclbin) > 1)
    {
      printf ("only one type of binary can be specified \n");
      return EXIT_FAILURE;
    }

  is_binary = (spir + spirv + poclbin);

  if ((argc > arg_i) && (argv[arg_i][0] != '-'))
    explicit_binary_path = argv[arg_i];

  printf ("OPTIONS: SPIR %i SPIR-V %i POCLBIN %i USE_LOCALS %i USE_REGS %i "
          "USE_FMA %i USE_SVM %i EXPLICIT_LWG %u\n"
          "EXPLICIT BINARY: %s \n",
          spir, spirv, poclbin, use_locals, use_2d_reg_block, use_fma, use_svm,
          explicit_local_wg, explicit_binary_path);

  if (explicit_binary_path && ((spir + spirv + poclbin) == 0))
    {
      printf ("explicit binary given, but no binary type specified!\n");
      return EXIT_FAILURE;
    }

  /**************************************************************************/

  err = clGetDeviceInfo (device, CL_DEVICE_MAX_WORK_GROUP_SIZE,
                         sizeof (max_wg_size), &max_wg_size, NULL);
  CHECK_OPENCL_ERROR_IN ("clGetDeviceInfo");

  err = clGetDeviceInfo (device, CL_DEVICE_LOCAL_MEM_SIZE,
                         sizeof (local_mem_size), &local_mem_size, NULL);
  CHECK_OPENCL_ERROR_IN ("clGetDeviceInfo");

  /**************************************************************************/

  if (explicit_local_wg)
    {
      local_wg = explicit_local_wg;
      assert (local_wg > 0);

      TSM = local_wg * WPT_R;
      TSK = local_wg;

      /* regs */
      if ((use_2d_reg_block && (local_wg * local_wg * WPT_R > max_wg_size))
          /* locals */
          || (use_locals && (local_wg * 4 > max_wg_size))
          /* simple */
          || (!use_2d_reg_block && !use_locals
              && (local_wg * local_wg > max_wg_size)))
        {
          printf ("Local WG size of the binary exceeds this "
                  "device's capabilities.\nTest SKIPPED\n");
          return 0;
        }

      if (use_locals && (local_wg * local_wg * 8 > local_mem_size))
        {
          printf ("required local memory exceeds this device's "
                  "capabilities.\n");
          return -1;
        }

      if (use_2d_reg_block && ((TSM * TSK * 8) > local_mem_size))
        {
          printf ("required local memory exceeds this device's "
                  "capabilities.\n");
          return -1;
        }
    }
  else
    {
      local_wg = 1;
      size_t minsize = (max_wg_size < matrix_size ? max_wg_size : matrix_size);
      if (!use_locals && !use_2d_reg_block)
        {
          while (local_wg * local_wg < minsize)
            local_wg <<= 1;
          while (local_wg * local_wg > minsize)
            local_wg >>= 1;
        }

      if (use_locals)
        {
          while (local_wg * 4 < minsize)
            local_wg <<= 1;
          while (local_wg * 4 > minsize)
            local_wg >>= 1;
          /* 8 = 2x array: float[local_wg][local_wg] */
          while (local_wg * local_wg * 8 > local_mem_size)
            local_wg >>= 1;
        }

      if (use_2d_reg_block)
        {
          while (local_wg * local_wg * WPT_R < minsize)
            local_wg <<= 1;
          while (local_wg * local_wg * WPT_R > minsize)
            local_wg >>= 1;
          // TSM * TSK * 8
          while (local_wg
                 && ((local_wg * local_wg * WPT_R * 8) > local_mem_size))
            local_wg >>= 1;

          if (local_wg == 0)
            {
              printf ("this machine doesn't have the resources to run the REG "
                      "version.\n");
              return -1;
            }

          TSM = local_wg * WPT_R;
          TSK = local_wg;

          /* 2 buffers x sizeof(float) * [TSK][TSM] */
          // tune TSK. can be tuned independently of local WG size
          while ((TSK < WPT_R) && ((TSM * TSK * 8) < local_mem_size))
            TSK <<= 1;
        }

      assert (local_wg > 0);
      printf ("Autodetected local_wg: %u \n", local_wg);
    }

  float div = (float)matrix_size / (float)local_wg;
  if (matrix_size % ((unsigned)div) != 0)
    {
      printf ("matrix size must be divisible by local_wg \n");
      return -1;
    }

  /**************************************************************************/

  size_t global_work_size[2];
  size_t local_work_size[2];
  const char *kernel_name = NULL;
  const char *fma = use_fma ? "-DFMA " : "";
  char extra_opts[1024];
  extra_opts[0] = 0;

  /* simple */
  if (!use_locals && !use_2d_reg_block)
    {
      printf ("Using simplest kernel (myGEMM2)\n");
      global_work_size[0] = global_work_size[1] = matrix_size;
      local_work_size[0] = local_work_size[1] = local_wg;
      kernel_name = "myGEMM2";
      if (use_fma)
        strcpy (extra_opts, fma);
    }

  /* myGEMM4 */
  if (use_locals)
    {
      kernel_name = "myGEMM4";

      printf ("Using locals (myGEMM4)\n");

      global_work_size[0] = matrix_size;
      global_work_size[1] = matrix_size / WPT_L;
      local_work_size[0] = local_wg;
      local_work_size[1] = 4;

      snprintf (extra_opts, 1024, "%s-DMYGEMM4 -DLOCAL_SIZE=%u", fma,
                local_wg);
      printf ("Using local group size: [%u, %u]\n", local_wg, 4);
    }

  /* myGEMM6 */
  if (use_2d_reg_block)
    {
      kernel_name = "myGEMM6";

      printf ("using 2d reg block (myGEMM6)\n");

      /* const size_t global[2] = { M/WPTM, N/WPTN }; */
      /* const size_t local[2] = { TSM/WPTM, TSN/WPTN }; */
      global_work_size[0] = global_work_size[1] = matrix_size / WPT_R;
      local_work_size[0] = local_work_size[1] = local_wg;

      snprintf (extra_opts, 1024, "%s-DMYGEMM6 -DTSM=%zu -DTSN=%zu -DTSK=%zu",
                fma, TSM, TSM, TSK);
      printf ("GLOBAL: [%zu, %zu] LOCAL: [%zu, %zu] TSM/TSN: %zu TSK: %zu\n",
              global_work_size[0], global_work_size[1], local_work_size[0],
              local_work_size[1], TSM, TSK);
    }

  /*****************************************************************************/

  err = poclu_load_program (context, device, "matrix1", spir, spirv, poclbin,
                            explicit_binary_path, extra_opts, &program);
  if (err != CL_SUCCESS)
    goto FINISH;

  srcA = (cl_float *)malloc (matrix_2d_size * sizeof (cl_float));
  srcB = (cl_float *)malloc (matrix_2d_size * sizeof (cl_float));
  dst = (cl_float *)malloc (matrix_2d_size * sizeof (cl_float));
  sums = (long *)calloc (matrix_size, sizeof (long));

  srandom (time (NULL));

  for (i = 0; i < matrix_size; ++i)
    {
      for (j = 0; j < matrix_size; ++j)
        {
          long r = random ();
          int r1 = (r >> 8) % 64;
          int r2 = (r >> 16) % 64;
          sums[j] += r2;
          long x = i * matrix_size + j;
          srcA[x] = (cl_float) (r1);
          srcB[x] = (cl_float) (r2);
          dst[x] = 0.0f;
        }
    }

  sum = 0;
  for (i = 0; i < matrix_size; ++i)
    {
      for (j = 0; j < matrix_size; ++j)
        {
          long x = i * matrix_size + j;
          long tmp = srcA[x] * sums[i];
          sum += tmp;
        }
    }
  printf ("\nExpected sum of all elements: %lu \n", sum);

  err = 0;

  local_work_size[0] = min (global_work_size[0], local_work_size[0]);
  local_work_size[1] = min (global_work_size[1], local_work_size[1]);

  if (exec_matrix_kernel (context, device, queue, program, matrix_size, srcA,
                          srcB, dst, kernel_name, global_work_size,
                          local_work_size, use_2d_reg_block, use_svm))
    {
      printf ("Error running the tests\n");
      err = 1;
      goto FINISH;
    }

  long total = 0;
  for (i = 0; i < (matrix_2d_size); ++i)
    {
      if (isnormal (dst[i]))
        total += (long)dst[i];
    }

  printf ("Sum of all elements: %li \n", total);

  if (total == sum)
    printf ("OK\n");
  else
    printf ("FAIL\n");

FINISH:
  CHECK_CL_ERROR (clReleaseProgram (program));
  CHECK_CL_ERROR (clReleaseCommandQueue (queue));
  CHECK_CL_ERROR (clReleaseContext (context));
  CHECK_CL_ERROR (clUnloadPlatformCompiler (platform));

  free (dst);
  free (srcA);
  free (srcB);
  free (sums);
  return err;
}
