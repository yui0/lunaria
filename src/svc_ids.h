/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Every SVC number, in one enum.
 *
 * The numbers used to be written out by hand, block by block, each block
 * starting at a base someone had picked as "the first free id".  Two blocks
 * that grew into each other, or two entries given the same offset, bound one
 * trampoline to two handlers, and nothing reported it until a guest called the
 * wrong one.  Here the compiler counts: an entry is one more than the entry
 * before it, so no two entries can share a number.
 *
 * Rules:
 *  - Add an SVC by adding a line, anywhere; no number is written.  Put new
 *    ones before SVC_TRAMP_TOTAL, which must stay last.
 *  - `= X` is only for an alias, written directly after the entry it names
 *    (the count then carries on from there), and for the start of a range
 *    whose members have no names of their own (math banks, detours, …).
 *  - Numbers the code still spells as literals are pinned by the
 *    static_asserts at the end; everything else may move freely.
 *  - A host function reached through a dlsym()ed name that has no fixed
 *    handler (Vulkan and the like) needs no number at all: see
 *    SVC_HOSTCALL and host_call_tramp() in arm_exec.cpp.
 */
#ifndef LUNARIA_SVC_IDS_H
#define LUNARIA_SVC_IDS_H

#include <stdint.h>

/* JNINativeInterface has 233 entries (4 reserved + 229 functions), and the
 * guest indexes it by the offsets in <jni.h>.  Lunaria used to build only 229
 * slots and dispatch slot n to SVC n, which silently dropped the four
 * "critical" accessors (GetPrimitiveArrayCritical, ReleasePrimitiveArrayCritical,
 * GetStringCritical, ReleaseStringCritical) and shifted everything after them
 * by four: a guest calling ExceptionCheck (228) landed on a stub that always
 * answered "no exception", GetPrimitiveArrayCritical returned a jobject where
 * a raw element pointer was due, and NewDirectByteBuffer (229) and its
 * companions fell off the end of the table entirely.
 *
 * libswappy walks exactly that path — loadClass, ExceptionCheck, then
 * InMemoryDexClassLoader over a direct ByteBuffer holding its own dex — so it
 * never saw the failure and ran on with a null SwappyDisplayManager class.
 * See jni_vtable_svc() for the slot→SVC mapping. */
constexpr uint32_t JNI_VTABLE_COUNT = 233u; /* indices 0–232 */
constexpr uint32_t SVC_MATH_F1_COUNT     = 27u;
constexpr uint32_t SVC_MATH_F2_COUNT     = 9u;
constexpr uint32_t SVC_MATH_D1_COUNT     = 25u;
constexpr uint32_t SVC_MATH_D2_COUNT     = 7u;
constexpr uint32_t NUM_DETOURS           = 20u;
constexpr uint32_t NUM_ICALL_PROBES        = 16u;



// AConfiguration_getXxx() integer getters.
constexpr uint32_t ACFG_I_MCC = 0u;
constexpr uint32_t ACFG_I_MNC = 1u;
constexpr uint32_t ACFG_I_ORIENTATION = 2u;
constexpr uint32_t ACFG_I_TOUCHSCREEN = 3u;
constexpr uint32_t ACFG_I_DENSITY = 4u;
constexpr uint32_t ACFG_I_KEYBOARD = 5u;
constexpr uint32_t ACFG_I_NAVIGATION = 6u;
constexpr uint32_t ACFG_I_KEYSHIDDEN = 7u;
constexpr uint32_t ACFG_I_NAVHIDDEN = 8u;
constexpr uint32_t ACFG_I_SCREENSIZE = 9u;
constexpr uint32_t ACFG_I_SCREENLONG = 10u;
constexpr uint32_t ACFG_I_UIMODETYPE = 11u;
constexpr uint32_t ACFG_I_UIMODENIGHT = 12u;
constexpr uint32_t ACFG_I_LAYOUTDIR = 13u;
constexpr uint32_t ACFG_I_SCREENWIDTHDP = 14u;
constexpr uint32_t ACFG_I_SCREENHEIGHTDP = 15u;
constexpr uint32_t ACFG_I_SMALLESTSCREENWIDTHDP = 16u;
constexpr uint32_t ACFG_I_COUNT = 17u;

enum SvcId : uint32_t {
    /* svc #0 is the raw syscall trap; 1..228 are JNINativeInterface slots
     * (slot n is SVC n, see jni_vtable_svc()). */
    SVC_JNI_SLOT_FIRST_ = 0,
    SVC_JNI_SLOT_LAST_  = SVC_JNI_SLOT_FIRST_ + JNI_VTABLE_COUNT - 5u,
    SVC_JVM_GETENV,
    SVC_JVM_ATTACH,
    SVC_JVM_DESTROY,
    SVC_LOG_PRINT,
    SVC_LOG_WRITE,
    SVC_DLOPEN,
    SVC_DLSYM,
    SVC_DLCLOSE,

    SVC_MALLOC,
    SVC_FREE,
    SVC_CALLOC,
    SVC_REALLOC,
    SVC_MEMCPY,
    SVC_MEMMOVE,
    SVC_MEMSET,
    SVC_STRLEN,
    SVC_STRCPY,
    SVC_STRNCPY,
    SVC_STRCMP,
    SVC_STRNCMP,
    SVC_STRDUP,
    SVC_STRNDUP,
    SVC_STRCAT,
    SVC_STRNCAT,
    SVC_ABORT,
    SVC_PTHREAD_KEY, /* pthread_key_create/set/get/once/mutex/cond */
    SVC_PTHREAD_CREATE, /* pthread_create — queues fn for deferred run */

    SVC_ANW_FROM_SURFACE,
    SVC_ANW_ACQUIRE,
    SVC_ANW_RELEASE,
    SVC_ANW_GETWIDTH,
    SVC_ANW_GETHEIGHT,
    SVC_ANW_SETBUFGEO,
    SVC_ANW_TOSURFACE,

    SVC_EGL_GETDISPLAY,
    SVC_EGL_INITIALIZE,
    SVC_EGL_CHOOSECONFIG,
    SVC_EGL_CREATEWSURF,
    SVC_EGL_CREATEPBUF,
    SVC_EGL_CREATECTX,
    SVC_EGL_MAKECURRENT,
    SVC_EGL_SWAPBUF,
    SVC_EGL_DESTROYSURF,
    SVC_EGL_DESTROYCTX,
    SVC_EGL_TERMINATE,
    SVC_EGL_GETPROC,
    SVC_EGL_QUERYSURF,
    SVC_EGL_GETERROR,
    SVC_EGL_GETCFGATTRIB,
    SVC_EGL_QUERYSTR,
    SVC_EGL_SURFACEATTRIB,
    SVC_EGL_SWAPINTERVAL,
    SVC_EGL_GETCURCTX,
    SVC_EGL_GETCURSURF,
    SVC_DL_UNWIND_EXIDX,
    SVC_GL_Viewport,


    SVC_GL_BASE = SVC_GL_Viewport,
    SVC_GL_Clear,
    SVC_GL_ClearColor,
    SVC_GL_ClearDepthf,
    SVC_GL_ClearStencil,
    SVC_GL_Enable,
    SVC_GL_Disable,
    SVC_GL_DepthFunc,
    SVC_GL_DepthMask,
    SVC_GL_ColorMask,
    SVC_GL_Scissor,
    SVC_GL_FrontFace,
    SVC_GL_CullFace,
    SVC_GL_BlendFuncSeparate,
    SVC_GL_BlendEquationSeparate,
    SVC_GL_GetError,
    SVC_GL_GetString,
    SVC_GL_GetIntegerv,
    SVC_GL_PixelStorei,
    SVC_GL_ReadPixels,
    SVC_GL_Flush,
    SVC_GL_Finish,

    SVC_GL_GenBuffers,
    SVC_GL_BindBuffer,
    SVC_GL_BufferData,
    SVC_GL_BufferSubData,
    SVC_GL_DeleteBuffers,

    SVC_GL_GenTextures,
    SVC_GL_BindTexture,
    SVC_GL_ActiveTexture,
    SVC_GL_DeleteTextures,
    SVC_GL_TexParameteri,
    SVC_GL_TexImage2D,
    SVC_GL_TexSubImage2D,
    SVC_GL_CopyTexSubImage2D,
    SVC_GL_CompressedTexImage2D,
    SVC_GL_CompressedTexSubImage2D,
    SVC_GL_GenerateMipmap,

    SVC_GL_GenFramebuffers,
    SVC_GL_BindFramebuffer,
    SVC_GL_DeleteFramebuffers,
    SVC_GL_CheckFramebufferStatus,
    SVC_GL_FramebufferTexture2D,
    SVC_GL_FramebufferRenderbuffer,
    SVC_GL_GetFramebufferAttachmentParameteriv,

    SVC_GL_GenRenderbuffers,
    SVC_GL_BindRenderbuffer,
    SVC_GL_DeleteRenderbuffers,
    SVC_GL_RenderbufferStorage,

    SVC_GL_CreateShader,
    SVC_GL_ShaderSource,
    SVC_GL_CompileShader,
    SVC_GL_DeleteShader,
    SVC_GL_GetShaderiv,
    SVC_GL_GetShaderInfoLog,
    SVC_GL_GetShaderSource,

    SVC_GL_CreateProgram,
    SVC_GL_AttachShader,
    SVC_GL_LinkProgram,
    SVC_GL_UseProgram,
    SVC_GL_DeleteProgram,
    SVC_GL_GetProgramiv,
    SVC_GL_GetProgramInfoLog,
    SVC_GL_GetAttribLocation,
    SVC_GL_GetUniformLocation,
    SVC_GL_GetActiveAttrib,
    SVC_GL_GetActiveUniform,
    SVC_GL_BindAttribLocation,

    SVC_GL_Uniform1i,
    SVC_GL_Uniform1iv,
    SVC_GL_Uniform2iv,
    SVC_GL_Uniform3iv,
    SVC_GL_Uniform4iv,
    SVC_GL_Uniform1fv,
    SVC_GL_Uniform2fv,
    SVC_GL_Uniform3fv,
    SVC_GL_Uniform4fv,
    SVC_GL_UniformMatrix3fv,
    SVC_GL_UniformMatrix4fv,

    SVC_GL_EnableVertexAttribArray,
    SVC_GL_DisableVertexAttribArray,
    SVC_GL_VertexAttribPointer,
    SVC_GL_GetVertexAttribiv,
    SVC_GL_GetVertexAttribPointerv,

    SVC_GL_DrawArrays,
    SVC_GL_DrawElements,

    SVC_GL_StencilFunc,
    SVC_GL_StencilFuncSeparate,
    SVC_GL_StencilMask,
    SVC_GL_StencilOp,
    SVC_GL_StencilOpSeparate,

    SVC_GL_BlendFunc,
    SVC_GL_TexParameterf,
    SVC_GL_DepthRangef,
    SVC_GL_PolygonOffset,
    SVC_GL_LineWidth,
    SVC_GL_SampleCoverage,

    SVC_GL_Uniform1f,
    SVC_GL_Uniform2f,
    SVC_GL_Uniform3f,
    SVC_GL_Uniform4f,

    SVC_GL_VertexAttrib1f,
    SVC_GL_VertexAttrib2f,
    SVC_GL_VertexAttrib3f,
    SVC_GL_VertexAttrib4f,
    SVC_GL_VertexAttrib4fv,

