/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice (including the next
* paragraph) shall be included in all copies or substantial portions of the
* Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* @file pa.h
*
* @brief Definitions for primitive assembly.
*        N primitives are assembled at a time, where N is the SIMD width.
*        A state machine, that is specific for a given topology, drives the
*        assembly of vertices into triangles.
*
******************************************************************************/
#pragma once

#include "frontend.h"

struct PA_STATE
{
    DRAW_CONTEXT *pDC;              // draw context
    uint8_t* pStreamBase;           // vertex stream
    uint32_t streamSizeInVerts;     // total size of the input stream in verts

    // The topology the binner will use. In some cases the FE changes the topology from the api state.
    PRIMITIVE_TOPOLOGY binTopology;

    PA_STATE() {}
    PA_STATE(DRAW_CONTEXT *in_pDC, uint8_t* in_pStreamBase, uint32_t in_streamSizeInVerts) :
        pDC(in_pDC), pStreamBase(in_pStreamBase), streamSizeInVerts(in_streamSizeInVerts) {}

    virtual bool HasWork() = 0;
    virtual simdvector& GetSimdVector(uint32_t index, uint32_t slot) = 0;
    virtual bool Assemble(uint32_t slot, simdvector verts[]) = 0;
    virtual void AssembleSingle(uint32_t slot, uint32_t primIndex, __m128 verts[]) = 0;
    virtual bool NextPrim() = 0;
    virtual simdvertex& GetNextVsOutput() = 0;
    virtual bool GetNextStreamOutput() = 0;
    virtual simdmask& GetNextVsIndices() = 0;
    virtual uint32_t NumPrims() = 0;
    virtual void Reset() = 0;
    virtual simdscalari GetPrimID(uint32_t startID) = 0;
};

// The Optimized PA is a state machine that assembles triangles from vertex shader simd
// output. Here is the sequence
//    1. Execute FS/VS to generate a simd vertex (4 vertices for SSE simd and 8 for AVX simd).
//    2. Execute PA function to assemble and bin triangles.
//        a.    The PA function is a set of functions that collectively make up the
//            state machine for a given topology.
//                1.    We use a state index to track which PA function to call.
//        b. Often the PA function needs to 2 simd vertices in order to assemble the next triangle.
//                1.    We call this the current and previous simd vertex.
//                2.    The SSE simd is 4-wide which is not a multiple of 3 needed for triangles. In
//                    order to assemble the second triangle, for a triangle list, we'll need the
//                    last vertex from the previous simd and the first 2 vertices from the current simd.
//                3. At times the PA can assemble multiple triangles from the 2 simd vertices.
//
// This optimized PA is not cut aware, so only should be used by non-indexed draws or draws without
// cuts
struct PA_STATE_OPT : public PA_STATE
{
    simdvertex leadingVertex;           // For tri-fan
    uint32_t numPrims;              // Total number of primitives for draw.
    uint32_t numPrimsComplete;      // Total number of complete primitives.

    uint32_t numSimdPrims;          // Number of prims in current simd.

    uint32_t cur;                   // index to current VS output.
    uint32_t prev;                  // index to prev VS output. Not really needed in the state.
    uint32_t first;                 // index to first VS output. Used for trifan.

    uint32_t counter;               // state counter
    bool reset;                     // reset state

    uint32_t primIDIncr;            // how much to increment for each vector (typically vector / {1, 2})
    simdscalari primID;

    typedef bool(*PFN_PA_FUNC)(PA_STATE_OPT& state, uint32_t slot, simdvector verts[]);
    typedef void(*PFN_PA_SINGLE_FUNC)(PA_STATE_OPT& pa, uint32_t slot, uint32_t primIndex, __m128 verts[]);

    PFN_PA_FUNC        pfnPaFunc;        // PA state machine function for assembling 4 triangles.
    PFN_PA_SINGLE_FUNC pfnPaSingleFunc;  // PA state machine function for assembling single triangle.
    PFN_PA_FUNC        pfnPaFuncReset;   // initial state to set on reset

    // state used to advance the PA when Next is called
    PFN_PA_FUNC        pfnPaNextFunc;
    uint32_t           nextNumSimdPrims;
    uint32_t           nextNumPrimsIncrement;
    bool               nextReset;
    bool               isStreaming;

    simdmask tmpIndices;             // temporary index store for unused virtual function
    
    PA_STATE_OPT() {}
    PA_STATE_OPT(DRAW_CONTEXT* pDC, uint32_t numPrims, uint8_t* pStream, uint32_t streamSizeInVerts,
        bool in_isStreaming, PRIMITIVE_TOPOLOGY topo = TOP_UNKNOWN);

    bool HasWork()
    {
        return (this->numPrimsComplete < this->numPrims) ? true : false;
    }

