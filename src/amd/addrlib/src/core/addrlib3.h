/*
************************************************************************************************************************
*
*  Copyright (C) 2023 Advanced Micro Devices, Inc.  All rights reserved.
*  SPDX-License-Identifier: MIT
*
***********************************************************************************************************************/


/**
************************************************************************************************************************
* @file  addrlib3.h
* @brief Contains the Addr::V3::Lib class definition.
************************************************************************************************************************
*/

#ifndef __ADDR3_LIB3_H__
#define __ADDR3_LIB3_H__

#include "addrlib.h"

namespace Addr
{
namespace V3
{

struct ADDR3_COORD
{
    INT_32  x;
    INT_32  y;
    INT_32  z;
};

// The HW address library utilizes an "addr_params" structure that is GPU-specific; therefore, we use a "void" pointer
// here to allow the HWL's to interpret this pointer with the appropriate structure.
// To reduce the frequency of conversion between the "ADDR3_COMPUTE_SURFACE_INFO_INPUT" structure and the "addr_params"
// structure, we create this super-structure to tie the two structures together.
struct ADDR3_COMPUTE_SURFACE_INFO_PARAMS_INPUT
{
    const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pSurfInfo;
    void*                                   pvAddrParams;
};

/**
************************************************************************************************************************
* @brief Bitmasks for swizzle mode determination on GFX12
************************************************************************************************************************
*/
const UINT_32 Gfx12Blk256KBSwModeMask = (1u << ADDR3_256KB_2D)  |
                                        (1u << ADDR3_256KB_3D);

const UINT_32 Gfx12Blk64KBSwModeMask  = (1u << ADDR3_64KB_2D)   |
                                        (1u << ADDR3_64KB_3D);

const UINT_32 Gfx12Blk4KBSwModeMask   = (1u << ADDR3_4KB_2D)    |
                                        (1u << ADDR3_4KB_3D);

const UINT_32 Gfx12Blk256BSwModeMask  = (1u << ADDR3_256B_2D);

/**
************************************************************************************************************************
* @brief Bit setting for swizzle pattern
************************************************************************************************************************
*/
union ADDR_BIT_SETTING
{
    struct
    {
        UINT_16 x;
        UINT_16 y;
        UINT_16 z;
        UINT_16 s;
    };
    UINT_64 value;
};

/**
************************************************************************************************************************
* @brief Flags for SwizzleModeTable
************************************************************************************************************************
*/
union SwizzleModeFlags
{
    struct
    {
        // Swizzle mode
        UINT_32 isLinear        : 1;    // Linear
        UINT_32 is2d            : 1;    // 2d mode
        UINT_32 is3d            : 1;    // 3d mode

        // Block size
        UINT_32 is256b          : 1;    // Block size is 256B
        UINT_32 is4kb           : 1;    // Block size is 4KB
        UINT_32 is64kb          : 1;    // Block size is 64KB
        UINT_32 is256kb         : 1;    // Block size is 256KB

        UINT_32 reserved        : 25;   // Reserved bits
    };