    SVC_GL_GetFloatv,
    SVC_GL_GetBooleanv,
    SVC_GL_IsEnabled,
    SVC_GL_IsProgram,
    SVC_GL_IsShader,
    SVC_GL_IsTexture,
    SVC_GL_IsBuffer,
    SVC_GL_IsFramebuffer,
    SVC_GL_IsRenderbuffer,

    SVC_GL_BlendEquation,
    SVC_GL_BlendColor,
    SVC_GL_ReleaseShaderCompiler,
    SVC_GL_GetShaderPrecisionFormat,
    SVC_GL_UniformMatrix2fv,
    SVC_GL_VertexAttrib1fv,
    SVC_GL_VertexAttrib2fv,
    SVC_GL_VertexAttrib3fv,

    SVC_AEABI_UIDIV,
    SVC_AEABI_UIDIVMOD,
    SVC_AEABI_IDIV,
    SVC_AEABI_LDIVMOD,
    SVC_AEABI_ULDIVMOD, /* unsigned 64-bit division */
    SVC_LIBC_OPEN,
    SVC_LIBC_CLOSE,
    SVC_LIBC_READ,
    SVC_LIBC_WRITE,
    SVC_LIBC_LSEEK,
    SVC_LIBC_FOPEN,
    SVC_LIBC_FCLOSE,
    SVC_LIBC_FREAD,
    SVC_LIBC_FWRITE,
    SVC_LIBC_FSEEK,
    SVC_LIBC_FTELL,
    SVC_LIBC_STAT,
    SVC_LIBC_FSTAT,
    SVC_LIBC_MMAP,
    SVC_LIBC_MUNMAP,


    SVC_CLOCK_GETTIME,
    SVC_GETTIMEOFDAY,
    SVC_TIME,
    SVC_NANOSLEEP,
    SVC_USLEEP,
    SVC_GETENV,
    SVC_GETPID,
    SVC_GETTID,
    SVC_SCHED_YIELD,
    SVC_GETPAGESIZE,
    SVC_SYSCONF,
    SVC_RET0, /* generic success stub */
    SVC_ERRNO_ADDR, /* __errno */
    SVC_SYSPROP_GET, /* __system_property_get */

    SVC_PTHREAD_SELF,
    SVC_PTHREAD_KEY_CREATE,
    SVC_PTHREAD_KEY_DELETE,
    SVC_PTHREAD_SETSPECIFIC,
    SVC_PTHREAD_GETSPECIFIC,

    SVC_Z_INFLATEINIT2,
    SVC_Z_INFLATE,
    SVC_Z_INFLATEEND,
    SVC_Z_INFLATERESET,
    SVC_Z_CRC32,
    SVC_Z_ADLER32,

    SVC_Z_INFLATEINIT, /* inflateInit_(z, ver, size) */
    SVC_Z_DEFLATEINIT2, /* deflateInit2_(z,lvl,method,wbits,mem,strategy,ver,sz) */
    SVC_Z_DEFLATE, /* deflate(z, flush) */
    SVC_Z_DEFLATEEND, /* deflateEnd(z) */
    SVC_Z_DEFLATERESET, /* deflateReset(z) */

    SVC_ATOI,
    SVC_ATOL,
    SVC_STRTOL,
    SVC_STRTOUL,
    SVC_STRTOD,
    SVC_STRTOF,

    SVC_GL_GetTexParameteriv,


    // Math passthrough block.
    SVC_MATH_F1_BASE, /* float  fn(float) */
    SVC_MATH_F2_BASE = SVC_MATH_F1_BASE + SVC_MATH_F1_COUNT, /* float  fn(float,float) */
    SVC_MATH_D1_BASE = SVC_MATH_F2_BASE + SVC_MATH_F2_COUNT, /* double fn(double) */
    SVC_MATH_D2_BASE = SVC_MATH_D1_BASE + SVC_MATH_D1_COUNT, /* double fn(double,double) */

    // Extended libc passthrough.

    SVC_EXT_BASE = SVC_MATH_D2_BASE + SVC_MATH_D2_COUNT,
    SVC_MEMALIGN = SVC_EXT_BASE,
    SVC_POSIX_MEMALIGN,
    SVC_MEMCMP,
    SVC_MEMCHR,
    SVC_MEMRCHR,
    SVC_MEMMEM,
    SVC_STRCHR,
    SVC_STRRCHR,
    SVC_STRSTR,
    SVC_STRNLEN,
    SVC_STRCASECMP,
    SVC_STRCSPN,
    SVC_STRSPN,
    SVC_STRTOK_R,
    SVC_PTHREAD_EQUAL,
    SVC_SNPRINTF,
    SVC_SPRINTF,
    SVC_VSNPRINTF,
    SVC_VASPRINTF,
    SVC_PRINTF,
    SVC_FPRINTF,
    SVC_VPRINTF,
    SVC_VFPRINTF,
    SVC_PUTS,
    SVC_FPUTS,
    SVC_FPUTC,
    SVC_ASSERT2,
    SVC_LOG_VPRINT,
    SVC_SINCOS,
    SVC_SINCOSF,
    SVC_LDEXP,
    SVC_LDEXPF,
    SVC_MODF,
    SVC_MODFF,
    SVC_STRTOLL,
    SVC_STRTOULL,
    SVC_ACCESS,
    SVC_REALPATH,
    SVC_PREAD,
    SVC_PWRITE,
    SVC_OPENDIR,
    SVC_READDIR,
    SVC_CLOSEDIR,
    SVC_WCSLEN,
    SVC_WMEMCPY,
    SVC_WMEMMOVE,
    SVC_WMEMSET,
    SVC_ISSPACE,
    SVC_FGETS,
    SVC_FILENO,
    SVC_FEOF,
    SVC_BASENAME,
    SVC_EXIT,
    // __aeabi_* / fortify (_chk) / additional libc stubs
    SVC_AEABI_MEMSET, /* (dst, n, c) — note argument order */
    SVC_AEABI_MEMCLR, /* (dst, n) */
    SVC_STRLCPY,
    SVC_STRNCASECMP,
    SVC_TOLOWER,
    SVC_ISALPHA,
    SVC_ISDIGIT,
    SVC_ISALNUM,
    SVC_ISXDIGIT,
    SVC_MKDIR,
    SVC_GETCWD,
    SVC_UNLINK,
    SVC_RENAME,
    SVC_FTRUNCATE,
    SVC_READLINK,
    SVC_CLOCK,
    SVC_LOCALTIME_R,
    SVC_GMTIME_R,
    SVC_LOCALTIME,
    SVC_GMTIME,
    SVC_MKTIME,
    SVC_DIFFTIME,
    SVC_STRFTIME,
    SVC_UNAME,
    SVC_GETRLIMIT,
    SVC_MREMAP,
    SVC_WRITEV,
    SVC_STRERROR,
    SVC_SETLOCALE,
    SVC_RETM1, /* stub returning -1 on failure */
    SVC_PTHREAD_GETATTR_NP,
    SVC_PTHREAD_ATTR_GETSTACK,
    SVC_PTHREAD_ATTR_GETSTACKSZ,
    SVC_VSNPRINTF_CHK,
    SVC_VSPRINTF_CHK,
    SVC_ABS,
    SVC_SSCANF,
    SVC_VSSCANF,
    SVC_ISASCII,
    SVC_LIBC_MMAP2, /* __mmap2: offset is in pages */
    SVC_WAIT, /* cond_wait/join: yield slice each call */
    // Semaphores (real counters) — required for Boehm GC thread registration
    SVC_SEM_INIT,
    SVC_SEM_POST,
    SVC_SEM_WAIT,
    SVC_SEM_TRYWAIT,
    SVC_SEM_TIMEDWAIT,
    SVC_SEM_DESTROY,
    SVC_SEM_GETVALUE,
    // setjmp/longjmp: save/restore context in jmp_buf.
    SVC_SETJMP,
    SVC_LONGJMP,
    // ARM Linux kuser helpers (called via BLX 0xffff0fa0 / 0xffff0fc0 / 0xffff0fe0)
    SVC_KUSER_CMPXCHG,
    SVC_KUSER_GET_TLS,
    SVC_SYSCALL, /* syscall() shim (futex/gettid) */
    SVC_SBRK,
    SVC_BRK,

    SVC_ALOOPER_FORTHREAD,
    SVC_ALOOPER_PREPARE,
    SVC_ALOOPER_POLLONCE,
    SVC_ALOOPER_POLLALL,
    SVC_ALOOPER_WAKE,
    SVC_STATFS,
    SVC_STATVFS,
    SVC_CHDIR,
    // reliable exception-identification logging SVCs.
    SVC_EXC_FROM_NAME,
    SVC_EXC_RAISE,
    // getdtablesize() — required by mono's io-layer _wapi_handle_init to size _wapi_fd_reserve.
    SVC_GETDTABLESIZE,
    // bsearch with a guest comparator callback.  libmono imports.
    SVC_BSEARCH,
    // inline-detour logging SVCs (LUNARIA_TRACE_EXC).
    SVC_DETOUR_BASE,

    // Itanium/ARM C++ ABI one-time static-initialisation guards.
    SVC_CXA_GUARD_ACQUIRE = SVC_DETOUR_BASE + NUM_DETOURS,
    SVC_CXA_GUARD_RELEASE,
    SVC_CXA_GUARD_ABORT,
    SVC_CXA_PURE_VIRTUAL,
    SVC_AEABI_ATEXIT,


    SVC_BTOWC,
    SVC_WCTOB,
    SVC_TOWLOWER,
    SVC_TOWUPPER,
    SVC_ISWCTYPE,
    SVC_WCTYPE,
    SVC_MBRTOWC,
    SVC_WCRTOMB,
    SVC_WMEMCHR,
    SVC_STRCOLL,
    SVC_STRXFRM,
    SVC_STRCASESTR,
    SVC_STRSEP,

    // fp classification, fenv, wide-char classification/conversion
    SVC_ISNAN,
    SVC_ISINF,
    SVC_ISFINITE,
    SVC_SIGNBIT,
    SVC_FEGETROUND,
    SVC_FESETROUND,
    SVC_FECLEAREXCEPT,
    SVC_FERAISEEXCEPT,
    SVC_FETESTEXCEPT,
    SVC_ISWSPACE,
    SVC_ISWDIGIT,
    SVC_ISWALPHA,
    SVC_ISWUPPER,
    SVC_ISWLOWER,
    SVC_ISWPRINT,
    SVC_ISWPUNCT,
    SVC_ISWGRAPH,
    SVC_ISWALNUM,
    SVC_ISWBLANK,
    SVC_ISWCNTRL,
    SVC_WCTRANS,
    SVC_TOWCTRANS,
    SVC_STRTOLD,
    SVC_WCSTOD,
    SVC_WCSTOL,
    SVC_WCSTOUL,
    SVC_WCSTOLL,
    SVC_WCSTOULL,
    SVC_STRTOIMAX,
    SVC_STRTOUMAX,
    SVC_PTHREAD_ATTR_GETGUARD, /* getguardsize */
    // SVC_WCSLEN already defined at SVC_EXT_BASE+43 — skip duplicate
    SVC_WCSNCMP,
    SVC_WCSCMP,
    SVC_WCSCPY,
    SVC_WCSCAT,
    SVC_DIV,
    SVC_LDIV,
    // Unresolved-symbol stubs (instead of tramp(0)) for distinct bind vs runtime logs
    SVC_UNKNOWN_CALL,