    simdvector& GetSimdVector(uint32_t index, uint32_t slot)
    {
        simdvertex* pVertex = (simdvertex*)pStreamBase;
        return pVertex[index].attrib[slot];
    }

    // Assembles 4 triangles. Each simdvector is a single vertex from 4
    // triangles (xxxx yyyy zzzz wwww) and there are 3 verts per triangle.
    bool Assemble(uint32_t slot, simdvector verts[])
    {
        return this->pfnPaFunc(*this, slot, verts);
    }

    // Assembles 1 primitive. Each simdscalar is a vertex (xyzw).
    void AssembleSingle(uint32_t slot, uint32_t primIndex, __m128 verts[])
    {
        return this->pfnPaSingleFunc(*this, slot, primIndex, verts);
    }

    bool NextPrim()
    {
        this->pfnPaFunc = this->pfnPaNextFunc;
        this->numSimdPrims = this->nextNumSimdPrims;
        this->numPrimsComplete += this->nextNumPrimsIncrement;
        this->reset = this->nextReset;

        if (this->isStreaming)
        {
            this->reset = false;
        }

        bool morePrims = false;

        if (this->numSimdPrims > 0)
        {
            morePrims = true;
            this->numSimdPrims--;
        }
        else
        {
            this->counter = (this->reset) ? 0 : (this->counter + 1);
            this->reset = false;
        }

        this->pfnPaFunc = this->pfnPaNextFunc;

        if (!HasWork())
        {
            morePrims = false;    // no more to do
        }

        return morePrims;
    }

    simdvertex& GetNextVsOutput()
    {
        // increment cur and prev indices
        const uint32_t numSimdVerts = this->streamSizeInVerts / KNOB_SIMD_WIDTH;
        this->prev = this->cur;  // prev is undefined for first state.
        this->cur = this->counter % numSimdVerts;

        simdvertex* pVertex = (simdvertex*)pStreamBase;
        return pVertex[this->cur];
    }
    
    simdmask& GetNextVsIndices()
    {
        // unused in optimized PA, pass tmp buffer back
        return tmpIndices;
    }

    bool GetNextStreamOutput()
    {
        this->prev = this->cur;
        this->cur = this->counter;

        return HasWork();
    }

    uint32_t NumPrims()
    {
        return (this->numPrimsComplete + this->nextNumPrimsIncrement > this->numPrims) ?
            (KNOB_SIMD_WIDTH - (this->numPrimsComplete + this->nextNumPrimsIncrement - this->numPrims)) : KNOB_SIMD_WIDTH;
    }

    void SetNextState(PA_STATE_OPT::PFN_PA_FUNC pfnPaNextFunc,
        PA_STATE_OPT::PFN_PA_SINGLE_FUNC pfnPaNextSingleFunc,
        uint32_t numSimdPrims = 0,
        uint32_t numPrimsIncrement = 0,
        bool reset = false)
    {
        this->pfnPaNextFunc = pfnPaNextFunc;
        this->nextNumSimdPrims = numSimdPrims;
        this->nextNumPrimsIncrement = numPrimsIncrement;
        this->nextReset = reset;

        this->pfnPaSingleFunc = pfnPaNextSingleFunc;
    }

    void Reset()
    {
        this->pfnPaFunc = this->pfnPaFuncReset;
        this->numPrimsComplete = 0;
        this->numSimdPrims = 0;
        this->cur = 0;
        this->prev = 0;
        this->first = 0;
        this->counter = 0;
        this->reset = false;
    }

    simdscalari GetPrimID(uint32_t startID)
    {
        return _simd_add_epi32(this->primID,
            _simd_set1_epi32(startID + this->primIDIncr * (this->numPrimsComplete / KNOB_SIMD_WIDTH)));
    }
};

// helper C wrappers to avoid having to rewrite all the PA topology state functions
INLINE void SetNextPaState(PA_STATE_OPT& pa, PA_STATE_OPT::PFN_PA_FUNC pfnPaNextFunc,
    PA_STATE_OPT::PFN_PA_SINGLE_FUNC pfnPaNextSingleFunc,
    uint32_t numSimdPrims = 0,
    uint32_t numPrimsIncrement = 0,
    bool reset = false)
{
    return pa.SetNextState(pfnPaNextFunc, pfnPaNextSingleFunc, numSimdPrims, numPrimsIncrement, reset);
}
INLINE simdvector& PaGetSimdVector(PA_STATE& pa, uint32_t index, uint32_t slot)
{
    return pa.GetSimdVector(index, slot);
}

INLINE __m128 swizzleLane0(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpacklo_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpacklo_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpacklo_ps(tmp0, tmp1), 0);
}

INLINE __m128 swizzleLane1(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpacklo_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpacklo_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpackhi_ps(tmp0, tmp1), 0);
}

