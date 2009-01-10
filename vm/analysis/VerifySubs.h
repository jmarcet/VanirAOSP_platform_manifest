/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Dalvik bytecode verification subroutines.
 */
#ifndef _DALVIK_VERIFYSUBS
#define _DALVIK_VERIFYSUBS

/*
 * InsnFlags is a 32-bit integer with the following layout:
 *  0-15  instruction length (or 0 if this address doesn't hold an opcode)
 *  16    opcode flag (indicating this address holds an opcode)
 *  17    try block (indicating exceptions thrown here may be caught locally)
 *  30    visited (verifier has examined this instruction at least once)
 *  31    changed (set/cleared as bytecode verifier runs)
 */
typedef u4 InsnFlags;

#define kInsnFlagWidthMask      0x0000ffff
#define kInsnFlagInTry          (1 << 16)
#define kInsnFlagBranchTarget   (1 << 17)
#define kInsnFlagGcPoint        (1 << 18)
#define kInsnFlagVisited        (1 << 30)
#define kInsnFlagChanged        (1 << 31)

/* add opcode widths to InsnFlags */
bool dvmComputeCodeWidths(const Method* meth, InsnFlags* insnFlags,
    int* pNewInstanceCount);

/* set the "in try" flag for sections of code wrapped with a "try" block */
bool dvmSetTryFlags(const Method* meth, InsnFlags* insnFlags);

/* check switch targets and set the "branch target" flag for destinations */
bool dvmCheckSwitchTargets(const Method* meth, InsnFlags* insnFlags,
    int curOffset);

/* verify branch target and set "branch target" flag on the destination */
bool dvmCheckBranchTarget(const Method* meth, InsnFlags* insnFlags,
    int curOffset, bool selfOkay);

/* verification failure reporting */
#define LOG_VFY(...)                dvmLogVerifyFailure(NULL, __VA_ARGS__)
#define LOG_VFY_METH(_meth, ...)    dvmLogVerifyFailure(_meth, __VA_ARGS__)

/* log verification failure with optional method info */
void dvmLogVerifyFailure(const Method* meth, const char* format, ...);

/* log verification failure due to resolution trouble */
void dvmLogUnableToResolveClass(const char* missingClassDescr,
    const Method* meth);

/* extract the relative branch target from a branch instruction */
bool dvmGetBranchTarget(const Method* meth, InsnFlags* insnFlags,
    int curOffset, int* pOffset, bool* pConditional);

#endif /*_DALVIK_VERIFYSUBS*/