    SVC_FREXP,
    SVC_RINT,
    SVC_LRAND48,
    SVC_SRAND48,
    SVC_STRPBRK,
    SVC_STRTOK,
    SVC_DUP2,
    SVC_CLOCK_GETRES,
    SVC_GETHOSTNAME,
    SVC_GETRUSAGE,
    SVC_VSPRINTF2,
    SVC_GETC,
    SVC_PUTCHAR,
    SVC_FPCLASSIFYF,
    SVC_INET_ADDR,
    SVC_FCNTL2,
    SVC_MONO_PATH_NORM,
    SVC_G_FILENAME_URI,

    SVC_LIBC_LSEEK64, /* lseek64(fd, r1:r2, whence_r3) */

    SVC_PTHREAD_MUTEX_INIT,
    SVC_PTHREAD_MUTEX_LOCK,
    SVC_PTHREAD_MUTEX_TRYLOCK,
    SVC_PTHREAD_MUTEX_UNLOCK,
    SVC_PTHREAD_MUTEX_DESTROY,
    SVC_PTHREAD_COND_INIT,
    SVC_PTHREAD_COND_DESTROY,
    SVC_PTHREAD_COND_SIGNAL,
    SVC_PTHREAD_COND_BROADCAST,
    SVC_PTHREAD_EXIT,
    SVC_ANW_SETFRAMERATE,
    SVC_PTHREAD_MUTEXATTR_NOOP, /* mutexattr_init/settype/destroy */
    SVC_PTHREAD_CONDATTR_NOOP, /* condattr_init/setclock/destroy */
    // pthread_rwlock: no real blocking under cooperative scheduling; track state for EBUSY
    SVC_PTHREAD_RWLOCK_INIT,
    SVC_PTHREAD_RWLOCK_RDLOCK,
    SVC_PTHREAD_RWLOCK_WRLOCK,
    SVC_PTHREAD_RWLOCK_UNLOCK,
    SVC_PTHREAD_RWLOCK_DESTROY,
    SVC_PTHREAD_RWLOCK_TRYRDLOCK,
    SVC_PTHREAD_RWLOCK_TRYWRLOCK,
    // pthread_join: wait for target thread finished flag
    SVC_PTHREAD_JOIN,
    // pthread_detach: mark thread detached
    SVC_PTHREAD_DETACH,
    // pthread_cond_wait/timedwait: check cond and schedule
    SVC_PTHREAD_COND_WAIT,
    SVC_PTHREAD_COND_TIMEDWAIT,
    // qsort: invoke guest comparator via call_guest_cb
    SVC_QSORT,
    // fdopen: register fd in g_file_tab, return guest shim
    SVC_FDOPEN,
    // strerror_r: write error string into buffer
    SVC_STRERROR_R,
    SVC_MONO_FILE_MAP_OPEN,
    SVC_MONO_FILE_MAP_SIZE,
    SVC_MONO_FILE_MAP_FD,
    SVC_MONO_FILE_MAP,
    SVC_G_FILENAME_FROM_URI,
    SVC_MONO_FILE_MAP_CLOSE,
    SVC_KUSER_DMB, /* 0xffff0fa0 */
    // Wrap mono_add_internal_call to probe Time/Transform icalls (LUNARIA_TRACE_ICALL).
    SVC_MONO_ADD_ICALL,
    SVC_ICALL_PROBE_BASE,
    // pread64/pwrite64: LP32 bionic passes the 64-bit offset as an 8-byte-aligned value.
    SVC_PREAD64 = SVC_ICALL_PROBE_BASE + NUM_ICALL_PROBES,
    SVC_PWRITE64,
    /* fstatfs/fstatvfs take a descriptor, not a path: they cannot share the SVC
     * with their path-taking siblings once the answer depends on which filesystem
     * was named. */
    SVC_FSTATFS,
    SVC_FSTATVFS,

    SVC_FMA, /* fma(double,double,double) */
    SVC_TOTAL = SVC_FMA,
    SVC_FMAF, /* fmaf(float,float,float) */
    SVC_SCALBN, /* scalbn(double,int) */
    SVC_SCALBNF, /* scalbnf(float,int) */
    SVC_ILOGB, /* ilogb(double)->int */
    SVC_ILOGBF, /* ilogbf(float)->int */

    SVC_DUP, /* dup(fd) */
    SVC_FERROR, /* ferror(FILE*) */
    SVC_REWINDDIR, /* rewinddir(DIR*) */
    SVC_MBTOWC, /* mbtowc(pwc,s,n) */
    SVC_MBRLEN, /* mbrlen(s,n,ps) */
    SVC_MBSRTOWCS, /* mbsrtowcs(dst,src,n,ps) */

    SVC_LOGB, /* logb(double)->double */
    SVC_LRINTF, /* lrintf(float)->int */
    SVC_EXPM1F, /* expm1f(float)->float */
    SVC_NANF, /* nanf(const char*)->float */
    SVC_WMEMCMP, /* wmemcmp(s1,s2,n)->int */
    SVC_SWPRINTF, /* swprintf(buf,n,fmt,...)->int */
    SVC_LOCALECONV, /* localeconv()->struct lconv* */
    SVC_SOCKETPAIR, /* socketpair(dom,type,prot,sv) */
    /* iswxdigit accepts a-f/A-F as well as 0-9; iswdigit does not, so the two are
     * not interchangeable — see the symbol table entry for why that mattered. */
    SVC_ACFG_SDKVER, /* AConfiguration_getSdkVersion */
    SVC_ACHOREOGRAPHER_GET, /* AChoreographer_getInstance */
    SVC_AASSETMGR_FROMJAVA, /* AAssetManager_fromJava */
    SVC_AASSETMGR_OPEN, /* AAssetManager_open */
    SVC_AASSET_GETBUFFER, /* AAsset_getBuffer */
    SVC_AASSET_GETLENGTH, /* AAsset_getLength */
    SVC_ACHOREOGRAPHER_POST, /* postFrameCallback */
    SVC_ACHOREOGRAPHER_POST64, /* postFrameCallback64 */
    SVC_ACHOREOGRAPHER_POSTDELAY, /* postFrameCallbackDelayed */
    SVC_MONO_PREP, /* mono config before jit init */
    SVC_ANW_LOCK,
    SVC_ANW_UNLOCK,
    SVC_AEABI_IDIV0,
    SVC_AEABI_LDIV0,
    SVC_AEABI_LLSL,
    SVC_AEABI_LLSR,
    SVC_ISFINITEF,
    SVC_WPRINTF,
    SVC_SWSCANF,
    SVC_LRINT, /* lrint(double)->long */
    /* iswxdigit accepts a-f/A-F as well as 0-9; iswdigit does not, so the two are
     * not interchangeable — see the symbol table entry for why that mattered. */
    SVC_ISWXDIGIT,
    /* clearerr(FILE*): it clears the end-of-file and error indicators, and a
     * no-op leaves a stream that has hit EOF permanently at EOF — the next
     * fread/fgets on it fails although the caller has just said to try again. */
    SVC_CLEARERR,
    SVC_SIGACTION, /* sigaction(signum,new,old) */
    SVC_PTHREAD_KILL, /* pthread_kill/tkill/kill(tid,sig) */
    SVC_BSD_SIGNAL, /* bsd_signal(signum,handler) */
    SVC_EGL_SYSTIME_FREQ, /* eglGetSystemTimeFrequencyNV() → u64 ticks/s */
    SVC_EGL_SYSTIME, /* eglGetSystemTimeNV() → u64 ticks */
    SVC_SIGSUSPEND, /* sigsuspend(mask): GC suspend loop */

    // pipe/pipe2: host-backed pipes (fds live in the same table as open() fds)
    SVC_PIPE,
    SVC_PIPE2,
    SVC_ALOOPER_ADDFD, /* ALooper_addFd → 1 on success */

    // AAudio (FMOD output/recorder path; API 26+).
    SVC_AAUDIO_CREATE_BUILDER, /* AAudio_createStreamBuilder(**b) */
    SVC_AAUDIO_OPEN_STREAM, /* AAudioStreamBuilder_openStream(b,**s) */
    SVC_AAUDIO_GET_FPB, /* AAudioStream_getFramesPerBurst */
    SVC_AAUDIO_GET_BUFSIZE, /* AAudioStream_getBufferSizeInFrames */
    SVC_AAUDIO_SET_BUFSIZE, /* AAudioStream_setBufferSizeInFrames */
    SVC_AAUDIO_GET_BUFCAP, /* AAudioStream_getBufferCapacityInFrames */
    SVC_AAUDIO_WAIT_STATE, /* AAudioStream_waitForStateChange */

    // getauxval(type): FMOD dlopen()s libc.
    SVC_GETAUXVAL,

    // AAudio builder setters that must record state for callback pumping
    SVC_AAUDIO_SET_DIRECTION,
    SVC_AAUDIO_SET_DATA_CB,
    SVC_AAUDIO_SET_FORMAT,
    SVC_AAUDIO_SET_CHANNELS,
    SVC_AAUDIO_SET_RATE,
    SVC_AAUDIO_START,
    SVC_AAUDIO_STOP, /* stop + close */

    // Shared SVC behind per-symbol stub trampolines for dlsym'd-but-unimplemented functions.
    SVC_UNKNOWN_SYM,

    // GLES 3.
    SVC_GL3_GetStringi,
    SVC_GL3_GetIntegeri_v,
    SVC_GL3_GetInternalformativ,
    SVC_GL3_GetProgramInterfaceiv,
    SVC_GL3_GetProgramResourceiv,
    SVC_GL3_GetProgramResourceName,
    SVC_GL3_GenVertexArrays,
    SVC_GL3_BindVertexArray,
    SVC_GL3_DeleteVertexArrays,
    SVC_GL3_IsVertexArray,
    SVC_GL3_BindSampler,
    SVC_GL3_BindBufferBase,
    SVC_GL3_BindBufferRange,
    SVC_GL3_MapBufferRange,
    SVC_GL3_UnmapBuffer,
    SVC_GL3_FlushMappedBufferRange,
    SVC_GL3_TexStorage2D,
    SVC_GL3_TexStorage3D,
    SVC_GL3_TexSubImage3D,
    SVC_GL3_ProgramParameteri,
    SVC_GL3_GetProgramBinary,
    SVC_GL3_ProgramBinary,
    SVC_GL3_FenceSync,
    SVC_GL3_ClientWaitSync,
    SVC_GL3_DeleteSync,
    SVC_GL3_InvalidateFramebuffer,
    SVC_GL3_DetachShader,
    SVC_GL3_DrawBuffers,
    SVC_GL3_DrawElementsBaseVertex,

    // cxa_throw logging (always on): identifies which managed exception IL2CPP throws.
    SVC_EXC_CXA_THROW,