INLINE __m128 swizzleLane2(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpackhi_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpackhi_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpacklo_ps(tmp0, tmp1), 0);
}

INLINE __m128 swizzleLane3(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpackhi_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpackhi_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpackhi_ps(tmp0, tmp1), 0);
}

INLINE __m128 swizzleLane4(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpacklo_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpacklo_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpacklo_ps(tmp0, tmp1), 1);

}

INLINE __m128 swizzleLane5(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpacklo_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpacklo_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpackhi_ps(tmp0, tmp1), 1);
}

INLINE __m128 swizzleLane6(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpackhi_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpackhi_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpacklo_ps(tmp0, tmp1), 1);
}

INLINE __m128 swizzleLane7(const simdvector &a)
{
    simdscalar tmp0 = _mm256_unpackhi_ps(a.x, a.z);
    simdscalar tmp1 = _mm256_unpackhi_ps(a.y, a.w);
    return _mm256_extractf128_ps(_mm256_unpackhi_ps(tmp0, tmp1), 1);
}

INLINE __m128 swizzleLaneN(const simdvector &a, int lane)
{
    switch (lane) {
    case 0:
        return swizzleLane0(a);
    case 1:
        return swizzleLane1(a);
    case 2:
        return swizzleLane2(a);
    case 3:
        return swizzleLane3(a);
    case 4:
        return swizzleLane4(a);
    case 5:
        return swizzleLane5(a);
    case 6:
        return swizzleLane6(a);
    case 7:
        return swizzleLane7(a);
    default:
        return _mm_setzero_ps();
    }
}

// Cut-aware primitive assembler.
struct PA_STATE_CUT : public PA_STATE
{
    simdmask* pCutIndices;          // cut indices buffer, 1 bit per vertex
    uint32_t numVerts;              // number of vertices available in buffer store
    uint32_t numAttribs;            // number of attributes
    int32_t numRemainingVerts;      // number of verts remaining to be assembled
    uint32_t numVertsToAssemble;    // total number of verts to assemble for the draw
    OSALIGNSIMD(uint32_t) indices[MAX_NUM_VERTS_PER_PRIM][KNOB_SIMD_WIDTH];    // current index buffer for gather
    simdscalari vOffsets[MAX_NUM_VERTS_PER_PRIM];           // byte offsets for currently assembling simd
    uint32_t numPrimsAssembled;     // number of primitives that are fully assembled
    uint32_t headVertex;            // current unused vertex slot in vertex buffer store
    uint32_t tailVertex;            // beginning vertex currently assembling
    uint32_t curVertex;             // current unprocessed vertex
    uint32_t startPrimId;           // starting prim id
    simdscalari vPrimId;            // vector of prim ID
    bool needOffsets;               // need to compute gather offsets for current SIMD
    uint32_t vertsPerPrim;
    simdvertex tmpVertex;               // temporary simdvertex for unimplemented API
    bool processCutVerts;           // vertex indices with cuts should be processed as normal, otherwise they
                                    // are ignored.  Fetch shader sends invalid verts on cuts that should be ignored
                                    // while the GS sends valid verts for every index 
    // Topology state tracking
    uint32_t vert[MAX_NUM_VERTS_PER_PRIM];
    uint32_t curIndex;
    bool reverseWinding;            // indicates reverse winding for strips
    int32_t adjExtraVert;           // extra vert uses for tristrip w/ adj

    typedef void(PA_STATE_CUT::* PFN_PA_FUNC)(uint32_t vert, bool finish);
    PFN_PA_FUNC pfnPa;              // per-topology function that processes a single vert

