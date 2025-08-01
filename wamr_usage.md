Build WASM applications
Prepare WASM building environments
For C and C++, WASI-SDK version 19.0+ is the major tool supported by WAMR to build WASM applications. Also, we can use Emscripten SDK (EMSDK), but it is not recommended. And there are some other compilers such as the standard clang compiler, which might also work here.

To install WASI SDK, please download the wasi-sdk release and extract the archive to default path /opt/wasi-sdk.

The official wasi-sdk release doesn't fully support latest 128-bit SIMD spec yet. WAMR provides a script in build-wasi-sdk to generate another wasi-sdk with llvm-15 from source code and installs it at ../test-tools/wasi-sdk. If you plan to build WASM applications with latest 128-bit SIMD, please use it instead of the official release.

And sample workloads are using the self-compiled wasi-sdk.

For AssemblyScript, please refer to AssemblyScript quick start and AssemblyScript compiler for how to install asc compiler and build WASM applications.

For Rust, please refer to Install Rust and Cargo to install cargo, rustc and rustup. By default they are under ~/.cargo/bin.

And then run such a command to install wasm32-wasip1 target.

$ rustup target add wasm32-wasip1
To build WASM applications, run

$ cargo build --target wasm32-wasip1
The output files are under target/wasm32-wasip1.

To build a release version

$ cargo build --release --target wasm32-wasip1
Build WASM applications with wasi-sdk
You can write a simple test.c as the first sample.

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    char *buf;

    printf("Hello world!\n");

    buf = malloc(1024);
    if (!buf) {
        printf("malloc buf failed\n");
        return -1;
    }

    printf("buf ptr: %p\n", buf);

    sprintf(buf, "%s", "1234\n");
    printf("buf: %s", buf);

    free(buf);
    return 0;
}
To build the source file to WASM bytecode, we can input the following command:

/opt/wasi-sdk/bin/clang -O3 -o test.wasm test.c
1. wasi-sdk options
There are some useful options that are used to compile C/C++ to Wasm (for a full introduction, please refer to clang command line argument reference and wasm-ld command line argument manual):

-nostdlib Do not use the standard system startup files or libraries when linking. In this mode, the libc-builtin library of WAMR must be built to run the wasm app, otherwise, the libc-wasi library must be built. You can specify -DWAMR_BUILD_LIBC_BUILTIN=1 or -DWAMR_BUILD_LIBC_WASI=1 for CMake to build WAMR with libc-builtin support or libc-wasi support.

-Wl,--no-entry Do not output any entry point

-Wl,--export=<value> Force a symbol to be exported, e.g. -Wl,--export=foo to export foo function

-Wl,--export-all Export all symbols (normally combined with --no-gc-sections)

-Wl,--initial-memory=<value> Initial size of the linear memory, which must be a multiple of 65536

-Wl,--max-memory=<value> Maximum size of the linear memory, which must be a multiple of 65536

-z stack-size=<value> The auxiliary stack size, which is an area of linear memory, must be smaller than the initial memory size.

-Wl,--strip-all Strip all symbols

-Wl,--shared-memory Use shared linear memory

-Wl,--allow-undefined Allow undefined symbols in linked binary

-Wl,--allow-undefined-file=<value> Allow symbols listed in <file> to be undefined in linked binary

-pthread Support POSIX threads in generated code

For example, we can build the wasm app with the command:

/opt/wasi-sdk/bin/clang -O3 -nostdlib \
    -z stack-size=8192 -Wl,--initial-memory=65536 \
    -o test.wasm test.c \
    -Wl,--export=main -Wl,--export=__main_argc_argv \
    -Wl,--export=__heap_base -Wl,--export=__data_end \
    -Wl,--no-entry -Wl,--strip-all -Wl,--allow-undefined
to generate a wasm binary with nostdlib mode, the auxiliary stack size is 8192 bytes, initial memory size is 64 KB, main function, heap base global and data end global are exported, no entry function is generated (no _start function is exported), and all symbols are stripped. Note that it is nostdlib mode, so libc-builtin should be enabled by runtime embedder or iwasm (with cmake -DWAMR_BUILD_LIBC_BUILT=1, enabled by iwasm in Linux by default).

If we want to build the wasm app with wasi mode, we may build the wasm app with the command:

/opt/wasi-sdk/bin/clang -O3 \
    -z stack-size=8192 -Wl,--initial-memory=65536 \
    -o test.wasm test.c \
    -Wl,--export=__heap_base -Wl,--export=__data_end \
    -Wl,--strip-all
to generate a wasm binary with wasi mode, the auxiliary stack size is 8192 bytes, initial memory size is 64 KB, heap base global and data end global are exported, wasi entry function exported (_start function), and all symbols are stripped. Note that it is wasi mode, so libc-wasi should be enabled by runtime embedder or iwasm (with cmake -DWAMR_BUILD_LIBC_WASI=1, enabled by iwasm in Linux by default), and normally no need to export main function, by default _start function is executed by iwasm.

Note: for the Rust project, we can set these flags by setting the rustflags in the Cargo configuration file, e.g. <project_dir>/.cargo/config.toml or $CARGO_HOME/config.toml, for example:

[build]
rustflags = [
  "-C", "link-arg=--initial-memory=65536",
  "-C", "link-arg=-zstack-size=8192",
  "-C", "link-arg=--export=__heap_base",
  "-C", "link-arg=--export=__data_end",
  "-C", "link-arg=--strip-all",
]
2. How to reduce the footprint?
Firstly if libc-builtin (-nostdlib) mode meets the requirements, e.g. there are no file io operations in wasm app, we should build the wasm app with -nostdlib option as possible as we can, since the compiler doesn't build the libc source code into wasm bytecodes, which greatly reduces the binary size.

