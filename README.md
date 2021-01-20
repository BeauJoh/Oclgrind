Oclgrind
========

About
-----
This project implements a virtual OpenCL device simulator, including
an OpenCL runtime with ICD support. The goal is to provide a platform
for creating tools to aid OpenCL development. In particular, this
project currently implements utilities for debugging memory access
errors, detecting data-races and barrier divergence, collecting
instruction histograms, and for interactive OpenCL kernel debugging.
The simulator is built on an interpreter for LLVM IR. This project is
being developed by James Price and Simon McIntosh-Smith at the
University of Bristol.

Binary releases can be found on the GitHub releases page:

  https://github.com/jrprice/Oclgrind/releases


Build dependencies
------------------
To build this project, you will need LLVM and Clang 5.0 (or newer)
development libraries and headers. If you build LLVM from source, it
is recommended to enable optimizations to significantly improve the
performance of Oclgrind (set `CMAKE_BUILD_TYPE` to `Release` or
`RelWithDebInfo`).

You will need to use a compiler that supports C++11. Python should
also be available in order to run the test suite.


Building on Linux and OS X (CMake)
----------------------------------
The recommended method of building Oclgrind is via CMake.

When configuring the CMake build, you may be prompted to supply a
value for the `LLVM_DIR` parameter (this shouldn't be necessary if
LLVM is installed in a standard system location). This should be set
to the directory containing your LLVM installation's
`LLVMConfig.cmake` file (typically either
`${LLVM_ROOT}/lib/cmake/llvm` or `${LLVM_ROOT}/share/llvm/cmake/`).
If Clang is installed separately to LLVM, then you may also be
prompted to supply a path for the `CLANG_ROOT` parameter, which should
be the root of your Clang installation (containing the `bin/`, `lib/`
and `include/` directories).

A typical CMake command-line might look like this:

    cmake ${OCLGRIND_SOURCE} \
          -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DCMAKE_INSTALL_PREFIX=${INSTALL_ROOT} \
          -DLLVM_DIR=${LLVM_ROOT}/lib/cmake/llvm

where `${OCLGRIND_SOURCE}` is the path to the root directory
containing the Oclgrind source code, `${LLVM_ROOT}` is the path to the
LLVM installation, and `${INSTALL_ROOT}` is the desired installation
root directory (this can be omitted if installing to system
directories).

Next, build and install with make:

    make
    make test
    make install

If installing to a non-system location, you should add the `bin/`
directory to the `PATH` environment variable in order to make use of
the `oclgrind` command. If you wish to use Oclgrind via the OpenCL ICD
loader (optional), then you should create an ICD loading point by
copying the `oclgrind.icd` file from the build directory to
`/etc/OpenCL/vendors/`.


Building on Windows
-------------------
Building Oclgrind on Windows requires Visual Studio 2013 (or newer),
and Windows 7 (or newer). Compiling against recent versions of LLVM
may require Visual Studio 2015.

When configuring the CMake build, you may be prompted to supply a
value for the `LLVM_DIR` parameter. This should be set to the
directory containing your LLVM installation's `LLVMConfig.cmake` file
(for example `C:\Program Files\LLVM\lib\cmake\llvm`). If Clang is
installed separately to LLVM, then you may also be prompted to supply
a path in the `CLANG_ROOT` parameter, which should be the root of your
Clang installation (containing the `bin/`, `lib/` and `include/`
directories).

You should add the `bin` directory of the Oclgrind installation to the
`PATH` environment variable in order to make use of the `oclgrind`
command. If you wish to use Oclgrind via the OpenCL ICD loader
(optional), then you should also create an ICD loading point. To do
this, you should add a `REG_DWORD` value to the Windows Registry under
one or both of the registry keys below, with the name set to the
absolute path of the `oclgrind-rt-icd.dll` library and the value set
to 0.

Key for 32-bit machines or 64-bit apps on a 64-bit machine:
`HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenCL\Vendors`

Key for 32-bit apps on a 64-bit machine:
`HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Khronos\OpenCL\Vendors`


Usage
-----
The recommended method of running an application with Oclgrind is to
use the `oclgrind` command, for example:

    oclgrind ./application

This command will make it such the only OpenCL platform and device
available to your application is Oclgrind. If you need more control
over platform selection then installing an ICD loading point for
Oclgrind will cause it to appear when an application calls
`clGetPlatformIDs()`, alongside any other OpenCL platforms installed
on your system.

If it encounters any invalid memory accesses, Oclgrind will
report the details to stderr, for example:

    Invalid write of size 4 at global memory address 0x1000000000040
        Kernel:  vecadd
        Entity:  Global(16,0,0) Local(0,0,0) Group(16,0,0)
        store i32 %tmp9, i32 addrspace(1)* %tmp15, align 4
        At line 4 of input.cl
          c[i] = a[i] + b[i]