    PA_STATE_CUT() {}
    PA_STATE_CUT(DRAW_CONTEXT* pDC, uint8_t* in_pStream, uint32_t in_streamSizeInVerts, simdmask* in_pIndices, uint32_t in_numVerts, 
        uint32_t in_numAttribs, PRIMITIVE_TOPOLOGY topo, bool in_processCutVerts)
        : PA_STATE(pDC, in_pStream, in_streamSizeInVerts)
    {
        numVerts = in_streamSizeInVerts;
        numAttribs = in_numAttribs;
        binTopology = topo;
        needOffsets = false;
        processCutVerts = in_processCutVerts;

        numVertsToAssemble = numRemainingVerts = in_numVerts;
        numPrimsAssembled = 0;
        headVertex = tailVertex = curVertex = 0;

        curIndex = 0;
        pCutIndices = in_pIndices;
        memset(indices, 0, sizeof(indices));
        vPrimId = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
        reverseWinding = false;
        adjExtraVert = -1;

        bool gsEnabled = pDC->pState->state.gsState.gsEnable;
        vertsPerPrim = NumVertsPerPrim(topo, gsEnabled);

        switch (topo)
        {
        case TOP_TRIANGLE_LIST:     pfnPa = &PA_STATE_CUT::ProcessVertTriList; break;
        case TOP_TRI_LIST_ADJ:      pfnPa = gsEnabled ? &PA_STATE_CUT::ProcessVertTriListAdj : &PA_STATE_CUT::ProcessVertTriListAdjNoGs; break;
        case TOP_TRIANGLE_STRIP:    pfnPa = &PA_STATE_CUT::ProcessVertTriStrip; break;
        case TOP_TRI_STRIP_ADJ:     if (gsEnabled)
                                    {
                                        pfnPa = &PA_STATE_CUT::ProcessVertTriStripAdj < true > ;
                                    }
                                    else
                                    {
                                        pfnPa = &PA_STATE_CUT::ProcessVertTriStripAdj < false > ;
                                    }
                                    break;

        case TOP_POINT_LIST:        pfnPa = &PA_STATE_CUT::ProcessVertPointList; break;
        case TOP_LINE_LIST:         pfnPa = &PA_STATE_CUT::ProcessVertLineList; break;
        case TOP_LINE_LIST_ADJ:     pfnPa = gsEnabled ? &PA_STATE_CUT::ProcessVertLineListAdj : &PA_STATE_CUT::ProcessVertLineListAdjNoGs; break;
        case TOP_LINE_STRIP:        pfnPa = &PA_STATE_CUT::ProcessVertLineStrip; break;
        case TOP_LISTSTRIP_ADJ:     pfnPa = gsEnabled ? &PA_STATE_CUT::ProcessVertLineStripAdj : &PA_STATE_CUT::ProcessVertLineStripAdjNoGs; break;
        default: assert(0 && "Unimplemented topology");
        }
    }

    simdvertex& GetNextVsOutput()
    {
        uint32_t vertexIndex = this->headVertex / KNOB_SIMD_WIDTH;
        this->headVertex = (this->headVertex + KNOB_SIMD_WIDTH) % this->numVerts;
        this->needOffsets = true;
        return ((simdvertex*)pStreamBase)[vertexIndex];
    }

    simdmask& GetNextVsIndices()
    {
        uint32_t vertexIndex = this->headVertex / KNOB_SIMD_WIDTH;
        simdmask* pCurCutIndex = this->pCutIndices + vertexIndex;
        return *pCurCutIndex;
    }

    simdvector& GetSimdVector(uint32_t index, uint32_t slot)
    {
        // unused
        SWR_ASSERT(0 && "Not implemented");
        return this->tmpVertex.attrib[0];
    }

    bool GetNextStreamOutput()
    {
        this->headVertex += KNOB_SIMD_WIDTH;
        this->needOffsets = true;
        return HasWork();
    }

    simdscalari GetPrimID(uint32_t startID)
    {
        return _simd_add_epi32(_simd_set1_epi32(startID), this->vPrimId);
    }

    void Reset()
    {
        this->numRemainingVerts = this->numVertsToAssemble;
        this->numPrimsAssembled = 0;
        this->curIndex = 0;
        this->curVertex = 0;
        this->tailVertex = 0;
        this->headVertex = 0;
        this->reverseWinding = false;
        this->adjExtraVert = -1;
        this->vPrimId = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
    }

    bool HasWork()
    {
        return this->numRemainingVerts > 0 || this->adjExtraVert != -1;
    }

    bool IsVertexStoreFull()
    {
        return ((this->headVertex + KNOB_SIMD_WIDTH) % this->numVerts) == this->tailVertex;
    }

    void RestartTopology()
    {
        this->curIndex = 0;
        this->reverseWinding = false;
        this->adjExtraVert = -1;
    }

    bool IsCutIndex(uint32_t vertex)
    {
        uint32_t vertexIndex = vertex / KNOB_SIMD_WIDTH;
        uint32_t vertexOffset = vertex & (KNOB_SIMD_WIDTH - 1);
        return _bittest((const LONG*)&this->pCutIndices[vertexIndex], vertexOffset) == 1;
    }

    // iterates across the unprocessed verts until we hit the end or we 
    // have assembled SIMD prims
    void ProcessVerts()
    {
        while (this->numPrimsAssembled != KNOB_SIMD_WIDTH &&
            this->numRemainingVerts > 0 &&
            this->curVertex != this->headVertex)
        {
            // if cut index, restart topology 
            if (IsCutIndex(this->curVertex))
            {
                if (this->processCutVerts)
                {
                    (this->*pfnPa)(this->curVertex, false);
                }
                // finish off tri strip w/ adj before restarting topo
                if (this->adjExtraVert != -1)
                {
                    (this->*pfnPa)(this->curVertex, true);
                }
                RestartTopology();
            }
            else
            {
                (this->*pfnPa)(this->curVertex, false);
            }

            this->curVertex = (this->curVertex + 1) % this->numVerts;
            this->numRemainingVerts--;
        }

        // special case last primitive for tri strip w/ adj
        if (this->numPrimsAssembled != KNOB_SIMD_WIDTH && this->numRemainingVerts == 0 && this->adjExtraVert != -1)
        {
            (this->*pfnPa)(this->curVertex, true);
        }
    }