    UINT_32 u32All;
};

const UINT_32 Log2Size256  = 8u;

const UINT_32 Log2Size256K = 18u;

/**
************************************************************************************************************************
* @brief Swizzle pattern information
************************************************************************************************************************
*/
// Accessed by index representing the logbase2 of (8bpp/16bpp/32bpp/64bpp/128bpp)
// contains the indices which map to 2D arrays SW_PATTERN_NIBBLE[1-4] which contain sections of an index equation.
struct ADDR_SW_PATINFO
{
    UINT_8 nibble1Idx;
    UINT_8 nibble2Idx;
    UINT_8 nibble3Idx;
    UINT_8 nibble4Idx;
};

/**
************************************************************************************************************************
*   InitBit
*
*   @brief
*       Initialize bit setting value via a return value
************************************************************************************************************************
*/
#define InitBit(c, index) (1ull << ((c << 4) + index))

const UINT_64 X0  = InitBit(0,  0);
const UINT_64 X1  = InitBit(0,  1);
const UINT_64 X2  = InitBit(0,  2);
const UINT_64 X3  = InitBit(0,  3);
const UINT_64 X4  = InitBit(0,  4);
const UINT_64 X5  = InitBit(0,  5);
const UINT_64 X6  = InitBit(0,  6);
const UINT_64 X7  = InitBit(0,  7);
const UINT_64 X8  = InitBit(0,  8);

const UINT_64 Y0  = InitBit(1,  0);
const UINT_64 Y1  = InitBit(1,  1);
const UINT_64 Y2  = InitBit(1,  2);
const UINT_64 Y3  = InitBit(1,  3);
const UINT_64 Y4  = InitBit(1,  4);
const UINT_64 Y5  = InitBit(1,  5);
const UINT_64 Y6  = InitBit(1,  6);
const UINT_64 Y7  = InitBit(1,  7);
const UINT_64 Y8  = InitBit(1,  8);

const UINT_64 Z0  = InitBit(2,  0);
const UINT_64 Z1  = InitBit(2,  1);
const UINT_64 Z2  = InitBit(2,  2);
const UINT_64 Z3  = InitBit(2,  3);
const UINT_64 Z4  = InitBit(2,  4);
const UINT_64 Z5  = InitBit(2,  5);

const UINT_64 S0  = InitBit(3,  0);
const UINT_64 S1  = InitBit(3,  1);
const UINT_64 S2  = InitBit(3,  2);

/**
************************************************************************************************************************
* @brief Bit setting for swizzle pattern
************************************************************************************************************************
*/

/**
************************************************************************************************************************
* @brief This class contains asic independent address lib functionalities
************************************************************************************************************************
*/
class Lib : public Addr::Lib
{
public:
    virtual ~Lib();

    static Lib* GetLib(
        ADDR_HANDLE hLib);

    //
    // Interface stubs
    //

    // For data surface
    ADDR_E_RETURNCODE ComputeSurfaceInfo(
        const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_INFO_OUTPUT*      pOut) const;

    ADDR_E_RETURNCODE GetPossibleSwizzleModes(
        const ADDR3_GET_POSSIBLE_SWIZZLE_MODE_INPUT*   pIn,
        ADDR3_GET_POSSIBLE_SWIZZLE_MODE_OUTPUT*        pOut) const;

    ADDR_E_RETURNCODE ComputeSurfaceAddrFromCoord(
        const ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT*      pOut) const;

    // Misc
    ADDR_E_RETURNCODE ComputePipeBankXor(
        const ADDR3_COMPUTE_PIPEBANKXOR_INPUT* pIn,
        ADDR3_COMPUTE_PIPEBANKXOR_OUTPUT*      pOut);

    ADDR_E_RETURNCODE ComputeNonBlockCompressedView(
        const ADDR3_COMPUTE_NONBLOCKCOMPRESSEDVIEW_INPUT* pIn,
        ADDR3_COMPUTE_NONBLOCKCOMPRESSEDVIEW_OUTPUT*      pOut);

    ADDR_E_RETURNCODE ComputeSubResourceOffsetForSwizzlePattern(
        const ADDR3_COMPUTE_SUBRESOURCE_OFFSET_FORSWIZZLEPATTERN_INPUT* pIn,
        ADDR3_COMPUTE_SUBRESOURCE_OFFSET_FORSWIZZLEPATTERN_OUTPUT*      pOut);

    ADDR_E_RETURNCODE ComputeSlicePipeBankXor(
        const ADDR3_COMPUTE_SLICE_PIPEBANKXOR_INPUT* pIn,
        ADDR3_COMPUTE_SLICE_PIPEBANKXOR_OUTPUT*      pOut);

protected:
    Lib();  // Constructor is protected
    Lib(const Client* pClient);

    static const UINT_32 MaxImageDim  = 65536; // Max image size is 64k
    static const UINT_32 MaxMipLevels = 17;
    UINT_32 m_pipesLog2;                ///< Number of pipe per shader engine Log2
    UINT_32 m_pipeInterleaveLog2;       ///< Log2 of pipe interleave bytes

    SwizzleModeFlags m_swizzleModeTable[ADDR3_MAX_TYPE];  ///< Swizzle mode table

    // Number of unique MSAA sample rates (1/2/4/8)
    static const UINT_32 MaxMsaaRateLog2     = 4;
    // Max number of bpp (8bpp/16bpp/32bpp/64bpp/128bpp)
    static const UINT_32 MaxElementBytesLog2 = 5;
    // Number of unique swizzle patterns (one entry per swizzle mode + MSAA + bpp configuration)
    static const UINT_32 NumSwizzlePatterns  = 19 * MaxElementBytesLog2;