(1) Methods to reduce the libc-builtin (-nostdlib) mode footprint
export __heap_base global and __data_end global
-Wl,--export=__heap_base -Wl,--export=__data_end
If the two globals are exported, and there are no memory.grow and memory.size opcodes (normally nostdlib mode doesn't introduce these opcodes since the libc malloc function isn't built into wasm bytecode), WAMR runtime will truncate the linear memory at the place of __heap_base and append app heap to the end, so we don't need to allocate the memory specified by -Wl,--initial-memory=n which must be at least 64 KB. This is helpful for some embedded devices whose memory resource might be limited.
For the Rust project, please set the flags in the Cargo configuration file, for example:

[build]
rustflags = [
  "-C", "link-arg=--export=__heap_base",
  "-C", "link-arg=--export=__data_end",
  "-C", "link-arg=--initial-memory=65536",
]
reduce auxiliary stack size

The auxiliary stack is an area of linear memory, normally the size is 64 KB by default which might be a little large for embedded devices and partly used, we can use -z stack-size=n to set its size.

For the Rust project, please set the flag in the Cargo configuration file, for example:

[build]
rustflags = [
  "-C", "link-arg=-zstack-size=8192"
]
use -O3 and -Wl,--strip-all
For the Rust project, please set the flag in the Cargo configuration file, for example:

[build]
rustflags = [
 "-C", "link-arg=--strip-all"
]
reduce app heap size when running iwasm

We can pass --heap-size=n option to set the maximum app heap size for iwasm, by default it is 16 KB. For the runtime embedder, we can set the uint32_t heap_size argument when calling API  wasm_runtime_instantiate.

reduce wasm operand stack size when running iwasm

WebAssembly is a binary instruction format for a stack-based virtual machine, which requires a stack to execute the bytecodes. We can pass --stack-size=n option to set the maximum stack size for iwasm, by default it is 16 KB. For the runtime embedder, we can set the uint32_t stack_size argument when calling API  wasm_runtime_instantiate and wasm_runtime_create_exec_env.

decrease block_addr_cache size for classic interpreter

The block_addr_cache is a hash cache to store the else/end addresses for WebAssembly blocks (BLOCK/IF/LOOP) to speed up address lookup. This is only available in the classic interpreter. We can set it by defineing macro -DBLOCK_ADDR_CACHE_SIZE=n, e.g. add add_defintion (-DBLOCK_ADDR_CACHE_SIZE=n) in CMakeLists.txt, by default it is 64, and the total block_addr_cache size is 3072 bytes in 64-bit platform and 1536 bytes in 32-bit platform.

(2) Methods to reduce the libc-wasi (without -nostdlib) mode footprint
Most of the above methods are also available for libc-wasi mode, besides them, we can export malloc and free functions with -Wl,--export=malloc -Wl,--export=free option, so WAMR runtime will disable its app heap and call the malloc/free function exported to allocate/free the memory from/to the heap space managed by libc.

Note: wasm-ld from LLVM 13 and later automatically inserts ctor/dtor calls for all exported functions for a command. (vs reactor) It breaks the malloc/free exports mentioned above.

3. Build wasm app with pthread support
Please ref to pthread library for more details.

4. Build wasm app with SIMD support
The official wasi-sdk release doesn't fully support latest 128-bit SIMD spec yet. WARM provides a script in build-wasi-sdk to generate another wasi-sdk with llvm-13 from source code and installs it at ../test-tools/wasi-sdk. If you plan to build WASM applications with latest 128-bit SIMD, please use it instead of the official release.

And also you can install emsdk and use its SSE header files, please ref to workload samples, e.g. bwa CMakeLists.txt and wasm-av1 CMakeLists.txt for more details.

For both wasi-sdk and emsdk, please add the option -msimd128 for clang or emcc to generate WASM application with SIMD bytecodes.

Build WASM applications with emsdk
1. Install emsdk
Assuming you are using Linux, you may install emcc and em++ from Emscripten EMSDK following the steps below:

git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
# And then source the emsdk_env.sh script before build wasm app
source emsdk_env.sh    (or add it to ~/.bashrc if you don't want to run it each time)
The Emscripten website provides other installation methods beyond Linux.

2. emsdk options
To build the wasm C source code into wasm binary, we can use the following command:

EMCC_ONLY_FORCED_STDLIBS=1 emcc -O3 -s STANDALONE_WASM=1 \
    -o test.wasm test.c \
    -s TOTAL_STACK=4096 -s TOTAL_MEMORY=65536 \
    -s "EXPORTED_FUNCTIONS=['_main']" \
    -s ERROR_ON_UNDEFINED_SYMBOLS=0
There are some useful options:

EMCC_ONLY_FORCED_STDLIBS=1 whether to link libc library into the output binary or not, similar to -nostdlib option of wasi-sdk clang. If specified, then no libc library is linked and the libc-builtin library of WAMR must be built to run the wasm app, otherwise, the libc-wasi library must be built. You can specify -DWAMR_BUILD_LIBC_BUILTIN=1 or -DWAMR_BUILD_LIBC_WASI=1 for CMake to build WAMR with libc-builtin support or libc-wasi support.

The emsdk's wasi implementation is incomplete, e.g. open a file might just return fail, so it is strongly not recommended to use this mode, especially when there are file io operations in wasm app, please use wasi-sdk instead.

-s STANDALONE_WASM=1 build wasm app in standalone mode (non-web mode), if the output file has suffix ".wasm", then only wasm file is generated (without html file and JavaScript file).

-s TOTAL_STACK=<value> the auxiliary stack size, same as -z stack-size=\<value\> of wasi-sdk

-s TOTAL_MEMORY=<value> or -s INITIAL_MEORY=<value> the initial linear memory size

-s MAXIMUM_MEMORY=<value> the maximum linear memory size, only take effect if -s ALLOW_MEMORY_GROWTH=1 is set

-s ALLOW_MEMORY_GROWTH=1/0 whether the linear memory is allowed to grow or not

-s "EXPORTED_FUNCTIONS=['func name1', 'func name2']" to export functions

-s ERROR_ON_UNDEFINED_SYMBOLS=0 disable the errors when there are undefined symbols

For more options, please ref to <EMSDK_DIR>/upstream/emscripten/src/settings.js, or Emscripten document.

Build a project with cmake
If you have a complex WASM application project which contains dozens of source files, you can consider using cmake for project building.

You can cross compile your project by using the toolchain provided by WAMR.

Assume the original CMakeLists.txt for test.c likes below:

cmake_minimum_required (VERSION 3.14)
project(hello_world)
add_executable(hello_world test.c)
It is easy to use wasi-sdk in CMake by setting CMAKE_TOOLCHAIN_FILE without any modification on the original CMakeLists.txt.

$ cmake -DWASI_SDK_PREFIX=${WASI_SDK_INSTALLTION_DIR}
        -DCMAKE_TOOLCHAIN_FILE=${WASI_SDK_INSTALLTION_DIR}/share/cmake/wasi-sdk.cmake
        -DCMAKE_SYSROOT=<a sysroot directory>
        ..
WASI_SDK_INSTALLTION_DIR is the directory in where you install the wasi-sdk. like /opt/wasi-sdk

If you prefer WASI, set CMAKE_SYSROOT to wasi-sysroot

$ cmake <same as above>
        -DCMAKE_SYSROOT=${WASI_SDK_INSTALLTION_DIR}/share/wasi-sysroot
        ..
If you prefer WAMR builtin libc, set CMAKE_SYSROOT to libc-builtin-sysroot

Note: If you have already built a SDK profile

$ cmake <same as above>
        -DCMAKE_SYSROOT=${WAMR_SOURCE_ROOT}/wamr-sdk/app/libc-builtin-sysroot
        ..
You will get hello_world which is the WASM app binary.

Compile WASM to AOT module
Please ensure the wamrc was already generated and available in your shell PATH. Then we can use wamrc to compile WASM app binary to WAMR AOT binary.

wamrc -o test.aot test.wasm
wamrc supports a number of compilation options through the command line arguments:

wamrc --help
Usage: wamrc [options] -o output_file wasm_file
  --target=<arch-name>      Set the target arch, which has the general format: <arch><sub>
                            <arch> = x86_64, i386, aarch64, arm, thumb, xtensa, mips,
                                     riscv64, riscv32.
                              Default is host arch, e.g. x86_64
                            <sub> = for ex. on arm or thumb: v5, v6m, v7a, v7m, etc.
                            Use --target=help to list supported targets
  --target-abi=<abi>        Set the target ABI, e.g. gnu, eabi, gnueabihf, msvc, etc.
                              Default is gnu if target isn't riscv64 or riscv32
                              For target riscv64 and riscv32, default is lp64d and ilp32d
                            Use --target-abi=help to list all the ABI supported
  --cpu=<cpu>               Set the target CPU (default: host CPU, e.g. skylake)
                            Use --cpu=help to list all the CPU supported
  --cpu-features=<features> Enable or disable the CPU features
                            Use +feature to enable a feature, or -feature to disable it
                            For example, --cpu-features=+feature1,-feature2
                            Use --cpu-features=+help to list all the features supported
  --opt-level=n             Set the optimization level (0 to 3, default is 3)
  --size-level=n            Set the code size level (0 to 3, default is 3)
  -sgx                      Generate code for SGX platform (Intel Software Guard Extension)
  --bounds-checks=1/0       Enable or disable the bounds checks for memory access:
                              by default it is disabled in all 64-bit platforms except SGX and
                              in these platforms runtime does bounds checks with hardware trap,
                              and by default it is enabled in all 32-bit platforms
  --format=<format>         Specifies the format of the output file
                            The format supported:
                              aot (default)  AoT file
                              object         Native object file
                              llvmir-unopt   Unoptimized LLVM IR
                              llvmir-opt     Optimized LLVM IR
  --disable-bulk-memory     Disable the MVP bulk memory feature
  --enable-multi-thread     Enable multi-thread feature, the dependent features bulk-memory and
                            thread-mgr will be enabled automatically
  --enable-tail-call        Enable the post-MVP tail call feature
  --disable-simd            Disable the post-MVP 128-bit SIMD feature:
                              currently 128-bit SIMD is only supported for x86-64 target,
                              and by default it is enabled in x86-64 target and disabled
                              in other targets
  --disable-ref-types       Disable the MVP reference types feature
  --disable-aux-stack-check Disable auxiliary stack overflow/underflow check
  --enable-dump-call-stack  Enable stack trace feature
  --enable-perf-profiling   Enable function performance profiling
  -v=n                      Set log verbose level (0 to 5, default is 2), larger with more log
Examples: wamrc -o test.aot test.wasm
          wamrc --target=i386 -o test.aot test.wasm
          wamrc --target=i386 --format=object -o test.o test.wasm
AoT-compiled module compatibility among WAMR versions
When making major ABI changes for AoT-compiled modules, we bump AOT_CURRENT_VERSION constant in core/config.h header. The runtime rejects to load a module AoT-compiled with wamrc with a non-compatibleAOT_CURRENT_VERSION.

We try our best to maintain our runtime ABI for AoT-compiled modules compatible among WAMR versions with compatible AOT_CURRENT_VERSION so that combinations of older wamrc and newer runtime usually work.

However, there might be minor incompatibilities from time to time. For example, we usually avoid bumping the version when making a change which affects only a small fraction of users. For productions, we recommend using exactly same versions of wamrc and the runtime.

WAMR version	AOT_CURRENT_VERSION	Compatible AOT version	
1.x	3	3	
2.0.0	3	3	
2.1.x	3	3	
2.2.0	3	3	
2.3.0	4	3,4	
2.4.0	4	3,4	See the following note
2.4.1	5	5	
Note: 2.4.0 had a broken AoT versioning. See issue 4504 for details. We recommend all 2.4.0 users to migrate to 2.4.1.

AoT compilation with 3rd-party toolchains
wamrc uses LLVM to compile wasm bytecode to AoT file, this works for most of the architectures, but there may be circumstances where you want to use 3rd-party toolchains to take over some steps of the compilation pipeline, e.g.

The upstream LLVM doesn't support generating object file for your CPU architecture (such as ARC), then we may need some other assembler to do such things.
You may get some other LLVM-based toolchains which may have better optimizations for the specific target, then you may want your toolchain to take over all optimization steps.
wamrc provides two environment variables to achieve these:

WAMRC_LLC_COMPILER

When specified, wamrc will emit the optimized LLVM-IR (.bc) to a file, and invoke $WAMRC_LLC_COMPILER with -c -O3 to generate the object file.

Optionally, you can use environment variable WAMRC_LLC_FLAGS to overwrite the default flags.

WAMRC_ASM_COMPILER

When specified, wamrc will emit the text based assembly file (.s), and invoke $WAMRC_ASM_COMPILER with -c -O3 to generate the object file.

Optionally, you can use environment variable WAMRC_ASM_FLAGS to overwrite the default flags.

Usage example
WAMRC_LLC_COMPILER=/usr/local/opt/llvm@14/bin/clang WAMRC_LLC_FLAGS="--target=x86_64-pc-linux-gnu -mcmodel=medium -c -O3" ./wamrc -o test.aot test.wasm
Note: wamrc will verify whether the specified file exists and executable. If verification failed, wamrc will report a warning and fallback to normal pipeline. Since the verification is based on file, you must specify the absolute path to the binary even if it's in $PATH

Note: WAMRC_LLC_COMPILER has higher priority than WAMRC_ASM_COMPILER, if WAMRC_LLC_COMPILER is set and verified, then WAMRC_ASM_COMPILER will be ignored.

Note: the LLC and ASM in the env name just means this compiler will be used to compile the LLVM IR file/assembly file to object file, usually passing the compiler driver is the simplest way. (e.g. for LLVM toolchain, you don't need to pass /usr/bin/llc, using /usr/bin/clang is OK)

Note: You might need to set WAMRC_LLC_FLAGS/WAMRC_ASM_FLAGS to match whatever the wamrc command would automatically do. In the above example, -mcmodel=medium corresponds to wamrc --size-level=1, which is the default of wamrc on macOS.

Run WASM app in WAMR mini product build
Run the test.wasm or test.aot with WAMR mini product build:

./iwasm test.wasm   or
./iwasm test.aot
You will get the following output:

Hello world!
buf ptr: 0xffffc2c8
buf: 1234
If you would like to run the test app on Zephyr, we have embedded a test sample into its OS image. You will need to execute:

ninja run


--------------------------------

Embedding WAMR guideline
Note: This document is about how to embed WAMR into C/C++ host applications, for other languages, please refer to: Embed WAMR into Python, Embed WAMR into Go.

All the embedding APIs supported by the runtime are defined under folder core/iwasm/include. The API details are available in the header files.

Embed WAMR into developer's project
WAMR is designed to be easy embeddable in any project, a typical way of building WAMR is to use cmake, developer can configure features by setting cmake variables and then include the script runtime_lib.cmake under folder build-scripts in his CMakeList.txt, for example:

set (WAMR_BUILD_PLATFORM "linux")
set (WAMR_BUILD_TARGET "X86_64")
set (WAMR_BUILD_INTERP 1)
set (WAMR_BUILD_FAST_INTERP 1)
set (WAMR_BUILD_AOT 1)
set (WAMR_BUILD_LIBC_BUILTIN 1)
set (WAMR_BUILD_LIBC_WASI 1)
set (WAMR_BUILD_SIMD 1)
set (WAMR_ROOT_DIR path/to/wamr/root)

include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib ${WAMR_RUNTIME_LIB_SOURCE})

# include bh_read_file.h
include (${SHARED_DIR}/utils/uncommon/shared_uncommon.cmake)

add_executable (your_project main.c ${UNCOMMON_SHARED_SOURCE})

target_link_libraries (your_project vmlib -lm)
Examples can be found in CMakeLists.txt of linux platform and other platforms. The available features to configure can be found in Build WAMR vmcore.

Developer can also use Makefile to embed WAMR, by defining macros and including directories, and adding the source files, examples can be found in makefile of alios-things platform and makefile of nuttx platform.

The runtime initialization
  #include "bh_platform.h"
  #include "bh_read_file.h"
  #include "wasm_export.h"

  char *buffer, error_buf[128];
  wasm_module_t module;
  wasm_module_inst_t module_inst;
  wasm_function_inst_t func;
  wasm_exec_env_t exec_env;
  uint32 size, stack_size = 8092, heap_size = 8092;

  /* initialize the wasm runtime by default configurations */
  wasm_runtime_init();

  /* read WASM file into a memory buffer */
  buffer = bh_read_file_to_buffer(…, &size);

  /* add line below if we want to export native functions to WASM app */
  wasm_runtime_register_natives(...);

  /* parse the WASM file from buffer and create a WASM module */
  module = wasm_runtime_load(buffer, size, error_buf, sizeof(error_buf));

  /* create an instance of the WASM module (WASM linear memory is ready) */
  module_inst = wasm_runtime_instantiate(module, stack_size, heap_size,
                                         error_buf, sizeof(error_buf));
The wasm_runtime_init() uses the default memory allocator os_malloc/os_free function from the core/shared/platform for the runtime memory management.

WAMR supports to restrict its all memory allocations in a raw buffer. It ensures the dynamic memories used by the WASM applications won't harm the system availability, which is extremely important for embedded systems. This can be done by using wasm_runtime_full_init(). This function also allows you to configure the native API's for exporting to WASM app, and set the maximum thread number when multi-thread feature is enabled.

Refer to the following sample:

/* the native functions that will be exported to WASM app */
static NativeSymbol native_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(display_input_read, "(*)i"),
    EXPORT_WASM_API_WITH_SIG(display_flush, "(iiii*)")
};