    void Advance()
    {
        // done with current batch
        // advance tail to the current unsubmitted vertex
        this->tailVertex = this->curVertex;
        this->numPrimsAssembled = 0;
        this->vPrimId = _simd_add_epi32(vPrimId, _simd_set1_epi32(KNOB_SIMD_WIDTH));
    }

    bool NextPrim()
    {
        // if we've assembled enough prims, we can advance to the next set of verts
        if (this->numPrimsAssembled == KNOB_SIMD_WIDTH || this->numRemainingVerts <= 0)
        {
            Advance();
        }
        return false;
    }

    void ComputeOffsets()
    {
        for (uint32_t v = 0; v < this->vertsPerPrim; ++v)
        {
            simdscalari vIndices = *(simdscalari*)&this->indices[v][0];

            // step to simdvertex batch
            const uint32_t simdShift = 3; // @todo make knob
            simdscalari vVertexBatch = _simd_srai_epi32(vIndices, simdShift);
            this->vOffsets[v] = _simd_mullo_epi32(vVertexBatch, _simd_set1_epi32(sizeof(simdvertex)));

            // step to index
            const uint32_t simdMask = 0x7; // @todo make knob
            simdscalari vVertexIndex = _simd_and_si(vIndices, _simd_set1_epi32(simdMask));
            this->vOffsets[v] = _simd_add_epi32(this->vOffsets[v], _simd_mullo_epi32(vVertexIndex, _simd_set1_epi32(sizeof(float))));
        }
    }

    bool Assemble(uint32_t slot, simdvector result[])
    {
        // process any outstanding verts
        ProcessVerts();

        // return false if we don't have enough prims assembled
        if (this->numPrimsAssembled != KNOB_SIMD_WIDTH && this->numRemainingVerts > 0)
        {
            return false;
        }

        // cache off gather offsets given the current SIMD set of indices the first time we get an assemble
        if (this->needOffsets)
        {
            ComputeOffsets();
            this->needOffsets = false;
        }

        for (uint32_t v = 0; v < this->vertsPerPrim; ++v)
        {
            simdscalari offsets = this->vOffsets[v];

            // step to attribute
            offsets = _simd_add_epi32(offsets, _simd_set1_epi32(slot * sizeof(simdvector)));

            float* pBase = (float*)this->pStreamBase;
            for (uint32_t c = 0; c < 4; ++c)
            {
                result[v].v[c] = _simd_i32gather_ps(pBase, offsets, 1);

                // move base to next component
                pBase += KNOB_SIMD_WIDTH;
            }
        }

        return true;
    }

    void AssembleSingle(uint32_t slot, uint32_t triIndex, __m128 tri[3])
    {
        // move to slot
        for (uint32_t v = 0; v < this->vertsPerPrim; ++v)
        {
            uint32_t* pOffset = (uint32_t*)&this->vOffsets[v];
            uint32_t offset = pOffset[triIndex];
            offset += sizeof(simdvector) * slot;
            float* pVert = (float*)&tri[v];
            for (uint32_t c = 0; c < 4; ++c)
            {
                float* pComponent = (float*)(this->pStreamBase + offset);
                pVert[c] = *pComponent;
                offset += KNOB_SIMD_WIDTH * sizeof(float);
            }
        }
    }

    uint32_t NumPrims()
    {
        return this->numPrimsAssembled;
    }