    // GL extension entry points the host may or may not back.
    SVC_GLX_DebugMessageControl,
    SVC_GLX_DebugMessageCallback,
    SVC_GLX_DebugMessageInsert,
    SVC_GLX_ObjectLabel,
    SVC_GLX_GetObjectLabel,
    SVC_GLX_PushDebugGroup,
    SVC_GLX_PopDebugGroup,
    SVC_GLX_MarkerNop, /* EXT_debug_marker/label */
    SVC_GLX_BufferStorage,
    SVC_GLX_QueryCounter,
    SVC_GLX_GetQueryObjectui64v,
    SVC_GLX_DrawElemInstBaseVertex,
    SVC_GLX_BlendBarrier,

    // UE4 NativeActivity / AssetManager APIs not covered above
    SVC_AASSETMGR_OPENDIR,
    SVC_AASSETDIR_NEXT,
    SVC_AASSETDIR_CLOSE,
    SVC_AASSET_OPENFD32, /* openFileDescriptor(off_t*) */
    SVC_AASSET_OPENFD = SVC_AASSET_OPENFD32, /* alias */
    SVC_ACFG_NEW, /* AConfiguration_new */
    SVC_ACFG_GETLANG, /* getLanguage → write 2 chars */
    SVC_ACFG_GETCOUNTRY, /* getCountry → write 2 chars */
    SVC_ACFG_FROM_AM, /* fromAssetManager */
    SVC_ATOF,
    SVC_FREXPF,
    SVC_RAND,
    SVC_SRAND,
    SVC_GETENTROPY,
    SVC_SYSINFO,
    SVC_COMPRESS2,
    SVC_ISLOWER,
    SVC_ISUPPER,
    SVC_ISBLANK,
    SVC_TOUPPER,

    /* eventfd(2) — host-backed, like pipe(): guest fds are host fds.  UE's
     * FHttpManager and the task-graph use one as a wakeup handle; without it the
     * dlsym stub handed back 0, which is a perfectly usable fd number, so the
     * engine wrote its wakeups into stdin forever and never woke. */
    SVC_EVENTFD,
    SVC_EVENTFD_READ,
    SVC_EVENTFD_WRITE,
    /* SVC_ALOOPER_REMOVEFD is in the SVC_ABI_BASE block: it used to be an alias
     * for addFd, which is why removeFd's absent third and fourth arguments were
     * read as `ident` and `events`. */

    // ASensor* — Unity Input.
    SVC_ASENSOR_MGR_INSTANCE,
    SVC_ASENSOR_MGR_DEFAULT,
    SVC_ASENSOR_MGR_LIST,
    SVC_ASENSOR_MGR_CREATEQ,
    SVC_ASENSOR_MGR_DESTROYQ,
    SVC_ASENSOR_Q_ENABLE,
    SVC_ASENSOR_Q_DISABLE,
    SVC_ASENSOR_Q_SETRATE,
    SVC_ASENSOR_Q_HASEVENTS,
    SVC_ASENSOR_Q_GETEVENTS,
    SVC_ASENSOR_GETTYPE,
    SVC_ASENSOR_GETNAME,
    SVC_ASENSOR_GETVENDOR,
    SVC_ASENSOR_GETRES,
    SVC_ASENSOR_GETMINDELAY,

    // Extra libc / EGL / zlib / GLES3 symbols needed by UE arm64 (libUnreal).
    SVC_EGL_BIND_API, /* eglBindAPI → EGL_TRUE */
    SVC_MALLOC_USABLE_SIZE,
    SVC_ZLIB_VERSION,
    SVC_DL_ITERATE_PHDR,
    SVC_ANDROID_ABORT_MSG,
    SVC_PAUSE,
    SVC_MINCORE,
    SVC_SCHED_GETSCHEDULER,
    SVC_TZSET,
    SVC_STRFTIME_L,
    SVC_WCSCHR,
    SVC_SL_CREATE_ENGINE,
    SVC_GL_BLIT_FRAMEBUFFER,
    SVC_GL_TEX_IMAGE_3D,
    SVC_GL_DRAW_INSTANCED, /* Arrays/Elements Instanced */
    SVC_GL_HINT,
    SVC_GL_READ_BUFFER,
    SVC_GL_GEN_QUERIES,
    SVC_GL_QUERY_OPS, /* Begin/End/GetQueryObjectuiv */
    SVC_GL_SAMPLER_OPS, /* Gen/Delete/Parameteri */
    SVC_GL_MISC3_NOP, /* safe no-op GL3 */
    SVC_GL3_IsSync,
    SVC_GL_TexParameterfv,
    SVC_MPROTECT,

    // zlib size helpers.
    SVC_Z_COMPRESSBOUND,
    SVC_Z_DEFLATEBOUND,

    // GLES 3.
    SVC_GLX_TexBuffer,
    SVC_GLX_TexBufferRange,
    SVC_GLX_CopyImageSubData,
    SVC_GLX_Enablei,
    SVC_GLX_Disablei,
    SVC_GLX_ColorMaski,
    SVC_GLX_BlendEquationi,
    SVC_GLX_BlendEquationSepi,
    SVC_GLX_BlendFunci,
    SVC_GLX_BlendFuncSepi,
    SVC_GLX_GetPointerv,

    // OpenSL ES object model.
    SVC_SL_OBJ_REALIZE,
    SVC_SL_OBJ_GETSTATE,
    SVC_SL_OBJ_GETINTERFACE,
    SVC_SL_ENG_CREATE_OUTMIX,
    SVC_SL_ENG_CREATE_PLAYER,
    SVC_SL_BQ_REGISTER,
    SVC_SL_BQ_ENQUEUE,
    SVC_SL_BQ_GETSTATE,
    // sched_getaffinity(pid, setsize, cpu_set_t*).
    SVC_SCHED_GETAFFINITY,

    // GLES 3.
    SVC_GL3_ClearBufferfv,
    SVC_GL3_ClearBufferiv,
    SVC_GL3_ClearBufferuiv,
    SVC_GL3_ClearBufferfi,
    SVC_GL3_GetUniformBlockIndex,
    SVC_GL3_UniformBlockBinding,
    SVC_GL3_GetActiveUniformBlockiv,
    SVC_GL3_GetUniformIndices,
    SVC_GL3_GetActiveUniformsiv,
    SVC_GL3_FramebufferTextureLayer,
    SVC_GL3_CopyBufferSubData,
    SVC_GL3_RenderbufferStorageMS,
    SVC_GL3_BindImageTexture,
    SVC_GL3_MemoryBarrier,
    SVC_GL3_DispatchCompute,
    SVC_GL3_BindVertexBuffer,
    SVC_GL3_VertexAttribFormat,
    SVC_GL3_VertexAttribIFormat,
    SVC_GL3_VertexAttribBinding,
    SVC_GL3_VertexBindingDivisor,
    SVC_GL3_TexStorage2DMS,
    SVC_GL3_Uniform4uiv,
    SVC_GL3_GetProgramResourceIndex,
    SVC_GL3_FramebufferTexture,
    SVC_GL3_FramebufferTexture3D,
    // GLES2 leftovers + GLES3.
    SVC_GL3_CopyTexImage2D,
    SVC_GL3_GetRenderbufferParameteriv,
    SVC_GL3_ValidateProgram,
    SVC_GL3_GetTexLevelParameterfv,
    SVC_GL3_GetTexLevelParameteriv,
    SVC_GL3_GetUniformiv,
    SVC_GL3_TexImage2DMultisample,
    SVC_GL3_TexParameteriv,
    SVC_GL3_Uniform1uiv,
    SVC_GL3_Uniform2uiv,
    SVC_GL3_Uniform3uiv,
    SVC_GL3_DeleteQueries,
    SVC_GL3_GetQueryiv,
    SVC_GL3_CompressedTexImage3D,
    SVC_GL3_GetActiveUniformBlockName,
    SVC_GL3_VertexAttribIPointer,
    SVC_GL3_ProgramUniform1fv,
    SVC_GL3_ProgramUniform1iv,
    SVC_GL3_ProgramUniform2fv,
    SVC_GL3_ProgramUniform2iv,
    SVC_GL3_ProgramUniform3fv,
    SVC_GL3_ProgramUniform3iv,
    SVC_GL3_ProgramUniform4fv,
    SVC_GL3_ProgramUniform4iv,
    SVC_GL3_ProgramUniformMatrix2fv,
    SVC_GL3_ProgramUniformMatrix3fv,
    SVC_GL3_ProgramUniformMatrix4fv,
    SVC_GL3_ProgramUniformMatrix2x3fv,
    SVC_GL3_ProgramUniformMatrix3x2fv,
    SVC_GL3_ProgramUniformMatrix2x4fv,
    SVC_GL3_ProgramUniformMatrix4x2fv,
    SVC_GL3_ProgramUniformMatrix3x4fv,
    SVC_GL3_ProgramUniformMatrix4x3fv,
    SVC_GL3_ProgramUniform1uiv,
    SVC_GL3_ProgramUniform2uiv,
    SVC_GL3_ProgramUniform3uiv,
    SVC_GL3_ProgramUniform4uiv,
    SVC_GL3_PatchParameteri,
    SVC_GL3_TexStorage3DMultisample,
    // Android app processes have no controlling terminal.
    SVC_TCGETATTR,
    SVC_TCSETATTR,
    SVC_TCFLUSH,
    SVC_GL3_BeginTransformFeedback,
    SVC_GL3_EndTransformFeedback,
    SVC_GL3_TransformFeedbackVaryings,
    SVC_GL3_BindTransformFeedback,
    SVC_GL3_DeleteTransformFeedbacks,
    SVC_GL3_GenTransformFeedbacks,
    SVC_ACFG_INT_BASE,
    SVC_ACFG_INT_END = SVC_ACFG_INT_BASE + ACFG_I_COUNT - 1u,

    // stdio / zlib / math entry points that libgnustl_shared.
    SVC_LIBC_REWIND,
    SVC_LIBC_FREOPEN,
    SVC_LIBC_TMPFILE,
    SVC_LIBC_TMPNAM,
    SVC_COMPRESS,
    SVC_FREXPL,
    SVC_ACFG_MATCH,
    // GL_OES_mapbuffer / GL_EXT_discard_framebuffer.
    SVC_GL_MapBufferOES,
    SVC_GL_UnmapBufferOES,
    SVC_GL_DiscardFramebufferEXT,

    // BSD sockets.
    SVC_NET_SOCKET,
    SVC_NET_SOCKETPAIR,
    SVC_NET_CONNECT,
    SVC_NET_BIND,
    SVC_NET_LISTEN,
    SVC_NET_ACCEPT,
    SVC_NET_ACCEPT4,
    SVC_NET_SEND,
    SVC_NET_SENDTO,
    SVC_NET_RECV,
    SVC_NET_RECVFROM,
    SVC_NET_SENDMSG,
    SVC_NET_RECVMSG,
    SVC_NET_SHUTDOWN,
    SVC_NET_SETSOCKOPT,
    SVC_NET_GETSOCKOPT,
    SVC_NET_GETSOCKNAME,
    SVC_NET_GETPEERNAME,
    SVC_NET_SELECT,
    SVC_NET_POLL,
    SVC_NET_GETADDRINFO,
    SVC_NET_FREEADDRINFO,
    SVC_NET_GAI_STRERROR,
    SVC_NET_GETHOSTBYNAME,
    SVC_NET_INET_NTOP,
    SVC_NET_INET_PTON,
    SVC_NET_INET_ATON,
    SVC_NET_INET_NTOA,
    SVC_NET_EPOLL_CREATE,
    SVC_NET_EPOLL_CTL,
    SVC_NET_EPOLL_WAIT,
    SVC_NET_IOCTL,
    SVC_NET_IF_NAMETOINDEX,
    SVC_NET_IF_INDEXTONAME,
    // bionic exports htons/htonl/ntohs/ntohl as real functions.
    SVC_NET_BSWAP16,
    SVC_NET_BSWAP32,

