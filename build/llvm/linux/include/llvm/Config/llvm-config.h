/* include/llvm/Config/llvm-config.h.  Generated from llvm-config.h.in by configure.  */
/*===------- llvm/Config/llvm-config.h - llvm configuration -------*- C -*-===*/
/*                                                                            */
/*                     The LLVM Compiler Infrastructure                       */
/*                                                                            */
/* This file is distributed under the University of Illinois Open Source      */
/* License. See LICENSE.TXT for details.                                      */
/*                                                                            */
/*===----------------------------------------------------------------------===*/

/* This file enumerates variables from the LLVM configuration so that they
   can be in exported headers and won't override package specific directives.
   This is a C header that can be included in the llvm-c headers. */

#ifndef LLVM_CONFIG_H
#define LLVM_CONFIG_H

/* Installation directory for binary executables */
#define LLVM_BINDIR "/home/ilezhankin/.local/opt/llvm-libc++/bin"

/* Time at which LLVM was configured */
#define LLVM_CONFIGTIME "Fri Feb  6 10:47:33 MSK 2015"

/* Installation directory for data files */
#define LLVM_DATADIR "/home/ilezhankin/.local/opt/llvm-libc++/share/llvm"

/* Target triple LLVM will generate code for by default */
#define LLVM_DEFAULT_TARGET_TRIPLE "x86_64-unknown-linux-gnu"

/* Installation directory for documentation */
#define LLVM_DOCSDIR "/home/ilezhankin/.local/opt/llvm-libc++/share/doc/llvm"

/* Define if threads enabled */
#define LLVM_ENABLE_THREADS 1

/* Installation directory for config files */
#define LLVM_ETCDIR "/home/ilezhankin/.local/opt/llvm-libc++/etc/llvm"

/* Has gcc/MSVC atomic intrinsics */
#define LLVM_HAS_ATOMICS 1

/* Host triple LLVM will be executed on */
#define LLVM_HOST_TRIPLE "x86_64-unknown-linux-gnu"

/* Installation directory for include files */
#define LLVM_INCLUDEDIR "/home/ilezhankin/.local/opt/llvm-libc++/include"

/* Installation directory for .info files */
#define LLVM_INFODIR "/home/ilezhankin/.local/opt/llvm-libc++/info"

/* Installation directory for man pages */
#define LLVM_MANDIR "/home/ilezhankin/.local/opt/llvm-libc++/man"

/* LLVM architecture name for the native architecture, if available */
#define LLVM_NATIVE_ARCH X86

/* LLVM name for the native AsmParser init function, if available */
#define LLVM_NATIVE_ASMPARSER LLVMInitializeX86AsmParser

/* LLVM name for the native AsmPrinter init function, if available */
#define LLVM_NATIVE_ASMPRINTER LLVMInitializeX86AsmPrinter

/* LLVM name for the native Disassembler init function, if available */
#define LLVM_NATIVE_DISASSEMBLER LLVMInitializeX86Disassembler

/* LLVM name for the native Target init function, if available */
#define LLVM_NATIVE_TARGET LLVMInitializeX86Target

/* LLVM name for the native TargetInfo init function, if available */
#define LLVM_NATIVE_TARGETINFO LLVMInitializeX86TargetInfo

/* LLVM name for the native target MC init function, if available */
#define LLVM_NATIVE_TARGETMC LLVMInitializeX86TargetMC

/* Define if this is Unixish platform */
#define LLVM_ON_UNIX 1

/* Define if this is Win32ish platform */
/* #undef LLVM_ON_WIN32 */

/* Installation prefix directory */
#define LLVM_PREFIX "/home/ilezhankin/.local/opt/llvm-libc++"

/* Define if we have the Intel JIT API runtime support library */
#define LLVM_USE_INTEL_JITEVENTS 0

/* Define if we have the oprofile JIT-support library */
#define LLVM_USE_OPROFILE 0

/* Major version of the LLVM API */
#define LLVM_VERSION_MAJOR 3

/* Minor version of the LLVM API */
#define LLVM_VERSION_MINOR 6

/* LLVM version string */
#define LLVM_VERSION_STRING "3.6.0svn"

#endif