    // Per-topology functions
    void ProcessVertTriStrip(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 3)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            if (reverseWinding)
            {
                this->indices[1][this->numPrimsAssembled] = this->vert[2];
                this->indices[2][this->numPrimsAssembled] = this->vert[1];
            }
            else
            {
                this->indices[1][this->numPrimsAssembled] = this->vert[1];
                this->indices[2][this->numPrimsAssembled] = this->vert[2];
            }

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->vert[0] = this->vert[1];
            this->vert[1] = this->vert[2];
            this->curIndex = 2;
            this->reverseWinding ^= 1;
        }
    }

    template<bool gsEnabled>
    void AssembleTriStripAdj()
    {
        if (!gsEnabled)
        {
            this->vert[1] = this->vert[2];
            this->vert[2] = this->vert[4];

            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];

            this->vert[4] = this->vert[2];
            this->vert[2] = this->vert[1];
        }
        else
        {
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];
            this->indices[3][this->numPrimsAssembled] = this->vert[3];
            this->indices[4][this->numPrimsAssembled] = this->vert[4];
            this->indices[5][this->numPrimsAssembled] = this->vert[5];
        }
        this->numPrimsAssembled++;
    }


    template<bool gsEnabled>
    void ProcessVertTriStripAdj(uint32_t index, bool finish)
    {
        // handle last primitive of tristrip
        if (finish && this->adjExtraVert != -1)
        {
            this->vert[3] = this->adjExtraVert;
            AssembleTriStripAdj<gsEnabled>();
            this->adjExtraVert = -1;
            return;
        }

        switch (this->curIndex)
        {
        case 0:
        case 1:
        case 2:
        case 4:
            this->vert[this->curIndex] = index;
            this->curIndex++;
            break;
        case 3:
            this->vert[5] = index;
            this->curIndex++;
            break;
        case 5:
            if (this->adjExtraVert == -1)
            {
                this->adjExtraVert = index;
            }
            else
            {
                this->vert[3] = index;
                if (!gsEnabled)
                {
                    AssembleTriStripAdj<gsEnabled>();

                    uint32_t nextTri[6];
                    if (this->reverseWinding)
                    {
                        nextTri[0] = this->vert[4];
                        nextTri[1] = this->vert[0];
                        nextTri[2] = this->vert[2];
                        nextTri[4] = this->vert[3];
                        nextTri[5] = this->adjExtraVert;
                    }
                    else
                    {
                        nextTri[0] = this->vert[2];
                        nextTri[1] = this->adjExtraVert;
                        nextTri[2] = this->vert[3];
                        nextTri[4] = this->vert[4];
                        nextTri[5] = this->vert[0];
                    }
                    for (uint32_t i = 0; i < 6; ++i)
                    {
                        this->vert[i] = nextTri[i];
                    }

                    this->adjExtraVert = -1;
                    this->reverseWinding ^= 1;
                }
                else
                {
                    this->curIndex++;
                }
            }
            break;
        case 6:
            SWR_ASSERT(this->adjExtraVert != -1, "Algorith failure!");
            AssembleTriStripAdj<gsEnabled>();
            
            uint32_t nextTri[6];
            if (this->reverseWinding)
            {
                nextTri[0] = this->vert[4];
                nextTri[1] = this->vert[0];
                nextTri[2] = this->vert[2];
                nextTri[4] = this->vert[3];
                nextTri[5] = this->adjExtraVert;
            }
            else
            {
                nextTri[0] = this->vert[2];
                nextTri[1] = this->adjExtraVert;
                nextTri[2] = this->vert[3];
                nextTri[4] = this->vert[4];
                nextTri[5] = this->vert[0]; 
            }
            for (uint32_t i = 0; i < 6; ++i)
            {
                this->vert[i] = nextTri[i];
            }
            this->reverseWinding ^= 1;
            this->adjExtraVert = index;
            this->curIndex--;
            break;
        }
    }

    void ProcessVertTriList(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 3)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->curIndex = 0;
        }
    }

    void ProcessVertTriListAdj(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 6)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];
            this->indices[3][this->numPrimsAssembled] = this->vert[3];
            this->indices[4][this->numPrimsAssembled] = this->vert[4];
            this->indices[5][this->numPrimsAssembled] = this->vert[5];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->curIndex = 0;
        }
    }

    void ProcessVertTriListAdjNoGs(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 6)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[2];
            this->indices[2][this->numPrimsAssembled] = this->vert[4];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->curIndex = 0;
        }
    }


    void ProcessVertLineList(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 2)
        {
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];

            this->numPrimsAssembled++;
            this->curIndex = 0;
        }
    }

    void ProcessVertLineStrip(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 2)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->vert[0] = this->vert[1];
            this->curIndex = 1;
        }
    }

    void ProcessVertLineStripAdj(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 4)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];
            this->indices[3][this->numPrimsAssembled] = this->vert[3];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->vert[0] = this->vert[1];
            this->vert[1] = this->vert[2];
            this->vert[2] = this->vert[3];
            this->curIndex = 3;
        }
    }

    void ProcessVertLineStripAdjNoGs(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 4)
        {
            // assembled enough verts for prim, add to gather indices
            this->indices[0][this->numPrimsAssembled] = this->vert[1];
            this->indices[1][this->numPrimsAssembled] = this->vert[2];

            // increment numPrimsAssembled
            this->numPrimsAssembled++;

            // set up next prim state
            this->vert[0] = this->vert[1];
            this->vert[1] = this->vert[2];
            this->vert[2] = this->vert[3];
            this->curIndex = 3;
        }
    }

    void ProcessVertLineListAdj(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 4)
        {
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->indices[1][this->numPrimsAssembled] = this->vert[1];
            this->indices[2][this->numPrimsAssembled] = this->vert[2];
            this->indices[3][this->numPrimsAssembled] = this->vert[3];

            this->numPrimsAssembled++;
            this->curIndex = 0;
        }
    }

    void ProcessVertLineListAdjNoGs(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 4)
        {
            this->indices[0][this->numPrimsAssembled] = this->vert[1];
            this->indices[1][this->numPrimsAssembled] = this->vert[2];

            this->numPrimsAssembled++;
            this->curIndex = 0;
        }
    }

    void ProcessVertPointList(uint32_t index, bool finish)
    {
        this->vert[this->curIndex] = index;
        this->curIndex++;
        if (this->curIndex == 1)
        {
            this->indices[0][this->numPrimsAssembled] = this->vert[0];
            this->numPrimsAssembled++;
            this->curIndex = 0;
        }
    }
};