    // EGL_KHR_fence_sync / EGL 1.
    SVC_EGL_CREATE_SYNC,
    SVC_EGL_DESTROY_SYNC,
    SVC_EGL_CLIENT_WAIT_SYNC,
    SVC_EGL_GET_SYNC_ATTRIB,
    SVC_EGL_WAIT_SYNC,
    // glVertexAttribDivisor is ES 3.
    SVC_GL3_VertexAttribDivisor,
    SVC_GL3_IsQuery,
    // pthread_setname_np was a no-op, so every diagnostic that lists guest threads could only show numbers.
    SVC_PTHREAD_SETNAME,
    SVC_ATOLL,
    /* Entry points the loader previously left as "unresolved → stub", i.e. calls
     * that silently returned 0 and left their out-parameters untouched. */
    SVC_EGL_GETCURDPY,
    SVC_GL_GETINTEGER64V,
    SVC_ARC4RANDOM_BUF,
    /* POSIX regex.  libCrashSight matches thread names and library paths with
     * these; stubbed out, every match failed and its filters selected nothing. */
    SVC_REGCOMP,
    SVC_REGEXEC,
    SVC_REGFREE,
    // scandir(dir, &namelist, filter, compar) — enumeration with guest callbacks.
    SVC_SCANDIR,
    /* process_vm_readv: read guest memory without risking a fault, which is the
     * whole reason a crash handler reaches for it. */
    SVC_PROCESS_VM_READV,
    /* Scheduling policy.  These were unresolved imports, i.e. stubs returning 0 —
     * "your SCHED_FIFO request was granted" — and sched_get_priority_max/min sat
     * at SVC_RET0, so the whole usable priority band read back as [0,0]. */
    SVC_SCHED_SETSCHEDULER,
    SVC_SCHED_SETPARAM,
    SVC_SCHED_GETPARAM,
    SVC_SCHED_PRIO_MAX,
    SVC_SCHED_PRIO_MIN,
    /* ASharedMemory_* (libandroid, API 26+).  A stub returned 0, which is a
     * perfectly valid fd number — the guest then mmap()ed and ftruncate()d stdin. */
    SVC_ASHMEM_CREATE,
    SVC_ASHMEM_GETSIZE,
    SVC_ASHMEM_SETPROT,
    /* Wide-char stdio.  putwc/fputwc sat at SVC_RET0: the call reported success
     * (0 is not WEOF) while the character went nowhere. */
    SVC_FPUTWC,
    SVC_FPUTWS,
    // ANativeWindow_setBuffersTransform — dlsym'd out of libnativewindow.so.
    SVC_ANW_SETBUFTRANSFORM,
    /* EGLImage / GL_OES_EGL_image.  UE resolves these with eglGetProcAddress and
     * later calls through the saved pointers unconditionally — a NULL entry is a
     * NoExecuteFault, not a skipped optional path.  There is no dma-buf import
     * here, so an EGLImage is the 2D texture it wraps (same contract as mapping
     * GL_TEXTURE_EXTERNAL_OES → GL_TEXTURE_2D). */
    SVC_EGL_CREATE_IMAGE,
    SVC_EGL_DESTROY_IMAGE,
    SVC_GL_EGLImageTargetTexture2DOES,
    SVC_GL_EGLImageTargetTexStorageEXT,
    SVC_EGL_GET_NATIVE_CLIENT_BUFFER,
    /* EGL extensions the host string advertises but which had no trampoline —
     * UE/Mesa probe them via eglGetProcAddress; NULL means a later crash, not a
     * skipped optional path.  Each handler below matches the extension's contract
     * on this host (no dma-buf plane to export, fences are GL syncs, …). */
    SVC_EGL_SET_BLOB_CACHE,
    SVC_EGL_DUP_NATIVE_FENCE,
    SVC_EGL_GET_MSC_RATE,
    SVC_EGL_QUERY_DMABUF_FORMATS,
    SVC_EGL_SWAP_DAMAGE,
    SVC_EGL_EXPORT_DMABUF,
    SVC_EGL_GET_DRIVER_NAME,
    SVC_EGL_PRESENTATION_TIME,

    /* AChoreographer refresh-rate callbacks (API 30+) — see
     * post_refresh_rate_callback() for why answering these with a stub is not a
     * harmless omission. */
    SVC_ACHOREOGRAPHER_REG_RR,
    SVC_ACHOREOGRAPHER_UNREG_RR,

    /* NDK input queue.  A NativeActivity gets every touch through this path — the
     * native_app_glue that UE links reads it in process_input() — so answering
     * AInputQueue_getEvent with "no events" forever is not a missing extra: it is
     * an emulator with no touchscreen. */
    SVC_AINPUTQ_ATTACH,
    SVC_AINPUTQ_DETACH,
    SVC_AINPUTQ_HASEVENTS,
    SVC_AINPUTQ_GETEVENT,
    SVC_AINPUTQ_PREDISPATCH,
    SVC_AINPUTQ_FINISH,
    SVC_AINPUTEV_TYPE,
    SVC_AINPUTEV_SOURCE,
    SVC_AINPUTEV_DEVICEID,
    SVC_AMOTION_ACTION,
    SVC_AMOTION_POINTERCOUNT,
    SVC_AMOTION_POINTERID,
    SVC_AMOTION_X,
    SVC_AMOTION_Y,
    SVC_AMOTION_EVENTTIME,
    SVC_AMOTION_DOWNTIME,
    SVC_AMOTION_PRESSURE,
    SVC_AMOTION_SIZE,
    SVC_AMOTION_TOOLTYPE,
    SVC_AMOTION_AXISVALUE,

    /* AAsset stream reads.  AAssetManager_open already materialises the whole
     * entry, so the stream API is a cursor over that buffer — the NDK contract
     * every non-mmap reader (`AAsset_read` loops until it returns 0) relies on. */
    SVC_AASSET_READ,
    SVC_AASSET_SEEK,
    SVC_AASSET_SEEK64,
    SVC_AASSET_GETLENGTH64,
    SVC_AASSET_GETREMAINING,
    SVC_AASSET_GETREMAINING64,
    SVC_AASSET_ISALLOCATED,
    SVC_AASSET_CLOSE,

    /* JNIEnv slots 222–232.  The layout of the vtable is fixed by <jni.h>; the SVC
     * numbers behind it are Lunaria's own, and the low ones were handed out before
     * these entries were modelled at all.  Rather than renumber every SVC in the
     * file, the tail of the table maps explicitly — see jni_vtable_svc(). */
    SVC_JNI_GET_PRIM_CRITICAL,
    SVC_JNI_REL_PRIM_CRITICAL,
    SVC_JNI_GET_STR_CRITICAL,
    SVC_JNI_REL_STR_CRITICAL,
    SVC_JNI_NEW_DIRECT_BB,
    SVC_JNI_DIRECT_BB_ADDR,
    SVC_JNI_DIRECT_BB_CAP,
    SVC_JNI_OBJECT_REF_TYPE,
    SVC_OPENAT,
    SVC_FDOPENDIR,
    SVC_UNLINKAT,
    SVC_SIGISMEMBER,
    SVC_SIGEMPTYSET,
    SVC_SIGFILLSET,
    SVC_SIGADDSET,
    SVC_SIGDELSET,
    SVC_CFI_SLOWPATH,
    SVC_SIGPROCMASK,
    /* Kept so trampoline numbers after it stay put.  SwappyGL_swap itself is
     * no longer patched: the guest runs it, and the EGL timestamp entry points
     * below are what its swap path actually calls. */
    SVC_SWAPPY_GL_SWAP,
    /* alarm(2): arms a one-shot SIGALRM and answers what was left on the previous
     * one.  Stubbed to zero it always claimed "no alarm was pending", so a caller
     * that arms a watchdog and later cancels it reads back a lie. */
    SVC_ALARM,
    /* unshare(2): needs privileges Android apps do not have.  The stub's implicit
     * success told the guest it had its own namespace when nothing had changed;
     * EPERM is what the call really returns to an app. */
    SVC_UNSHARE,
    /* eglGetSyncValuesCHROMIUM: the counters a frame pacer reads to line its
     * submissions up with the display.  Without an entry point the whole
     * EGL_CHROMIUM_sync_control extension had to be stripped from the string. */
    SVC_EGL_GET_SYNC_VALUES,
    /* glDrawElementsInstanced.  It used to share SVC_GL_DRAW_INSTANCED with
     * glDrawArraysInstanced, and the handler could not tell them apart: every
     * indexed instanced draw was executed as glDrawArraysInstanced(mode, count,
     * type, indices) — first = the index count, count = the *type enum* (0x1403 =
     * 5123 vertices), instancecount = the index offset.  Whatever that submits, it
     * is not the geometry the guest asked for. */
    SVC_GL_DRAW_ELEM_INSTANCED,
    /* JavaVM::DetachCurrentThread.  The slot had no SVC of its own, so it kept the
     * default fill below — trampoline index 0, which on A64 is the raw-syscall
     * entry, not a stub that returns.  Every worker thread that attached, did its
     * JNI work and detached therefore ended its life on ENOSYS from a syscall it
     * never made. */
    SVC_JVM_DETACH,
    /* EGL_ANDROID_get_frame_timestamps.  Frame pacers (Swappy) look these up
     * with eglGetProcAddress and, when they are missing, either disable
     * themselves or wait forever for a present that can never be observed. */
    SVC_EGL_GET_NEXT_FRAME_ID,
    SVC_EGL_GET_FRAME_TIMESTAMPS,
    SVC_EGL_FRAME_TS_SUPPORTED,
    SVC_EGL_GET_COMPOSITOR_TIMING,
    SVC_EGL_COMPOSITOR_TIMING_SUP,
    /* Previously fell through to the unknown-symbol stub (silent 0 / untouched
     * out-params).  arc4random fills entropy; mallinfo reports heap shape;
     * signalfd is the crash-handler wake path. */
    SVC_ARC4RANDOM,
    SVC_MALLINFO,
    SVC_SIGNALFD,

    /* Occlusion queries.  These three used to share SVC_GL_QUERY_OPS, which was a
     * plain no-op — including glGetQueryObjectuiv, whose whole job is to write the
     * out-param.  UE4's RHI thread polls GL_QUERY_RESULT_AVAILABLE in a
     * sched_yield loop, so an untouched out-param that happens to hold 0 is an
     * RHI thread that never presents again.  (The 64-bit sibling
     * SVC_GLX_GetQueryObjectui64v always answered "available", which is why only
     * the 32-bit path hung.) */
    SVC_GL_BEGIN_QUERY,
    SVC_GL_END_QUERY,
    SVC_GL_GET_QUERY_OBJECT_UIV,
    /* clock_nanosleep(clkid, flags, req, rem) — its own entry, not an alias of
     * nanosleep(req, rem): the two put the timespec in different argument slots,
     * and reading the clock id as a pointer made every clock_nanosleep ask to
     * sleep for zero and spin instead. */
    SVC_CLOCK_NANOSLEEP,
    /* fflush(3).  It was bound to the generic "returns 0" stub, which is a lie the
     * guest cannot see through: the emulator keeps a real host FILE* per guest
     * stream, so an unflushed write is still sitting in the host's buffer when the
     * guest goes on to read the file back. */
    SVC_LIBC_FFLUSH,
    /* ANativeWindow::query for the fake native window.  Keeping the policy in the
     * host avoids baking an incomplete, version-specific switch into guest code. */
    SVC_ANW_QUERY,