Since it is interpreting an abstract intermediate representation and
bounds-checking each memory access, Oclgrind will run quite slowly
(typically a couple of orders of magnitude slower than a regular CPU
implementation). Therefore, it is recommended to run your application
with a small problem if possible.

To enable an interactive, GDB-style debugging session, supply the `-i`
flag to the oclgrind command, or export the environment variable
`OCLGRIND_INTERACTIVE=1`. This will cause Oclgrind to automatically
break at the beginning of each kernel invocation, and upon
encountering an invalid memory access. Type `help` for details of
available commands.

For more detailed information about using Oclgrind please visit the
GitHub Wiki:

  https://github.com/jrprice/Oclgrind/wiki/


Architecture-Independent Workload Characterization (AIWC)
---------------------------------------------------------

Oclgrind now has AIWC plugin support. It is used when oclgrind is run with an
`--aiwc` flag.

Architecture Independent Workload Characterization collects the statistical
properties of OpenCL kernels free from any hardware specific traits.
For instance, certain characteristics around computation, memory, branching
and parallelism, are largely fixed  -- the task and it's requirements are the
same and only the performance will differ according to the hardware properties
when mapped to a given accelerator.
In other words, provided they are collected over a representative workload, a
graph traversal program maintains the characteristics of a graph traversal
program regardless of problem size or on what platform it is run.
Thus, AIWC metrics are representative statistics of the workload and are
independent of any particular device.

A summary of each collected AIWC metrics is provided below:

|Type        | Metric		  	        | Description                                                               |
|:----------:|:---------------------------------|--------------------------------------------------------------------------:|
|Compute     | Opcode 		  	        | total # of unique opcodes required to cover 90% of dynamic instructions   |
|            | Total Instruction Count 	        | total # of instructions executed                                          |
|------------|----------------------------------|---------------------------------------------------------------------------|
|Parallelism | Work-items                       | total # of work-items or threads executed                                 |
|            | Total Barriers Hit               | total # of barrier instructions					    |
|            | Min ITB                          | minimum # of instructions executed until a barrier                        |
|            | Max ITB 		  	        | maximum # of instructions executed until a barrier			    |
|            | Median ITB                       | median # of instructions executed until a barrier			    |
|            | Min IPT                          | minimum # of instructions executed per thread			            |
|            | Max IPT 		  	        | maximum # of instructions executed per thread			            |
|            | Median IPT                       | median # of instructions executed per thread			            |
|            | Max SIMD Width 	  	        | maximum # of data items operated on during an instruction		    |
|            | Mean SIMD Width 	  	        | mean # of data items operated on during an instruction		    |
|            | SD SIMD Width 	  	        | standard deviation across # of data items affected			    |
|------------|----------------------------------|---------------------------------------------------------------------------|
|Memory      | Total Memory Footprint           | total # of unique memory addresses accessed			            |
|            | 90% Memory Footprint             | # of unique memory addresses that cover 90% of memory accesses	    |
|            | Unique Reads                     | total # of unique memory addresses read				    |
|            | Unique Writes                    | total # of unique memory addresses written				    |
|            | Unique Read/Write Ratio          | indication of workload being (unique reads / unique writes)               |
|            | Total Reads                      | total # of memory addresses read                                          |
|            | Total Writes                     | total # of memory addresses written				            |
|            | Reread Ratio                     | indication of memory reuse for reads (unique reads/total reads)           |
|            | Rewrite Ratio                    | indication of memory reuse for writes (unique writes/total writes)        |
|            | Global Memory Address Entropy    | measure of the randomness of memory addresses                             |
|            | Local Memory Address Entropy     | measure of the spatial locality of memory addresses                       |
|------------|----------------------------------|---------------------------------------------------------------------------|
|Control     | Total Unique Branch Instructions | total # of unique branch instructions                                     |
|            | 90% Branch Instructions          | # of unique branch instructions that cover 90% of branch instructions     |
|            | Yokota Branch Entropy            | branch history entropy using Shannon's information entropy                |
|            | Average Linear Branch Entropy    | branch history entropy score using the average linear branch entropy      |

For examples on how these features can be used for diversity analysis and device predictions see:

  https://github.com/BeauJoh/aiwc-opencl-based-architecture-independent-workload-characterization-artefact

and

  https://github.com/BeauJoh/opencl-predictions-with-aiwc

If you have any questions or comments please contact the developers via email:

  Beau Johnston `<beau.johnston@anu.edu.au>` or Josh Milthorpe `<josh.milthorpe@anu.edu.au>`


Contact
-------
If you encounter any issues or have any questions, please use the
GitHub issues page:

  https://github.com/jrprice/Oclgrind/issues