/* all the runtime memory allocations are retricted in the global_heap_buf array */
static char global_heap_buf[512 * 1024];
RuntimeInitArgs init_args;
memset(&init_args, 0, sizeof(RuntimeInitArgs));

/* configure the memory allocator for the runtime */
init_args.mem_alloc_type = Alloc_With_Pool;
init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

/* configure the native functions being exported to WASM app */
init_args.native_module_name = "env";
init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
init_args.native_symbols = native_symbols;

/* set maximum thread number if needed when multi-thread is enabled,
   the default value is 4 */
init_args.max_thread_num = max_thread_num;

/* initialize runtime environment with user configurations*/
if (!wasm_runtime_full_init(&init_args)) {
    return -1;
}
Native calls WASM functions and passes parameters
After a module is instantiated, the runtime embedder can lookup the target WASM function by name, and create execution environment to call the function.

  /* lookup a WASM function by its name
     The function signature can NULL here */
  func = wasm_runtime_lookup_function(module_inst, "fib");

  /* creat an execution environment to execute the WASM functions */
  exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
There are several ways to call WASM function:

Function call with parameters in an array of 32 bits elements and size:
  uint32 argv[2];

  /* arguments are always transferred in 32-bit element */
  argv[0] = 8;

  /* call the WASM function */
  if (wasm_runtime_call_wasm(exec_env, func, 1, argv) ) {
      /* the return value is stored in argv[0] */
      printf("fib function return: %d\n", argv[0]);
  }
  else {
      /* exception is thrown if call fails */
      printf("%s\n", wasm_runtime_get_exception(module_inst));
  }