    /* stdio pushback and the wide-character read side.  Both sat at SVC_RET0.
     * ungetc() returning 0 is indistinguishable from success for a caller that
     * only checks against EOF, so a parser that peeks one byte and pushes it back
     * silently lost it — the byte was never put anywhere, and the next getc()
     * returned the one after.  getwc() answering 0 is worse: 0 is L'\0', a
     * perfectly good wide character, so a read loop that stops at WEOF never
     * stops. */
    SVC_UNGETC,
    SVC_UNGETWC,
    SVC_GETWC,
    /* Per-object locales (POSIX 2008).  newlocale() returning NULL is the "out of
     * memory / unsupported locale" answer, and libc++'s std::locale constructor
     * turns that into a runtime_error; uselocale() returning NULL is not even a
     * legal locale_t.  Android has exactly one locale — C.UTF-8, under several
     * names — so these are cheap to answer truthfully. */
    SVC_NEWLOCALE,
    SVC_USELOCALE,
    SVC_FREELOCALE,
    SVC_DUPLOCALE,
    /* wcstold(): the wide-character long-double parse.  See SVC_STRTOLD for the
     * return width — on A64 a long double is a 128-bit quad in q0, not a double. */
    SVC_WCSTOLD,
    /* Wide-string collation.  Returning 0 from wcscoll means "these two strings
     * are equal", which turns every sort that uses it into a no-op and every
     * lookup keyed on it into a false hit. */
    SVC_WCSCOLL,
    SVC_WCSXFRM,
    /* wcsnrtombs(): the wide->multibyte direction of SVC_MBSRTOWCS. */
    SVC_WCSNRTOMBS,
    SVC_WCSRTOMBS,
    /* mbsnrtowcs() is not mbsrtowcs() with an extra argument: it takes the source
     * limit *before* the destination limit, so sharing one handler read the wrong
     * register as "how many wide characters fit" and wrote past the caller's
     * buffer whenever the two differed. */
    SVC_MBSNRTOWCS,
    /* rmdir(2).  It was bound to the "returns -1" template, so every attempt to
     * remove a directory failed — with no errno set, so the guest could not even
     * tell why.  A game that cleans up its own cache directory tree leaves it
     * behind and, worse, may treat the failure as "the directory is in use". */
    SVC_RMDIR,
    /* pthread_getschedparam / pthread_setschedparam.  The getter returning 0
     * without writing its two out-parameters is the dangerous one: the caller
     * reads an uninitialised policy and priority off its own stack and then hands
     * them straight back to the setter. */
    SVC_PTHREAD_GETSCHEDPARAM,
    SVC_PTHREAD_SETSCHEDPARAM,
    /* __sched_cpucount() is what CPU_COUNT() expands to.  Answering 0 tells the
     * caller its affinity mask contains no CPUs at all, which is how a worker-pool
     * size computed from "how many cores may I use" comes out as zero. */
    SVC_SCHED_CPUCOUNT,
    /* libc calls that were bound to the shared "return 0" template and then showed
     * up as `[stub] CALLED … nothing was done`.  A zero answer is often a lie the
     * guest acts on (getuid()=0 is root; dladdr()=0 means "no module"; setenv()=0
     * looks like success without writing).  Each gets its own trampoline. */
    SVC_GETUID,
    SVC_GETEUID,
    SVC_GETGID,
    SVC_GETEGID,
    SVC_PRCTL,
    SVC_SETPRIORITY,
    SVC_GETPRIORITY,
    SVC_MADVISE,
    SVC_MSYNC,
    SVC_SETENV,
    SVC_UNSETENV,
    SVC_PTHREAD_SIGMASK,
    SVC_DLADDR,
    SVC_DLERROR,
    SVC_FSCANF,
    SVC_FSYNC,
    SVC_FLOCK,
    /* Entry points that used to be bound to the generic "returns 0" / "returns
     * -1" templates and turned out to be called for real.  A template answer is a
     * guess about what the caller wanted; these are the answers the caller can
     * actually act on. */
    SVC_SCHED_SETAFFINITY,