    // Number of equation entries in the table
    UINT_32              m_numEquations;

    // Swizzle equation lookup table according to swizzle mode, MSAA sample rate and bpp. This does not include linear.
    UINT_32              m_equationLookupTable[ADDR3_MAX_TYPE - 1][MaxMsaaRateLog2][MaxElementBytesLog2];

    // Equation table
    ADDR_EQUATION        m_equationTable[NumSwizzlePatterns];

    // Block dimension lookup table according to swizzle mode, MSAA sample rate and bpp. This includes linear.
    ADDR_EXTENT3D        m_blockDimensionTable[ADDR3_MAX_TYPE][MaxMsaaRateLog2][MaxElementBytesLog2];

    void SetEquationTableEntry(
        Addr3SwizzleMode swMode,
        UINT_32          msaaLog2,
        UINT_32          elementBytesLog2,
        UINT_32          value)
    {
        m_equationLookupTable[swMode - 1][msaaLog2][elementBytesLog2] = value;
    }

    const UINT_32 GetEquationTableEntry(
        Addr3SwizzleMode swMode,
        UINT_32          msaaLog2,
        UINT_32          elementBytesLog2) const
    {
        return m_equationLookupTable[swMode - 1][msaaLog2][elementBytesLog2];
    }

    const ADDR_EXTENT3D GetBlockDimensionTableEntry(
        Addr3SwizzleMode swMode,
        UINT_32          msaaLog2,
        UINT_32          elementBytesLog2) const
    {
        return m_blockDimensionTable[swMode][msaaLog2][elementBytesLog2];
    }

    static BOOL_32 Valid3DMipSliceIdConstraint(
        UINT_32 numSlices,
        UINT_32 mipId,
        UINT_32 slice)
    {
        return (Max((numSlices >> mipId), 1u) > slice);
    }

    UINT_32 GetBlockSize(
        Addr3SwizzleMode  swizzleMode,
        BOOL_32           forPitch = FALSE) const;

    UINT_32 GetBlockSizeLog2(
        Addr3SwizzleMode  swizzleMode,
        BOOL_32           forPitch = FALSE) const;

    BOOL_32 IsValidSwMode(Addr3SwizzleMode swizzleMode) const
    {
        return (m_swizzleModeTable[swizzleMode].u32All != 0);
    }

    UINT_32 IsLinear(Addr3SwizzleMode swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].isLinear;
    }

    // Checking block size
    BOOL_32 IsBlock256b(Addr3SwizzleMode swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is256b;
    }

    // Checking block size
    BOOL_32 IsBlock4kb(Addr3SwizzleMode swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is4kb;
    }

    // Checking block size
    BOOL_32 IsBlock64kb(Addr3SwizzleMode swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is64kb;
    }

    // Checking block size
    BOOL_32 IsBlock256kb(Addr3SwizzleMode swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is256kb;
    }

    BOOL_32  Is2dSwizzle(Addr3SwizzleMode  swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is2d;
    }

    BOOL_32  Is3dSwizzle(Addr3SwizzleMode  swizzleMode) const
    {
        return m_swizzleModeTable[swizzleMode].is3d;
    }

    // miptail is applied to only larger block size (4kb, 64kb, 256kb), so there is no miptail in linear and
    // 256b_2d addressing since they are both 256b block.
    BOOL_32 SupportsMipTail(Addr3SwizzleMode swizzleMode) const
    {
        return GetBlockSize(swizzleMode) > 256u;
    }

    // The max alignment is tied to the swizzle mode and since the largest swizzle mode is 256kb, so the maximal
    // alignment is also 256kb.
    virtual UINT_32 HwlComputeMaxBaseAlignments() const  { return 262144u; }

    virtual BOOL_32 HwlInitGlobalParams(const ADDR_CREATE_INPUT* pCreateIn)
    {
        ADDR_NOT_IMPLEMENTED();
        // Although GFX12 addressing should be consistent regardless of the configuration, we still need to
        // call some initialization for member variables.
        return TRUE;
    }

    virtual UINT_32 HwlComputeMaxMetaBaseAlignments() const { return 0; }