The parameters are transferred in an array of 32 bits elements. For parameters that occupy 4 or fewer bytes, each parameter can be a single array element. For parameters in types like double or int64, each parameter will take two array elements. The function return value will be sent back in the first one or two elements of the array according to the value type. See the sample code below:

  uint32 argv[6];
  char arg1 = 'a';
  int arg2 = 10;
  double arg3 = 1.0;
  int64 arg4 = 100;
  double ret;

  argv[0] = arg1;
  argv[1] = arg2;

  /**
   * use memory copy for 8-byte parameters rather than
   * *(double*)(&argv[2]) = arg3 here because some archs
   * like ARM, MIPS require the address must be 8-byte aligned.
   * Or use the aligned malloc or compiler align attribute
   * to ensure the array address is 8-byte aligned
   */
  memcpy(&argv[2], &arg3, sizeof(arg3));
  memcpy(&argv[4], &arg4, sizeof(arg4));

  /* attention: the arg number is 6 here since both
                arg3 and arg4 each takes 2 elements */
  wasm_runtime_call_wasm(exec_env, func, 6, argv);

  /* if the return value is type of 8 bytes, it takes
     the first two array elements */
  memcpy(&ret, &argv[0], sizeof(ret));
Function call with results and arguments both in wasm_val_t struct and size:
  uint32 num_args = 1, num_results = 1;
  wasm_val_t args[1], results[1];

  /* set the argument type and value */
  args[0].kind = WASM_I32;
  args[0].of.i32 = 8;

  /* call the WASM function */
  if (wasm_runtime_call_wasm_a(exec_env, func, num_results, results, num_args, args)) {
      /* the return value is stored in results */
      printf("fib function return: %d\n", results[0].of.i32);
  }
  else {
      /* exception is thrown if call fails */
      printf("%s\n", wasm_runtime_get_exception(module_inst));
  }
Function call with variant argument support:
  uint32 num_args = 1, num_results = 1;
  wasm_val_t results[1];

  /* call the WASM function */
  if (wasm_runtime_call_wasm_v(exec_env, func, 1, results, 1, 8)) {
      /* the return value is stored in results */
      printf("fib function return: %d\n", results[0].of.i32);
  }
  else {
      /* exception is thrown if call fails */
      printf("%s\n", wasm_runtime_get_exception(module_inst));
  }
Pass buffer to WASM function
If we need to transfer a buffer to WASM function, we can pass the buffer address through a parameter. Attention: The sandbox will forbid the WASM code to access outside memory, we must allocate the buffer from WASM instance's own memory space and pass the buffer address in instance's space (not the runtime native address).

There are two runtime APIs available for this purpose.

/**
 * malloc a buffer from instance's private memory space.
 *
 * return: the buffer address in instance's memory space (pass to the WASM function)
 * p_native_addr: return the native address of allocated memory
 * size: the buffer size to allocate
 */
uint64_t
wasm_runtime_module_malloc(wasm_module_inst_t module_inst,
                           uint64_t size, void **p_native_addr);

/**
 * malloc a buffer from instance's private memory space,
 * and copy the data from another native buffer to it.
 *
 * return: the buffer address in instance's memory space (pass to the WASM function)
 * src: the native buffer address
 * size: the size of buffer to be allocated and copy data
 */
uint64_t
wasm_runtime_module_dup_data(wasm_module_inst_t module_inst,
                             const char *src, uint64_t size);

/* free the memory allocated from module memory space */
void
wasm_runtime_module_free(wasm_module_inst_t module_inst, uint64_t ptr);
Usage sample:

char * buffer = NULL;
uint64_t buffer_for_wasm;

buffer_for_wasm = wasm_runtime_module_malloc(module_inst, 100, &buffer);
if (buffer_for_wasm != 0) {
    uint32 argv[3];
    strncpy(buffer, "hello", 100); /* use native address for accessing in runtime */
    argv[0] = buffer_for_wasm;     /* pass the buffer address for WASM space */
    argv[2] = 100;                 /* the size of buffer */
    wasm_runtime_call_wasm(exec_env, func, 3, argv);

    /* it is runtime embedder's responsibility to release the memory,
       unless the WASM app will free the passed pointer in its code */
    wasm_runtime_module_free(module_inst, buffer_for_wasm);
}
Pass structured data to WASM function
We can't pass structure data or class objects through the pointer since the memory layout can different in two worlds. The way to do it is serialization. Refer to export_native_api.md for the details.

Execute wasm functions in multiple threads
It isn't safe to use an exec_env object in multiple threads concurrently. To run a multi-threaded application, you basically need a separate exec_env for each threads.

Approaches to manage exec_env objects and threads
WAMR supports two approaches to manage exec_env and threads as described below. While they are not exclusive, you usually only need to use one of them.

Make your WASM application manage threads
You can make your WASM application spawn threads by itself, typically using pthread APIs like pthread_create. See pthread library and pthread implementations for more details. In this case, WAMR manages exec_env for the spawned threads.