    SVC_HONEST_BASE = SVC_SCHED_SETAFFINITY,
    SVC_CXA_ATEXIT,
    SVC_CXA_FINALIZE,
    SVC_ATEXIT,
    SVC_SETRLIMIT,
    SVC_CHMOD,
    SVC_FCHMOD,
    SVC_SYSTEM,
    SVC_FORK,
    SVC_ANA_SET_WINDOW_FORMAT,
    SVC_TRUNCATE,
    SVC_SYMLINK,
    SVC_LINK,
    SVC_FDATASYNC,
    SVC_UTIMENSAT,
    SVC_FCHMODAT,
    SVC_FNMATCH,
    SVC_LLDIV,
    SVC_PATHCONF,
    SVC_GETNAMEINFO,
    SVC_SETVBUF,
    SVC_ANW_GETFORMAT,
    SVC_ALOOPER_ACQUIRE,
    SVC_ALOOPER_RELEASE,
    SVC_PTHREAD_ATFORK,
    SVC_MLOCK,
    SVC_MUNLOCK,
    SVC_GETPWUID_R,
    SVC_CXA_THREAD_ATEXIT,
    /* pthread_condattr_setclock/getclock: a condvar may be created on
     * CLOCK_MONOTONIC, and its timedwait deadlines are then on that clock. */
    SVC_PTHREAD_CONDATTR_SETCLOCK,
    SVC_PTHREAD_CONDATTR_GETCLOCK,
    /* pthread_mutexattr_settype/gettype: NORMAL, RECURSIVE and ERRORCHECK are
     * three different contracts and a mutex has to know which one it was made
     * with. */
    SVC_PTHREAD_MUTEXATTR_SETTYPE,
    SVC_PTHREAD_MUTEXATTR_GETTYPE,
    /* pthread_attr_t is bionic's plain struct in guest memory, so the setters
     * write the same fields the getters above already read. */
    SVC_PTHREAD_ATTR_INIT,
    SVC_PTHREAD_ATTR_SETSTACKSZ,
    SVC_PTHREAD_ATTR_SETDETACH,
    SVC_PTHREAD_ATTR_GETDETACH,
    /* __pthread_cleanup_push/pop: the handler stack a thread unwinds through when
     * it is cancelled or exits. */
    SVC_PTHREAD_CLEANUP_PUSH,
    SVC_PTHREAD_CLEANUP_POP,
    /* Destroying an attribute has to leave it *invalid*, not untouched. */
    SVC_PTHREAD_MUTEXATTR_DESTROY,
    SVC_AASSET_OPENFD64,
    SVC_RAISE,
    SVC_SIGALTSTACK,
    SVC_SYSPROP_FIND,
    SVC_SYSPROP_READ,
    SVC_SYSPROP_READ_CB,
    SVC_AKEY_ACTION,
    SVC_AKEY_KEYCODE,
    SVC_AKEY_META,
    SVC_AKEY_FLAGS,
    SVC_ACFG_DELETE,
    SVC_ACFG_COPY,
    SVC_ACFG_DIFF,
    SVC_ACFG_SET_INT_BASE,
    SVC_ACFG_SET_INT_END = SVC_ACFG_SET_INT_BASE + ACFG_I_COUNT - 1u,
    SVC_ACFG_SETLANG,
    SVC_ACFG_SETCOUNTRY,
    SVC_ACFG_SET_SDKVER,
    SVC_UNWIND_FAIL,
    SVC_WAITPID,
    SVC_PTRACE,
    SVC_GETPPID,
    /* popen/pclose: an app that runs one of the device's own utilities and reads
     * its output does it through these as often as through fork()+execve().  They
     * were bound to the "returns -1" template, which says the process could not be
     * started at all — a state a device is never in for /system/bin/sh. */
    SVC_POPEN,
    SVC_PCLOSE,
    /* bionic's crt entry point.  Only a program image (an executable) calls it —
     * a shared object never does — so it appeared only once this emulator could
     * start one.  Left unbound it resolves to the "returns 0" template, and the
     * program returns from _start without ever entering main. */
    SVC_LIBC_INIT,
    SVC_HONEST_LAST = SVC_LIBC_INIT,
    SVC_FD_SET_CHK,
    /* bionic fd_set fortification, getresuid, strxfrm_l, and honest GL stubs. */
    SVC_COMPAT_BASE = SVC_FD_SET_CHK,
    SVC_FD_ISSET_CHK,
    SVC_FD_CLR_CHK,
    SVC_FD_ZERO_CHK,
    SVC_GETRESUID,
    SVC_STRXFRM_L,
    SVC_GL_UNIMPL,
    /* ctype classes that used to be answered by a different class entirely:
     * ispunct/isgraph/isprint were bound to isalnum and iscntrl to isspace.
     * '!' is punctuation and graphic but not alphanumeric, ' ' is printable but
     * not alphanumeric, and '\t' is a space but not a control-only answer — the
     * guest acts on the wrong classification wherever it parses text. */
    SVC_ISPUNCT,
    SVC_ISPRINT,
    SVC_ISCNTRL,
    SVC_ISGRAPH,
    /* AMotionEvent_getButtonState: the mouse/stylus buttons held during the
     * event.  A template zero is the right answer for a finger, but it is the
     * right answer by accident — the call has to look at the event. */
    SVC_AMOTION_BUTTONSTATE,
    /* gethostbyaddr(3): the reverse of gethostbyname, which is implemented.
     * Returning NULL from a stub told the caller the address has no name, which
     * is a lookup result it then acts on. */
    SVC_NET_GETHOSTBYADDR,
    /* getpwuid(3).  Android has no /etc/passwd, but bionic answers for app uids
     * from its own table: an app's name is u<user>_a<appid>, its home is the
     * data directory and its shell is /system/bin/sh.  NULL means "no such user",
     * which is not true of the uid the app is running as. */
    SVC_GETPWUID,
    /* sleep(3) takes seconds and returns the unslept seconds.  It cannot share
     * SVC_USLEEP: treating the same register as microseconds made sleep(1) a
     * one-microsecond delay and turned ordinary retry loops into busy loops. */
    SVC_SLEEP,
    /* std::__ndk1::condition_variable::wait(unique_lock<mutex>&) — libc++'s own
     * out-of-line instantiation, not a bionic/pthread symbol, so it needs its
     * own binding rather than reusing SVC_PTHREAD_COND_WAIT: r1 here is the
     * address of a stack-local unique_lock<mutex>, not a mutex* directly. Guest
     * ABI (verified by disassembling a real call site and resolving its
     * mutex::lock()/unlock() relocations, not assumed): unique_lock<mutex> is
     * {mutex_type *__m_; bool __owns_;} at offsets 0/8, and both libc++ mutex
     * and condition_variable hold their pthread_mutex_t/pthread_cond_t as the
     * sole member at offset 0 — so `this` (r0) already *is* the guest VA
     * SVC_PTHREAD_COND_WAIT wants for the cond, and *(r1) already *is* the one
     * it wants for the mutex. */
    SVC_CXX_CONDVAR_WAIT,
    /* android_get_application_target_sdk_version(3) and
     * android_get_device_api_level(3).  Both are bionic entry points, and both
     * are how native code asks which behaviour changes apply to it.  Unresolved
     * they bound to the return-0 template, and 0 is not "unknown": it is a
     * target older than API 1, which is what a repackaged or patched app looks
     * like.  The answers already exist — the manifest's target SDK and the
     * device profile's SDK_INT, the same two numbers Java sees. */
    SVC_ANDROID_TARGET_SDK,
    SVC_ANDROID_DEVICE_API_LEVEL,
    /* Android's FORTIFY entry points.  Each takes the destination's size as an
     * extra, compiler-supplied argument and dies if the operation would not fit
     * in it; binding them to the unchecked function of the same name — which is
     * what this used to do — turns every one of those checks off, so an overrun
     * Android catches at the call site runs here instead and lands on whatever
     * the program keeps after the buffer.  See fortify_check in dispatch_svc. */
    SVC_MEMCPY_CHK,
    SVC_MEMMOVE_CHK,
    SVC_MEMSET_CHK,
    SVC_STRCPY_CHK,
    SVC_STRNCPY_CHK,
    SVC_STRCAT_CHK,
    SVC_STRNCAT_CHK,
    SVC_STRLEN_CHK,
    SVC_STRLCPY_CHK,
    SVC_READ_CHK,
    SVC_WRITE_CHK,
    SVC_FGETS_CHK,
    SVC_POLL_CHK,
    SVC_PREAD64_CHK,
    SVC_PWRITE64_CHK,
    /* "Fails, and here is why" — as distinct from SVC_RETM1, which answers -1
     * and leaves errno holding whatever the last failed call put there.  A POSIX
     * caller reads errno to decide what to do next (retry, fall back, report), so
     * an unimplemented entry point has to say ENOSYS, and one that is refused
     * because an app process may not do it has to say EPERM. */
    SVC_RETM1_ENOSYS,
    SVC_RETM1_EPERM,
    /* umask(), and the fortified form bionic's headers redirect it to when the
     * mask is not a compile-time constant.  A library built against bionic with
     * _FORTIFY_SOURCE references __umask_chk and nothing else, so leaving it out
     * makes the whole .so unloadable, not just that one call. */
    SVC_UMASK,
    SVC_UMASK_CHK,
    /* pthread_mutex_timedlock(mutex, abstime) and the clock-explicit form
     * pthread_mutex_clocklock(mutex, clock, abstime).  A blocking lock with a
     * deadline is not the same operation as pthread_mutex_lock: a caller uses it
     * precisely so that a lock it cannot get does not become a hang. */
    SVC_PTHREAD_MUTEX_TIMEDLOCK,
    SVC_PTHREAD_MUTEX_CLOCKLOCK,
    /* <android/thermal.h> — the ADPF thermal API (API 30).  A game asks the
     * platform how close it is to throttling and drops quality settings when it
     * is; a library that links these cannot load at all without them. */
    SVC_ATHERMAL_ACQUIRE,
    SVC_ATHERMAL_RELEASE,
    SVC_ATHERMAL_STATUS,
    SVC_ATHERMAL_HEADROOM,
    SVC_CPP_NEW,
    SVC_Z_GET_CRC_TABLE,
    SVC_ALPHASORT,
    /* zError(err) and deflateInit_(strm, level, version, stream_size) — the
     * rest of the host zlib family Crashlytics (and any NEEDED on libz) imports.
     * Kept on the SVC side with inflate/deflate so one ABI owns every z_stream. */
    SVC_Z_ERROR,
    SVC_Z_DEFLATEINIT,
    SVC_COMPAT_LAST = SVC_Z_DEFLATEINIT,
    SVC_FAES_DECRYPT,
    // Host AES-ECB for FAES::DecryptData — UE pak indexes in this title need it.
    /* Host implementations of UE's own functions, reached by an inline detour that
     * overwrites the first instruction with `svc #N` — not by a symbol binding, so
     * these numbers never appear in kSymbolSvcMap. */
    SVC_UE_HOOK_BASE = SVC_FAES_DECRYPT,
    // Host SHA-1 for FSHA1::HashBuffer — startup profiler showed 27% of load time.
    SVC_FSHA1_HASHBUFFER,
    // Host CityHash64 — FName interning showed 11% of load time.
    SVC_CITYHASH64,
    /* Host OpenSSL DES-CBC.  This title decrypts its content with single DES and
     * the guest's own OpenSSL was 78% of every instruction the emulator executed
     * — 39.7 billion of them in 140 s, one thread, no SVCs, all of it inside
     * DES_ncbc_encrypt.  Same trade as FAES/FSHA1/CityHash above. */
    SVC_DES_NCBC,
    SVC_DES_EDE3_CBC,
    /* Host FGenericPlatformStricmp::Stricmp.  UE compares FNames and paths with
     * it a character at a time; it was 2.7% of every guest instruction on the load
     * screen.  One SVC for all the width combinations — which one a call is comes
     * from the address the SVC was taken at. */
    SVC_UE_STRICMP,
    /* Host FGenericPlatformStricmp::Strnicmp — the same function with a count.
     * UE reaches for it wherever it compares a prefix, and mounting this title's
     * patch paks (570k filenames, each turned into a package name) spends 10% of
     * every guest instruction in it. */
    SVC_UE_STRNICMP,
    /* Host FString::ReplaceInline.  Mounting this title's patch paks turns every
     * one of 570k pak entries into a package name, and each conversion normalises
     * the filename — which is a ReplaceInline of "\\" by "/".  That is 12% of
     * every guest instruction on the load screen, and it is the one shape of the
     * function that needs no allocation at all: search and replacement are the
     * same length, so the characters are overwritten in place.  The handler takes
     * only that shape and hands every other call back to the guest's own code
     * through a resume stub, so the growing path keeps its own semantics. */
    SVC_UE_REPLACE_INLINE,
    /* Host TStringViewImpl<T>::FindChar.  A one-character scan over a path, 9.5%
     * of the load screen: the loop is four instructions, so the guest pays for
     * fetch and decode rather than for the comparison.  Whole function, no
     * fallback — there is nothing in it to fall back to. */
    SVC_UE_FINDCHAR,
    /* Host CityHash32.  Same reasoning as SVC_CITYHASH64 above, and the same
     * emulator: FName's 32-bit hash (FCrc::StrCrc32 aside) and a handful of other
     * UE hash paths call CityHash32 directly, not just its 64-bit sibling —
     * profiling the post-title-load stall showed it alone at up to 40-50% of
     * every guest instruction in some windows, more than CityHash64 ever was.
     * Answered host-side with the same v1.1 algorithm UE bundles (verified
     * against the reference `cityhash` implementation on every code-length class:
     * 0-4, 5-12, 13-24 and >24 bytes, including a >64-byte string that loops the
     * main round more than once). */
    SVC_CITYHASH32,
    /* Host Ogg Vorbis decode via stb_vorbis (src/lib/stb_vorbis.c), for
     * FVorbisAudioInfo::ReadCompressedInfo/ReadCompressedData/StreamCompressedData.
     * Profiling the post-title-load stall (2026-09-04) found libvorbis itself
     * (mdct_backward, floor1_encode, oggpack_look, ...) dominating the guest's
     * instructions once a sound starts playing — Vorbis has no host bridge the
     * way MediaCodec's H.264 (openh264) and AAC (libavcodec) already do.
     *
     * SVC_STB_VORBIS_INFO is an *observe* hook: the displaced instruction runs
     * and the guest's own ReadCompressedInfo executes unmodified, so
     * FSoundQualityInfo — whose field layout this file has no source for — is
     * filled by the game itself, not guessed at here.  This SVC only opens a
     * parallel, independent stb_vorbis decoder from the same compressed bytes,
     * keyed by the `this` pointer, entirely separate from whatever internal
     * state FVorbisAudioInfo keeps (also not this file's business, for the same
     * reason).
     *
     * SVC_STB_VORBIS_READ *replaces* ReadCompressedData/StreamCompressedData
     * outright (same svc+ret patch as CityHash above): both are a closed
     * contract over plain bytes — `(uint8* Destination, bool bLooping,
     * uint32 BufferSize)`, fill Destination and say whether the sound is done —
     * with no struct to get wrong. */
    SVC_STB_VORBIS_INFO,
    SVC_STB_VORBIS_READ,
    /* Host UxCsv::FetchRow over UxBufferReader.  Must not share a number with
     * Vorbis above: CallSVC dispatches by handler id, and a collision sent
     * ReadCompressedInfo through the CSV path (and starved the host decoder). */
    SVC_UXCSV_FETCHROW,
    /* Host TStringConversion<FUTF8ToTCHAR_Convert,128>::Init for the inline
     * buffer case (output fits in 128 TCHAR).  ReloadInfoAll's stall PC sat in
     * this Init while FNk*InfoManager::Load turned every CSV field into an
     * FString. */
    SVC_UE_UTF8_TO_TCHAR,
    /* Host Audio::FLateReflectionsFast::GeneraterPlateModulations.  The plate
     * reverb's two modulation LFOs: one closed float loop per output sample, and
     * 8.8% of every guest instruction of Cross Worlds' post-title load (the
     * reverb submix runs for the loading music).  Whole function, with a resume
     * stub for the one shape that would have to allocate. */
    SVC_UE_PLATE_LFO,
    SVC_UE_HOOK_LAST = SVC_UE_PLATE_LFO,
    SVC_SIGACTION64,