// Primitive Assembly for data output from the DomainShader.
struct PA_TESS : PA_STATE
{
    PA_TESS(
        DRAW_CONTEXT *in_pDC,
        const simdscalar* in_pVertData,
        uint32_t in_attributeStrideInVectors,
        uint32_t in_numAttributes,
        uint32_t* (&in_ppIndices)[3],
        uint32_t in_numPrims,
        PRIMITIVE_TOPOLOGY in_binTopology) :

        PA_STATE(in_pDC, nullptr, 0),
        m_pVertexData(in_pVertData),
        m_attributeStrideInVectors(in_attributeStrideInVectors),
        m_numAttributes(in_numAttributes),
        m_numPrims(in_numPrims)
    {
        m_vPrimId = _simd_setzero_si();
        binTopology = in_binTopology;
        m_ppIndices[0] = in_ppIndices[0];
        m_ppIndices[1] = in_ppIndices[1];
        m_ppIndices[2] = in_ppIndices[2];

        switch (binTopology)
        {
        case TOP_POINT_LIST:
            m_numVertsPerPrim = 1;
            break;

        case TOP_LINE_LIST:
            m_numVertsPerPrim = 2;
            break;

        case TOP_TRIANGLE_LIST:
            m_numVertsPerPrim = 3;
            break;

        default:
            SWR_ASSERT(0, "Invalid binTopology (%d) for %s", binTopology, __FUNCTION__);
            break;
        }
    }

    bool HasWork()
    {
        return m_numPrims != 0;
    }

    simdvector& GetSimdVector(uint32_t index, uint32_t slot)
    {
        SWR_ASSERT(0, "%s NOT IMPLEMENTED", __FUNCTION__);
        static simdvector junk = { 0 };
        return junk;
    }

    static simdscalari GenPrimMask(uint32_t numPrims)
    {
        SWR_ASSERT(numPrims <= KNOB_SIMD_WIDTH);
#if KNOB_SIMD_WIDTH == 8
        static const OSALIGN(int32_t, 64) maskGen[KNOB_SIMD_WIDTH * 2] =
        {
            -1, -1, -1, -1, -1, -1, -1, -1,
             0,  0,  0,  0,  0,  0,  0,  0
        };
#elif KNOB_SIMD_WIDTH == 16
        static const OSALIGN(int32_t, 128) maskGen[KNOB_SIMD_WIDTH * 2] =
        {
            -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
             0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
        };
#else
#error "Help, help, I can't get up!"
#endif

        return _simd_loadu_si((const simdscalari*)&maskGen[KNOB_SIMD_WIDTH - numPrims]);
    }

    bool Assemble(uint32_t slot, simdvector verts[])
    {
        static_assert(KNOB_SIMD_WIDTH == 8, "Need to revisit this when AVX512 is implemented");
        SWR_ASSERT(slot < m_numAttributes);

        uint32_t numPrimsToAssemble = PA_TESS::NumPrims();
        if (0 == numPrimsToAssemble)
        {
            return false;
        }

        simdscalari mask = GenPrimMask(numPrimsToAssemble);

        const float* pBaseAttrib = (const float*)&m_pVertexData[slot * m_attributeStrideInVectors * 4];
        for (uint32_t i = 0; i < m_numVertsPerPrim; ++i)
        {
            simdscalari indices = _simd_load_si((const simdscalari*)m_ppIndices[i]);

            const float* pBase = pBaseAttrib;
            for (uint32_t c = 0; c < 4; ++c)
            {
                verts[i].v[c] = _simd_mask_i32gather_ps(
                    _simd_setzero_ps(),
                    pBase,
                    indices,
                    _simd_castsi_ps(mask),
                    4 /* gcc doesn't like sizeof(float) */);
                pBase += m_attributeStrideInVectors * KNOB_SIMD_WIDTH;
            }
        }

        return true;
    }