    virtual ADDR_E_RETURNCODE HwlComputeSurfaceInfo(
         const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pIn,
         ADDR3_COMPUTE_SURFACE_INFO_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_NOTSUPPORTED;
    }

    virtual ADDR_E_RETURNCODE HwlComputePipeBankXor(
        const ADDR3_COMPUTE_PIPEBANKXOR_INPUT* pIn,
        ADDR3_COMPUTE_PIPEBANKXOR_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_NOTSUPPORTED;
    }

    VOID ComputeBlockDimensionForSurf(
        const ADDR3_COMPUTE_SURFACE_INFO_PARAMS_INPUT* pIn,
        ADDR_EXTENT3D*                                 pExtent) const;

    ADDR_EXTENT3D GetMipTailDim(
        const ADDR3_COMPUTE_SURFACE_INFO_PARAMS_INPUT* pIn,
        const ADDR_EXTENT3D&                           blockDims) const;

    ADDR_E_RETURNCODE ComputeSurfaceAddrFromCoordLinear(
        const ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT*      pOut) const;

    ADDR_E_RETURNCODE ComputeSurfaceAddrFromCoordTiled(
        const ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT*      pOut) const;

    virtual ADDR_E_RETURNCODE HwlComputeSurfaceAddrFromCoordTiled(
        const ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_NOTIMPLEMENTED;
    }

    virtual ADDR_E_RETURNCODE HwlComputeNonBlockCompressedView(
        const ADDR3_COMPUTE_NONBLOCKCOMPRESSEDVIEW_INPUT* pIn,
        ADDR3_COMPUTE_NONBLOCKCOMPRESSEDVIEW_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_NOTSUPPORTED;
    }

    virtual VOID HwlComputeSubResourceOffsetForSwizzlePattern(
        const ADDR3_COMPUTE_SUBRESOURCE_OFFSET_FORSWIZZLEPATTERN_INPUT* pIn,
        ADDR3_COMPUTE_SUBRESOURCE_OFFSET_FORSWIZZLEPATTERN_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
    }

    virtual ADDR_E_RETURNCODE HwlComputeSlicePipeBankXor(
        const ADDR3_COMPUTE_SLICE_PIPEBANKXOR_INPUT* pIn,
        ADDR3_COMPUTE_SLICE_PIPEBANKXOR_OUTPUT*      pOut) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_NOTSUPPORTED;
    }

    virtual UINT_32 HwlGetEquationIndex(
        const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pIn) const
    {
        ADDR_NOT_IMPLEMENTED();
        return ADDR_INVALID_EQUATION_INDEX;
    }

    void SetEquationIndex(
        const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_INFO_OUTPUT*      pOut) const
    {
        UINT_32 equationIdx = HwlGetEquationIndex(pIn);

        if (pOut->pMipInfo != NULL)
        {
            for (UINT_32 i = 0; i < pIn->numMipLevels; i++)
            {
                pOut->pMipInfo[i].equationIndex = equationIdx;
            }
        }
    }

    ADDR_E_RETURNCODE ApplyCustomizedPitchHeight(
        const ADDR3_COMPUTE_SURFACE_INFO_INPUT* pIn,
        ADDR3_COMPUTE_SURFACE_INFO_OUTPUT*      pOut) const;

    BOOL_32 UseCustomHeight(const ADDR3_COMPUTE_SURFACE_INFO_INPUT*  pIn) const;
    BOOL_32 UseCustomPitch(const ADDR3_COMPUTE_SURFACE_INFO_INPUT*  pIn) const;
    BOOL_32 CanTrimLinearPadding(const ADDR3_COMPUTE_SURFACE_INFO_INPUT*  pIn) const;

    virtual VOID HwlCalcBlockSize(
        const ADDR3_COMPUTE_SURFACE_INFO_PARAMS_INPUT* pIn,
        ADDR_EXTENT3D*                                 pExtent) const = 0;

    virtual ADDR_EXTENT3D HwlGetMipInTailMaxSize(
        const ADDR3_COMPUTE_SURFACE_INFO_PARAMS_INPUT* pIn,
        const ADDR_EXTENT3D&                           blockDims) const = 0;

private:
    // Disallow the copy constructor
    Lib(const Lib& a);

    // Disallow the assignment operator
    Lib& operator=(const Lib& a);

    void Init();
};

} // V3
} // Addr

#endif