    /* Android ABI block.
     *
     * Entry points whose *guest-visible shape* depends on which of two bionic
     * declarations the caller compiled against, plus a few that had been sharing
     * a handler with a near neighbour whose contract is not the same.
     *
     * On LP32 bionic keeps two signal ABIs side by side: `sigset_t` is 32 bits
     * and `sigset64_t` is 64, and `struct sigaction` and `struct sigaction64`
     * therefore have different layouts.  One SVC per pair meant every `sigset_t`
     * the guest handed us was read and written eight bytes wide, which runs four
     * bytes past the object the guest actually allocated.  On LP64 the two are
     * the same type, so both numbers land in the same handler there. */
    SVC_ABI_BASE = SVC_SIGACTION64,
    SVC_SIGEMPTYSET64,
    SVC_SIGFILLSET64,
    SVC_SIGADDSET64,
    SVC_SIGDELSET64,
    SVC_SIGISMEMBER64,
    SVC_SIGPROCMASK64,
    SVC_PTHREAD_SIGMASK64,
    SVC_SIGSUSPEND64,
    /* SIGRTMIN/SIGRTMAX are function calls in bionic, not constants: the platform
     * reserves the first few realtime signals for itself.  Answering 0 named the
     * "no signal" slot, so SIGRTMIN+n addressed the ordinary signals. */
    SVC_LIBC_SIGRTMIN,
    SVC_LIBC_SIGRTMAX,
    /* lstat() shared SVC_LIBC_STAT, i.e. it followed symlinks. */
    SVC_LIBC_LSTAT,
    /* media_status_t: 0 is AMEDIA_OK, so "unimplemented" cannot be spelled 0. */
    SVC_MEDIA_UNSUPPORTED,
    SVC_FUTIMENS,
    /* A 64-bit -1.  int64_t/ssize_t entry points that mean "nothing here" cannot
     * borrow SVC_RETM1, which only sets the low 32 bits: on AArch64 the caller
     * reads 4294967295 rather than -1. */
    SVC_RETM1_64,
    /* lunaria_fortify_fatal(what, want, have) -- the failing half of the
     * _FORTIFY_SOURCE checks, which now run as guest code in
     * liblunaria_guest.so.  Only a violation comes here, so the trap costs
     * nothing on the path that matters. */
    SVC_FORTIFY_FATAL,
    /* ALooper_removeFd(looper, fd) takes two arguments; ALooper_addFd takes six.
     * Sharing one SVC meant removeFd's r2/r3 -- whatever the caller happened to
     * leave there -- were read as `ident` and `events`, so a remove could be
     * taken for an add and re-register the descriptor it was asked to drop. */
    SVC_ALOOPER_REMOVEFD,
    /* AAssetDir_rewind(3): a documented NDK entry point that puts a directory
     * enumeration back at its first name.  Unbound it fell to the
     * return-a-constant stub, so the cursor never moved and a second walk of the
     * same AAssetDir came back empty. */
    SVC_AASSETDIR_REWIND,
    /* ANativeWindow::dequeueBuffer for the private-ABI window the emulator hands
     * out, and its pre-API-18 two-argument form.  A host handler rather than a
     * guest stub: what it has to publish -- an ANativeWindowBuffer and a fence
     * descriptor -- is emulator state, not arithmetic on the window struct. */
    SVC_ANW_DEQUEUE,
    SVC_ANW_DEQUEUE_DEP,
    SVC_NET_GETADDRINFOFORNET,
    SVC_PTHREAD_ATTR_SETSTACK,
    SVC_SETUID,
    SVC_SETGID,
    SVC_SETREUID,
    SVC_SETREGID,
    SVC_SETRESUID,
    SVC_SETRESGID,
    /* epoll_pwait(2) is not epoll_wait(2) with a spare argument: the mask it is
     * given replaces the calling thread's signal mask for exactly the length of
     * the wait, which is the whole reason the call exists -- it is how a thread
     * waits for a descriptor and a signal without the race of unblocking the
     * signal first.  Sharing epoll_wait's number dropped the mask on the floor.
     * epoll_pwait64 is the LP32 spelling that takes a sigset64_t (see
     * sigset_is_wide); on LP64 the two sets are the same type. */
    SVC_NET_EPOLL_PWAIT,
    SVC_NET_EPOLL_PWAIT64,
    /* AHardwareBuffer, CPU-backed.
     *
     * These were five entries returning a constant, which left the family saying
     * two different things: `fromHardwareBuffer` answered "there is no buffer"
     * while `describe` -- whose whole job is to fill the caller's
     * AHardwareBuffer_Desc -- answered "done" and wrote nothing, and
     * acquire/release claimed to take and drop a reference that never existed.
     * `allocate` was not bound at all, so a library that imports it could not
     * load once unresolved strong symbols became a load failure, exactly as on a
     * device where the symbol is in libnativewindow.so.
     *
     * So the emulator allocates the thing: an ordinary CPU-visible buffer in the
     * guest's own heap, a reference count, a lock that hands out the pixels and a
     * describe() that answers with the description the buffer was made from.
     * Usages that mean "and the GPU will read this" (sampled image, colour
     * output, cube map, data buffer, protected, video encode, sensor data) are
     * *refused* rather than allocated: nothing here can hand such a buffer to
     * EGL or Vulkan, and a buffer that allocates and then fails to bind is a
     * worse answer than one that says up front it cannot be had. */
    SVC_AHB_ALLOCATE,
    SVC_AHB_ACQUIRE,
    SVC_AHB_RELEASE,
    SVC_AHB_DESCRIBE,
    SVC_AHB_LOCK,
    SVC_AHB_LOCK_INFO,
    SVC_AHB_UNLOCK,
    SVC_AHB_GETID,
    SVC_AHB_IS_SUPPORTED,
    SVC_AHB_SOCKET,
    SVC_ABI_LAST = SVC_AHB_SOCKET,
    /* The float forms of the classification macros and of the wide-string
     * functions that take a maximum length.  They used to share the SVC of their
     * double / unbounded namesake, which is only correct when the two have the
     * same ABI signature — and none of these pairs does.  isnanf() reached a
     * handler that reads a double out of two registers; wcsnlen()'s limit was
     * dropped; and wcsncat(s, t, 0), which must append nothing, took the `n == 0`
     * branch into an unbounded wcscat(). */
    SVC_ISNANF,

    /* Entry points that had to stop sharing another function's SVC. */
    SVC_SPLIT_BASE = SVC_ISNANF,
    SVC_ISINFF,
    SVC_SIGNBITF,
    SVC_WCSTOF,
    SVC_WCSNLEN,
    SVC_WCSNCPY,
    SVC_WCSNCAT,
    /* GLES 3.0 sampler objects.  glGenSamplers/glDeleteSamplers/glSamplerParameter*
     * used to share one SVC whose handler did nothing at all, so glGenSamplers()
     * left the caller's array untouched: every id the guest then bound was
     * whatever had been on the stack, and the filter and wrap state a title set
     * through a sampler object was dropped while the same state set through
     * glTexParameteri took effect. */
    SVC_GL3_GenSamplers,
    SVC_GL3_DeleteSamplers,
    SVC_GL3_SamplerParameteri,
    SVC_GL3_SamplerParameterf,
    SVC_GL3_SamplerParameteriv,
    SVC_GL3_SamplerParameterfv,
    SVC_GL3_IsSampler,
    SVC_GL3_GetSamplerParameteriv,
    SVC_GL3_GetSamplerParameterfv,
    SVC_PTHREAD_ATTR_SETSCHEDPARAM,
    SVC_PTHREAD_ATTR_SETSCHEDPOLICY,
    SVC_ANW_SETFRAMERATE_STRATEGY,
    SVC_PTHREAD_ATTR_GETSCHEDPARAM,
    SVC_PTHREAD_ATTR_GETSCHEDPOLICY,
    SVC_PTHREAD_ATTR_DESTROY,
    /* h_errno is `(*__get_h_errno())` in bionic, so __get_h_errno() has to answer
     * with a real `int *`.  Bound to the generic "returns 0" stub it answered
     * NULL, and every guest that so much as reads h_errno after a failed
     * gethostbyname() dereferences it. */
    SVC_GET_H_ERRNO,
    /* setjmp/sigsetjmp are not one entry point: bionic follows BSD, where
     * setjmp/longjmp carry the signal mask and _setjmp/_longjmp do not, and
     * sigsetjmp takes the choice as an argument.  One SVC could not tell them
     * apart, so the buffer could not record which longjmp() owes a mask. */
    SVC_SETJMP_SIG,
    SVC_SIGSETJMP,
    /* pthread_attr_setguardsize: the getter was bound and the setter was not, so
     * a guard size the guest asked for was dropped and then read back wrong. */
    SVC_PTHREAD_ATTR_SETGUARD,
    /* pthread_mutexattr/condattr process-shared attribute (Bionic API). */
    SVC_PTHREAD_MUTEXATTR_SETPSHARED,
    SVC_PTHREAD_MUTEXATTR_GETPSHARED,
    SVC_PTHREAD_CONDATTR_SETPSHARED,
    SVC_PTHREAD_CONDATTR_GETPSHARED,
    /* fenv: the whole environment, not only the rounding mode.  These were bound
     * to the "returns 0" stub, which tells a caller that the FP environment was
     * saved and restored when nothing happened at all. */
    SVC_FEGETENV,
    SVC_FESETENV,
    SVC_FEHOLDEXCEPT,
    SVC_FEUPDATEENV,
    /* stdio locking, for real: see the handlers. */
    SVC_FLOCKFILE,
    SVC_FTRYLOCKFILE,
    SVC_FUNLOCKFILE,
    /* mkstemp/mkstemps/mkdtemp are ordinary bionic APIs. */
    SVC_MKSTEMP,
    SVC_MKSTEMPS,
    SVC_MKDTEMP,
    /* The rest of GLES 3.1, and the two 3.0 texture calls that were left out.
     *
     * These were the entry points still bound to SVC_GL_UNIMPL or to nothing at
     * all, and they are why glGetString(GL_VERSION) had to be capped below 3.1:
     * a bridge that answers "ES 3.1" owes the guest every 3.1 entry point, and
     * UE4 refuses to start at all below 3.1 ("Unable to run on this device!").
     * Forwarding them is what makes the 3.1 answer true.
     *
     * The scalar glProgramUniform*f forms take their values in s0.. on AArch64,
     * so they also appear in the hard-float adapter in arm_exec.cpp; the vector
     * (*v) forms already had SVCs and pass everything by pointer. */
    SVC_GL3_CopyTexSubImage3D,
    SVC_GL3_CompressedTexSubImage3D,
    SVC_GL3_DrawArraysIndirect,
    SVC_GL3_DrawElementsIndirect,
    SVC_GL3_DispatchComputeIndirect,
    SVC_GL3_FramebufferParameteri,
    SVC_GL3_GetFramebufferParameteriv,
    SVC_GL3_GetProgramResourceLocation,
    /* Separate shader objects (program pipelines). */
    SVC_GL3_UseProgramStages,
    SVC_GL3_ActiveShaderProgram,
    SVC_GL3_CreateShaderProgramv,
    SVC_GL3_BindProgramPipeline,
    SVC_GL3_DeleteProgramPipelines,
    SVC_GL3_GenProgramPipelines,
    SVC_GL3_IsProgramPipeline,
    SVC_GL3_GetProgramPipelineiv,
    SVC_GL3_GetProgramPipelineInfoLog,
    SVC_GL3_ValidateProgramPipeline,
    SVC_GL3_ProgramUniform1i,
    SVC_GL3_ProgramUniform2i,
    SVC_GL3_ProgramUniform3i,
    SVC_GL3_ProgramUniform4i,
    SVC_GL3_ProgramUniform1ui,
    SVC_GL3_ProgramUniform2ui,
    SVC_GL3_ProgramUniform3ui,
    SVC_GL3_ProgramUniform4ui,
    SVC_GL3_ProgramUniform1f,
    SVC_GL3_ProgramUniform2f,
    SVC_GL3_ProgramUniform3f,
    SVC_GL3_ProgramUniform4f,
    SVC_GL3_GetBooleani_v,
    SVC_GL3_MemoryBarrierByRegion,
    SVC_GL3_GetMultisamplefv,
    SVC_GL3_SampleMaski,
    SVC_SPLIT_LAST = SVC_GL3_SampleMaski,
    /* A host function bound at run time, found through the index its
     * trampoline carries (see HostCallFn in arm_exec.h).  One number for all of them. */
    SVC_HOSTCALL,
    /* Add new SVCs above this line.  One past the highest SVC: build_jni_tables()
     * builds a trampoline for every number below it, and the unknown-symbol pool
     * starts here. */
    SVC_TRAMP_TOTAL,
};

/* An A64 SVC immediate is 16 bits wide. */
static_assert(SVC_TRAMP_TOTAL <= 0x10000u, "SVC numbers must fit an A64 svc #imm16");

#endif