    void AssembleSingle(uint32_t slot, uint32_t primIndex, __m128 verts[])
    {
        SWR_ASSERT(slot < m_numAttributes);
        SWR_ASSERT(primIndex < PA_TESS::NumPrims());

        const float* pVertDataBase = (const float*)&m_pVertexData[slot * m_attributeStrideInVectors * 4];
        for (uint32_t i = 0; i < m_numVertsPerPrim; ++i)
        {
            uint32_t index = m_ppIndices[i][primIndex];
            const float* pVertData = pVertDataBase;
            float* pVert = (float*)&verts[i];

            for (uint32_t c = 0; c < 4; ++c)
            {
                pVert[c] = pVertData[index];
                pVertData += m_attributeStrideInVectors * KNOB_SIMD_WIDTH;
            }
        }
    }

    bool NextPrim()
    {
        uint32_t numPrims = PA_TESS::NumPrims();
        m_numPrims -= numPrims;
        m_ppIndices[0] += numPrims;
        m_ppIndices[1] += numPrims;
        m_ppIndices[2] += numPrims;

        return HasWork();
    }

    simdvertex& GetNextVsOutput()
    {
        SWR_ASSERT(0, "%s", __FUNCTION__);
        static simdvertex junk;
        return junk;
    }

    bool GetNextStreamOutput()
    {
        SWR_ASSERT(0, "%s", __FUNCTION__);
        return false;
    }

    simdmask& GetNextVsIndices()
    {
        SWR_ASSERT(0, "%s", __FUNCTION__);
        static simdmask junk;
        return junk;
    }

    uint32_t NumPrims()
    {
        return std::min<uint32_t>(m_numPrims, KNOB_SIMD_WIDTH);
    }

    void Reset() { SWR_ASSERT(0); };

    simdscalari GetPrimID(uint32_t startID)
    {
        return _simd_add_epi32(_simd_set1_epi32(startID), m_vPrimId);
    }

private:
    const simdscalar*   m_pVertexData = nullptr;
    uint32_t            m_attributeStrideInVectors = 0;
    uint32_t            m_numAttributes = 0;
    uint32_t            m_numPrims = 0;
    uint32_t*           m_ppIndices[3];

    uint32_t            m_numVertsPerPrim = 0;

    simdscalari         m_vPrimId;
};

// Primitive Assembler factory class, responsible for creating and initializing the correct assembler
// based on state.
template <bool IsIndexedT>
struct PA_FACTORY
{
    PA_FACTORY(DRAW_CONTEXT* pDC, PRIMITIVE_TOPOLOGY in_topo, uint32_t numVerts) : topo(in_topo)
    {
#if KNOB_ENABLE_CUT_AWARE_PA == TRUE
        const API_STATE& state = GetApiState(pDC);
        if ((IsIndexedT && (
            topo == TOP_TRIANGLE_STRIP || topo == TOP_POINT_LIST ||
            topo == TOP_LINE_LIST || topo == TOP_LINE_STRIP ||
            topo == TOP_TRIANGLE_LIST || topo == TOP_LINE_LIST_ADJ ||
            topo == TOP_LISTSTRIP_ADJ || topo == TOP_TRI_LIST_ADJ ||
            topo == TOP_TRI_STRIP_ADJ)) ||

            // non-indexed draws with adjacency topologies must use cut-aware PA until we add support
            // for them in the optimized PA
            (!IsIndexedT && (
            topo == TOP_LINE_LIST_ADJ || topo == TOP_LISTSTRIP_ADJ || topo == TOP_TRI_LIST_ADJ || topo == TOP_TRI_STRIP_ADJ)))
        {
            memset(&indexStore, 0, sizeof(indexStore));
            DWORD numAttribs;
            _BitScanReverse(&numAttribs, state.feAttribMask);
            numAttribs++;
            new (&this->paCut) PA_STATE_CUT(pDC, (uint8_t*)&this->vertexStore[0], MAX_NUM_VERTS_PER_PRIM * KNOB_SIMD_WIDTH, 
                &this->indexStore[0], numVerts, numAttribs, state.topology, false);
            cutPA = true;
        }
        else
#endif
        {
            uint32_t numPrims = GetNumPrims(in_topo, numVerts);
            new (&this->paOpt) PA_STATE_OPT(pDC, numPrims, (uint8_t*)&this->vertexStore[0], MAX_NUM_VERTS_PER_PRIM * KNOB_SIMD_WIDTH, false);
            cutPA = false;
        }

    }

    PA_STATE& GetPA()
    {
#if KNOB_ENABLE_CUT_AWARE_PA == TRUE
        if (cutPA)
        {
            return this->paCut;
        }
        else
#endif
        {
            return this->paOpt;
        }
    }

    PA_STATE_OPT paOpt;
    PA_STATE_CUT paCut;
    bool cutPA;

    PRIMITIVE_TOPOLOGY topo;

    simdvertex vertexStore[MAX_NUM_VERTS_PER_PRIM];
    simdmask indexStore[MAX_NUM_VERTS_PER_PRIM];
};