Make your embedder manage threads
The spawn exec_env and spawn thread APIs are available for the embedder. You can use these APIs to manage the threads. See Thread related embedder API for details.

Other notes about threads
You can manage the maximum number of threads

init_args.max_thread_num = THREAD_NUM;
/* If this init argument is not set, the default maximum thread number is 4 */
To share memory among threads, you need to build your WASM application with shared memory

For example, it can be done with --shared-memory and -pthread.

  /opt/wasi-sdk/bin/clang -o test.wasm test.c -nostdlib -pthread    \
    -Wl,--shared-memory,--max-memory=131072                         \
    -Wl,--no-entry,--export=__heap_base,--export=__data_end         \
    -Wl,--export=__wasm_call_ctors,--export=${your_func_name}
The corresponding threading feature should be enabled while building the runtime

WAMR lib-pthread (legacy)

cmake .. -DWAMR_BUILD_LIB_PTHREAD=1
wasi-threads

cmake .. -DWAMR_BUILD_LIB_WASI_THREADS=1
wasm_runtime_spawn_exec_env and wasm_runtime_spawn_thread

cmake .. -DWAMR_BUILD_THREAD_MGR=1 -DWAMR_BUILD_SHARED_MEMORY=1
The deinitialization procedure
  wasm_runtime_destroy_exec_env(exec_env);
  wasm_runtime_deinstantiate(module_inst);
  wasm_runtime_unload(module);
  wasm_runtime_destroy();

Native calling WASM function working flow
WAMR embed diagram


--------------------------------

Export native API to WASM application
Exporting native API steps
Step 1: Declare the function interface in WASM app
Create a header file in a WASM app and declare the functions that are exported from native. In this example, we declare foo and foo2 as below in the header file example.h

/*** file name: example.h  ***/

int  foo(int a, int b);
void foo2(char * msg, char * buffer, int buf_len);
Step 2: Define the native API
Then we should define the native functions in runtime source tree for handling the calls from the WASM app. The native function can be any name, for example foo_native and foo2 here:

int foo_native(wasm_exec_env_t exec_env , int a, int b)
{
    return a+b;
}

void foo2(wasm_exec_env_t exec_env, char * msg, uint8 * buffer, int buf_len)
{
    strncpy(buffer, msg, buf_len);
}
The first parameter exec_env must be defined using type wasm_exec_env_t which is the calling convention by WAMR.

The rest parameters should be in the same types as the parameters of WASM function foo(), but there are a few special cases that are explained in section "Buffer address conversion and boundary check". Regarding the parameter names, they don't have to be the same, but we would suggest using the same names for easy maintenance.

Step 3: Register the native APIs
Register the native APIs in the runtime, then everything is fine. It is ready to build the runtime software.

// Define an array of NativeSymbol for the APIs to be exported.
// Note: the array must be static defined since runtime
//       will keep it after registration
static NativeSymbol native_symbols[] =
{
    {
        "foo", 		// the name of WASM function name
     	foo_native, // the native function pointer
        "(ii)i"		// the function prototype signature
    },
    {
        "foo2", 		// the name of WASM function name
     	foo2, 			// the native function pointer
        "($*~)"			// the function prototype signature
    }
};

// initialize the runtime before registering the native functions
wasm_runtime_init();

int n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
if (!wasm_runtime_register_natives("env",
                                   native_symbols,
                                   n_native_symbols)) {
    goto fail1;
}

// natives registration must be done before loading WASM modules
module = wasm_runtime_load(buffer, size, error_buf, sizeof(error_buf));
Function signature:

The function signature field in NativeSymbol structure is a string for describing the function prototype. It is critical to ensure the function signature is correctly mapping the native function interface.

Each letter in the "()" represents a parameter type, and the one following after ")" represents the return value type. The meaning of each letter:

'i': i32
'I': i64
'f': f32
'F': f64
'r': externref (has to be the value of a uintptr_t variable), or all kinds of GC reference types when GC feature is enabled
'*': the parameter is a buffer address in WASM application
'~': the parameter is the byte length of WASM buffer as referred by preceding argument "*". It must follow after '*', otherwise, registration will fail
'$': the parameter is a string in WASM application
The signature can defined as NULL, then all function parameters are assumed as i32 data type.

Use EXPORT_WASM_API_WITH_SIG

The NativeSymbol element for foo2  above can be also defined with macro EXPORT_WASM_API_WITH_SIG. This macro can be used when the native function name is the same as the WASM symbol name.

static NativeSymbol native_symbols[] =
{
	EXPORT_WASM_API_WITH_SIG(foo2, "($*~)")   // wasm symbol name will be "foo2"
};
​

Call exported API in WASM application
Now we can call the exported native API in wasm application like this:

#include <stdio.h>
#include "example.h"   // where the APIs are declared

int main(int argc, char **argv)
{
    int a = 0, b = 1;
    char * msg = "hello";
    char buffer[100];

    int c = foo(a, b);   				// call into native foo_native()
    foo2(msg, buffer, sizeof(buffer));   // call into native foo2()

    return 0;
}
Build native lib into shared library and register it with iwasm application
Developer can also build the native library into a shared library and register it with iwasm application:

iwasm --native-lib=<lib file> <wasm file>
Refer to native lib sample for more details.

Buffer address conversion and boundary check
A WebAssembly sandbox ensures applications only access to its own memory with a private address space. When passing a pointer address from WASM to native, the address value must be converted to native address before the native function can access it. It is also the native world's responsibility to check the buffer length is not over its sandbox boundary.

The signature letter '$', '*' and '~' help the runtime do automatic address conversion and buffer boundary check, so the native function directly uses the string and buffer address. Notes: if '*' is not followed by '~', the native function should not assume the length of the buffer is more than 1 byte.

As function parameters are always passed in 32 bits numbers, you can also use 'i' for the pointer type argument, then you must do all the address conversion and boundary checking in your native function. For example, if you change the foo2 signature to "(iii)", then you will implement the native part as the following sample:

//
// If the function signature used i32 data type ("i")
// for buffer address or string parameters, here
// is how to do address conversion and boundary check manually
//
void foo2(wasm_exec_env_t exec_env,
          uint32 msg_offset,
          uint32 buffer_offset,
          int32 buf_len)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char *buffer;
    char * msg ;

    // do boundary check
    if (!wasm_runtime_validate_app_str_add(msg_offset))
        return 0;

    if (!wasm_runtime_validate_app_addr((uint64)buffer_offset, (uint64)buf_len))
        return;

    // do address conversion
    buffer = wasm_runtime_addr_app_to_native((uint64)buffer_offset);
    msg = wasm_runtime_addr_app_to_native((uint64)msg_offset);

    strncpy(buffer, msg, buf_len);
}
Sandbox security attention
The runtime builder should ensure not broking the memory sandbox when exporting the native function to WASM.

A ground rule:

Do the pointer address conversion in the native API if "$*" is not used for the pointer in the function signature
A few recommendations:

Never pass any structure/class object pointer to native (do data serialization instead)
Never pass a function pointer to the native
Note: while not recommended here, nothing prevents you from passing structure/function pointers as far as the native API is aware of and careful about the ABI used in the wasm module. For example, C function pointers are usually represented as table indexes which the native API can call with wasm_runtime_call_indirect() or similar. However, in this document, we don't recommend to implement your native API that way unless necessary because it needs extra carefulness.

Pass structured data or class object
We must do data serialization for passing structured data or class objects between the two worlds of WASM and native. There are two serialization methods available in WASM as below, and yet you can introduce more like json, cbor etc.

attributes container
restful request/response
Note the serialization library is separately compiled into WASM and runtime. And the source files are located in the folder "wamr-app-framework/app-framework/app-native-shared“ where all source files will be compiled into both worlds.

The following sample code demonstrates WASM app packs a response structure to buffer, then pass the buffer pointer to the native:

/*** file name: https://github.com/bytecodealliance/wamr-app-framework/blob/main/app-framework/base/app/request.c ***/

void api_response_send(response_t *response)
{
    int size;
    char * buffer = pack_response(response, &size);
    if (buffer == NULL)
        return;

    wasm_response_send(buffer, size); // calling exported native API
    free_req_resp_packet(buffer);
}
The following code demonstrates the native API unpack the WASM buffer to local native data structure:

/*** file name: https://github.com/bytecodealliance/wamr-app-framework/blob/main/app-framework/base/native/request_response.c  ***/

bool
wasm_response_send(wasm_exec_env_t exec_env, char *buffer, int size)
{
    if (buffer != NULL) {
        response_t response[1];

        if (NULL == unpack_response(buffer, size, response))
            return false;

        am_send_response(response);

        return true;
    }

    return false;
}
Native API implementation notes
Async thread termination
When threading support is enabled, a thread can be requested to terminate itself either explicitly by the wasm_runtime_terminate API or implicitly by cluster-wide activities like WASI proc_exit. If a call to your native function can take long or even forever, you should make it to respond to termination requests in a timely manner. There are a few implementation approaches for that:

Design your API so that it always returns in a timely manner. This is the most simple approach when possible.

Make your native function check the cluster state by calling wasm_cluster_is_thread_terminated from time to time. If wasm_cluster_is_thread_terminated returns true, make your native function return. Note: as of writing this, wasm_cluster_is_thread_terminated is not exported as a runtime API.

If your native function is a simple wrapper of blocking system call, you can probably use the wasm_runtime_begin_blocking_op and wasm_runtime_end_blocking_op runtime API. See the comment in wamr_export.h for details.

If your native function is very complex, it might be simpler to put the guts of it into a separate worker process so that it can be cleaned up by simply killing it.


------------------------------

Memory model and memory usage tunning
References:

Blog: Understand WAMR heap
Blog: Understand WAMR stacks
The memory model
The memory model of WAMR can be basically described as below:



Note:

global heap: the heap to allocate memory for runtime data structures, including wasm module, wasm module instance, exec env, wasm operand stack and so on. It is initialized by wasm_runtime_init or wasm_runtime_full_init. And for wasm_runtime_full_init, developer can specify the memory allocation mode with RuntimeInitArgs *init_args: allocate memory from a user defined byte buffer, from user defined allocation function, or from the platform's os_malloc function. Refer to wasm_export.h and Embedding WAMR guideline for more details. And developer can use wasm_runtime_malloc/wasm_runtime_free to allocate/free memory from/to the global heap.
wasm operand stack: the stack to store the operands required by wasm bytecodes as WebAssembly is based on a stack machine. If the exec_env is created by developer with wasm_runtime_create_exec_env, then its size is specified by wasm_runtime_create_exec_env, otherwise if the exec_env is created by runtime internally, e.g. by wasm_application_execute_main or wasm_application_execute_func, then the size is specified by wasm_runtime_instantiate.
linear memory: a contiguous, mutable array of raw bytes. It is created with an initial size but might be grown dynamically. For most compilers, e.g. wasi-sdk, emsdk, rustc or asc, normally it includes three parts, data area, auxiliary stack area and heap area. For wasi-sdk, the initial/max size can be specified with -Wl,--initial-memory=n1,--max-memory=n2, for emsdk, the initial/max size can be specified with -s INITIAL_MEMORY=n1 -s MAXIMUM_MEMORY=n2 -s ALLOW_MEMORY_GROWTH=1 or -s TOTAL_MEMORY=n, and for asc, they can be specified with --initialMemory and --maximumMemory flags.
If the memory access boundary check with hardware trap feature is enabled, e.g. in Linux/MacOS/Windows x86-64 by default, the linear memory is allocated by os_mmap from virtual address space instead of global heap.
aux stack: the auxiliary stack resides in linear memory to store some temporary data when calling wasm functions, for example, calling a wasm function with complex struct arguments. For wasi-sdk, the size can be specified with -z stack-size=n, for emsdk, the size can be specified with -s TOTAL_STACK=n.
app heap and libc heap: the heap to allocate memory for wasm app, note that app heap is created only when the malloc/free functions (or __new/__release functions for AssemblyScript) are not exported and runtime can not detect the libc heap. To export the malloc/free functions, for wasi-sdk and emsdk, developer can use -Wl,--export=malloc -Wl,--export=free options, for asc, developer can use --exportRuntime option. For app heap, the size is specified by wasm_runtime_instantiate. It is recommended to export the malloc/free functions and disable app heap. However, if you are using the old pthread implementation, you might need some workaround to avoid the libc heap as mentioned in WAMR pthread library. And developer can use wasm_runtime_module_malloc/wasm_runtime_module_free to allocate/free memory from/to app heap (or libc heap if malloc/free functions are exported).
__data_end global and __heap_base global: two globals exported by wasm application to indicate the end of data area and the base address of libc heap. For WAMR, it is recommended to export them as when there are no possible memory grow operations, runtime will truncate the linear memory into the size indicated by __heap_base, so as to reduce the footprint, or at least one page (64KB) is required by linear memory.
Tune the memory usage
Normally there are some methods to tune the memory usage:

set the global heap size with wasm_runtime_full_init
set the wasm operand stack size with wasm_runtime_create_exec_env or wasm_runtime_instantiate
set the linear memory size
set the auxiliary stack size
export malloc/free functions to use libc heap and disable app heap
set the app heap size with wasm_runtime_instantiate
use nostdlib mode, add -Wl,--strip-all: refer to How to reduce the footprint of building wasm app for more details
use XIP mode, refer to WAMR XIP (Execution In Place) feature introduction for more details
when using the Wasm C API in fast interpreter or AOT mode, set clone_wasm_binary=false in LoadArgs and free the wasm binary buffer (with wasm_byte_vec_delete) after module loading; wasm_module_is_underlying_binary_freeable can be queried to check if the wasm binary buffer can be safely freed (see the example); after the buffer is freed, wasm_runtime_get_custom_section cannot be called anymore
when using the wasm/AOT loader in fast interpreter or AOT mode, set wasm_binary_freeable=true in LoadArgs and free the wasm binary buffer (with wasm_byte_vec_delete) after module loading; wasm_runtime_is_underlying_binary_freeable can be queried to check if the wasm binary buffer can be safely freed; after the buffer is freed, wasm_runtime_get_custom_section cannot be called anymore
WAMR_BUILD_SHRUNK_MEMORY can be used to reduce the memory usage of WAMR, but it might affect the standard expected behavior of WAMR.

------------------------

Memory usage estimation for a module
This document aims to provide information useful to make a rough estimation of necessary memory to execute a WASM module.

Instead of trying to cover every possible configurations, the following configuration is assumed in this document:

Module is built with wasi-sdk
Module is loaded with wasm_runtime_load
AOT is used
WASI is used
libc heap is used
app heap is not used
The pthread implementation in wasi-libc, which is based on wasi-threads (WASM_ENABLE_LIB_WASI_THREADS) might be used
The another pthread implementation (WASM_ENABLE_LIB_PTHREAD) is not used
Module
The memory to store the module binary is allocated by the embedder and passed to wasm_runtime_load. While WAMR owns the buffer, WAMR might make in-place modifications to its contents.

Loaded module and its instances
Many of data structures for module and instances are allocated from the global heap. (aka. wasm_runtime_malloc)

AOT code section
Memory to load AOT machine code section.

Because this memory needs to be executable, depending on platforms, it's allocated from a separate allocator. For example, mmap and mprotect are used on POSIX-like platforms.

Linear memory
A WASM linear memory is either shared or non-shared.

A WASM linear memory has min and max sizes. (They correspond to wasm-ld's --init-memory and --max-memory options.) They are in the number of WASM pages, each of which is of 65536 bytes. The max is optional for non-shared memory. When omitted, it effectively means unlimited.

The linear memory is allocated via os_mmap and os_mem_commit/os_mprotect.

The min size of memory is allocated on instantiation. It can later grow up to the max size via the memory.grow instruction.

Libc heap
The libc heap is the last (highest address) part of linear memory, which might be dynamically grown with memory.grow instruction, when necessary to serve memory allocations within the module.

App heap
Not used for the above mentioned configuration.

You can safely disable the app heap creation by specifying 0 for the heap_size argument of wasm_runtime_instantiate. (It's automatically disabled if malloc/free are exported from the module.)

WASM stack
Operand stack is not used for AOT.

However, a small amount of WASM stack is used for call frames when certain features are enabled. (WASM_ENABLE_DUMP_CALL_STACK or WASM_ENABLE_PERF_PROFILING)

It's allocated from the global heap.

You can specify its size with the stack_size argument of wasm_runtime_instantiate and wasm_runtime_create_exec_env. (1 is the minimum because 0 means the default.)

AUX stack (aka. C shadow stack)
For the main thread, it's a part of the linear memory, between __data_end and __heap_base symbols. You can control the size of this stack with wasm-ld's -z stack-size option.

For threads created by pthread_create, libc allocates the stack for them dynamically from the libc heap. The size of this stack is inherited from the main thread's one unless overwritten with pthread_attr_setstacksize etc.

WAMR tries to detect overflow/underflow when updating the stack pointer global. For threads created by pthread_create, the detection mechanism is disabled as of writing this.

Native stack
The stack of the host environment thread which runs WAMR.

For threads created by pthread_create, WAMR automatically creates host threads to run those WASM threads. The stack size of these host threads are controlled by a build-time configuration. (APP_THREAD_STACK_SIZE_DEFAULT)

In some configurations, runtime overflow can be detected using hardware traps. (OS_ENABLE_HW_BOUND_CHECK)

In some configurations, explicit overflow detection logic can be emitted into AOT modules themselves. (cf. os_thread_get_stack_boundary, check_stack_boundary, wamrc --stack-bounds-checks=1/0)

Memory profiling
You can collect and dump detailed information about memory usage by actually running a module with the WASM_ENABLE_MEMORY_PROFILING build-time option.